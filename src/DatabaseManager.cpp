#include "DatabaseManager.h"
#include <iostream>

void DatabaseManager::initSchema() {
    try {
        std::lock_guard<std::mutex> lock(dbMutex_);
        pqxx::work W(conn);
        W.exec("CREATE TABLE IF NOT EXISTS accounts ("
               "id INT PRIMARY KEY, "
               "name TEXT NOT NULL, "
               "balance NUMERIC(10, 2));");
        W.commit();
    } catch (const std::exception &e) {
        std::cerr << "[Schema Init Error] " << e.what() << "\n";
    }
}

void DatabaseManager::insertRecord(int id, const std::string& name, double balance) {
    Record rec(id, name, balance);
    try {
        // Write-through: persist to PostgreSQL first (source of truth),
        // using a parameterized statement to avoid SQL injection. The DB
        // connection is shared across threads, so access is serialized
        // via dbMutex_ (see class-level comment for the concurrency
        // trade-off this implies).
        {
            std::lock_guard<std::mutex> lock(dbMutex_);
            pqxx::work W(conn);
            W.exec_params(
                "INSERT INTO accounts (id, name, balance) VALUES ($1, $2, $3) "
                "ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, balance = EXCLUDED.balance;",
                id, name, balance);
            W.commit();
        }

        // Cache has its own internal lock -- no need to hold dbMutex_ here.
        cache.put(rec);
    } catch (const std::exception &e) {
        std::cerr << "[DB Insert Error] " << e.what() << "\n";
    }
}

std::optional<Record> DatabaseManager::getRecord(int id) {
    // Step 1: check the LRU cache first (O(1) in-memory lookup, safe to
    // call concurrently -- this is the fast path most requests take).
    auto cached = cache.get(id);
    if (cached.has_value()) {
        return cached;
    }

    // Step 2: cache miss -> fall back to PostgreSQL. Serialized behind
    // dbMutex_ since the connection can't be used concurrently.
    try {
        std::lock_guard<std::mutex> lock(dbMutex_);
        pqxx::nontransaction N(conn);
        pqxx::result R = N.exec_params(
            "SELECT name, balance FROM accounts WHERE id = $1", id);

        if (!R.empty()) {
            std::string name = R[0][0].as<std::string>();
            double balance = R[0][1].as<double>();
            Record fetched(id, name, balance);

            // Populate the cache so subsequent reads are fast.
            cache.put(fetched);
            return fetched;
        }
    } catch (const std::exception &e) {
        std::cerr << "[DB Fetch Error] " << e.what() << "\n";
    }

    return std::nullopt;
}
