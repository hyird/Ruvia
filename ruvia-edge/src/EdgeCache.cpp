#include "ruvia/edge/EdgeCache.h"

namespace ruvia::edge {

std::size_t CachedResponse::byteSize() const noexcept {
    std::size_t total = body.size();
    for (const auto& [name, value] : headers) {
        total += name.size() + value.size();
    }
    return total;
}

EdgeCache::EdgeCache(Limits limits) noexcept : limits_(limits) {}

CacheLookupResult EdgeCache::lookup(std::string_view key, std::time_t now) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return {};
    }
    // Promote to most-recently-used.
    recency_.splice(recency_.begin(), recency_, it->second);
    const std::shared_ptr<const CachedResponse>& value = it->second->value;
    const CacheLookupStatus status =
        now < value->expiresAt ? CacheLookupStatus::kFresh : CacheLookupStatus::kStale;
    return {status, value};
}

bool EdgeCache::store(std::string key, std::shared_ptr<const CachedResponse> entry) {
    if (!entry) {
        return false;
    }
    const std::size_t bytes = entry->byteSize();
    // Refuse an entry that could never fit: admitting it would evict everything
    // else and still overflow.
    if (bytes > limits_.maxBytes) {
        return false;
    }

    std::lock_guard<std::mutex> guard(mutex_);

    if (const auto existing = index_.find(key); existing != index_.end()) {
        // Replace in place, keeping the node (and its stable map key) but
        // adjusting the accounted bytes.
        Node& node = *existing->second;
        totalBytes_ -= node.bytes;
        node.value = std::move(entry);
        node.bytes = bytes;
        totalBytes_ += bytes;
        recency_.splice(recency_.begin(), recency_, existing->second);
    } else {
        recency_.push_front(Node{std::move(key), std::move(entry), bytes});
        index_.emplace(recency_.front().key, recency_.begin());
        totalBytes_ += bytes;
    }

    evictWhileOverBudget();
    return true;
}

bool EdgeCache::purge(std::string_view key) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    totalBytes_ -= it->second->bytes;
    recency_.erase(it->second);
    index_.erase(it);
    return true;
}

void EdgeCache::clear() {
    std::lock_guard<std::mutex> guard(mutex_);
    index_.clear();
    recency_.clear();
    totalBytes_ = 0;
}

std::size_t EdgeCache::entryCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return index_.size();
}

std::size_t EdgeCache::byteSize() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return totalBytes_;
}

void EdgeCache::evictWhileOverBudget() noexcept {
    while (!recency_.empty() &&
           (totalBytes_ > limits_.maxBytes || index_.size() > limits_.maxEntries)) {
        Node& victim = recency_.back();
        totalBytes_ -= victim.bytes;
        index_.erase(victim.key);
        recency_.pop_back();
    }
}

}  // namespace ruvia::edge
