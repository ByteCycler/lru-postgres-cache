#include "LRUCache.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

static int testsRun = 0;

#define RUN_TEST(fn) do { \
    fn(); \
    testsRun++; \
    std::cout << "[PASS] " #fn "\n"; \
} while (0)

void test_lru_insertion_and_get() {
    LRUCache cache(2);
    cache.put(Record(1, "Alice", 100.0));

    auto res = cache.get(1);
    assert(res.has_value());
    assert(res->name == "Alice");
    assert(res->balance == 100.0);
}

void test_cache_miss_returns_nullopt() {
    LRUCache cache(2);
    cache.put(Record(1, "Alice", 100.0));

    auto res = cache.get(999); // never inserted
    assert(!res.has_value());
}

void test_lru_eviction_policy() {
    LRUCache cache(2);
    cache.put(Record(1, "Key1", 10.0));
    cache.put(Record(2, "Key2", 20.0));

    // Access Key1 so Key2 becomes the Least Recently Used entry.
    cache.get(1);

    // Inserting Key3 should evict Key2 (the LRU entry), not Key1.
    cache.put(Record(3, "Key3", 30.0));

    assert(cache.contains(1));   // still present (was accessed recently)
    assert(!cache.contains(2));  // evicted
    assert(cache.contains(3));   // newly inserted
    assert(cache.size() == 2);
}

void test_put_update_existing_key_promotes_to_mru() {
    LRUCache cache(2);
    cache.put(Record(1, "Old Name", 10.0));
    cache.put(Record(2, "Key2", 20.0));

    // Update Key1 -> should refresh its value AND promote it to MRU,
    // meaning Key2 becomes LRU and gets evicted next.
    cache.put(Record(1, "New Name", 999.0));
    cache.put(Record(3, "Key3", 30.0));

    assert(!cache.contains(2));  // evicted (was LRU after the update)
    auto res = cache.get(1);
    assert(res.has_value());
    assert(res->name == "New Name");
    assert(res->balance == 999.0);
}

void test_get_promotes_recency_order() {
    LRUCache cache(3);
    cache.put(Record(1, "A", 1.0));
    cache.put(Record(2, "B", 2.0));
    cache.put(Record(3, "C", 3.0));

    // Touch 1 then 2, leaving 3 as the least recently used.
    cache.get(1);
    cache.get(2);

    cache.put(Record(4, "D", 4.0)); // capacity 3 full -> evicts LRU (3)

    assert(!cache.contains(3));
    assert(cache.contains(1));
    assert(cache.contains(2));
    assert(cache.contains(4));
}

void test_zero_capacity_cache_never_stores() {
    LRUCache cache(0);
    cache.put(Record(1, "A", 1.0));
    assert(cache.size() == 0);
    assert(!cache.contains(1));
    assert(!cache.get(1).has_value());
}

void test_size_tracks_entry_count() {
    LRUCache cache(5);
    assert(cache.size() == 0);
    cache.put(Record(1, "A", 1.0));
    cache.put(Record(2, "B", 2.0));
    assert(cache.size() == 2);
    // Updating an existing key should not grow size.
    cache.put(Record(1, "A2", 1.5));
    assert(cache.size() == 2);
}

void test_stats_track_hits_misses_evictions() {
    LRUCache cache(2);
    cache.put(Record(1, "A", 1.0));
    cache.put(Record(2, "B", 2.0));

    cache.get(1);     // hit
    cache.get(999);   // miss
    cache.put(Record(3, "C", 3.0)); // evicts 2 (LRU, since 1 was just touched)

    CacheStats stats = cache.getStats();
    assert(stats.hits == 1);
    assert(stats.misses == 1);
    assert(stats.evictions == 1);
    assert(stats.hitRate() > 0.49 && stats.hitRate() < 0.51); // 1 hit / 2 lookups
}

void test_reset_stats_clears_counters_not_entries() {
    LRUCache cache(2);
    cache.put(Record(1, "A", 1.0));
    cache.get(1);
    cache.get(999);

    cache.resetStats();
    CacheStats stats = cache.getStats();
    assert(stats.hits == 0);
    assert(stats.misses == 0);
    assert(stats.evictions == 0);
    assert(cache.contains(1)); // entries untouched by resetStats()
}

// Hammers a shared cache from multiple threads doing a mix of get()/put()
// calls. This does not assert on exact LRU order (concurrent interleaving
// makes that non-deterministic) -- it asserts the cache survives with no
// data race crashes/UB and ends up in a internally consistent state
// (size never exceeds capacity, every stats counter is non-negative and
// hits+misses matches the number of get() calls issued).
void test_concurrent_access_is_thread_safe() {
    constexpr size_t kCapacity = 50;
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 2000;

    LRUCache cache(kCapacity);
    for (int i = 0; i < static_cast<int>(kCapacity); ++i) {
        cache.put(Record(i, "seed", 0.0));
    }

    auto worker = [&](int threadId) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            int key = (threadId * 13 + i) % (static_cast<int>(kCapacity) * 2);
            if (i % 4 == 0) {
                cache.put(Record(key, "t" + std::to_string(threadId), key * 1.0));
            } else {
                cache.get(key);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& t : threads) t.join();

    assert(cache.size() <= kCapacity);
    CacheStats stats = cache.getStats();
    long long getsIssued = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            if (i % 4 != 0) getsIssued++;
        }
    }
    assert(static_cast<long long>(stats.hits + stats.misses) == getsIssued);
}

int main() {
    std::cout << "=== Running LRUCache Unit Tests ===\n";
    RUN_TEST(test_lru_insertion_and_get);
    RUN_TEST(test_cache_miss_returns_nullopt);
    RUN_TEST(test_lru_eviction_policy);
    RUN_TEST(test_put_update_existing_key_promotes_to_mru);
    RUN_TEST(test_get_promotes_recency_order);
    RUN_TEST(test_zero_capacity_cache_never_stores);
    RUN_TEST(test_size_tracks_entry_count);
    RUN_TEST(test_stats_track_hits_misses_evictions);
    RUN_TEST(test_reset_stats_clears_counters_not_entries);
    RUN_TEST(test_concurrent_access_is_thread_safe);
    std::cout << "=== " << testsRun << "/" << testsRun << " tests passed ===\n";
    return 0;
}
