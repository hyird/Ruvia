#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_types.h"

struct pg_conn;
struct pg_result;

namespace ruvia::detail {

// Builds the error for a failed libpq call or result. A statement_failed on a
// connection libpq has marked bad is reported as io_error.
[[nodiscard]] db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type code, const pg_result* result = nullptr);

// Parameters are sent in text format. A double declares float8 so the server
// never infers an integer type from an integral value; every other parameter
// is left untyped (OID 0): the SQL decides its type, and compiled SQL casts
// each placeholder from its value category.
struct postgresql_params final {
    explicit postgresql_params(std::pmr::memory_resource* resource);

    std::pmr::vector<std::pmr::string> encoded_;
    std::pmr::vector<const char*> values_;
    // libpq's Oid is unsigned int; 0 leaves the type to the server.
    std::pmr::vector<unsigned int> types_;
};

// Borrows owned string parameters from the prepared statement. The caller must
// keep params alive and unchanged through PQsendQueryParams; numeric encodings
// are owned by the returned object. No text-format lengths array is needed.
[[nodiscard]] postgresql_params encode_postgresql_params(
    std::span<const db_value> params, std::pmr::memory_resource* resource);

[[nodiscard]] std::uint64_t postgresql_affected_rows(const pg_result& result_value) noexcept;

// Named server-side statements prepared on one PostgreSQL connection, so a
// repeated statement skips the server's parse and analysis. The server fixes
// parameter types at Parse, so a statement is keyed by its SQL and the
// parameter types it declares. A statement is prepared on its second use: the
// first runs unnamed, so SQL that never repeats (literals, generated lists)
// costs no more than without the cache and does not evict reused statements.
// A name is derived from the key's SHA-256: equal names denote equal
// statements, so a name reaching a server session it was not prepared on (a
// pooler that shares server sessions) can never run a different statement.
// Entries live as long as the connection; the least recently used one is
// closed on the server when a new one needs room.
//
// The pool leases a connection to one operation at a time, which owns the
// cache for its duration; indices stay valid until that owner forgets or
// inserts an entry.
class postgresql_statement_cache final {
public:
    static constexpr std::size_t capacity = 128;
    // "ruvia_", 32 lowercase hex digits of the key digest, and a NUL.
    using name_type = std::array<char, 39>;

    struct lookup final {
        // Absent on a first use, which runs as the unnamed statement.
        std::optional<std::size_t> index_;
        // The statement is new to the connection: Parse it before binding.
        bool inserted_{false};
        // A statement evicted to make room; it is still prepared on the server.
        std::optional<name_type> evicted_{};
    };

    explicit postgresql_statement_cache(std::pmr::memory_resource* resource);

    [[nodiscard]] lookup find_or_insert(std::string_view sql, std::span<const unsigned int> types);
    [[nodiscard]] const char* name(std::size_t index) const noexcept {
        return entries_[index].name_.data();
    }
    // Drops an entry the server does not hold (a failed Parse) or no longer
    // holds as cached (a stale statement).
    void forget(std::size_t index);
    // A new connection starts without server-side statements.
    void clear() noexcept;

private:
    struct entry final {
        std::pmr::string sql_;
        std::pmr::vector<unsigned int> types_;
        std::size_t hash_;
        std::uint64_t last_use_;
        name_type name_;
    };

    std::pmr::vector<entry> entries_;
    // Key hashes of recent first uses; zero marks a free position. A hash
    // collision only prepares a statement on its first use.
    std::array<std::size_t, capacity> first_uses_{};
    std::size_t next_first_use_{0};
    std::uint64_t clock_{0};
};

}  // namespace ruvia::detail
