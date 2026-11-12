#include "../src/db.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase> g_tests;

#define TEST(name)                                                  \
    static void name();                                             \
    static int reg_##name = (g_tests.push_back({#name, &name}), 0); \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond))                                                         \
            throw std::runtime_error("line " + std::to_string(__LINE__) +    \
                                     ": " #cond);                            \
    } while (0)

struct Scratch {
    fs::path old_dir, dir;
    explicit Scratch(const std::string& tag) {
        old_dir = fs::current_path();
        dir = fs::temp_directory_path() / ("lsm-test-" + tag);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        fs::current_path(dir);
    }
    ~Scratch() {
        std::error_code ec;
        fs::current_path(old_dir, ec);
        fs::remove_all(dir, ec);
    }
};

static std::string K(int i) { return "key-" + std::to_string(i); }
static std::string V(int i) { return "val-" + std::to_string(i); }

static bool is_value(const DB& db, const std::string& key, const std::string& want) {
    std::string out;
    return db.get(key, out) == FOUND && out == want;
}

static bool is_gone(const DB& db, const std::string& key) {
    std::string out;
    return db.get(key, out) != FOUND;
}

TEST(bloom_no_false_negatives) {
    auto b = lsm::Bloom::optimal(1000, 0.01);
    for (int i = 0; i < 1000; ++i) b.add(K(i));
    for (int i = 0; i < 1000; ++i) CHECK(b.maybe_present(K(i)));
}

TEST(bloom_false_positive_rate_close_to_target) {
    const std::size_t n = 5000;
    auto b = lsm::Bloom::optimal(n, 0.01);
    for (std::size_t i = 0; i < n; ++i) b.add("in-" + std::to_string(i));

    std::size_t fp = 0;
    const std::size_t probes = n * 10;
    for (std::size_t i = 0; i < probes; ++i)
        if (b.maybe_present("out-" + std::to_string(i))) ++fp;

    double observed = double(fp) / probes;
    CHECK(observed < 0.03);
}

TEST(bloom_survives_save_and_load) {
    auto b = lsm::Bloom::optimal(500, 0.05);
    for (int i = 0; i < 200; ++i) b.add(K(i));
    auto blob = b.to_bytes();
    auto b2 = lsm::Bloom::from_bytes(blob.data(), blob.size());
    for (int i = 0; i < 200; ++i) CHECK(b2.maybe_present(K(i)));
}

TEST(empty_db_returns_not_found) {
    Scratch s("empty");
    DB db("t.wal", 1000, 10);
    std::string out;
    CHECK(db.get("anything", out) == NOT_FOUND);
}

TEST(put_then_get) {
    Scratch s("putget");
    DB db("t.wal", 1000, 10);
    db.put("hello", "world");
    CHECK(is_value(db, "hello", "world"));
}

TEST(later_put_overwrites_earlier) {
    Scratch s("overwrite");
    DB db("t.wal", 1000, 10);
    db.put("k", "v1");
    db.put("k", "v2");
    db.put("k", "v3");
    CHECK(is_value(db, "k", "v3"));
}

TEST(delete_hides_key) {
    Scratch s("delete");
    DB db("t.wal", 1000, 10);
    db.put("k", "v");
    db.del("k");
    std::string out;
    CHECK(db.get("k", out) == DELETED);
}

TEST(flush_creates_one_sstable) {
    Scratch s("flush");
    DB db("t.wal", 1000, 10);
    for (int i = 0; i < 100; ++i) db.put(K(i), V(i));
    CHECK(db.table_count() == 0);

    db.flush_memtable();
    CHECK(db.table_count() == 1);

    for (int i = 0; i < 100; ++i) CHECK(is_value(db, K(i), V(i)));
}

TEST(reopen_recovers_flushed_data) {
    Scratch s("reopen");
    {
        DB db("t.wal", 30, 10);
        for (int i = 0; i < 100; ++i) db.put(K(i), V(i));
        db.flush_memtable();
    }
    DB db("t.wal", 30, 10);
    for (int i = 0; i < 100; ++i) CHECK(is_value(db, K(i), V(i)));
}

TEST(wal_replay_recovers_unflushed_writes) {
    Scratch s("walreplay");
    {
        DB db("t.wal", 10000, 10);
        db.put("alpha", "1");
        db.put("beta", "2");
        db.del("alpha");
    }
    DB db("t.wal", 10000, 10);
    CHECK(is_gone(db, "alpha"));
    CHECK(is_value(db, "beta", "2"));
}

TEST(wal_ignores_half_written_record) {
    Scratch s("torn");
    {
        DB db("t.wal", 10000, 10);
        db.put("a", "1");
        db.put("b", "2");
    }
    {
        std::ofstream f("t.wal", std::ios::binary | std::ios::app);
        f.write("\x01\x05", 2);
    }
    {
        DB db("t.wal", 10000, 10);
        CHECK(is_value(db, "a", "1"));
        CHECK(is_value(db, "b", "2"));
        db.put("c", "3");
    }
    DB db("t.wal", 10000, 10);
    CHECK(is_value(db, "c", "3"));
    CHECK(is_value(db, "a", "1"));
}

TEST(newer_sstable_wins) {
    Scratch s("newer");
    DB db("t.wal", 1000, 10);
    db.put("k", "old");
    db.flush_memtable();
    db.put("k", "new");
    db.flush_memtable();
    CHECK(db.table_count() == 2);
    CHECK(is_value(db, "k", "new"));
}

TEST(tombstone_in_newer_sstable_hides_older_value) {
    Scratch s("tombmulti");
    DB db("t.wal", 1000, 10);
    db.put("k", "alive");
    db.flush_memtable();
    db.del("k");
    db.flush_memtable();
    CHECK(db.table_count() == 2);
    std::string out;
    CHECK(db.get("k", out) == DELETED);
}

TEST(compaction_merges_tables_and_keeps_newest) {
    Scratch s("compact-newest");
    DB db("t.wal", 1000, 2);
    db.put("k", "v1");
    db.flush_memtable();
    db.put("k", "v2");
    db.flush_memtable();
    CHECK(db.table_count() == 1);
    CHECK(is_value(db, "k", "v2"));
}

TEST(compaction_keeps_deleted_keys_deleted) {
    Scratch s("compact-delete");
    DB db("t.wal", 10, 3);
    for (int i = 0; i < 100; ++i) db.put(K(i), "v");
    for (int i = 0; i < 100; i += 2) db.del(K(i));
    db.flush_memtable();

    CHECK(db.table_count() < 3);

    for (int i = 0; i < 100; ++i) {
        if (i % 2 == 0) CHECK(is_gone(db, K(i)));
        else            CHECK(is_value(db, K(i), "v"));
    }
}

TEST(compaction_result_survives_reopen) {
    Scratch s("compact-reopen");
    {
        DB db("t.wal", 10, 3);
        for (int i = 0; i < 100; ++i) db.put(K(i), V(i));
        for (int i = 0; i < 100; i += 5) db.del(K(i));
        db.flush_memtable();
    }
    DB db("t.wal", 10, 3);
    for (int i = 0; i < 100; ++i) {
        if (i % 5 == 0) CHECK(is_gone(db, K(i)));
        else            CHECK(is_value(db, K(i), V(i)));
    }
}

TEST(stress_5000_keys_with_deletes_and_reopen) {
    Scratch s("stress");
    const int N = 5000;
    {
        DB db("t.wal", 200, 10);
        for (int i = 0; i < N; ++i) db.put(K(i), V(i));
        for (int i = 0; i < N; i += 7) db.del(K(i));
        db.flush_memtable();
    }
    DB db("t.wal", 200, 10);
    for (int i = 0; i < N; ++i) {
        if (i % 7 == 0) CHECK(is_gone(db, K(i)));
        else            CHECK(is_value(db, K(i), V(i)));
    }
}

int main() {
    int passed = 0, failed = 0;
    for (const auto& t : g_tests) {
        try {
            t.fn();
            std::printf("  [ok]   %s\n", t.name);
            ++passed;
        } catch (const std::exception& e) {
            std::printf("  [FAIL] %s -- %s\n", t.name, e.what());
            ++failed;
        }
    }
    std::printf("\n  %d passed, %d failed (of %zu)\n", passed, failed, g_tests.size());
    return failed == 0 ? 0 : 1;
}