#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_statement_cache.h"

struct st_mysql;
struct st_mysql_stmt;

namespace ruvia::detail {

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
// The cache owns every statement handle, including those evicted or found
// stale that still wait to be closed on the server, and each entry's SQL,
// which libmariadb reads while a prepare is suspended. After the connection
// was closed, clear() frees the handles without I/O.
class mariadb_statement_cache final {
public:
    using handle = db_statement_handle;

    explicit mariadb_statement_cache(std::pmr::memory_resource* resource) noexcept;
    ~mariadb_statement_cache();

    mariadb_statement_cache(mariadb_statement_cache&& other) noexcept;
    mariadb_statement_cache& operator=(mariadb_statement_cache&& other) noexcept;
    mariadb_statement_cache(const mariadb_statement_cache&) = delete;
    mariadb_statement_cache& operator=(const mariadb_statement_cache&) = delete;

    // Counts a use of `sql` with `params`. A statement comes back only when
    // the caller may run it prepared now and it would return what its text
    // returns; another use still counts toward preparing it later.
    [[nodiscard]] std::optional<handle> find(std::string_view sql, std::span<const db_value> params, bool may_prepare);
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

    // Services for mariadb_prepared_run.
    struct state;
    [[nodiscard]] state& data();

private:
    std::pmr::memory_resource* resource_;
    state* state_{nullptr};
};

// Whether `sql` names one of the session tracking variables.
[[nodiscard]] bool mariadb_names_session_tracking(std::string_view sql) noexcept;

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
inline constexpr std::string_view mariadb_session_tracking_setup =
    "SET SESSION session_track_system_variables = '"
    "sql_mode,old_mode,character_set_client,character_set_connection,character_set_results,time_zone,"
    "div_precision_increment,default_week_format,lc_time_names,max_sort_length,group_concat_max_len,"
    "session_track_system_variables,session_track_schema"
    "', session_track_schema = ON";
static_assert(mariadb_session_tracking_setup.find(mariadb_tracked_variables) != std::string_view::npos);

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
    mariadb_statement_cache::handle handle_{};
    std::span<const db_value> params_{};
    step step_{step::done};
    // The current step's operation has been started and waits to continue.
    bool started_{false};
    // A handle being closed on the server, or drained.
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
    mariadb_statement_cache& cache, mariadb_statement_cache::handle statement, std::span<const db_value> params) noexcept;
// Continues a run with the readiness libmariadb waited for (0 to start the
// current step); returns the next wait mask, or 0 when the caller goes on.
// A failure that leaves the connection out of step is thrown.
[[nodiscard]] int resume_mariadb_prepared(st_mysql& connection, mariadb_prepared_run& run, int ready);
// Keeps the stored rows of the run's first result in `output`, or discards
// them when it is null; then the run resumes.
void take_mariadb_prepared_rows(mariadb_prepared_run& run, db_rows* output, std::pmr::memory_resource* resource);
// Ends a run whose statement was abandoned before it executed.
void abandon_mariadb_prepared(mariadb_prepared_run& run) noexcept;
// What a statement's responses amount to, whichever protocol ran it: its own
// failure, a second row set, a failure to keep its rows, or a row set the
// caller did (not) ask for; null when it succeeded.
[[nodiscard]] std::exception_ptr mariadb_statement_failure(const std::exception_ptr& failure, bool extra_row_set,
    const std::exception_ptr& output_failure, bool returns_rows, bool returned_rows);

}  // namespace ruvia::detail
