// DiskCache is the edge node's persistent second cache tier. These checks cover
// the store/lookup roundtrip (all CachedResponse fields survive serialization),
// misses, replacement, purge/purgePrefix/clear, LRU eviction under the byte
// budget, refusal of an oversized entry, persistence across a "restart",
// directory ownership, and crash/corruption recovery rules.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "ruvia/edge/detail/cache/DiskCache.h"
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
    entry.initialAge = 17;
    entry.staleWhileRevalidate = 30;
    entry.staleIfError = 90;
    entry.headers.emplace_back("Content-Type", "text/plain");
    entry.headers.emplace_back("X-Marker", std::string(1, fill));
    return entry;
}

std::optional<std::filesystem::path> committedFile(const std::filesystem::path& directory) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!ec && entry.is_regular_file(ec) && entry.path().extension() == ".rvc") {
            return entry.path();
        }
    }
    return std::nullopt;
}

}  // namespace

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ruvia_disk_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // Roundtrip: every field survives serialization; unknown keys miss.
    {
        DiskCache cache(dir, 1u << 20);
        check(!cache.lookup("k1"), "unknown key misses");

        check(cache.store("k1", makeEntry(16, 'a', 12345)), "store succeeds");
        const auto got = cache.lookup("k1");
        check(static_cast<bool>(got), "stored key is found");
        check(got && got->status == 200, "status preserved");
        check(got && got->body == std::string(16, 'a'), "body preserved");
        check(got && got->expiresAt == 12345, "expiresAt preserved");
        check(got && got->initialAge == 17, "initial Age preserved");
        check(got && got->staleWhileRevalidate == 30, "swr preserved");
        check(got && got->staleIfError == 90, "sie preserved");
        check(got && got->headers.size() == 2, "header count preserved");
        check(got && got->headers.size() == 2 && got->headers[0].first == "Content-Type" && got->headers[0].second == "text/plain", "header name/value preserved");
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
        check(!cache.lookup("GET\nhost\n/a"), "purged entry is gone");
        check(!cache.purge("GET\nhost\n/a"), "re-purge reports nothing");

        const auto purge = cache.purgePrefix("GET\nhost\n");
        check(purge.complete, "prefix purge reports complete filesystem removal");
        check(purge.removed == 1, "prefix purge removes only the matching prefix");
        check(static_cast<bool>(cache.lookup("GET\nother\n/c")), "non-matching entry survives prefix purge");
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
        check(static_cast<bool>(cache.lookup("a")), "a present before pressure");  // a now MRU
        cache.store("c", makeEntry(100, 'c', 1));                                  // evicts LRU (b)
        check(static_cast<bool>(cache.lookup("a")), "recently-used a survives eviction");
        check(static_cast<bool>(cache.lookup("c")), "newest c present");
        check(!cache.lookup("b"), "least-recently-used b was evicted");
        check(cache.byteSize() <= 600, "byte budget is respected");
    }

#if !defined(_WIN32)
    // Filesystem failures remain observable and do not lie by dropping the
    // in-memory index entry. Once permissions recover, the same clear succeeds.
    {
        std::filesystem::remove_all(dir, ec);
        DiskCache cache(dir, 1u << 20);
        check(cache.store("undeletable", makeEntry(8, 'd', 1)), "delete-failure fixture is stored");
        std::filesystem::permissions(dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec, std::filesystem::perm_options::replace, ec);
        check(!ec, "cache directory can be made read-only for failure injection");

        const auto purge = cache.purgePrefix("undeletable");
        check(!purge.complete, "prefix purge reports a filesystem failure");
        check(purge.removed == 0, "failed prefix purge reports no removal");
        check(cache.entryCount() == 1, "failed prefix purge keeps the entry indexed");
        check(!cache.clear(), "clear reports a filesystem failure");
        check(cache.entryCount() == 1, "failed clear keeps the entry indexed");

        std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
        check(!ec, "cache directory permissions are restored");
        check(cache.clear(), "clear succeeds after filesystem recovery");
    }
#endif

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
        check(got && got->body == std::string(64, 'p'), "persisted entry is readable after restart");
        check(got && got->expiresAt == 42, "persisted freshness survives restart");
    }

    // The in-memory LRU/byte index is authoritative, so two live cache objects
    // must not mutate the same directory independently.
    {
        std::filesystem::remove_all(dir, ec);
        DiskCache owner(dir, 1u << 20);
        bool rejected = false;
        try {
            DiskCache conflicting(dir, 1u << 20);
        } catch (const std::filesystem::filesystem_error&) {
            rejected = true;
        }
        check(rejected, "a second live owner of one cache directory is rejected");
    }

    // A crash before the atomic rename may leave a complete temporary record.
    // Startup must clean it rather than make an uncommitted value visible.
    {
        std::filesystem::remove_all(dir, ec);
        {
            DiskCache cache(dir, 1u << 20);
            check(cache.store("uncommitted", makeEntry(16, 'u', 1)), "record for temporary-file recovery is stored");
        }
        const auto committed = committedFile(dir);
        check(static_cast<bool>(committed), "committed cache file is discoverable");
        if (committed) {
            std::filesystem::path temporary = *committed;
            temporary += ".tmp999";
            std::filesystem::copy_file(*committed, temporary, ec);
            std::filesystem::remove(*committed, ec);
            DiskCache reopened(dir, 1u << 20);
            check(!reopened.lookup("uncommitted"), "uncommitted temporary record is not adopted");
            check(!std::filesystem::exists(temporary), "orphaned temporary record is cleaned");
        }
    }

    // A checksum mismatch must invalidate the complete record rather than
    // serving silently corrupted headers or body bytes.
    {
        std::filesystem::remove_all(dir, ec);
        {
            DiskCache cache(dir, 1u << 20);
            check(cache.store("corrupt", makeEntry(16, 'c', 1)), "record for corruption recovery is stored");
        }
        const auto committed = committedFile(dir);
        check(static_cast<bool>(committed), "corruption fixture file is discoverable");
        if (committed) {
            std::fstream file(*committed, std::ios::binary | std::ios::in | std::ios::out);
            file.seekg(-1, std::ios::end);
            char byte = 0;
            file.read(&byte, 1);
            byte ^= 0x1;
            file.seekp(-1, std::ios::end);
            file.write(&byte, 1);
            file.close();

            DiskCache reopened(dir, 1u << 20);
            check(reopened.entryCount() == 0, "checksum-invalid record is excluded from the rebuilt index");
            check(!reopened.lookup("corrupt"), "checksum-invalid record cannot be served");
        }
    }

    // clear() empties both the index and the directory.
    {
        std::filesystem::remove_all(dir, ec);
        {
            DiskCache cache(dir, 1u << 20);
            cache.store("x", makeEntry(8, 'x', 1));
            cache.store("y", makeEntry(8, 'y', 1));
            check(cache.clear(), "clear reports complete filesystem removal");
            check(cache.entryCount() == 0, "clear empties the index");
            check(cache.byteSize() == 0, "clear zeroes the byte count");
            check(!cache.lookup("x"), "cleared entry is unreadable");
        }
        DiskCache reopened(dir, 1u << 20);
        check(reopened.entryCount() == 0, "cleared files are gone from disk");
    }

    std::filesystem::remove_all(dir, ec);
    if (failures == 0) {
        std::puts("disk cache: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
