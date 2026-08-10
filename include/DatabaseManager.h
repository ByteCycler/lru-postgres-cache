#ifndef DATABASE_MANAGER_H
#define DATABASE_MANAGER_H

#include "Record.h"
#include "LRUCache.h"
#include <pqxx/pqxx>
#include <mutex>
#include <optional>
#include <string>

// DatabaseManager glues the LRUCache to a real PostgreSQL-backed
// "accounts" table, implementing a read-through / write-through policy:
//
//   write path:  insertRecord() -> PostgreSQL (durable) -> LRUCache (fast path)
//   read path:   getRecord()    -> LRUCache (fast path) -> PostgreSQL (fallback)
//
// Concurrency model:
//   - The cache (LRUCache) has its own internal lock and is safe to hit
//     from many threads concurrently. Cache HITS -- the common, fast
//     path this whole design exists for -- are fully concurrent.
//   - The database connection is a single long-lived pqxx::connection,
//     guarded here by dbMutex_. libpqxx connections are not safe to use
//     concurrently from multiple threads, so DB calls (cache MISSES,
//     and all writes) are serialized behind that mutex: only one thread
//     talks to Postgres at a time.
//   This is a deliberate simplification over a connection pool. It
//   keeps the DB-access code small and easy to reason about, at the
//   cost of not parallelizing the miss/fallback path. A pool of N
//   connections (see README) would let up to N threads hit Postgres
//   concurrently instead of 1, and is the natural next step if
//   miss-heavy concurrent load becomes the bottleneck.
class DatabaseManager {
private:
    std::mutex dbMutex_;
    pqxx::connection conn;
    LRUCache cache;

public:
    DatabaseManager(const std::string& connection_info, size_t cache_capacity)
        : conn(connection_info), cache(cache_capacity) {
        initSchema();
    }

    void initSchema();
    void insertRecord(int id, const std::string& name, double balance);
    std::optional<Record> getRecord(int id);
    bool isCached(int id) const { return cache.contains(id); }
    size_t cacheSize() const { return cache.size(); }
    CacheStats cacheStats() const { return cache.getStats(); }
};

#endif // DATABASE_MANAGER_H
