#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/web/db/db_types.h"

#include "db/db_pipeline.h"
#include "db/db_slot_socket.h"
#include "db/mariadb_statements.h"

struct st_mysql_res;
struct ma_charset_info_st;

// MariaDB's part of statement pipelining (db_pipeline.h). A group's
// statements are sent as separate COM_QUERY commands (mysql_send_query) and
// their responses read in order (mysql_read_query_result and
// mysql_store_result). A failed statement does not affect the ones after it:
// the server runs each command on its own.
//
// libmariadb runs one nonblocking operation per connection at a time, and
// accepts a command only once every result before it has been fully read. So
// flights queue in the group and the reader sends them whenever it stands
// between two responses, before it reads the next one: statements that arrive
// while a response is being read go out right after it. The bytes sent ahead
// of unread responses are bounded (mariadb_pipeline_send_budget), so neither
// side can fill its socket buffers with requests while the other waits to
// write responses; a statement larger than the budget is sent once nothing is
// outstanding.
//
// A command is sent ahead of responses only when the statements before it
// cannot change what it means. Its parameters are interpolated when it is
// queued, in text every session reads alike; a statement whose interpolation
// depends on the session (mariadb_interpolation_needs_session()) is
// interpolated and sent once every response before it has been read, a SET of
// sql_mode among them, and when its escape depends on the session's client
// character set (mariadb_interpolation_needs_charset()), that is read from the
// server first, unless session tracking says it is still utf8mb4. A LOAD ...
// LOCAL INFILE (mariadb_exclusive_statement()) goes out alone, and nothing
// follows it until its response has been read.
//
// A statement found in the connection's statement cache runs prepared only
// when it is the only statement its worker runs: the binary protocol has no
// nonblocking way to read an execution's response other than by waiting for
// it, so a prepared statement cannot share the wire with other flights, and
// waiting for each response costs more than the server saves on parsing once
// other statements compete for the worker. Those run as pipelined text.

namespace ruvia::detail {

template <typename driver_type>
class db_pool;
struct mariadb_driver;

inline constexpr std::size_t mariadb_pipeline_send_budget = 64 * 1024;

// One statement's command: its text, kept by its caller until it is sent,
// and the statement as written, for running it prepared or interpolating it
// late.
struct mariadb_flight final {
    // Empty while `deferred_`.
    std::string_view sql_{};
    std::string_view statement_sql_{};
    std::span<const db_value> params_{};
    // The bytes it put on the wire, once sent.
    std::size_t size_{0};
    bool sent_{false};
    // Interpolated when sent, once every response before it has been read.
    bool deferred_{false};
    // Sent once every response before it has been read, and followed by
    // nothing until its own response has been read.
    bool exclusive_{false};
    // May run a stored procedure (mariadb_calls_routine()).
    bool routine_{false};
    // Its use was counted in the statement cache: when the reader comes back
    // to it, it goes out as text.
    bool counted_{false};
    // The session's client character set, read for a deferred statement
    // whose escape depends on it.
    const ma_charset_info_st* charset_{nullptr};
};

// The operation a group's connection runs, one at a time.
enum class mariadb_operation : std::uint8_t {
    none,
    send,
    read,
    store,
    next_result,
    // Reading the session's client character set for the head statement:
    // the query, then its row.
    charset_query,
    charset_store,
    // A statement running alone in the group as a prepared statement.
    prepared,
};

// The pipeline hooks of MariaDB (see db_pipeline_impl.h).
struct mariadb_pipeline_io final {
    using driver_type = mariadb_driver;
    using pool_type = db_pool<mariadb_driver>;
    using state_type = db_pipeline_state<mariadb_pipeline_io>;
    using member_type = db_pipeline_member<mariadb_pipeline_io>;
    using flight_type = mariadb_flight;

    struct group_data final {
        explicit group_data(std::pmr::memory_resource* resource)
            : send_buffer_(resource) {}

        // Frees a stored result nobody took; called whenever the group's
        // flights clear, which every group does before its slot goes away.
        void reset() noexcept;

        // The operation in progress: the wait mask it suspended with, and,
        // once exchange() finished it, its result.
        mariadb_operation operation_{mariadb_operation::none};
        int status_{0};
        bool done_{false};
        int result_{0};
        st_mysql_res* stored_{nullptr};
        // Records from the head that were sent, or dropped unsent because
        // their statement was abandoned first.
        std::size_t sent_{0};
        std::size_t bytes_in_flight_{0};
        // Commands were sent since the reader last paused for a turn.
        bool batched_{false};
        // The command being sent, copied (or, deferred, interpolated) so that
        // a statement abandoned while its send is suspended leaves libmariadb
        // nothing dangling. Above the send budget it is given back once sent.
        std::pmr::string send_buffer_;
        // What has been read of the head's responses.
        std::exception_ptr head_failure_{};
        std::exception_ptr head_output_failure_{};
        std::uint64_t head_affected_rows_{0};
        std::uint64_t head_insert_id_{0};
        bool head_returned_rows_{false};
        bool head_extra_row_set_{false};
        // The head statement's prepared execution, while operation_ is
        // prepared.
        mariadb_prepared_run prepared_{};
    };

    // The statement as its caller wrote it; queue() interpolates the
    // parameters unless that waits for the session (mariadb_flight).
    struct statement_type final {
        std::string_view sql_;
        std::span<const db_value> params_;
        std::pmr::string interpolated_;
    };

    [[nodiscard]] static statement_type prepare(pool_type& pool, std::string_view sql, std::span<const db_value> params);
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

// Per connection: cached prepared statements and the pipeline group sharing
// it.
struct mariadb_session final {
    explicit mariadb_session(std::pmr::memory_resource* resource)
        : statements_(resource),
          pipeline_(resource) {}

    // Cleared by connect(): a new connection holds no server statements.
    mariadb_statement_cache statements_;
    db_pipeline_group<mariadb_pipeline_io> pipeline_;
};

}  // namespace ruvia::detail
