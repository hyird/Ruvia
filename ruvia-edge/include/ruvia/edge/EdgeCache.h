#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ruvia::edge {

// A stored origin response, ready to replay to clients without touching the
// origin again. Immutable once stored: the cache hands out shared_ptr<const
// CachedResponse> so a request can serialize it after the cache lock is
// released. Freshness bookkeeping (expiresAt and the stale-* windows) is
// precomputed by the freshness policy at store time; the cache only compares
// them against the current time.
struct CachedResponse final {
    std::uint16_t status{0};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::time_t storedAt{0};
    std::time_t expiresAt{0};
    std::uint64_t staleWhileRevalidate{0};
    std::uint64_t staleIfError{0};

    // Approximate in-memory footprint used for the byte budget: the payload
    // plus the header name/value bytes. Deliberately ignores container and
    // per-node overhead -- the budget is a soft cap, not an allocator accountant.
    [[nodiscard]] std::size_t byteSize() const noexcept;
};

enum class CacheLookupStatus {
    kMiss,   // no entry for the key
    kFresh,  // entry present and still within its freshness lifetime
    kStale,  // entry present but past expiresAt (caller decides: serve stale or refetch)
};

struct CacheLookupResult final {
    CacheLookupStatus status{CacheLookupStatus::kMiss};
    std::shared_ptr<const CachedResponse> entry;  // null iff status is kMiss
};

// A thread-safe, bounded LRU cache of origin responses keyed by an opaque
// string (the caller composes it, e.g. from method + host + target). Two
// independent caps bound memory: a total-byte budget and a maximum entry
// count. On insertion the least-recently-used entries are evicted until both
// caps hold; an entry whose own footprint exceeds the byte budget is refused
// outright rather than evicting the whole cache to admit it.
//
// A single mutex guards all state. Lookups and stores are short (a hash probe
// and a list splice); the response body is copied by shared_ptr, never under
// the lock, so serialization to the client happens after the lock is released.
class EdgeCache final {
public:
    struct Limits final {
        std::size_t maxBytes{64u * 1024u * 1024u};
        std::size_t maxEntries{4096};
    };

    explicit EdgeCache(Limits limits) noexcept;

    EdgeCache(const EdgeCache&) = delete;
    EdgeCache& operator=(const EdgeCache&) = delete;

    // Look the key up and mark it most-recently-used on a hit. `now` classifies
    // the hit as fresh or stale; a stale entry is still returned so the caller
    // can choose to serve it (stale-while-revalidate / stale-if-error) or treat
    // it as a miss.
    [[nodiscard]] CacheLookupResult lookup(std::string_view key, std::time_t now);

    // Store (or replace) the entry under `key`, then evict LRU entries until the
    // byte and entry caps hold. Returns false without storing if the entry's own
    // footprint exceeds the byte budget.
    bool store(std::string key, std::shared_ptr<const CachedResponse> entry);

    // Drop the entry for `key` if present. Returns true if something was removed.
    bool purge(std::string_view key);

    // Drop every entry whose key begins with `prefix` (used to invalidate all
    // cached variants of one URL). Returns the number of entries removed.
    std::size_t purgePrefix(std::string_view prefix);

    // Remove every entry.
    void clear();

    [[nodiscard]] std::size_t entryCount() const;
    [[nodiscard]] std::size_t byteSize() const;

private:
    struct Node final {
        std::string key;
        std::shared_ptr<const CachedResponse> value;
        std::size_t bytes{0};
    };

    // Heterogeneous hashing so a string_view key probes the map without
    // allocating a std::string.
    struct TransparentHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view v) const noexcept {
            return std::hash<std::string_view>{}(v);
        }
    };

    using RecencyList = std::list<Node>;

    void evictWhileOverBudget() noexcept;  // caller holds mutex_

    mutable std::mutex mutex_;
    Limits limits_;
    std::size_t totalBytes_{0};
    RecencyList recency_;  // front = most recently used, back = least
    std::unordered_map<std::string, RecencyList::iterator, TransparentHash, std::equal_to<>> index_;
};

}  // namespace ruvia::edge
