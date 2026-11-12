#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lsm {

enum class WalOp : uint8_t { PUT = 1, DEL = 2 };

struct WalRecord {
    WalOp op;
    std::string key;
    std::string value;
};

inline uint32_t crc32(const uint8_t* data, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= data[i];
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

class Wal {
public:
    explicit Wal(std::string path) : path_(std::move(path)) {
        // Cut off a half-written record left by a crash, so new
        // records are not appended after garbage.
        uint64_t good = 0;
        replay(path_, &good);
        std::error_code ec;
        if (std::filesystem::exists(path_, ec) &&
            std::filesystem::file_size(path_, ec) > good)
            std::filesystem::resize_file(path_, good, ec);

        out_.open(path_, std::ios::binary | std::ios::app);
        if (!out_.is_open())
            throw std::runtime_error("Wal: cannot open " + path_);
    }

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    void append(WalOp op, std::string_view key, std::string_view value) {
        std::vector<uint8_t> buf;
        buf.push_back(static_cast<uint8_t>(op));
        write_32(buf, static_cast<uint32_t>(key.size()));
        buf.insert(buf.end(), key.begin(), key.end());
        write_32(buf, static_cast<uint32_t>(value.size()));
        buf.insert(buf.end(), value.begin(), value.end());

        write_32(buf, crc32(buf.data(), buf.size()));   // checksum of everything before it

        out_.write(reinterpret_cast<const char*>(buf.data()),
                   static_cast<std::streamsize>(buf.size()));
        out_.flush();
    }

    void truncate() {
        out_.close();
        std::ofstream clear(path_, std::ios::binary | std::ios::trunc);
        clear.close();
        out_.open(path_, std::ios::binary | std::ios::app);
        if (!out_.is_open())
            throw std::runtime_error("Wal: cannot reopen " + path_);
    }

    // If goodBytes is given, it receives the size of the valid part of the file.
    static std::vector<WalRecord> replay(const std::string& path, uint64_t* goodBytes = nullptr) {
        std::vector<WalRecord> records;
        uint64_t good = 0;
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) return records;
        const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
        in.seekg(0);

        while (true) {
            char opByte;
            if (!in.read(&opByte, 1)) break;
            const uint8_t opRaw = static_cast<uint8_t>(opByte);
            if (opRaw != 1 && opRaw != 2) break;
            const auto op = static_cast<WalOp>(opRaw);

            uint32_t keyLen, valLen;
            if (!read_32(in, keyLen)) break;
            if (keyLen > fileSize) break;               // torn/corrupt length
            std::string key(keyLen, '\0');
            if (keyLen > 0 && !in.read(key.data(), keyLen)) break;

            if (!read_32(in, valLen)) break;
            if (valLen > fileSize) break;
            std::string value(valLen, '\0');
            if (valLen > 0 && !in.read(value.data(), valLen)) break;

            uint32_t received;
            if (!read_32(in, received)) break;

            // Rebuild the bytes that were checksummed and compare
            std::vector<uint8_t> buf;
            buf.push_back(opRaw);
            write_32(buf, keyLen);
            buf.insert(buf.end(), key.begin(), key.end());
            write_32(buf, valLen);
            buf.insert(buf.end(), value.begin(), value.end());

            if (received != crc32(buf.data(), buf.size())) break;
            records.push_back(WalRecord{op, std::move(key), std::move(value)});
            good += 1 + 4 + keyLen + 4 + valLen + 4;   // size of this record
        }
        if (goodBytes) *goodBytes = good;
        return records;
    }

    const std::string& path() const noexcept { return path_; }

private:
    static void write_32(std::vector<uint8_t>& out, uint32_t v) {
        for (int i = 0; i < 4; i++)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    static bool read_32(std::ifstream& in, uint32_t& v) {
        char b[4];
        if (!in.read(b, 4)) return false;
        v = 0;
        for (int i = 0; i < 4; i++)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(b[i])) << (8 * i);
        return true;
    }

    std::string path_;
    std::ofstream out_;
};

}  // namespace lsm
