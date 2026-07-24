#include "ruvia/edge/detail/cache/EdgeCache.h"

#include <cassert>
#include <limits>
#include <memory>
#include <utility>

#include "ruvia/core/memory/PmrObject.h"

namespace ruvia::edge {

struct EdgeCachedResponseControl final {
    EdgeCachedResponseControl(CachedResponse source, std::pmr::memory_resource* sourceResource)
        : value(std::move(source)),
          resource(sourceResource) {}

    std::size_t references{1};  // the cache owns the initial reference
    CachedResponse value;
    std::pmr::memory_resource* resource;
};

namespace {

void retainCachedResponse(EdgeCachedResponseControl* control) noexcept {
    if (control != nullptr) {
        assert(control->references > 0);
        ++control->references;
    }
}

void releaseCachedResponse(EdgeCachedResponseControl* control) noexcept {
    if (control == nullptr) {
        return;
    }
    assert(control->references > 0);
    if (--control->references == 0) {
        ::ruvia::detail::destroyPmrObject(control, control->resource);
    }
}

}  // namespace

std::size_t CachedResponse::byteSize() const noexcept {
    std::size_t total = body.size();
    for (const auto& [name, value] : headers) {
        total += name.size() + value.size();
    }
    return total;
}

std::uint64_t cachedResponseAge(const CachedResponse& entry, std::time_t now) noexcept {
    const std::uint64_t resident = entry.storedAt <= now ? static_cast<std::uint64_t>(now - entry.storedAt) : std::uint64_t{0};
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    return entry.initialAge > maximum - resident ? maximum : entry.initialAge + resident;
}

CacheEntryLease::CacheEntryLease(EdgeCachedResponseControl* control) noexcept
    : control_(control) {
    retainCachedResponse(control_);
}

CacheEntryLease::~CacheEntryLease() {
    releaseCachedResponse(control_);
}

CacheEntryLease::CacheEntryLease(const CacheEntryLease& other) noexcept
    : control_(other.control_) {
    retainCachedResponse(control_);
}

CacheEntryLease& CacheEntryLease::operator=(const CacheEntryLease& other) noexcept {
    if (this != &other) {
        retainCachedResponse(other.control_);
        releaseCachedResponse(control_);
        control_ = other.control_;
    }
    return *this;
}

CacheEntryLease::CacheEntryLease(CacheEntryLease&& other) noexcept
    : control_(std::exchange(other.control_, nullptr)) {}

CacheEntryLease& CacheEntryLease::operator=(CacheEntryLease&& other) noexcept {
    if (this != &other) {
        releaseCachedResponse(control_);
        control_ = std::exchange(other.control_, nullptr);
    }
    return *this;
}

const CachedResponse* CacheEntryLease::get() const noexcept {
    return control_ != nullptr ? &control_->value : nullptr;
}

const CachedResponse& CacheEntryLease::operator*() const noexcept {
    assert(control_ != nullptr);
    return control_->value;
}

const CachedResponse* CacheEntryLease::operator->() const noexcept {
    assert(control_ != nullptr);
    return &control_->value;
}

EdgeCache::EdgeCache(EdgeCacheLimits limits, std::pmr::memory_resource* resource) noexcept
    : resource_(::ruvia::detail::pmrResourceOrDefault(resource)),
      limits_(limits),
      recency_(resource_),
      index_(resource_) {}

EdgeCache::~EdgeCache() {
    clear();
}

CacheLookupResult EdgeCache::lookup(std::string_view key, std::time_t now) noexcept {
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return {};
    }
    recency_.splice(recency_.begin(), recency_, it->second);
    EdgeCachedResponseControl* value = it->second->value;
    const CacheLookupStatus status = now < value->value.expiresAt ? CacheLookupStatus::kFresh : CacheLookupStatus::kStale;
    return {status, CacheEntryLease(value)};
}

bool EdgeCache::store(std::string key, CachedResponse entry) {
    const std::size_t bytes = entry.byteSize();
    if (bytes > limits_.maxBytes) {
        return false;
    }

    auto replacement = ::ruvia::detail::makePmrObject<EdgeCachedResponseControl>(resource_, std::move(entry), resource_);
    if (const auto existing = index_.find(key); existing != index_.end()) {
        Node& node = *existing->second;
        totalBytes_ -= node.bytes;
        EdgeCachedResponseControl* previous = std::exchange(node.value, replacement.release());
        node.bytes = bytes;
        totalBytes_ += bytes;
        recency_.splice(recency_.begin(), recency_, existing->second);
        releaseCachedResponse(previous);
    } else {
        recency_.emplace_front(std::move(key), replacement.get(), bytes, resource_);
        try {
            index_.emplace(recency_.front().key, recency_.begin());
        } catch (...) {
            recency_.pop_front();
            throw;
        }
        (void)replacement.release();
        totalBytes_ += bytes;
    }

    evictWhileOverBudget();
    return true;
}

void EdgeCache::erase(decltype(index_)::iterator it) noexcept {
    auto node = it->second;
    totalBytes_ -= node->bytes;
    EdgeCachedResponseControl* removed = node->value;
    recency_.erase(node);
    index_.erase(it);
    releaseCachedResponse(removed);
}

bool EdgeCache::purge(std::string_view key) noexcept {
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    erase(it);
    return true;
}

std::size_t EdgeCache::purgePrefix(std::string_view prefix) noexcept {
    std::size_t removed = 0;
    for (auto it = recency_.begin(); it != recency_.end();) {
        if (!std::string_view(it->key).starts_with(prefix)) {
            ++it;
            continue;
        }
        const auto victim = index_.find(it->key);
        ++it;
        erase(victim);
        ++removed;
    }
    return removed;
}

void EdgeCache::clear() noexcept {
    for (const Node& node : recency_) {
        releaseCachedResponse(node.value);
    }
    index_.clear();
    recency_.clear();
    totalBytes_ = 0;
}

void EdgeCache::evictWhileOverBudget() noexcept {
    while (!recency_.empty() && (totalBytes_ > limits_.maxBytes || index_.size() > limits_.maxEntries)) {
        erase(index_.find(recency_.back().key));
    }
}

}  // namespace ruvia::edge
