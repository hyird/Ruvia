#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/db/db.h"

struct st_mysql;
struct st_mysql_res;
struct ma_charset_info_st;

namespace ruvia::detail {

// Builds the error for a failed driver call from what libmariadb reports for
// it. A statement_failed whose cause ended the connection is reported as
// io_error.
[[nodiscard]] db_error mariadb_error(std::string_view operation, unsigned int native_code, const char* sql_state,
    const char* message, db_error::code_type code);
[[nodiscard]] db_error mysql_error(
    const st_mysql& connection, std::string_view operation, db_error::code_type code);

// libmariadb's own errors: the CR_ range and its extended CER_ range. Every
// other code is the server's.
[[nodiscard]] bool mariadb_client_error(unsigned int native_code) noexcept;
// A failed command after which the connection cannot carry another one: the
// client library failed or lost the exchange, or the server ended the session
// or read a packet it did not expect. The server answers any other error once
// the command is done, and the connection is ready for the next.
[[nodiscard]] bool mariadb_connection_failure(unsigned int native_code) noexcept;

// MariaDB's C API carries SQL and escaped-value lengths in unsigned long.
// Reject a wider C++ view before any narrowing conversion reaches the driver.
void validate_mariadb_sql_length(std::size_t length);

void free_stored_result(void* result_value) noexcept;

// Whether interpolating `params` into `sql` depends on the session: only a
// backslash does, in the statement or in a string parameter, since
// NO_BACKSLASH_ESCAPES decides whether it escapes inside a literal. Without
// one, interpolate_sql() writes text every session reads alike.
[[nodiscard]] bool mariadb_interpolation_needs_session(
    std::string_view sql, std::span<const db_value> params) noexcept;

// Whether escaping `params` for the session needs its client character set.
// Only an escape a string needs could change its meaning: a backslash put in
// front of its own backslash, in a session that reads backslash escapes (its
// current server status tells). That is safe in every character set right
// after an ASCII byte; right after another byte a multibyte character set
// (GBK, Big5, SJIS) may read the two as one character and leave the inserted
// backslash to escape what follows.
[[nodiscard]] bool mariadb_interpolation_needs_charset(
    const st_mysql& connection, std::span<const db_value> params) noexcept;

// The query that reads the session's client character set, and the
// character set its stored result names. The latter throws
// std::invalid_argument when libmariadb does not know it.
inline constexpr std::string_view mariadb_charset_query = "SELECT @@character_set_client";
[[nodiscard]] const ma_charset_info_st& mariadb_queried_charset(st_mysql_res& result);
// utf8mb4, the client character set a connection opens with: the one to
// escape for while the session's tracking says it is still in effect.
[[nodiscard]] const ma_charset_info_st& mariadb_utf8mb4_charset() noexcept;

// Writes `sql` with each placeholder replaced by its parameter's literal to
// `output`. A quote is escaped by doubling it, which reads the same in every
// sql_mode and character set; a backslash, in a session that reads backslash
// escapes, by another one, as `charset` (the session's client character set)
// splits the string into characters. The session's status must be current:
// every response before has been read (see
// mariadb_interpolation_needs_session()); `charset` is needed only where
// mariadb_interpolation_needs_charset() says so.
void interpolate_sql(st_mysql& connection, std::string_view sql, std::span<const db_value> params,
    std::pmr::string& output, const ma_charset_info_st* charset = nullptr);

// What a statement's text tells about how it may run.
struct mariadb_statement_traits final {
    // A cached statement may run prepared: not a CALL, whose OUT parameters
    // come back as an extra result set only in the binary protocol; not a
    // LOAD; not a SET or USE, which change the session every cached
    // statement was prepared in and so would drop their own handle each
    // time they run; not one with RETURNING, whose changes a failure after
    // its result changed shape would hide; and no executable comment, whose
    // content the server runs but this scan cannot see.
    bool preparable_{false};
    // Placeholders, by position, where the text protocol takes only an
    // integer literal (LIMIT, OFFSET, FETCH and ROWS EXAMINED counts).
    std::uint64_t integer_params_{0};
};

[[nodiscard]] mariadb_statement_traits mariadb_statement_traits_of(std::string_view sql) noexcept;

// A statement that must have the connection to itself: LOAD ... LOCAL INFILE
// answers its command with a request for the file, then reads the file's
// content as packets of the same command, so a command sent behind it would
// be read as file content. A statement led by an executable comment counts as
// one, since the comment may hide a LOAD.
[[nodiscard]] bool mariadb_exclusive_statement(std::string_view sql) noexcept;

// A statement that may run a stored procedure: a CALL, or one hidden behind
// an executable comment.
[[nodiscard]] bool mariadb_calls_routine(std::string_view sql) noexcept;

}  // namespace ruvia::detail
