// DiskCache is the edge node's persistent second cache tier. These checks cover
// the store/lookup roundtrip (all CachedResponse fields survive serialization),
// misses, replacement, purge/purgePrefix/clear, LRU eviction under the byte
// budget, refusal of an oversized entry, and persistence across a "restart"
// (a fresh DiskCache over the same directory adopts the files already there).

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "ruvia/edge/DiskCache.h"
#include "ruvia/http/HttpStatus.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using ruvia::edge::CachedResponse;
using ruvia::edge::DiskCache;

CachedResponse makeEntry(std::size_t size, char fill, std::time_t expiresAt) {
    CachedResponse entry;
    entry.status = ruvia::http_status::kOk.value();
    entry.body.assign(size, fill);
    entry.expiresAt = expiresAt;
    entry.storedAt = expiresAt - 60;
    entry.staleWhileRevalidate = 30;
    entry.staleIfError = 90;
    entry.headers.emplace_back("Content-Type", "text/plain");
    entry.headers.emplace_back("X-Marker", std::string(1, fill));
    return entry;
}

}  // namespace

int main() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "ruvia_disk_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // Roundtrip: every field survives serialization; unknown keys miss.
    {
        DiskCache cache(dir, 1u << 20);
        check(cache.lookup("k1") == nullptr, "unknown key misses");

        check(cache.store("k1", makeEntry(16, 'a', 12345)), "store succeeds");
        const auto got = cache.lookup("k1");
        check(got != nullptr, "stored key is found");
        check(got && got->status == 200, "status preserved");
        check(got && got->body == std::string(16, 'a'), "body preserved");
        check(got && got->expiresAt == 12345, "expiresAt preserved");
        check(got && got->staleWhileRevalidate == 30, "swr preserved");
        check(got && got->staleIfError == 90, "sie preserved");
        check(got && got->headers.size() == 2, "header count preserved");
        check(got && got->headers.size() == 2 && got->headers[0].first == "Content-Type" &&
                  got->headers[0].second == "text/plain",
              "header name/value preserved");
        check(cache.entryCount() == 1, "one entry accounted");
    }

    // Replacement in place: same key, new payload, still one entry.
    {
        DiskCache cache(dir, 1u << 20);
        check(cache.store("k1", makeEntry(32, 'b', 999)), "replacement store succeeds");
        const auto got = cache.lookup("k1");
        check(got && got->body == std::string(32, 'b'), "replacement body served");
        check(got && got->expiresAt == 999, "replacement freshness served");
        check(cache.entryCount() == 1, "replacement did not add an entry");
    }

    // Purge and prefix purge.
    {
        std::filesystem::remove_all(dir, ec);
        DiskCache cache(dir, 1u << 20);
        cache.store("GET\nhost\n/a", makeEntry(8, 'x', 1));
        cache.store("GET\nhost\n/b", makeEntry(8, 'y', 1));
        cache.store("GET\nother\n/c", makeEntry(8, 'z', 1));
        check(cache.entryCount() == 3, "three entries stored");

        check(cache.purge("GET\nhost\n/a"), "purge reports removal");
        check(cache.lookup("GET\nhost\n/a") == nullptr, "purged entry is gone");
        check(!cache.purge("GET\nhost\n/a"), "re-purge reports nothing");

        const std::size_t removed = cache.purgePrefix("GET\nhost\n");
        check(removed == 1, "prefix purge removes only the matching prefix");
        check(cache.lookup("GET\nother\n/c") != nullptr, "non-matching entry survives prefix purge");
        check(cache.entryCount() == 1, "one entry remains after purges");
    }

    // LRU eviction under the byte budget: the least-recently-used entry is
    // evicted; a recent lookup protects an entry from eviction.
    {
        std::filesystem::remove_all(dir, ec);
        // Each serialized entry is well under 300 bytes; budget holds ~2.
        DiskCache cache(dir, 600);
        cache.store("a", makeEntry(100, 'a', 1));
        cache.store("b", makeEntry(100, 'b', 1));
        check(cache.lookup("a") != nullptr, "a present before pressure");  // a now MRU
        cache.store("c", makeEntry(100, 'c', 1));  // evicts LRU (b)
        check(cache.lookup("a") != nullptr, "recently-used a survives eviction");
        check(cache.lookup("c") != nullptr, "newest c present");
        check(cache.lookup("b") == nullptr, "least-recently-used b was evicted");
        check(cache.byteSize() <= 600, "byte budget is respected");
    }

    // An entry larger than the whole budget is refused.
    {
        std::filesystem::remove_all(dir, ec);
        DiskCache cache(dir, 200);
        check(!cache.store("big", makeEntry(500, 'q', 1)), "oversized entry is refused");
        check(cache.entryCount() == 0, "refused entry is not stored");
    }

    // Persistence: a fresh DiskCache over the same directory adopts the files.
    {
        std::filesystem::remove_all(dir, ec);
        {
            DiskCache cache(dir, 1u << 20);
            cache.store("persist", makeEntry(64, 'p', 42));
            check(cache.entryCount() == 1, "entry stored before restart");
        }
        DiskCache reopened(dir, 1u << 20);
        check(reopened.entryCount() == 1, "reopened cache adopts the on-disk entry");
        const auto got = reopened.lookup("persist");
        check(got != nullptr && got->body == std::string(64, 'p'),
              "persisted entry is readable after restart");
        check(got && got->expiresAt == 42, "persisted freshness survives restart");
    }

    // clear() empties both the index and the directory.
    {
        std::filesystem::remove_all(dir, ec);
        DiskCache cache(dir, 1u << 20);
        cache.store("x", makeEntry(8, 'x', 1));
        cache.store("y", makeEntry(8, 'y', 1));
        cache.clear();
        check(cache.entryCount() == 0, "clear empties the index");
        check(cache.byteSize() == 0, "clear zeroes the byte count");
        check(cache.lookup("x") == nullptr, "cleared entry is unreadable");
        DiskCache reopened(dir, 1u << 20);
        check(reopened.entryCount() == 0, "cleared files are gone from disk");
    }

    std::filesystem::remove_all(dir, ec);
    if (failures == 0) {
        std::puts("disk cache: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
