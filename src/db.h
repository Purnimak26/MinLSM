#pragma once
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "wal.h"
#include "memtable.h"
#include "sstable.h"
#include "compaction.h"

class DB {
    lsm::Wal wal;
    std::unique_ptr<MemTable> mem;
    size_t flush_limit;
    size_t max_tables;
    std::vector<lsm::SSTable> tables;
    std::vector<std::string> paths;
    size_t writes = 0;
    int next_id = 1;

    static std::string sst_name(int id) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "sst_%06d.sst", id);
        return buf;
    }

    void load_tables() {
        std::vector<std::string> names;
        for (auto& f : std::filesystem::directory_iterator(".")) {
            std::string n = f.path().filename().string();
            if (n.size() > 8 && n.compare(0, 4, "sst_") == 0 && n.compare(n.size() - 4, 4, ".sst") == 0)
                names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            tables.emplace_back(n);
            paths.push_back(n);
            next_id = std::max(next_id, std::stoi(n.substr(4, 6)) + 1);
        }
    }

    void flush() {
        std::vector<lsm::SstEntry> entries;
        for (const Entry& e : mem->dump())
            entries.push_back({e.key, e.value, e.deleted});

        std::string name = sst_name(next_id++);
        lsm::SSTable::build(name, entries);
        tables.emplace_back(name);
        paths.push_back(name);

        mem = std::make_unique<MemTable>();
        wal.truncate();
        writes = 0;

        if (tables.size() >= max_tables) compact_all();
    }

    void compact_all() {
        std::vector<std::string> newest_first(paths.rbegin(), paths.rend());

        lsm::compact(newest_first, "sst_merged.tmp", true);

        tables.clear();

        std::string name = sst_name(next_id++);
        std::filesystem::rename("sst_merged.tmp", name);
        for (const std::string& p : paths) std::remove(p.c_str());

        paths.clear();
        tables.emplace_back(name);
        paths.push_back(name);
    }

    void after_write() {
        if (++writes >= flush_limit) flush();
    }

public:
    DB(const std::string& walPath, size_t flush_limit_ = 1024, size_t max_tables_ = 10)
        : wal(walPath), mem(std::make_unique<MemTable>()),
          flush_limit(flush_limit_), max_tables(max_tables_) {
        load_tables();

        for (auto& r : lsm::Wal::replay(walPath)) {
            if (r.op == lsm::WalOp::PUT) mem->put(r.key, r.value);
            else mem->del(r.key);
            ++writes;
        }
    }

    void put(const std::string& key, const std::string& value) {
        wal.append(lsm::WalOp::PUT, key, value);
        mem->put(key, value);
        after_write();
    }

    void del(const std::string& key) {
        wal.append(lsm::WalOp::DEL, key, "");
        mem->del(key);
        after_write();
    }

    Result get(const std::string& key, std::string& out) const {
        Result r = mem->get(key, out);
        if (r != NOT_FOUND) return r;

        for (auto it = tables.rbegin(); it != tables.rend(); ++it) {
            lsm::SstStatus s = it->get(key, out);
            if (s == lsm::SstStatus::FOUND)   return FOUND;
            if (s == lsm::SstStatus::DELETED) return DELETED;
        }
        return NOT_FOUND;
    }

    void flush_memtable() {
        if (writes > 0) flush();
    }

    size_t table_count() const { return tables.size(); }
};