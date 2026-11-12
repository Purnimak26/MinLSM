#include "../src/db.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::high_resolution_clock;

// Declared before DB, so it is destroyed AFTER DB (files are closed by then).
struct Cleanup {
    fs::path old_dir, dir;
    ~Cleanup() {
        std::error_code ec;
        fs::current_path(old_dir, ec);
        fs::remove_all(dir, ec);
    }
};

static std::string random_string(std::mt19937& rng, size_t n) {
    static const char ALPHABET[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<size_t> pick(0, sizeof(ALPHABET) - 2);
    std::string s(n, ' ');
    for (char& c : s) c = ALPHABET[pick(rng)];
    return s;
}

int main(int argc, char** argv) {
    // usage: bench [num_keys] [max_tables]
    size_t N = 100000;
    if (argc > 1) N = std::stoull(argv[1]);

    const size_t FLUSH_LIMIT = 1024;
    size_t MAX_TABLES = 10;
    if (argc > 2) MAX_TABLES = std::stoull(argv[2]);

    const size_t NUM_PROBES = std::min(N, size_t(500'000));

    fs::path old_dir = fs::current_path();
    fs::path dir = fs::temp_directory_path() / "lsm-bench";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::current_path(dir);
    Cleanup cleanup{old_dir, dir};

    std::printf("== lsm-tree benchmark ==\n");
    std::printf("  inserting %zu keys (10-char) with 20-byte values\n", N);
    std::printf("  flush every %zu writes, compact at %zu SSTables\n\n",
                FLUSH_LIMIT, MAX_TABLES);

    std::mt19937 rng(42);
    const std::string value(20, 'v');

    std::vector<std::string> sample_keys;
    sample_keys.reserve(NUM_PROBES / 2);

    DB db("bench.wal", FLUSH_LIMIT, MAX_TABLES);

    // -------------------- WRITES --------------------
    auto t0 = Clock::now();
    for (size_t i = 0; i < N; i++) {
        std::string k = random_string(rng, 10);
        db.put(k, value);

        if (sample_keys.size() < NUM_PROBES / 2)
            sample_keys.push_back(std::move(k));
    }
    db.flush_memtable();
    auto t1 = Clock::now();
    double write_sec = std::chrono::duration<double>(t1 - t0).count();

    // -------------------- READS --------------------
    std::vector<std::string> probes;
    probes.reserve(NUM_PROBES);

    for (const auto& k : sample_keys)
        probes.push_back(k);

    while (probes.size() < NUM_PROBES)
        probes.push_back(random_string(rng, 10));

    std::shuffle(probes.begin(), probes.end(), rng);

    std::vector<double> latencies;
    latencies.reserve(probes.size());
    size_t hits = 0;
    std::string out;

    auto t2 = Clock::now();
    for (const std::string& k : probes) {
        auto a = Clock::now();
        Result r = db.get(k, out);
        auto b = Clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(b - a).count());
        if (r == FOUND) hits++;
    }
    auto t3 = Clock::now();
    double read_sec = std::chrono::duration<double>(t3 - t2).count();

    std::sort(latencies.begin(), latencies.end());
    auto percentile = [&](double p) {
        return latencies[size_t(p * (latencies.size() - 1))];
    };

    uintmax_t bytes = 0;
    for (auto& f : fs::directory_iterator("."))
        if (f.path().extension() == ".sst")
            bytes += fs::file_size(f.path());

    std::printf("  WRITES\n");
    std::printf("    time            : %.4f s\n", write_sec);
    std::printf("    throughput      : %.2f K ops/s\n", N / write_sec / 1e3);
    std::printf("    SSTables on disk: %zu (%.2f MB)\n",
                db.table_count(), bytes / 1e6);

    std::printf("\n  READS (%zu probes, %.0f%% hit rate)\n",
                probes.size(), 100.0 * hits / probes.size());
    std::printf("    total time      : %.4f s\n", read_sec);
    std::printf("    throughput      : %.2f K ops/s\n",
                probes.size() / read_sec / 1e3);
    std::printf("    p50 latency     : %.2f us\n", percentile(0.50));
    std::printf("    p95 latency     : %.2f us\n", percentile(0.95));
    std::printf("    p99 latency     : %.2f us\n", percentile(0.99));

    return 0;   // `cleanup` runs after `db` is destroyed
}