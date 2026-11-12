#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bloom.h"

namespace lsm {

struct SstEntry {
    std::string key;
    std::string value;
    bool tombstone = false;
};

enum class SstStatus { FOUND, DELETED, NOT_FOUND };

class SSTable {
public:
    static constexpr std::uint32_t MAGIC = 0x31545353u;
    static constexpr std::size_t   INDEX_INTERVAL = 16;

    static void build(const std::string& path, const std::vector<SstEntry>& entries) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("SSTable::build: cannot open " + path);

        put_u32(out, MAGIC);
        put_u64(out, entries.size());
        put_u64(out, 0);
        put_u64(out, 0);

        Bloom bloom = Bloom::optimal(std::max<std::size_t>(64, entries.size()), 0.01);
        std::vector<std::pair<std::string, std::uint64_t>> index;

        for (std::size_t i = 0; i < entries.size(); ++i) {
            const SstEntry& e = entries[i];
            if (i % INDEX_INTERVAL == 0)
                index.push_back({e.key, static_cast<std::uint64_t>(out.tellp())});
            put_u32(out, static_cast<std::uint32_t>(e.key.size()));
            out.write(e.key.data(), e.key.size());
            put_u32(out, static_cast<std::uint32_t>(e.value.size()));
            out.write(e.value.data(), e.value.size());
            out.put(e.tombstone ? 1 : 0);
            bloom.add(e.key);
        }

        const std::uint64_t index_offset = out.tellp();
        for (const auto& [key, offset] : index) {
            put_u32(out, static_cast<std::uint32_t>(key.size()));
            out.write(key.data(), key.size());
            put_u64(out, offset);
        }

        const std::uint64_t bloom_offset = out.tellp();
        const std::vector<std::uint8_t> blob = bloom.to_bytes();
        out.write(reinterpret_cast<const char*>(blob.data()), blob.size());

        out.seekp(4 + 8);
        put_u64(out, index_offset);
        put_u64(out, bloom_offset);
        out.close();
        if (!out) throw std::runtime_error("SSTable::build: write failed for " + path);
    }

    explicit SSTable(const std::string& path) : path_(path) {
        in_.open(path_, std::ios::binary);
        if (!in_) throw std::runtime_error("SSTable: cannot open " + path_);
        std::ifstream& in = in_;

        if (get_u32(in) != MAGIC) throw std::runtime_error("SSTable: bad magic in " + path_);
        count_        = get_u64(in);
        index_offset_ = get_u64(in);
        bloom_offset_ = get_u64(in);

        in.seekg(static_cast<std::streamoff>(index_offset_));
        while (static_cast<std::uint64_t>(in.tellg()) < bloom_offset_) {
            std::uint32_t klen = get_u32(in);
            std::string key(klen, '\0');
            in.read(&key[0], klen);
            std::uint64_t offset = get_u64(in);
            index_.push_back({std::move(key), offset});
        }

        in.seekg(0, std::ios::end);
        const std::uint64_t file_size = static_cast<std::uint64_t>(in.tellg());
        std::vector<std::uint8_t> blob(file_size - bloom_offset_);
        in.seekg(static_cast<std::streamoff>(bloom_offset_));
        in.read(reinterpret_cast<char*>(blob.data()), blob.size());
        bloom_ = Bloom::from_bytes(blob.data(), blob.size());
    }

    SstStatus get(const std::string& key, std::string& value) const {
        if (!bloom_.maybe_present(key)) return SstStatus::NOT_FOUND;

        auto it = std::upper_bound(index_.begin(), index_.end(), key,
            [](const std::string& k, const std::pair<std::string, std::uint64_t>& e) {
                return k < e.first;
            });
        if (it == index_.begin()) return SstStatus::NOT_FOUND;
        --it;

        const std::uint64_t start = it->second;
        const std::uint64_t end   = (it + 1 != index_.end()) ? (it + 1)->second : index_offset_;

        std::ifstream& in = in_;
        in.clear();
        in.seekg(static_cast<std::streamoff>(start));
        while (in && static_cast<std::uint64_t>(in.tellg()) < end) {
            std::uint32_t klen = get_u32(in);
            std::string k(klen, '\0');
            in.read(&k[0], klen);
            std::uint32_t vlen = get_u32(in);
            std::string v(vlen, '\0');
            in.read(&v[0], vlen);
            bool tomb = in.get() != 0;

            if (k == key) {
                if (tomb) return SstStatus::DELETED;
                value = std::move(v);
                return SstStatus::FOUND;
            }
            if (k > key) break;
        }
        return SstStatus::NOT_FOUND;
    }

    std::vector<SstEntry> read_all() const {
        std::vector<SstEntry> all;
        std::ifstream in(path_, std::ios::binary);
        in.seekg(4 + 8 + 8 + 8);
        while (in && static_cast<std::uint64_t>(in.tellg()) < index_offset_) {
            SstEntry e;
            std::uint32_t klen = get_u32(in);
            e.key.resize(klen);
            in.read(&e.key[0], klen);
            std::uint32_t vlen = get_u32(in);
            e.value.resize(vlen);
            in.read(&e.value[0], vlen);
            e.tombstone = in.get() != 0;
            all.push_back(std::move(e));
        }
        return all;
    }

    std::uint64_t count() const { return count_; }
    const std::string& path() const { return path_; }

private:
    static void put_u32(std::ofstream& o, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) o.put(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    static void put_u64(std::ofstream& o, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) o.put(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    static std::uint32_t get_u32(std::ifstream& in) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(in.get())) << (8 * i);
        return v;
    }
    static std::uint64_t get_u64(std::ifstream& in) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(in.get())) << (8 * i);
        return v;
    }

    std::string path_;
    std::uint64_t count_ = 0, index_offset_ = 0, bloom_offset_ = 0;
    std::vector<std::pair<std::string, std::uint64_t>> index_;
    Bloom bloom_;
    mutable std::ifstream in_;
};

}