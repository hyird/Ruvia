#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_pipeline.h"
#include "db/db_slot_socket.h"
#include "db/mariadb_sql.h"

struct st_mysql;
struct st_mysql_stmt;

namespace ruvia::detail {

// Storage a bound parameter points at for the length of an execution.
union mariadb_param_value {
    long long integer_;
    double real_;
    signed char flag_;
};

// Storage of one result column between mysql_stmt_fetch() and formatting.
struct mariadb_column final {
    std::vector<char> buffer_;
    unsigned long length_{0};
    char null_{0};
    char error_{0};
};

// One cached statement: its entry_ref once prepared, the placeholders its text
// restricts (mariadb_statement_traits::integer_params_), and the hash of its
// key, for refusing it.
struct mariadb_statement final {
    st_mysql_stmt* handle_{nullptr};
    std::uint64_t integer_params_{0};
    std::size_t key_hash_{0};
};

// Server-side prepared statements of one MariaDB connection, so a repeated
// statement skips the server's parse and travels in the binary protocol
// (db_statement_cache: keyed by SQL and parameter types, prepared on its
// second use, the least recently used one closed for room).
//
// The text protocol is the reference: a statement runs prepared only where
// the binary protocol returns the same field text and errors. So it runs as
// text when the server will not prepare it, when its statement text rules it
// out (mariadb_statement_traits), when a result column is one whose text the
// binary value cannot tell (a YEAR the prepare describes), and for a call whose
// parameters the server would read otherwise than their literals: a negative
// number (a literal's unary minus binds looser than COLLATE, and -? of
// INT64_MIN overflows only as a parameter), an integer above INT64_MAX, a
// string with a NUL byte or a non-finite double (refused by the text
// protocol), and anything but a non-negative integer where the text protocol
// takes only an integer literal (LIMIT ?).
//
// The server parses a statement when it prepares it, under the session's
// sql_mode, character sets, default schema and other variables of that
// moment, and keeps that reading. The session reports every change of those
// variables (mariadb_tracked_variables, tracked with the schema from when the
// connection opens): such a change drops every cached statement, so each is
// prepared anew under the new state; the session's other reports (last_gtid
// after each commit when binary logging is on, autocommit) leave the cache
// alone. A connection whose server does not track its session, whose
// tracking a statement turned off, or whose client or connection character
// set is not the utf8mb4 it opened with (the binary protocol converts
// parameters otherwise than the text protocol does literals) runs nothing
// prepared. A stored procedure may turn tracking off without a report, so
// after a CALL the next prepared execution first reads the tracked list back
// from the server.
//
// The cache owns every statement entry_ref, including those evicted or found
// stale that still wait to be closed on the server, and each entry's SQL,
// which libmariadb reads while a prepare is suspended. After the connection
// was closed, clear() frees the handles without I/O.
class mariadb_statement_cache final {
public:
    using entry_ref = db_statement_entry;

    mariadb_statement_cache();
    ~mariadb_statement_cache();

    mariadb_statement_cache(const mariadb_statement_cache&) = delete;
    mariadb_statement_cache& operator=(const mariadb_statement_cache&) = delete;

    // Counts a use of `sql` with `params`. A statement comes back only when
    // the caller may run it prepared now and it would return what its text
    // returns; another use still counts toward preparing it later.
    [[nodiscard]] std::optional<entry_ref> find(std::string_view sql, std::span<const db_value> params, bool may_prepare);
    // A new connection starts without server-side statements; the old one is
    // closed, so its handles are freed without I/O. Until connected() says
    // otherwise, the session counts as untracked.
    void clear() noexcept;

    // Hears the session changes `connection` reads from the server; false
    // when libmariadb refused the callback. Called before it connects.
    [[nodiscard]] bool observe(st_mysql& connection);
    // The connection opened: whether the server tracks every change of its
    // session for it from now on.
    void connected(bool tracked) noexcept;
    // A statement that may turn session tracking off (it names one of its
    // variables, whose change the server does not report) is about to run.
    void untrack() noexcept;
    // A statement that may run a stored procedure was sent; the procedure
    // may have turned tracking off unreported.
    void routine_called() noexcept;
    // Whether the session's client character set is known to be utf8mb4:
    // tracked, and so current once every response before has been read.
    [[nodiscard]] bool client_utf8mb4() const noexcept;

    [[nodiscard]] mariadb_statement& entry(entry_ref statement);
    void reserve_retired(std::size_t count = 1);
    void retire(st_mysql_stmt* statement) noexcept;
    void refuse(std::size_t hash) noexcept;
    void refuse(entry_ref statement, std::size_t hash) noexcept;
    [[nodiscard]] bool refused(std::size_t hash) const noexcept;
    void invalidate();
    [[nodiscard]] bool usable() const noexcept;
    void lose_tracking() noexcept;

    db_statement_cache<mariadb_statement> statements_;
    std::vector<st_mysql_stmt*> retired_;
    // Key hashes of statements that run as text on this connection; zero
    // marks a free position.
    std::array<std::size_t, db_statement_cache<mariadb_statement>::capacity> refused_{};
    std::size_t next_refused_{0};
    // What the session reported: whether it tracks every variable the cache
    // relies on and the schema, and whether its client and connection
    // character sets are still utf8mb4; and a change since the last find().
    bool tracked_{false};
    bool all_variables_{false};
    bool schema_tracked_{false};
    bool untracked_{false};
    bool client_utf8mb4_{true};
    bool connection_utf8mb4_{true};
    bool changed_{false};
    // A stored procedure ran since the tracked list was last read back.
    bool verify_{false};
    // Scratch of the one execution a connection runs at a time.
    std::vector<unsigned int> types_;
    std::vector<std::max_align_t> binds_;
    std::vector<mariadb_param_value> values_;
    std::vector<mariadb_column> columns_;
    // The bytes the column buffers hold, and those the last result needed.
    std::size_t column_bytes_{0};
    std::size_t column_needed_{0};
};

// The system variables a statement's parse or result depends on, as
// session_track_system_variables lists them, with the two tracking variables
// themselves, so that turning the schema's tracking off or changing this
// list to one that still names it is reported. A change of
// collation_connection is reported as one of character_set_connection.
inline constexpr std::string_view mariadb_tracked_variables =
    "sql_mode,old_mode,character_set_client,character_set_connection,character_set_results,time_zone,"
    "div_precision_increment,default_week_format,lc_time_names,max_sort_length,group_concat_max_len,"
    "session_track_system_variables,session_track_schema";

// Turns on the session tracking the statement cache relies on.
inline constexpr auto mariadb_session_tracking_storage = [] {
    constexpr std::string_view prefix = "SET SESSION session_track_system_variables = '";
    constexpr std::string_view suffix = "', session_track_schema = ON";
    std::array<char, prefix.size() + mariadb_tracked_variables.size() + suffix.size()> statement{};
    auto out = statement.begin();
    for (const auto part : std::array{prefix, mariadb_tracked_variables, suffix}) {
        for (const char byte : part) {
            *out++ = byte;
        }
    }
    return statement;
}();
inline constexpr std::string_view mariadb_session_tracking_setup(mariadb_session_tracking_storage.data(), mariadb_session_tracking_storage.size());

// One prepared execution as a sequence of libmariadb operations: after a
// stored procedure ran, reading back the session's tracked variables;
// closing the statements waiting to be closed, preparing the statement on its
// first prepared use, executing it, and storing each result.
// resume_mariadb_prepared() starts or continues the current operation and
// returns the wait mask its continuation needs; 0 hands the run back to its
// caller, which goes on according to step_.
struct mariadb_prepared_run final {
    enum class step : std::uint8_t {
        // Querying @@session_track_system_variables, then storing its row.
        verifying,
        verified,
        close,
        prepare,
        // The caller confirms the statement still runs, then resumes.
        execute,
        executing,
        storing,
        // The rows failed (the server's error ended them): back to the state
        // after prepare, so that the next execution starts clean.
        resetting,
        // The caller takes the stored rows, then resumes.
        rows,
        next_result,
        // Reading past the rows of a result the run cannot use.
        draining,
        done,
    };

    mariadb_statement_cache* cache_{nullptr};
    mariadb_statement_cache::entry_ref handle_{};
    std::span<const db_value> params_{};
    step step_{step::done};
    // The current step's operation has been started and waits to continue.
    bool started_{false};
    // A entry_ref being closed on the server, or drained.
    st_mysql_stmt* closing_{nullptr};
    // The statement's outcome: its failure, or what its first result reported.
    std::exception_ptr failure_{};
    std::exception_ptr output_failure_{};
    std::uint64_t affected_rows_{0};
    std::uint64_t insert_id_{0};
    bool returned_rows_{false};
    bool extra_row_set_{false};
    // It did not execute because the server no longer holds it as prepared
    // (ER_NEED_REPREPARE, ER_UNKNOWN_STMT_HANDLER). It was dropped from the
    // cache, and a run outside a transaction may go again, preparing it
    // anew. A statement whose result changed shape after it ran is dropped
    // as well, but fails: the server has run it, and running it again would
    // repeat its effects.
    bool stale_{false};
    // The server refused to prepare it, its result is not readable as text,
    // or the session no longer tracks what the cache relies on: run it as
    // text instead.
    bool text_{false};
};

// Starts a run for a statement find() returned.
[[nodiscard]] mariadb_prepared_run start_mariadb_prepared(
    mariadb_statement_cache& cache, mariadb_statement_cache::entry_ref statement, std::span<const db_value> params) noexcept;
// Continues a run with the readiness libmariadb waited for (0 to start the
// current step); returns the next wait mask, or 0 when the caller goes on.
// A failure that leaves the connection out of step is thrown.
[[nodiscard]] int resume_mariadb_prepared(st_mysql& connection, mariadb_prepared_run& run, int ready);
// Keeps the stored rows of the run's first result in `output`, or discards
// them when it is null; then the run resumes.
void take_mariadb_prepared_rows(mariadb_prepared_run& run, db_rows* output);
// Ends a run whose statement was abandoned before it executed.
void abandon_mariadb_prepared(mariadb_prepared_run& run) noexcept;
// What a statement's responses amount to, whichever protocol ran it: its own
// failure, a second row set, a failure to keep its rows, or a row set the
// caller did (not) ask for; null when it succeeded.
[[nodiscard]] std::exception_ptr mariadb_statement_failure(const std::exception_ptr& failure, bool extra_row_set,
    const std::exception_ptr& output_failure, bool returns_rows, bool returned_rows);

}  // namespace ruvia::detail

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
// depends on the session -- a backslash in its text or in a string parameter,
// which NO_BACKSLASH_ESCAPES reads either way -- is interpolated and sent once
// every response before it has been read, a SET of sql_mode among them, and
// when its escape depends on the session's client character set
// (mariadb_escape_needs_charset()), that is read from the server first, unless
// session tracking says it is still utf8mb4. A LOAD ... LOCAL INFILE
// (mariadb_statement_classification::exclusive_) goes out alone, and nothing
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
    std::string* interpolated_{nullptr};
    // The bytes it put on the wire, once sent.
    std::size_t size_{0};
    bool sent_{false};
    // Interpolated when sent, once every response before it has been read.
    bool deferred_{false};
    // Sent once every response before it has been read, and followed by
    // nothing until its own response has been read.
    bool exclusive_{false};
    // May run a stored procedure (mariadb_statement_classification::calls_routine_).
    bool routine_{false};
    // Its use was counted in the statement cache: when the reader comes back
    // to it, it goes out as text.
    bool counted_{false};
    // Whether `params_` contains a backslash preceded by a non-ASCII byte.
    bool ambiguous_backslash_{false};
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

    struct group_state final {
        group_state()
            : send_buffer_() {}

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
        std::string send_buffer_;
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
        std::string interpolated_;
        mariadb_statement_classification classification_{};
        bool deferred_{false};
        bool ambiguous_backslash_{false};
    };

    [[nodiscard]] static statement_type prepare(std::string_view sql, std::span<const db_value> params);
    [[nodiscard]] static db_slot_socket::native_socket_type socket(pool_type& pool, std::size_t slot);
    static void open(state_type& state, std::size_t slot);
    [[nodiscard]] static bool queue(
        state_type& state, std::size_t slot, member_type& member, statement_type& statement, flight_type& flight);
    [[nodiscard]] static bool advance(state_type& state, std::size_t slot, member_type& reader);
    [[nodiscard]] static db_socket_readiness wanted(pool_type& pool, std::size_t slot) noexcept;
    static void exchange(state_type& state, std::size_t slot, db_socket_readiness ready);
    [[nodiscard]] static bool reusable(state_type& state, std::size_t slot) noexcept;
};

// Per connection: cached prepared statements and the pipeline group sharing
// it.
struct mariadb_session final {
    // Cleared by connect(): a new connection holds no server statements.
    mariadb_statement_cache statements_;
    db_pipeline_group<mariadb_pipeline_io> pipeline_;
};

}  // namespace ruvia::detail
