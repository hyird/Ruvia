#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

// A migration is identified by its id for the rest of a schema's life, so
// editing one that has already run changes nothing: the id is on record and the
// new text is skipped, silently, on every machine that already migrated. The
// digest of the text is recorded alongside the id so that edit is reported
// instead.
//
// SHA-256, lowercase hex. This is drift detection, not a security boundary --
// nobody is choosing migration text to collide with -- but a digest with no
// known collisions costs nothing here and leaves no judgement call.
inline constexpr std::size_t kMigrationChecksumSize = 64;

[[nodiscard]] std::pmr::string migrationChecksum(std::string_view sql, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
