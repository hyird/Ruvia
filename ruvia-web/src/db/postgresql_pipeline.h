#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_pipeline.h"
#include "db/db_slot_socket.h"
#include "db/db_statement_cache.h"

struct pg_conn;
struct pg_result;
// PostgreSQL's part of statement pipelining (db_pipeline.h): a group's
// connection runs in libpq pipeline mode, and each statement sends its own
// flight ending in its own Sync, so the server aborts no statement for another
// one's failure. Flights are queued and flushed as they are sent; the reader
// consumes input and reads results whenever libpq holds them.

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
    static constexpr std::size_t inline_capacity = 8;
    static constexpr std::size_t numeric_slot_size = 32;

    postgresql_params(std::span<const db_value> params, std::pmr::memory_resource* resource);
    postgresql_params(const postgresql_params&) = delete;
    postgresql_params& operator=(const postgresql_params&) = delete;
    postgresql_params(postgresql_params&&) = delete;
    postgresql_params& operator=(postgresql_params&&) = delete;

    [[nodiscard]] int count() const noexcept {
        return static_cast<int>(count_);
    }

    [[nodiscard]] const char* const* values() const noexcept {
        if (count_ == 0) {
            return nullptr;
        }
        return count_ <= inline_capacity ? inline_values_.data() : heap_values_.data();
    }

    [[nodiscard]] const unsigned int* types() const noexcept {
        if (count_ == 0) {
            return nullptr;
        }
        return count_ <= inline_capacity ? inline_types_.data() : heap_types_.data();
    }

    [[nodiscard]] std::span<const unsigned int> type_span() const noexcept {
        return {types(), count_};
    }

    std::size_t count_{0};
    std::array<char, inline_capacity * numeric_slot_size> inline_encoded_;
    std::array<const char*, inline_capacity> inline_values_;
    std::array<unsigned int, inline_capacity> inline_types_;
    std::pmr::vector<std::array<char, numeric_slot_size>> heap_encoded_;
    std::pmr::vector<const char*> heap_values_;
    // libpq's Oid is unsigned int; 0 leaves the type to the server.
    std::pmr::vector<unsigned int> heap_types_;
};

[[nodiscard]] std::uint64_t postgresql_affected_rows(const pg_result& result_value) noexcept;

// Named server-side statements prepared on one PostgreSQL connection, so a
// repeated statement skips the server's parse and analysis
// (db_statement_cache: keyed by SQL and the parameter types it declares,
// prepared on its second use, least recently used closed for room). A name is
// derived from the key's SHA-256: equal names denote equal statements, so a
// name reaching a server session it was not prepared on (a pooler that shares
// server sessions) can never run a different statement. Every statement in
// flight on a pipelined connection shares the cache.
//
// "ruvia_", 32 lowercase hex digits of the key digest, and a NUL.
using postgresql_statement_name = std::array<char, 39>;
using postgresql_statement_cache = db_statement_cache<postgresql_statement_name>;

[[nodiscard]] postgresql_statement_name make_postgresql_statement_name(
    std::string_view sql, std::span<const unsigned int> types, std::pmr::memory_resource* resource);

template <typename driver_type>
class db_pool;
struct postgresql_driver;

// The commands of one buffered statement flight. A first use runs as the
// unnamed statement. Otherwise a Close of a statement evicted to make room and
// a Close and Parse of a statement new to the connection precede the
// statement. A Sync ends the flight.
struct postgresql_flight final {
    // The cache entry of a named statement.
    std::optional<postgresql_statement_cache::handle> statement_{};
    // Commands queued ahead of the statement.
    std::size_t setup_commands_{0};
    bool parses_{false};
};

// What has been read of one flight's results, up to its Sync.
struct postgresql_flight_results final {
    // Index of the command whose results are being read; each command's
    // results end with a null result, and the Sync result follows the last.
    std::size_t command_{0};
    std::uint64_t affected_rows_{0};
    // The first error of the flight; the server skips the rest up to Sync.
    std::optional<db_error> failure_{};
    // Keeping the rows failed (an allocation in the caller's resource); the
    // flight is still read up to its Sync, so only its statement fails.
    std::exception_ptr output_failure_{};
    // The error is the statement's own rather than its setup's.
    bool statement_failed_{false};
    // The error reports a stale cached statement (26000 / 0A000) ...
    bool stale_{false};
    // ... found while looking it up or revalidating it, before it ran.
    bool stale_before_execution_{false};
    bool returned_rows_{false};
    bool parsed_{false};
};

// The pipeline hooks of PostgreSQL (see db_pipeline_impl.h).
struct postgresql_pipeline_io final {
    using driver_type = postgresql_driver;
    using pool_type = db_pool<postgresql_driver>;
    using state_type = db_pipeline_state<postgresql_pipeline_io>;
    using member_type = db_pipeline_member<postgresql_pipeline_io>;
    using flight_type = postgresql_flight;

    struct group_data final {
        explicit group_data(std::pmr::memory_resource*) noexcept {}

        postgresql_flight_results head_results_{};
        // libpq holds output the socket has not accepted yet.
        bool flush_pending_{false};

        void reset() noexcept {
            head_results_ = {};
            flush_pending_ = false;
        }
    };

    // The statement text, kept by its caller, and its encoded parameters.
    struct statement_type final {
        statement_type(std::string_view sql, std::span<const db_value> params, std::pmr::memory_resource* resource)
            : sql_(sql),
              encoded_(params, resource) {}

        std::string_view sql_;
        postgresql_params encoded_;
    };

    [[nodiscard]] static statement_type prepare(pool_type& pool, std::string_view sql, std::span<const db_value> params);
    // The connection's socket for a wait; throws when it has none.
    [[nodiscard]] static db_slot_socket::native_socket_type socket(pool_type& pool, std::size_t slot);
    static void open(state_type& state, std::size_t slot);
    [[nodiscard]] static bool queue(
        state_type& state, std::size_t slot, member_type& member, statement_type& statement, flight_type& flight);
    [[nodiscard]] static bool advance(state_type& state, std::size_t slot, member_type& reader);
    [[nodiscard]] static db_socket_readiness wanted(pool_type& pool, std::size_t slot) noexcept;
    static void exchange(state_type& state, std::size_t slot, db_socket_readiness ready);
    [[nodiscard]] static bool reusable(state_type& state, std::size_t slot) noexcept;
};

// Per connection: cached statements and the pipeline group sharing it.
struct postgresql_session final {
    explicit postgresql_session(std::pmr::memory_resource* resource)
        : statements_(resource),
          pipeline_(resource) {}

    // Cleared by connect(): a new connection holds no server statements.
    postgresql_statement_cache statements_;
    db_pipeline_group<postgresql_pipeline_io> pipeline_;
};

}  // namespace ruvia::detail
