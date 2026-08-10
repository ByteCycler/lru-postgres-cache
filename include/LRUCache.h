#ifndef LRU_CACHE_H
#define LRU_CACHE_H

#include "Record.h"
#include <unordered_map>
#include <list>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Snapshot of cache performance counters. Returned by value so callers
// get a consistent point-in-time view without holding a lock.
struct CacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;

    double hitRate() const {
        uint64_t total = hits + misses;
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
};

// LRUCache: thread-safe O(1) get/put in-memory cache.
//
// Data structures:
//   - std::list<Record>  -> doubly linked list ordered by recency.
//                            front() = Most Recently Used (MRU)
//                            back()  = Least Recently Used (LRU)
//   - std::unordered_map<int, list<Record>::iterator>
//                         -> O(1) key -> node lookup, avoiding a linear
//                            scan of the linked list on every access.
//
// Thread-safety: every public method acquires a single internal mutex.
// Note that get() is NOT a read-only operation on the underlying
// structure -- a cache hit mutates recency order (splices the node to
// the front), so it cannot safely use a shared/read lock. All methods
// therefore take an exclusive lock. This keeps the implementation
// simple and correct under concurrent access; if reads vastly
// outnumbered writes and exact LRU ordering weren't required, a
// sharded cache (N independent LRUCache instances keyed by hash(id) %
// N) would reduce lock contention further -- see README "Possible
// extensions".
class LRUCache {
private:
    mutable std::mutex mutex_;
    size_t capacity;
    std::list<Record> itemsList;
    std::unordered_map<int, std::list<Record>::iterator> cacheMap;
    mutable CacheStats stats_;

public:
    explicit LRUCache(size_t cap) : capacity(cap) {}

    // Non-copyable (contains a mutex); move is also disabled for
    // simplicity since callers are expected to hold LRUCache by value
    // or reference, not relocate it while in use.
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    // Returns the record if present and promotes it to MRU. std::nullopt on miss.
    std::optional<Record> get(int id);

    // Inserts or updates a record, promoting it to MRU. Evicts the LRU
    // entry first if the cache is at capacity.
    void put(const Record& record);

    // Read-only membership check (does NOT alter recency order, does
    // NOT count as a hit/miss in stats).
    bool contains(int id) const;

    size_t size() const;
    size_t getCapacity() const { return capacity; }

    // Point-in-time snapshot of hit/miss/eviction counters.
    CacheStats getStats() const;

    // Resets hit/miss/eviction counters to zero (does not clear entries).
    void resetStats();
};

#endif // LRU_CACHE_H
