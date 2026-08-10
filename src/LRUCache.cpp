#include "LRUCache.h"

std::optional<Record> LRUCache::get(int id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cacheMap.find(id);
    if (it == cacheMap.end()) {
        stats_.misses++;
        return std::nullopt; // Cache miss
    }
    // Move the accessed node to the front of the list in O(1) via splice
    // (no copying/reallocation of the node itself).
    itemsList.splice(itemsList.begin(), itemsList, it->second);
    stats_.hits++;
    return *(it->second); // Cache hit
}

void LRUCache::put(const Record& record) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cacheMap.find(record.id);
    if (it != cacheMap.end()) {
        // Key already cached: update in place and promote to MRU.
        it->second->name = record.name;
        it->second->balance = record.balance;
        itemsList.splice(itemsList.begin(), itemsList, it->second);
        return;
    }

    // Evict the LRU entry (back of the list) if we're at capacity.
    if (capacity > 0 && itemsList.size() >= capacity) {
        int lruKey = itemsList.back().id;
        cacheMap.erase(lruKey);
        itemsList.pop_back();
        stats_.evictions++;
    }

    if (capacity == 0) {
        return; // Zero-capacity cache never stores anything.
    }

    // Insert the new record at the front (MRU position).
    itemsList.push_front(record);
    cacheMap[record.id] = itemsList.begin();
}

bool LRUCache::contains(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cacheMap.find(id) != cacheMap.end();
}

size_t LRUCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return itemsList.size();
}

CacheStats LRUCache::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void LRUCache::resetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = CacheStats{};
}
