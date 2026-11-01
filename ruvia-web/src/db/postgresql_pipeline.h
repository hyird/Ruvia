#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_pipeline.h"
#include "db/db_postgresql.h"
#include "db/db_slot_socket.h"

// PostgreSQL's part of statement pipelining (db_pipeline.h): a group's
// connection runs in libpq pipeline mode, and each statement sends its own
// flight ending in its own Sync, so the server aborts no statement for another
// one's failure. Flights are queued and flushed as they are sent; the reader
// consumes input and reads results whenever libpq holds them.

namespace ruvia::detail {

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
    static void check_wait(const db_socket_wait_result& result);
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
