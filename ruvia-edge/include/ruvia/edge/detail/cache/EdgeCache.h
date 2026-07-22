#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <list>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/edge/EdgeTypes.h"
#include "ruvia/edge/detail/proxy/HeaderRules.h"

namespace ruvia::edge {

struct CachedResponse final {
    std::uint16_t status{0};
    Headers headers;
    std::string body;
    std::time_t storedAt{0};
    std::uint64_t initialAge{0};
    std::time_t expiresAt{0};
    std::uint64_t staleWhileRevalidate{0};
    std::uint64_t staleIfError{0};

    [[nodiscard]] std::size_t byteSize() const noexcept;
};

// The Age a stored response carries now: the age it arrived with plus the time
// it has since been resident, saturating instead of wrapping.
[[nodiscard]] std::uint64_t cachedResponseAge(
    const CachedResponse& entry,
    std::time_t now) noexcept;

struct EdgeCachedResponseControl;

// A stable cache-entry lease. Copies only touch a non-atomic owner-thread
// counter. Purge, replacement and LRU eviction cannot invalidate a response
// that an in-flight coroutine still serializes.
class CacheEntryLease final {
public:
    CacheEntryLease() noexcept = default;
    ~CacheEntryLease();

    CacheEntryLease(const CacheEntryLease& other) noexcept;
    CacheEntryLease& operator=(const CacheEntryLease& other) noexcept;
    CacheEntryLease(CacheEntryLease&& other) noexcept;
    CacheEntryLease& operator=(CacheEntryLease&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return control_ != nullptr;
    }
    [[nodiscard]] const CachedResponse* get() const noexcept;
    [[nodiscard]] const CachedResponse& operator*() const noexcept;
    [[nodiscard]] const CachedResponse* operator->() const noexcept;

private:
    friend class EdgeCache;
    explicit CacheEntryLease(EdgeCachedResponseControl* control) noexcept;

    EdgeCachedResponseControl* control_{nullptr};
};

enum class CacheLookupStatus {
    kMiss,
    kFresh,
    kStale,
};

struct CacheLookupResult final {
    CacheLookupStatus status{CacheLookupStatus::kMiss};
    CacheEntryLease entry{};
};

// Internal bounded LRU cache confined to one event-loop thread. Its runtime must
// marshal control-plane operations to that owner. This makes a hit one hash
// probe, one list splice and one plain reference increment: no mutex and no
// atomic shared ownership on the request path.
class EdgeCache final {
public:
    explicit EdgeCache(
        EdgeCacheLimits limits,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept;
    ~EdgeCache();

    EdgeCache(const EdgeCache&) = delete;
    EdgeCache& operator=(const EdgeCache&) = delete;

    [[nodiscard]] CacheLookupResult lookup(std::string_view key, std::time_t now) noexcept;

    // Takes ownership of the response value. Returns false without storing if
    // its payload cannot fit in the byte budget.
    bool store(std::string key, CachedResponse entry);

    bool purge(std::string_view key) noexcept;
    std::size_t purgePrefix(std::string_view prefix) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t entryCount() const noexcept {
        return index_.size();
    }
    [[nodiscard]] std::size_t byteSize() const noexcept {
        return totalBytes_;
    }

private:
    struct Node final {
        std::pmr::string key;
        EdgeCachedResponseControl* value;
        std::size_t bytes{0};

        Node(
            std::string sourceKey,
            EdgeCachedResponseControl* sourceValue,
            std::size_t sourceBytes,
            std::pmr::memory_resource* resource)
            : key(std::move(sourceKey), resource),
              value(sourceValue),
              bytes(sourceBytes) {}
    };

    struct TransparentHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };
    struct TransparentEqual final {
        using is_transparent = void;
        template <typename Left, typename Right>
        [[nodiscard]] bool operator()(const Left& left, const Right& right) const noexcept {
            return std::string_view(left) == std::string_view(right);
        }
    };

    using RecencyList = std::pmr::list<Node>;
    using Index = std::pmr::unordered_map<
        std::pmr::string,
        RecencyList::iterator,
        TransparentHash,
        TransparentEqual>;

    void erase(Index::iterator it) noexcept;
    void evictWhileOverBudget() noexcept;

    std::pmr::memory_resource* resource_;
    EdgeCacheLimits limits_;
    std::size_t totalBytes_{0};
    RecencyList recency_;
    Index index_;
};

}  // namespace ruvia::edge
