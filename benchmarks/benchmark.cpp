// Measures real latency/throughput numbers for the engine instead of
// just asserting a Big-O complexity class:
//
//   1. Pure LRUCache get/put latency (in-process, no I/O).
//   2. DatabaseManager cache-hit latency vs. cache-miss (Postgres
//      fallback) latency, for the same record, from the same process.
//   3. Multi-threaded throughput of the cache under concurrent
//      readers+writers, to demonstrate the thread-safety added on top
//      of the base LRU implementation.
//
// Run with: ./benchmark  (needs DB_CONN_STRING or the default local
// Postgres connection string to succeed; part 1 and 3 also work
// standalone without a DB).

#include "LRUCache.h"
#include "DatabaseManager.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static std::string connStringFromEnv() {
    const char* envConn = std::getenv("DB_CONN_STRING");
    return envConn ? std::string(envConn)
        : "host=localhost port=5432 dbname=postgres user=postgres password=password";
}

// Part 1: raw LRUCache get/put latency, single-threaded.
static void benchmarkPureCache() {
    constexpr int kCapacity = 10'000;
    constexpr int kOps = 500'000;

    LRUCache cache(kCapacity);
    for (int i = 0; i < kCapacity; ++i) {
        cache.put(Record(i, "user" + std::to_string(i), i * 1.5));
    }

    auto start = Clock::now();
    for (int i = 0; i < kOps; ++i) {
        cache.get(i % kCapacity); // always a hit, exercises the splice-to-front path
    }
    double elapsedMs = msSince(start);

    std::cout << "[Pure cache] " << kOps << " get() calls (all hits) in "
              << std::fixed << std::setprecision(2) << elapsedMs << " ms => "
              << std::setprecision(0) << (kOps / (elapsedMs / 1000.0)) << " ops/sec, "
              << std::setprecision(3) << (elapsedMs * 1000.0 / kOps) << " us/op avg\n";
}

// Part 2: cache hit vs. Postgres fallback latency, via DatabaseManager.
static void benchmarkCacheVsDb() {
    try {
        DatabaseManager db(connStringFromEnv(), /*cache_capacity=*/100);
        db.insertRecord(1, "Benchmark User", 42.0);

        constexpr int kWarmup = 5;
        constexpr int kIterations = 100'000;

        // Warm up (first getRecord() after insert is already a hit, but
        // run a few extra to stabilize).
        for (int i = 0; i < kWarmup; ++i) db.getRecord(1);

        // --- Cache-hit timing ---
        auto start = Clock::now();
        for (int i = 0; i < kIterations; ++i) {
            db.getRecord(1); // stays cached the whole time -> all hits
        }
        double hitMs = msSince(start);

        // --- Cache-miss timing: evict the record by filling the cache
        // with other keys, then measure the forced Postgres fallback. ---
        double missTotalMs = 0.0;
        int missSamples = 20;
        for (int i = 0; i < missSamples; ++i) {
            // Flood the (capacity-100) cache with unrelated keys so
            // record 1 gets evicted (it's now the coldest entry).
            for (int j = 1000; j < 1000 + 150; ++j) {
                db.insertRecord(j, "filler", 0.0);
            }
            auto missStart = Clock::now();
            db.getRecord(1); // forced miss -> Postgres fallback -> re-cached
            missTotalMs += msSince(missStart);
        }

        std::cout << "[Cache hit]  " << kIterations << " getRecord() calls (all hits) in "
                  << std::fixed << std::setprecision(2) << hitMs << " ms => "
                  << std::setprecision(3) << (hitMs * 1000.0 / kIterations) << " us/op avg\n";
        std::cout << "[Cache miss] " << missSamples << " forced Postgres fallback calls, "
                  << std::setprecision(2) << (missTotalMs / missSamples) << " ms/op avg\n";
        std::cout << "[Speedup]    cache hit is ~"
                  << std::setprecision(0)
                  << ((missTotalMs / missSamples) / (hitMs / kIterations))
                  << "x faster than a Postgres round-trip\n";

        CacheStats stats = db.cacheStats();
        std::cout << "[Stats]      hits=" << stats.hits << " misses=" << stats.misses
                  << " evictions=" << stats.evictions
                  << " hitRate=" << std::setprecision(1) << (stats.hitRate() * 100.0) << "%\n";
    } catch (const std::exception& e) {
        std::cout << "[Cache vs DB benchmark skipped] Could not reach PostgreSQL: "
                  << e.what() << "\n";
    }
}

// Part 3: concurrent throughput with multiple reader/writer threads.
static void benchmarkConcurrentCache() {
    constexpr int kCapacity = 1'000;
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 100'000;

    LRUCache cache(kCapacity);
    for (int i = 0; i < kCapacity; ++i) {
        cache.put(Record(i, "user" + std::to_string(i), i * 1.5));
    }

    auto worker = [&](int threadId) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            int key = (threadId * 37 + i) % (kCapacity * 2); // mix of hits and misses/inserts
            if (i % 5 == 0) {
                cache.put(Record(key, "user" + std::to_string(key), key * 1.5));
            } else {
                cache.get(key);
            }
        }
    };

    auto start = Clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& t : threads) t.join();
    double elapsedMs = msSince(start);

    long long totalOps = static_cast<long long>(kThreads) * kOpsPerThread;
    std::cout << "[Concurrent] " << kThreads << " threads x " << kOpsPerThread
              << " ops = " << totalOps << " total ops in "
              << std::fixed << std::setprecision(2) << elapsedMs << " ms => "
              << std::setprecision(0) << (totalOps / (elapsedMs / 1000.0)) << " ops/sec\n";
}

int main() {
    std::cout << "=== LRU Cache Benchmark ===\n\n";

    std::cout << "--- Part 1: Pure in-memory cache latency ---\n";
    benchmarkPureCache();

    std::cout << "\n--- Part 2: Cache hit vs. PostgreSQL fallback latency ---\n";
    benchmarkCacheVsDb();

    std::cout << "\n--- Part 3: Concurrent multi-threaded throughput ---\n";
    benchmarkConcurrentCache();

    return 0;
}
