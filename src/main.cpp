#include "DatabaseManager.h"
#include <iostream>
#include <cstdlib>

static std::string connStringFromEnv() {
    const char* envConn = std::getenv("DB_CONN_STRING");
    return envConn ? std::string(envConn)
        : "host=localhost port=5432 dbname=postgres user=postgres password=password";
}

static void demoQuery(DatabaseManager& db, int id) {
    bool wasCached = db.isCached(id);
    auto result = db.getRecord(id);
    if (!result.has_value()) {
        std::cout << "Record " << id << " -> not found\n";
        return;
    }
    std::cout << "Record " << id << " -> " << result->name
               << " (balance $" << result->balance << ") ["
               << (wasCached ? "LRU CACHE HIT" : "POSTGRES FALLBACK") << "]\n";
}

int main() {
    // Cache capacity 2 -> demonstrates eviction with only three inserted records.
    DatabaseManager dbEngine(connStringFromEnv(), /*cache_capacity=*/2);

    std::cout << "\n--- Inserting Records ---\n";
    dbEngine.insertRecord(101, "Isha Gupta", 15000.0);
    dbEngine.insertRecord(102, "Alex Smith", 8200.5);
    std::cout << "Inserted 101, 102 (write-through: Postgres + cache)\n";

    std::cout << "\n--- Querying Records ---\n";
    demoQuery(dbEngine, 101); // cache hit (101 is MRU)

    std::cout << "\n--- Adding 3rd Record (Triggers LRU Eviction) ---\n";
    dbEngine.insertRecord(103, "Charlie Davis", 3400.0); // capacity 2 full -> 102 evicted (101 was MRU)
    std::cout << "Inserted 103 -> capacity (2) exceeded, LRU entry (102) evicted from cache\n";

    std::cout << "\n--- Verifying Eviction & Fallback ---\n";
    demoQuery(dbEngine, 101); // still cached
    demoQuery(dbEngine, 102); // cache miss -> Postgres fallback -> re-cached
    demoQuery(dbEngine, 102); // now cached again

    CacheStats stats = dbEngine.cacheStats();
    std::cout << "\n--- Cache Stats ---\n"
              << "hits=" << stats.hits
              << " misses=" << stats.misses
              << " evictions=" << stats.evictions
              << " hitRate=" << (stats.hitRate() * 100.0) << "%\n";

    return 0;
}
