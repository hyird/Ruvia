#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/edge/detail/cache/EdgeCache.h"

namespace ruvia::edge {

// The on-disk record format of one cache entry: a magic-tagged, length-prefixed
// encoding of the key and the CachedResponse, with a trailing checksum. Knowing
// nothing about files or directories keeps it directly testable and lets the
// cache decide when a record is written.

// A stable 64-bit hash (FNV-1a) over record bytes: the record's own integrity
// checksum, and the value an entry's file name is derived from. Stable across
// runs and platforms so a scanned directory keeps naming its entries the same
// way, unlike std::hash.
[[nodiscard]] std::uint64_t diskRecordHash(std::string_view data) noexcept;

// Encode one entry, or nullopt when a field exceeds what the format can express.
[[nodiscard]] std::optional<std::string> encodeDiskRecord(
    std::string_view key,
    const CachedResponse& entry);

// Parse a serialized entry. On success fills `key` and, if `entry` is non-null,
// the full payload; when `entry` is null only the key is decoded (used by the
// startup scan, which does not need to materialize bodies).
[[nodiscard]] bool decodeDiskRecord(
    std::string_view data,
    std::string& key,
    CachedResponse* entry);

}  // namespace ruvia::edge
