// EdgeCache is the edge node's bounded LRU of origin responses. These checks
// cover hit/miss and fresh/stale classification, LRU eviction under both the
// entry-count and byte-budget caps, recency promotion on lookup, replacement of
// an existing key, purge/clear, and the refusal of an oversized entry.

#include <cstdio>
#include <memory>
#include <string>

#include "ruvia/edge/EdgeCache.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using ruvia::edge::CachedResponse;
using ruvia::edge::CacheLookupStatus;
using ruvia::edge::EdgeCache;

// Build an entry whose body is `size` bytes and whose freshness deadline is
// `expiresAt`.
std::shared_ptr<const CachedResponse> makeEntry(std::size_t size, std::time_t expiresAt) {
    auto entry = std::make_shared<CachedResponse>();
    entry->status = 200;
    entry->body.assign(size, 'x');
    entry->expiresAt = expiresAt;
    return entry;
}

}  // namespace

int main() {
    constexpr std::time_t kNow = 1'000'000'000;

    // Miss, then fresh hit, then stale hit as time passes the deadline.
    {
        EdgeCache cache(EdgeCache::Limits{});
        check(cache.lookup("a", kNow).status == CacheLookupStatus::kMiss, "unknown key misses");

        cache.store("a", makeEntry(10, kNow + 100));
        const auto fresh = cache.lookup("a", kNow);
        check(fresh.status == CacheLookupStatus::kFresh, "stored key hits fresh");
        check(fresh.entry != nullptr && fresh.entry->body.size() == 10, "entry payload returned");

        const auto stale = cache.lookup("a", kNow + 200);
        check(stale.status == CacheLookupStatus::kStale, "past the deadline the hit is stale");
        check(stale.entry != nullptr, "stale hit still returns the entry");
    }

    // Entry-count cap evicts the least-recently-used key.
    {
        EdgeCache cache(EdgeCache::Limits{/*maxBytes=*/1u << 20, /*maxEntries=*/2});
        cache.store("a", makeEntry(1, kNow + 100));
        cache.store("b", makeEntry(1, kNow + 100));
        cache.store("c", makeEntry(1, kNow + 100));  // evicts "a"
        check(cache.entryCount() == 2, "entry count capped at 2");
        check(cache.lookup("a", kNow).status == CacheLookupStatus::kMiss, "LRU key a evicted");
        check(cache.lookup("b", kNow).status == CacheLookupStatus::kFresh, "b retained");
        check(cache.lookup("c", kNow).status == CacheLookupStatus::kFresh, "c retained");
    }

    // A lookup promotes recency, changing which key is evicted next.
    {
        EdgeCache cache(EdgeCache::Limits{/*maxBytes=*/1u << 20, /*maxEntries=*/2});
        cache.store("a", makeEntry(1, kNow + 100));
        cache.store("b", makeEntry(1, kNow + 100));
        (void)cache.lookup("a", kNow);               // a is now most-recent
        cache.store("c", makeEntry(1, kNow + 100));  // evicts b, not a
        check(cache.lookup("a", kNow).status == CacheLookupStatus::kFresh,
              "recently used a survives");
        check(cache.lookup("b", kNow).status == CacheLookupStatus::kMiss, "now-LRU b evicted");
    }

    // Byte budget evicts until the total footprint fits.
    {
        EdgeCache cache(EdgeCache::Limits{/*maxBytes=*/100, /*maxEntries=*/1000});
        cache.store("a", makeEntry(60, kNow + 100));
        cache.store("b", makeEntry(60, kNow + 100));  // 120 > 100 -> evict a
        check(cache.byteSize() == 60, "byte total reflects the surviving entry");
        check(cache.lookup("a", kNow).status == CacheLookupStatus::kMiss, "a evicted by byte cap");
        check(cache.lookup("b", kNow).status == CacheLookupStatus::kFresh, "b fits");
    }

    // Replacing an existing key updates value and byte accounting, not count.
    {
        EdgeCache cache(EdgeCache::Limits{});
        cache.store("a", makeEntry(10, kNow + 100));
        cache.store("a", makeEntry(25, kNow + 100));
        check(cache.entryCount() == 1, "replacement keeps a single entry");
        check(cache.byteSize() == 25, "replacement updates accounted bytes");
        const auto hit = cache.lookup("a", kNow);
        check(hit.entry != nullptr && hit.entry->body.size() == 25, "replacement value served");
    }

    // An entry larger than the whole byte budget is refused, leaving the cache untouched.
    {
        EdgeCache cache(EdgeCache::Limits{/*maxBytes=*/100, /*maxEntries=*/1000});
        cache.store("keep", makeEntry(50, kNow + 100));
        const bool stored = cache.store("huge", makeEntry(200, kNow + 100));
        check(!stored, "oversized entry is refused");
        check(cache.lookup("huge", kNow).status == CacheLookupStatus::kMiss, "oversized not stored");
        check(cache.lookup("keep", kNow).status == CacheLookupStatus::kFresh,
              "refusing oversized does not disturb existing entries");
    }

    // Purge and clear.
    {
        EdgeCache cache(EdgeCache::Limits{});
        cache.store("a", makeEntry(10, kNow + 100));
        cache.store("b", makeEntry(10, kNow + 100));
        check(cache.purge("a"), "purge reports removal");
        check(!cache.purge("a"), "purging an absent key reports nothing removed");
        check(cache.lookup("a", kNow).status == CacheLookupStatus::kMiss, "purged key gone");
        check(cache.entryCount() == 1 && cache.byteSize() == 10, "purge updates counters");

        cache.clear();
        check(cache.entryCount() == 0 && cache.byteSize() == 0, "clear empties the cache");
        check(cache.lookup("b", kNow).status == CacheLookupStatus::kMiss, "clear removes all");
    }

    if (failures == 0) {
        std::fprintf(stderr, "edge cache: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
