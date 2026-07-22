#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ruvia/edge/detail/EdgeCache.h"

namespace ruvia::edge {

// Internal persistent, bounded second cache tier backed by a directory on disk. It
// stores the same CachedResponse payloads as the in-memory EdgeCache, one
// checksummed file per entry, and survives process restarts: the constructor
// takes an exclusive directory lease and rebuilds its index only from complete,
// atomically committed records. Orphaned temporary and corrupt records are not
// adopted.
//
// This class is intentionally synchronous and self-contained -- every method
// blocks on filesystem I/O and takes a single mutex. It knows nothing about the
// event loop. Because that I/O must never run on the reactor thread, the
// EdgeServer drives a DiskCache from a dedicated background thread and only ever
// touches it off the loop; a plain synchronous class keeps it unit-testable in
// isolation and lets the caller own the threading policy.
//
// A byte budget bounds the directory. On store, least-recently-used entries are
// evicted (their files deleted) until the budget holds; an entry whose own
// footprint exceeds the budget is refused. The eviction order is process-local
// recency, seeded arbitrarily from the on-disk set at construction.
class DiskCache final {
public:
    struct PurgeResult final {
        std::size_t removed{0};
        bool complete{true};
    };

    // Open (creating if absent) a cache directory bounded to `maxBytes`. Scans
    // the directory and adopts every well-formed entry file it finds. Throws
    // std::filesystem::filesystem_error if the directory cannot be created,
    // scanned, or exclusively leased by this cache instance.
    DiskCache(std::filesystem::path directory, std::size_t maxBytes);
    ~DiskCache();

    DiskCache(const DiskCache&) = delete;
    DiskCache& operator=(const DiskCache&) = delete;

    // Read the entry stored under `key`, or an empty optional if absent or unreadable. On a
    // hit the entry is promoted to most-recently-used. Freshness is not judged
    // here: the caller compares the returned entry's expiresAt against now, as
    // it does for the memory tier.
    [[nodiscard]] std::optional<CachedResponse> lookup(std::string_view key);

    // Write (or replace) the entry for `key`, then evict LRU entries until the
    // byte budget holds. Returns false without storing if the entry's own
    // footprint exceeds the budget or the write fails.
    bool store(std::string_view key, const CachedResponse& entry);

    // Delete the entry for `key` if present. Returns true if one was removed.
    bool purge(std::string_view key);

    // Delete every entry whose key begins with `prefix` (invalidating all cached
    // variants of one URL). `complete` is false if any matching committed file
    // could not be removed; those entries remain indexed and readable.
    [[nodiscard]] PurgeResult purgePrefix(std::string_view prefix);

    // Delete every entry. Returns false if any committed file remains because
    // its filesystem removal failed.
    [[nodiscard]] bool clear();

    [[nodiscard]] std::size_t entryCount() const;
    [[nodiscard]] std::size_t byteSize() const;

private:
    class DirectoryLease;

    struct Entry final {
        std::string fileName;  // relative to directory_
        std::size_t bytes{0};
    };

    using RecencyList = std::list<std::string>;  // keys, front = most recent

    // The on-disk file name for a key (a hash, stable across runs).
    [[nodiscard]] static std::string fileNameFor(std::string_view key);
    [[nodiscard]] bool evictWhileOverBudget() noexcept;  // caller holds mutex_
    [[nodiscard]] bool removeLocked(
        std::unordered_map<std::string, Entry>::iterator it) noexcept;
    void eraseIndexLocked(std::unordered_map<std::string, Entry>::iterator it) noexcept;

    std::filesystem::path directory_;
    std::size_t maxBytes_;
    // One live DiskCache owns a directory. Besides avoiding cross-process file
    // replacement races, this makes the in-memory byte/LRU index authoritative.
    std::unique_ptr<DirectoryLease> directoryLease_;

    mutable std::mutex mutex_;
    std::size_t totalBytes_{0};
    RecencyList recency_;
    std::unordered_map<std::string, RecencyList::iterator> lru_;
    std::unordered_map<std::string, Entry> index_;
    std::unordered_map<std::string, std::string> fileOwners_;
    std::uint64_t tempCounter_{0};
};

}  // namespace ruvia::edge
