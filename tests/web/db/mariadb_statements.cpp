#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "pipeline_support.h"
#include "test_harness.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_MARIADB
namespace mariadb_statement_test {

using pipeline_test::peer_type;
using reply = peer_type::maria_reply;
using column = peer_type::maria_column;
using binary_row = std::vector<std::optional<std::string>>;

// MariaDB type codes and column flags of the binary protocol.
constexpr std::uint8_t type_long = 3;
constexpr std::uint8_t type_float = 4;
constexpr std::uint8_t type_double = 5;
constexpr std::uint8_t type_longlong = 8;
constexpr std::uint8_t type_date = 10;
constexpr std::uint8_t type_time = 11;
constexpr std::uint8_t type_datetime = 12;
constexpr std::uint8_t type_year = 13;
constexpr std::uint8_t type_newdecimal = 0xf6;
constexpr std::uint8_t type_blob = 0xfc;
constexpr std::uint16_t unsigned_flag = 32;
constexpr std::uint16_t zerofill_flag = 64;
constexpr std::uint8_t not_fixed = 31;

// @@session_track_system_variables as MariaDB 11.8 reads it back after the
// client's setup: the same names, sorted.
constexpr std::string_view server_tracked_list =
    "character_set_client,character_set_connection,character_set_results,default_week_format,"
    "div_precision_increment,group_concat_max_len,lc_time_names,max_sort_length,old_mode,session_track_schema,"
    "session_track_system_variables,sql_mode,time_zone";

[[nodiscard]] inline ruvia::db_config config(peer_type& peer) {
    auto value = peer.config();
    value.driver_ = ruvia::db_driver::mariadb;
    value.username_ = "test";
    return value;
}

[[nodiscard]] inline std::string little(std::uint64_t value, int width) {
    std::string bytes;
    for (int index = 0; index < width; ++index) {
        bytes.push_back(static_cast<char>((value >> (8 * index)) & 255));
    }
    return bytes;
}

[[nodiscard]] inline std::string real(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return little(bits, 8);
}

[[nodiscard]] inline std::string single(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return little(bits, 4);
}

[[nodiscard]] inline std::string text(std::string_view value) {
    std::string bytes(1, static_cast<char>(value.size()));
    bytes.append(value);
    return bytes;
}

// DATE, DATETIME and TIMESTAMP: a length, then year, month, day, hour,
// minute, second and microseconds as far as they are not zero.
[[nodiscard]] inline std::string datetime(unsigned year, unsigned month, unsigned day, unsigned hour = 0,
    unsigned minute = 0, unsigned second = 0, unsigned microsecond = 0) {
    if (year == 0 && month == 0 && day == 0 && hour == 0 && minute == 0 && second == 0 && microsecond == 0) {
        return std::string(1, '\0');
    }
    std::string bytes = little(year, 2) + static_cast<char>(month) + static_cast<char>(day);
    if (hour != 0 || minute != 0 || second != 0 || microsecond != 0) {
        bytes += std::string{static_cast<char>(hour), static_cast<char>(minute), static_cast<char>(second)};
        if (microsecond != 0) {
            bytes += little(microsecond, 4);
        }
    }
    return static_cast<char>(bytes.size()) + bytes;
}

// TIME: a length, then the sign, days, hours, minutes, seconds and
// microseconds.
[[nodiscard]] inline std::string time(
    bool negative, unsigned days, unsigned hour, unsigned minute, unsigned second, unsigned microsecond = 0) {
    std::string bytes(1, negative ? '\1' : '\0');
    bytes += little(days, 4);
    bytes += std::string{static_cast<char>(hour), static_cast<char>(minute), static_cast<char>(second)};
    if (microsecond != 0) {
        bytes += little(microsecond, 4);
    }
    return static_cast<char>(bytes.size()) + bytes;
}

// Runs `sql` twice alone: the first use as text, the second prepared.
inline ruvia::task<void> twice(ruvia::db_handle handle, std::string_view sql, std::optional<ruvia::db_rows>& text,
    std::optional<ruvia::db_rows>& prepared) {
    text.emplace(co_await handle.query(sql));
    prepared.emplace(co_await handle.query(sql));
}

[[nodiscard]] inline std::vector<std::optional<std::string>> values(const ruvia::db_rows& rows) {
    std::vector<std::optional<std::string>> result;
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (std::size_t field = 0; field < rows[row].size(); ++field) {
            const auto value = rows[row][field].value();
            result.push_back(value.has_value() ? std::optional<std::string>(std::string(*value)) : std::nullopt);
        }
    }
    return result;
}

// The first row's fields, joined by '|'.
[[nodiscard]] inline std::string first_row(const ruvia::db_rows& rows) {
    if (rows.empty()) {
        return "<no rows>";
    }
    std::string joined;
    for (std::size_t field = 0; field < rows.front().size(); ++field) {
        joined += field == 0 ? "" : "|";
        joined += rows.front()[field].value().value_or("");
    }
    return joined;
}

template <typename... param_types>
inline ruvia::task<void> query_with(
    ruvia::db_handle handle, std::string_view sql, pipeline_test::outcome& result, param_types... params) {
    try {
        result.value_ = first_row(co_await handle.query(sql, params...));
    } catch (const ruvia::db_error& error) {
        pipeline_test::record_failure(error, result);
    }
}

template <typename... param_types>
inline ruvia::task<void> execute_with(
    ruvia::db_handle handle, std::string_view sql, pipeline_test::outcome& result, param_types... params) {
    try {
        const auto executed = co_await handle.execute(sql, params...);
        result.affected_rows_ = executed.affected_rows();
    } catch (const ruvia::db_error& error) {
        pipeline_test::record_failure(error, result);
    }
}

}  // namespace mariadb_statement_test

// A statement is prepared on its second use and executed in the binary
// protocol from then on, while it runs alone on its connection.
RUVIA_TEST(db_mariadb_repeated_statement_runs_prepared_from_its_second_use) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer({{.rows_ = {"a"}}, {.rows_ = {"b"}}, {.rows_ = {"c"}}}, peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (auto& result : outcomes) {
            co_await pipeline_test::query_id(handle, "SELECT v FROM t WHERE id = ?", 7, result);
        }
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT v FROM t WHERE id = 7", "prepare:SELECT v FROM t WHERE id = ?",
                     "execute:1:i:7", "execute:1:i:7"}));
}

// Parameters travel in the binary protocol with the types their literals
// read as: an unsigned integer within INT64_MAX is a signed BIGINT, as its
// literal is.
RUVIA_TEST(db_mariadb_prepared_parameters_keep_their_types) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer({{.rows_ = {"a"}}, {.rows_ = {"b"}}}, peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<std::string, 2> results;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (auto& result : results) {
            const auto rows = co_await handle.query(
                "SELECT v FROM t WHERE a = ? AND b = ? AND c = ? AND d <=> ? "
                "AND e = ? AND f = ?",
                std::int64_t{42}, std::string_view("it's"), 1.5, nullptr, true, std::uint64_t{7});
            result = std::string(rows.front()["v"].value().value_or(""));
        }
    });
    RUVIA_CHECK(results[0] == "a");
    RUVIA_CHECK(results[1] == "b");
    const auto commands = peer.maria_commands();
    RUVIA_CHECK_EQ(commands.size(), std::size_t{3});
    RUVIA_CHECK(commands.front() ==
                "query:SELECT v FROM t WHERE a = 42 AND b = 'it''s' AND c = 1.5e0 AND d <=> NULL "
                "AND e = 1 AND f = 7");
    RUVIA_CHECK(commands.back() == "execute:1:i:42,s:it's,d:1.5,null,t:1,i:7");
}

// Values read in the binary protocol become the text the text protocol sends
// for them. Each expected text below was read from MariaDB 11.8's text
// protocol for the same stored value.
RUVIA_TEST(db_mariadb_prepared_reals_read_as_the_text_protocol_sends_them) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::vector<std::pair<double, std::string>> doubles{{0.1, "0.1"}, {1e20, "1e20"}, {1e15, "1e15"},
        {123456789012345.0, "123456789012345"}, {1.5e-15, "0.0000000000000015"}, {1e-5, "0.00001"},
        {-0.000123, "-0.000123"}, {1.0 / 3, "0.3333333333333333"}, {1e-14, "0.00000000000001"},
        {2.5e300, "2.5e300"}, {12345678901234567890.0, "1.2345678901234567e19"}, {1e16, "1e16"},
        // dtoa reports an overflow for infinity and NaN, which the server
        // writes as 0 (VARIANCE() of values near DBL_MAX overflows).
        {std::numeric_limits<double>::infinity(), "0"}, {-std::numeric_limits<double>::infinity(), "0"},
        {std::numeric_limits<double>::quiet_NaN(), "0"}};
    const std::vector<std::pair<float, std::string>> floats{{0.1F, "0.1"}, {3.14159265F, "3.14159"},
        {16777217.0F, "16777200"}, {-1.17549435e-38F, "-1.17549e-38"}, {3.4e38F, "3.4e38"}, {1e-5F, "0.00001"},
        {0.33333334F, "0.333333"}, {1e-7F, "0.0000001"}, {1234567.0F, "1234570"}, {1e10F, "10000000000"},
        {123456.0F, "123456"}, {2.5F, "2.5"}, {std::numeric_limits<float>::infinity(), "0"},
        {-std::numeric_limits<float>::infinity(), "0"}, {std::numeric_limits<float>::quiet_NaN(), "0"}};
    std::vector<binary_row> rows;
    std::vector<std::optional<std::string>> expected;
    for (std::size_t index = 0; index < doubles.size(); ++index) {
        rows.push_back({real(doubles[index].first), single(floats[index].first)});
        expected.emplace_back(doubles[index].second);
        expected.emplace_back(floats[index].second);
    }
    peer_type peer({{.rows_ = {"x"}},
                       {.columns_ = {{.name_ = "d", .type_ = type_double, .decimals_ = not_fixed, .length_ = 22},
                            {.name_ = "f", .type_ = type_float, .decimals_ = not_fixed, .length_ = 12}},
                           .binary_rows_ = rows}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::optional<db_rows> text_rows;
    std::optional<db_rows> prepared_rows;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        co_await twice(connected.handle({.timeout_ = std::chrono::seconds(5)}), "SELECT d, f FROM reals",
            text_rows, prepared_rows);
    });
    RUVIA_CHECK(prepared_rows.has_value());
    RUVIA_CHECK((values(*prepared_rows) == expected));
}

RUVIA_TEST(db_mariadb_prepared_values_read_as_the_text_protocol_sends_them) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::vector<column> columns{{.name_ = "dd", .type_ = type_double, .decimals_ = 6, .length_ = 20},
        {.name_ = "fd", .type_ = type_float, .decimals_ = 3, .length_ = 9},
        {.name_ = "z", .type_ = type_long, .flags_ = unsigned_flag | zerofill_flag, .length_ = 8},
        {.name_ = "dt3", .type_ = type_datetime, .decimals_ = 3, .length_ = 23},
        {.name_ = "t3", .type_ = type_time, .decimals_ = 3, .length_ = 14},
        {.name_ = "t", .type_ = type_time, .length_ = 10},
        {.name_ = "da", .type_ = type_date, .length_ = 10},
        {.name_ = "de", .type_ = type_newdecimal, .decimals_ = 10, .length_ = 32},
        {.name_ = "bu", .type_ = type_longlong, .flags_ = unsigned_flag, .length_ = 20},
        {.name_ = "b", .type_ = type_longlong, .length_ = 20},
        {.name_ = "bl", .type_ = type_blob, .flags_ = 128, .length_ = 65535},
        {.name_ = "n", .type_ = type_double, .decimals_ = not_fixed, .length_ = 22}};
    const std::vector<binary_row> rows{
        {real(1.5), single(1.5F), little(42, 4), datetime(2001, 2, 3, 4, 5, 6, 789000), time(true, 34, 22, 59, 59, 500000),
            time(false, 1, 1, 0, 0), datetime(0, 0, 0), text("-1.5000000000"), little(18446744073709551615ULL, 8),
            little(static_cast<std::uint64_t>(INT64_MIN), 8), text(std::string("\0\xff\x10", 3)), std::nullopt},
        {real(123456789.123457), single(0.1F), little(0, 4), datetime(0, 0, 0), time(true, 0, 0, 0, 0, 500000),
            time(false, 34, 22, 59, 59), datetime(2024, 2, 29), text("0.0000000001"), little(0, 8), little(7, 8),
            text(""), real(0.1)},
        {real(1e10), single(2.001F), little(99999999, 4), datetime(2038, 1, 19, 3, 14, 7, 999000),
            time(false, 0, 12, 34, 56, 1000), time(false, 0, 0, 0, 1), datetime(9999, 12, 31),
            text("12345678901234567890.0000000000"), little(1, 8), little(static_cast<std::uint64_t>(-1), 8),
            text("x"), std::nullopt}};
    const std::vector<std::optional<std::string>> expected{"1.500000", "1.500", "00000042", "2001-02-03 04:05:06.789",
        "-838:59:59.500", "25:00:00", "0000-00-00", "-1.5000000000", "18446744073709551615", "-9223372036854775808",
        std::string("\0\xff\x10", 3), std::nullopt, "123456789.123457", "0.100", "00000000", "0000-00-00 00:00:00.000",
        "-00:00:00.500", "838:59:59", "2024-02-29", "0.0000000001", "0", "7", "", "0.1", "10000000000.000000", "2.001",
        "99999999", "2038-01-19 03:14:07.999", "12:34:56.001", "00:00:01", "9999-12-31",
        "12345678901234567890.0000000000", "1", "-1", "x", std::nullopt};
    peer_type peer({{.rows_ = {"x"}}, {.columns_ = columns, .binary_rows_ = rows}}, peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::optional<db_rows> text_rows;
    std::optional<db_rows> prepared_rows;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        co_await twice(
            connected.handle({.timeout_ = std::chrono::seconds(5)}), "SELECT * FROM typed", text_rows, prepared_rows);
    });
    RUVIA_CHECK(prepared_rows.has_value());
    RUVIA_CHECK((values(*prepared_rows) == expected));
    RUVIA_CHECK((prepared_rows->front()["dt3"].value() == std::optional<std::string_view>("2001-02-03 04:05:06.789")));
}

// A statement the server will not prepare runs as text from then on.
RUVIA_TEST(db_mariadb_statement_the_server_will_not_prepare_runs_as_text) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer({{.rows_ = {"a"}},
                       {.error_ = "This command is not supported in the prepared statement protocol yet",
                           .code_ = 1295,
                           .sql_state_ = "HY000",
                           .prepare_fails_ = true},
                       {.rows_ = {"b"}}, {.rows_ = {"c"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (auto& result : outcomes) {
            co_await pipeline_test::query(handle, "SHOW v", result);
        }
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SHOW v", "prepare:SHOW v", "query:SHOW v", "query:SHOW v"}));
}

// A statement the server no longer holds is prepared and run once more; the
// handle it dropped is closed on the server.
RUVIA_TEST(db_mariadb_stale_prepared_statement_is_prepared_again_once) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    const peer_type::maria_reply unknown{
        .error_ = "Unknown prepared statement handler", .code_ = 1243, .sql_state_ = "HY000"};
    peer_type peer({{.rows_ = {"a"}}, {.rows_ = {"b"}}, unknown, {.rows_ = {"c"}}, unknown, unknown},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (auto& result : outcomes) {
            co_await pipeline_test::query(handle, "SELECT v FROM t", result);
        }
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    // Stale again on its retry: the error reaches the caller.
    RUVIA_CHECK(outcomes[3].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT v FROM t", "prepare:SELECT v FROM t", "execute:1:",
                     "execute:1:", "close:1", "prepare:SELECT v FROM t", "execute:2:", "execute:2:", "close:2",
                     "prepare:SELECT v FROM t", "execute:3:"}));
}

// A cached statement alone on its connection runs prepared; the statements
// arriving while it runs wait for its response, then go out together as
// pipelined text, since the binary protocol cannot share the connection. The
// peer holds the prepared execution's response until the others have
// arrived, so the first is still running when they do.
RUVIA_TEST(db_mariadb_cached_statements_arriving_together_run_as_pipelined_text) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer({{.rows_ = {"a"}}, {.rows_ = {"b"}}, {.rows_ = {"c"}, .hold_ = 1}, {.rows_ = {"d"}, .batch_ = 2},
                       {.rows_ = {"e"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<pipeline_test::outcome, 5> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[1]);
        task_scope together(connected.worker());
        for (std::size_t index = 2; index < outcomes.size(); ++index) {
            together.spawn(pipeline_test::query(handle, "SELECT v FROM t", outcomes[index]));
        }
        peer.release_hold(1);
        co_await together.join();
    });
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        RUVIA_CHECK(outcomes[index].value_ == std::string(1, static_cast<char>('a' + index)));
    }
    RUVIA_CHECK((peer.maria_commands() == std::vector<std::string>{"query:SELECT v FROM t", "prepare:SELECT v FROM t",
                                              "execute:1:", "execute:1:", "query:SELECT v FROM t",
                                              "query:SELECT v FROM t"}));
}

// Inside a transaction a statement is prepared on its second use as well.
RUVIA_TEST(db_mariadb_transaction_statement_runs_prepared_from_its_second_use) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer({{.column_ = "", .status_ = 3}, {.rows_ = {"a"}, .status_ = 3}, {.rows_ = {"b"}, .status_ = 3},
                       {.column_ = ""}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<std::string, 2> results;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto transaction = co_await connected.handle({.timeout_ = std::chrono::seconds(5)}).begin_transaction();
        for (auto& result : results) {
            const auto rows = co_await transaction.query("SELECT v FROM t WHERE id = ?", std::int64_t{3});
            result = std::string(rows.front()["v"].value().value_or(""));
        }
        co_await transaction.commit();
    });
    RUVIA_CHECK(results[0] == "a");
    RUVIA_CHECK(results[1] == "b");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:START TRANSACTION", "query:SELECT v FROM t WHERE id = 3",
                     "prepare:SELECT v FROM t WHERE id = ?", "execute:1:i:3", "query:COMMIT"}));
}

// The least recently used statement is closed on the server when a new one
// needs its room.
RUVIA_TEST(db_mariadb_least_recently_used_prepared_statement_is_closed_for_a_new_one) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    constexpr std::size_t statements = 129;
    std::vector<peer_type::maria_reply> replies(statements * 2, peer_type::maria_reply{.rows_ = {"v"}});
    peer_type peer(std::move(replies), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::size_t failures = 0;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (std::size_t index = 0; index < statements; ++index) {
            const auto sql = "SELECT v FROM t" + std::to_string(index);
            for (int use = 0; use < 2; ++use) {
                pipeline_test::outcome result;
                co_await pipeline_test::query(handle, sql, result);
                failures += result.value_ == "v" ? 0 : 1;
            }
        }
    });
    RUVIA_CHECK_EQ(failures, std::size_t{0});
    const auto commands = peer.maria_commands();
    RUVIA_CHECK_EQ(commands.size(), statements * 3 + 1);
    // The first statement's handle goes before the last one is prepared.
    RUVIA_CHECK(commands[commands.size() - 3] == "close:1");
    RUVIA_CHECK(commands[commands.size() - 2] == "prepare:SELECT v FROM t128");
}

// A server error numbered above the client library's range (4025, a violated
// CHECK constraint) is the prepared statement's own failure, as it is for the
// statement run as text; the connection is kept.
RUVIA_TEST(db_mariadb_prepared_server_error_above_the_client_range_is_the_statements_own) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer({{.column_ = "", .affected_rows_ = 1},
                       {.column_ = "",
                           .error_ = "CONSTRAINT `t.v` failed for `db`.`t`",
                           .code_ = 4025,
                           .sql_state_ = "23000"},
                       {.column_ = "", .affected_rows_ = 1}},
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (auto& result : outcomes) {
            co_await pipeline_test::execute(handle, "INSERT INTO t (v) VALUES (1)", result);
        }
    });
    RUVIA_CHECK_EQ(outcomes[0].affected_rows_, std::uint64_t{1});
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[1].sql_state_ == "23000");
    RUVIA_CHECK_EQ(outcomes[2].affected_rows_, std::uint64_t{1});
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:INSERT INTO t (v) VALUES (1)",
                     "prepare:INSERT INTO t (v) VALUES (1)", "execute:1:", "execute:1:"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A call runs prepared only with parameters the server reads as it reads
// their literals, and a statement is prepared for each set of parameter
// types, since the server fixes result types when it prepares. A negative
// number reads as a literal's unary minus, which binds looser than COLLATE
// and turns -? of INT64_MIN into an overflow only as a parameter; an
// unsigned value above INT64_MAX makes arithmetic unsigned only as a
// parameter (3 - ? with 5 is -2 as text, an error prepared). Those calls run
// as text, while the statement stays prepared for the others.
RUVIA_TEST(db_mariadb_parameters_read_otherwise_than_their_literals_run_as_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::string_view sql = "SELECT v FROM t WHERE a = ?";
    peer_type peer(std::vector<reply>(10, reply{.rows_ = {"v"}}), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 10> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await query_with(handle, sql, outcomes[0], std::int64_t{5});
        co_await query_with(handle, sql, outcomes[1], std::int64_t{6});
        co_await query_with(handle, sql, outcomes[2], std::int64_t{-1});
        co_await query_with(handle, sql, outcomes[3], (std::numeric_limits<std::int64_t>::min)());
        co_await query_with(handle, sql, outcomes[4], (std::numeric_limits<std::uint64_t>::max)());
        co_await query_with(handle, sql, outcomes[5], std::uint64_t{7});
        co_await query_with(handle, sql, outcomes[6], 1.5);
        co_await query_with(handle, sql, outcomes[7], 2.5);
        co_await query_with(handle, sql, outcomes[8], -2.5);
        co_await query_with(handle, sql, outcomes[9], -0.0);
    });
    for (const auto& result : outcomes) {
        RUVIA_CHECK(result.value_ == "v");
    }
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT v FROM t WHERE a = 5", "prepare:SELECT v FROM t WHERE a = ?",
                     "execute:1:i:6", "query:SELECT v FROM t WHERE a = -1",
                     "query:SELECT v FROM t WHERE a = -9223372036854775808",
                     "query:SELECT v FROM t WHERE a = 18446744073709551615", "execute:1:i:7",
                     "query:SELECT v FROM t WHERE a = 1.5e0", "prepare:SELECT v FROM t WHERE a = ?", "execute:2:d:2.5",
                     "query:SELECT v FROM t WHERE a = -2.5e0", "query:SELECT v FROM t WHERE a = -0e0"}));
}

// Where the text protocol takes only an integer literal (LIMIT, OFFSET and
// FETCH counts), it refuses a string, a real, NULL or a negative number with
// a syntax error, while a parameter there would take them: such a call runs
// as text. Parameters elsewhere in the statement are not restricted.
RUVIA_TEST(db_mariadb_integer_only_placeholders_run_prepared_only_with_integers) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::string_view limit = "SELECT v FROM t LIMIT ?";
    constexpr std::string_view filtered = "SELECT v FROM t WHERE w = ? LIMIT ?, ?";
    constexpr std::string_view fetch = "SELECT v FROM t ORDER BY v OFFSET ? ROWS FETCH FIRST ? ROWS ONLY";
    peer_type peer(std::vector<reply>(19, reply{.rows_ = {"v"}}), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 19> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        auto* result = outcomes.data();
        co_await query_with(handle, limit, *result++, std::int64_t{2});
        co_await query_with(handle, limit, *result++, std::int64_t{3});
        for (int use = 0; use < 2; ++use) {
            co_await query_with(handle, limit, *result++, std::string_view("4"));
            co_await query_with(handle, limit, *result++, 1.5);
            co_await query_with(handle, limit, *result++, nullptr);
            co_await query_with(handle, limit, *result++, true);
        }
        co_await query_with(handle, limit, *result++, std::uint64_t{5});
        for (int use = 0; use < 2; ++use) {
            co_await query_with(handle, filtered, *result++, std::string_view("x"), std::int64_t{1}, std::int64_t{2});
        }
        for (int use = 0; use < 2; ++use) {
            co_await query_with(handle, filtered, *result++, std::string_view("x"), std::string_view("1"), std::int64_t{2});
        }
        for (int use = 0; use < 2; ++use) {
            co_await query_with(handle, fetch, *result++, std::int64_t{1}, std::int64_t{2});
        }
        for (int use = 0; use < 2; ++use) {
            co_await query_with(handle, fetch, *result++, std::int64_t{1}, 2.0);
        }
    });
    for (const auto& result : outcomes) {
        RUVIA_CHECK(result.value_ == "v");
    }
    const std::vector<std::string> expected{"query:SELECT v FROM t LIMIT 2", "prepare:SELECT v FROM t LIMIT ?",
        "execute:1:i:3", "query:SELECT v FROM t LIMIT '4'", "query:SELECT v FROM t LIMIT 1.5e0",
        "query:SELECT v FROM t LIMIT NULL", "query:SELECT v FROM t LIMIT 1", "query:SELECT v FROM t LIMIT '4'",
        "query:SELECT v FROM t LIMIT 1.5e0", "query:SELECT v FROM t LIMIT NULL", "query:SELECT v FROM t LIMIT 1",
        "execute:1:i:5", "query:SELECT v FROM t WHERE w = 'x' LIMIT 1, 2",
        "prepare:SELECT v FROM t WHERE w = ? LIMIT ?, ?", "execute:2:s:x,i:1,i:2",
        "query:SELECT v FROM t WHERE w = 'x' LIMIT '1', 2", "query:SELECT v FROM t WHERE w = 'x' LIMIT '1', 2",
        "query:SELECT v FROM t ORDER BY v OFFSET 1 ROWS FETCH FIRST 2 ROWS ONLY",
        "prepare:SELECT v FROM t ORDER BY v OFFSET ? ROWS FETCH FIRST ? ROWS ONLY", "execute:3:i:1,i:2",
        "query:SELECT v FROM t ORDER BY v OFFSET 1 ROWS FETCH FIRST 2e0 ROWS ONLY",
        "query:SELECT v FROM t ORDER BY v OFFSET 1 ROWS FETCH FIRST 2e0 ROWS ONLY"};
    RUVIA_CHECK((peer.maria_commands() == expected));
}

// Statements the binary protocol cannot run as the text protocol does are
// never prepared, however they are written: a CALL (its OUT parameters come
// back as an extra result set only when prepared) also behind a comment, a
// statement with RETURNING (a re-run after its result changed shape would
// repeat its writes), one led by an executable comment, whose content the
// server runs, and a LOAD. A statement in parentheses is prepared as usual.
RUVIA_TEST(db_mariadb_statements_the_binary_protocol_runs_otherwise_are_never_prepared) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::array<std::string_view, 4> text_only{"/* note */ CALL p()",
        "INSERT INTO t (v) VALUES (1) RETURNING v", "/*!100000 SELECT v FROM t */", "LOAD XML INFILE 'x' INTO TABLE t"};
    // The tenth answers the read-back of the tracked variables after the
    // CALL and the executable comment.
    std::vector<reply> replies(12, reply{.rows_ = {"v"}});
    replies[9] = {.rows_ = {"*"}};
    peer_type peer(std::move(replies), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 11> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        auto* result = outcomes.data();
        for (const auto sql : text_only) {
            for (int use = 0; use < 2; ++use) {
                co_await pipeline_test::query(handle, sql, *result++);
            }
        }
        for (int use = 0; use < 3; ++use) {
            co_await pipeline_test::query(handle, "(SELECT v FROM t)", *result++);
        }
    });
    for (const auto& result : outcomes) {
        RUVIA_CHECK(result.value_ == "v");
    }
    std::vector<std::string> expected;
    for (const auto sql : text_only) {
        expected.insert(expected.end(), 2, "query:" + std::string(sql));
    }
    expected.insert(expected.end(), {"query:(SELECT v FROM t)", "query:SELECT @@session_track_system_variables",
                                        "prepare:(SELECT v FROM t)", "execute:1:", "execute:1:"});
    RUVIA_CHECK((peer.maria_commands() == expected));
}

// Values of a column without a fixed scale read as the text protocol sends
// them: a DATETIME or TIME of AUTO_SEC_PART_DIGITS shows six fractional
// digits when the value has a fraction and none otherwise; a non-finite
// DOUBLE, also of a fixed scale, is written as 0.
RUVIA_TEST(db_mariadb_prepared_values_without_a_fixed_scale_read_as_the_text_protocol_sends_them) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::uint8_t auto_digits = 39;
    const std::vector<column> columns{{.name_ = "dd", .type_ = type_double, .decimals_ = 6, .length_ = 20},
        {.name_ = "dt", .type_ = type_datetime, .decimals_ = auto_digits, .length_ = 26},
        {.name_ = "t", .type_ = type_time, .decimals_ = auto_digits, .length_ = 17}};
    const std::vector<binary_row> rows{
        {real(std::numeric_limits<double>::infinity()), datetime(1970, 1, 1, 0, 0, 1, 500000),
            time(false, 0, 0, 0, 1, 500000)},
        {real(std::numeric_limits<double>::quiet_NaN()), datetime(1970, 1, 1, 0, 0, 1), time(false, 0, 1, 2, 3)},
        {real(-std::numeric_limits<double>::infinity()), datetime(2020, 1, 1, 10, 0, 0, 10),
            time(true, 1, 10, 0, 0, 250000)}};
    const std::vector<std::optional<std::string>> expected{"0", "1970-01-01 00:00:01.500000", "00:00:01.500000", "0",
        "1970-01-01 00:00:01", "01:02:03", "0", "2020-01-01 10:00:00.000010", "-34:00:00.250000"};
    peer_type peer({{.rows_ = {"x"}}, {.columns_ = columns, .binary_rows_ = rows}}, peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::optional<db_rows> text_rows;
    std::optional<db_rows> prepared_rows;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        co_await twice(
            connected.handle({.timeout_ = std::chrono::seconds(5)}), "SELECT * FROM auto", text_rows, prepared_rows);
    });
    RUVIA_CHECK(prepared_rows.has_value());
    RUVIA_CHECK((values(*prepared_rows) == expected));
}

// The text protocol pads a YEAR column's value to four digits when it comes
// from a table column and not when it comes from an expression, under the
// same metadata without ZEROFILL, and the metadata of the prepare is not that
// of the execution: a UNION of YEAR columns is prepared with ZEROFILL and
// runs without it. A statement whose prepared result holds a YEAR runs as
// text.
RUVIA_TEST(db_mariadb_statement_with_a_year_column_runs_as_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::vector<column> padded{
        {.name_ = "y", .type_ = type_year, .flags_ = unsigned_flag | zerofill_flag, .length_ = 4}};
    const std::vector<column> plain{{.name_ = "y", .type_ = type_year, .flags_ = unsigned_flag, .length_ = 4}};
    peer_type peer({{.rows_ = {"0000"}},
                       {.rows_ = {"0000"}, .columns_ = plain, .binary_rows_ = {{little(0, 2)}}, .prepare_columns_ = padded},
                       {.rows_ = {"0000"}}, {.rows_ = {"0"}}, {.rows_ = {"0"}, .columns_ = plain}, {.rows_ = {"0"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 6> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (std::size_t index = 0; index < 3; ++index) {
            co_await query_with(handle, "SELECT y FROM t UNION ALL SELECT y FROM u", outcomes[index]);
        }
        for (std::size_t index = 3; index < 6; ++index) {
            co_await query_with(handle, "SELECT MIN(y) AS y FROM t", outcomes[index]);
        }
    });
    for (const std::size_t index : {0, 1, 2}) {
        RUVIA_CHECK(outcomes[index].value_ == "0000");
    }
    for (const std::size_t index : {3, 4, 5}) {
        RUVIA_CHECK(outcomes[index].value_ == "0");
    }
    RUVIA_CHECK((peer.maria_commands() == std::vector<std::string>{"query:SELECT y FROM t UNION ALL SELECT y FROM u",
                                              "prepare:SELECT y FROM t UNION ALL SELECT y FROM u",
                                              "query:SELECT y FROM t UNION ALL SELECT y FROM u",
                                              "query:SELECT y FROM t UNION ALL SELECT y FROM u",
                                              "query:SELECT MIN(y) AS y FROM t", "close:1",
                                              "prepare:SELECT MIN(y) AS y FROM t", "query:SELECT MIN(y) AS y FROM t",
                                              "query:SELECT MIN(y) AS y FROM t"}));
}

// A YEAR the prepare did not describe (the column was altered to YEAR since,
// and the server prepared the statement anew inside the execution): one
// with ZEROFILL reads padded, as the text protocol sends it; one without it
// cannot be told from its binary value, and the server has run the statement
// already, so it fails once and runs as text from its next use.
RUVIA_TEST(db_mariadb_year_column_appearing_at_execution_reads_padded_or_fails_once) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::vector<column> number{{.name_ = "y", .type_ = type_long, .length_ = 11}};
    const std::vector<column> padded{
        {.name_ = "y", .type_ = type_year, .flags_ = unsigned_flag | zerofill_flag, .length_ = 4}};
    const std::vector<column> plain{{.name_ = "y", .type_ = type_year, .flags_ = unsigned_flag, .length_ = 4}};
    peer_type peer({{.rows_ = {"x"}},
                       {.columns_ = padded,
                           .binary_rows_ = {{little(0, 2)}, {little(2155, 2)}},
                           .prepare_columns_ = number},
                       {.rows_ = {"y"}},
                       {.columns_ = plain, .binary_rows_ = {{little(0, 2)}}, .prepare_columns_ = number},
                       {.rows_ = {"0"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::optional<db_rows> padded_rows;
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        std::optional<db_rows> text_rows;
        co_await twice(handle, "SELECT y FROM t", text_rows, padded_rows);
        co_await query_with(handle, "SELECT y FROM u", outcomes[0]);
        co_await query_with(handle, "SELECT y FROM u", outcomes[1]);
        co_await query_with(handle, "SELECT y FROM u", outcomes[2]);
    });
    RUVIA_CHECK(padded_rows.has_value());
    RUVIA_CHECK((values(*padded_rows) == std::vector<std::optional<std::string>>{"0000", "2155"}));
    RUVIA_CHECK(outcomes[0].value_ == "y");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[2].value_ == "0");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT y FROM t", "prepare:SELECT y FROM t", "execute:1:",
                     "query:SELECT y FROM u", "prepare:SELECT y FROM u", "execute:2:", "query:SELECT y FROM u"}));
}

// A table that gained a column since the statement was prepared: the server
// prepares it anew, runs it, and its result has a column the handle was not
// prepared for (CR_NEW_STMT_METADATA). The server has run it, and running it
// again would repeat what it did (NEXTVAL() in a SELECT): the statement fails
// once, as a statement, with its result read past, and is prepared anew on
// its next use.
RUVIA_TEST(db_mariadb_statement_whose_result_changed_shape_fails_once_and_is_prepared_anew) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::vector<column> one{{.name_ = "v"}};
    const std::vector<column> two{{.name_ = "v"}, {.name_ = "w"}};
    peer_type peer({{.rows_ = {"1"}}, {.columns_ = one, .binary_rows_ = {{text("1")}}},
                       {.columns_ = two, .binary_rows_ = {{text("2"), text("2")}, {text("3"), text("4")}}},
                       {.columns_ = two, .binary_rows_ = {{text("3"), text("2")}}}},
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (auto& result : outcomes) {
            co_await query_with(handle, "SELECT NEXTVAL(s), t.* FROM t", result);
        }
    });
    RUVIA_CHECK(outcomes[0].value_ == "1");
    RUVIA_CHECK(outcomes[1].value_ == "1");
    RUVIA_CHECK(outcomes[2].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[2].value_.empty());
    RUVIA_CHECK(outcomes[3].value_ == "3|2");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT NEXTVAL(s), t.* FROM t", "prepare:SELECT NEXTVAL(s), t.* FROM t",
                     "execute:1:", "execute:1:", "close:1", "prepare:SELECT NEXTVAL(s), t.* FROM t", "execute:2:"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// Inside a transaction the statement whose result changed shape fails as a
// statement, not as a lost connection: the transaction rolls back on the
// same connection, and the statement is prepared anew on its next use.
RUVIA_TEST(db_mariadb_statement_whose_result_changed_shape_fails_inside_a_transaction) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::vector<column> one{{.name_ = "v"}};
    const std::vector<column> two{{.name_ = "v"}, {.name_ = "w"}};
    peer_type peer({{.column_ = "", .status_ = 3}, {.rows_ = {"1"}, .status_ = 3},
                       {.status_ = 3, .columns_ = one, .binary_rows_ = {{text("1")}}},
                       {.status_ = 3, .columns_ = two, .binary_rows_ = {{text("1"), text("2")}}},
                       {.column_ = ""}, {.rows_ = {"after"}}},
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::vector<std::string> results;
    std::optional<db_error::code_type> failure;
    pipeline_test::outcome after;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        try {
            auto transaction = co_await handle.begin_transaction();
            for (int use = 0; use < 3; ++use) {
                results.push_back(first_row(co_await transaction.query("SELECT * FROM t")));
            }
            co_await transaction.commit();
        } catch (const db_error& error) {
            failure = error.code();
        }
        co_await pipeline_test::query(handle, "SELECT * FROM t", after);
    });
    RUVIA_CHECK((results == std::vector<std::string>{"1", "1"}));
    RUVIA_CHECK(failure == db_error::code_type::statement_failed);
    RUVIA_CHECK(after.value_ == "after");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:START TRANSACTION", "query:SELECT * FROM t", "prepare:SELECT * FROM t",
                     "execute:1:", "execute:1:", "query:ROLLBACK", "close:1", "prepare:SELECT * FROM t",
                     "execute:2:"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A prepare whose command is still being written when its statement is
// stopped is finished by the next statement on the connection, from the
// statement text the connection's cache owns: the stopped caller's text is
// gone by then. The peer reads nothing until the caller has left, so the
// write is suspended when it does.
RUVIA_TEST(db_mariadb_abandoned_prepare_is_finished_from_the_cached_statement_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    std::string sql = "SELECT v FROM t /* ";
    // More than the socket buffers of both ends hold.
    sql.append(std::size_t{15} * 1024 * 1024, 'x');
    sql.append(" */");
    peer_type peer({{.rows_ = {"text"}}, {.rows_ = {"second"}, .read_hold_ = 1}, {.rows_ = {"third"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(20)});
        co_await pipeline_test::query(handle, sql, outcomes[0]);
        stop_source stop;
        const auto stoppable = handle.with_options({.stop_token_ = stop.token()});
        task_scope first(connected.worker());
        task_scope second(connected.worker());
        first.spawn(pipeline_test::query(stoppable, sql, outcomes[1]));
        second.spawn(pipeline_test::query(handle, "SELECT v FROM t", outcomes[2]));
        stop.request_stop();
        co_await first.join();
        peer.release_hold(1);
        co_await second.join();
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[3]);
    });
    RUVIA_CHECK(outcomes[0].value_ == "text");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::cancelled);
    RUVIA_CHECK(outcomes[2].value_ == "second");
    RUVIA_CHECK(outcomes[3].value_ == "third");
    const auto commands = peer.maria_commands();
    RUVIA_CHECK_EQ(commands.size(), std::size_t{5});
    if (commands.size() == 5) {
        RUVIA_CHECK(commands[1] == "prepare:" + sql);
        RUVIA_CHECK((std::vector<std::string>(commands.begin() + 2, commands.end()) ==
                     std::vector<std::string>{"query:SELECT v FROM t", "prepare:SELECT v FROM t", "execute:2:"}));
    }
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A prepared statement whose rows end in an error (a DOUBLE out of range)
// fails alone and leaves nothing pending: it is reset to its prepared state,
// and another statement found stale afterwards is still closed on the server
// and prepared anew.
RUVIA_TEST(db_mariadb_prepared_statement_failing_in_its_rows_leaves_the_others_usable) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const reply unknown{.error_ = "Unknown prepared statement handler", .code_ = 1243, .sql_state_ = "HY000"};
    peer_type peer({{.rows_ = {"1"}},
                       {.rows_ = {"2"},
                           .error_ = "DOUBLE value is out of range",
                           .code_ = 1690,
                           .sql_state_ = "22003",
                           .error_after_rows_ = true},
                       {.rows_ = {"b"}}, {.rows_ = {"b"}}, unknown, {.rows_ = {"b"}}},
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 5> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query(handle, "SELECT v * 3 FROM t", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT v * 3 FROM t", outcomes[1]);
        for (std::size_t index = 2; index < outcomes.size(); ++index) {
            co_await pipeline_test::query(handle, "SELECT v FROM u", outcomes[index]);
        }
    });
    RUVIA_CHECK(outcomes[0].value_ == "1");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[1].sql_state_ == "22003");
    RUVIA_CHECK(outcomes[2].value_ == "b");
    RUVIA_CHECK(outcomes[3].value_ == "b");
    RUVIA_CHECK(outcomes[4].value_ == "b");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT v * 3 FROM t", "prepare:SELECT v * 3 FROM t", "execute:1:",
                     "reset:1", "query:SELECT v FROM u", "prepare:SELECT v FROM u", "execute:2:", "execute:2:",
                     "close:2", "prepare:SELECT v FROM u", "execute:3:"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// The same prepared statement runs again after its rows failed (1242, a
// subquery returning more than one row): its next execution goes out and
// reads its own response, and the statements behind it get theirs, on the
// same connection.
RUVIA_TEST(db_mariadb_prepared_statement_runs_again_after_its_rows_failed) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer({{.rows_ = {"1"}},
                       {.rows_ = {"2"},
                           .error_ = "Subquery returns more than 1 row",
                           .code_ = 1242,
                           .sql_state_ = "21000",
                           .error_after_rows_ = true},
                       {.rows_ = {"3"}}, {.rows_ = {"f1"}}, {.rows_ = {"f2"}}},
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 5> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query(handle, "SELECT (SELECT v FROM t)", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT (SELECT v FROM t)", outcomes[1]);
        task_scope together(connected.worker());
        together.spawn(pipeline_test::query(handle, "SELECT (SELECT v FROM t)", outcomes[2]));
        together.spawn(pipeline_test::query(handle, "SELECT v FROM f1", outcomes[3]));
        together.spawn(pipeline_test::query(handle, "SELECT v FROM f2", outcomes[4]));
        co_await together.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "1");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[1].sql_state_ == "21000");
    RUVIA_CHECK(outcomes[2].value_ == "3");
    RUVIA_CHECK(outcomes[3].value_ == "f1");
    RUVIA_CHECK(outcomes[4].value_ == "f2");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT (SELECT v FROM t)", "prepare:SELECT (SELECT v FROM t)",
                     "execute:1:", "reset:1", "execute:1:", "query:SELECT v FROM f1", "query:SELECT v FROM f2"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A cached statement keeps the session state it was prepared in, so a change
// the session reports (here sql_mode: PIPES_AS_CONCAT, ANSI_QUOTES) drops
// every cached statement: it is closed on the server, runs as text, and is
// prepared anew under the new state on its second use after the change.
RUVIA_TEST(db_mariadb_session_change_drops_the_cached_statements) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer(
        {{.rows_ = {"a"}}, {.rows_ = {"b"}},
            {.column_ = "", .tracked_ = {{"sql_mode", "PIPES_AS_CONCAT,ANSI_QUOTES,STRICT_TRANS_TABLES"}}},
            {.rows_ = {"c"}}, {.rows_ = {"d"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 5> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query(handle, "SELECT 'a' || 'b' AS v", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT 'a' || 'b' AS v", outcomes[1]);
        co_await pipeline_test::execute(
            handle, "SET SESSION sql_mode = CONCAT(@@sql_mode, ',PIPES_AS_CONCAT,ANSI_QUOTES')", outcomes[2]);
        co_await pipeline_test::query(handle, "SELECT 'a' || 'b' AS v", outcomes[3]);
        co_await pipeline_test::query(handle, "SELECT 'a' || 'b' AS v", outcomes[4]);
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(!outcomes[2].error_.has_value());
    RUVIA_CHECK(outcomes[3].value_ == "c");
    RUVIA_CHECK(outcomes[4].value_ == "d");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT 'a' || 'b' AS v", "prepare:SELECT 'a' || 'b' AS v",
                     "execute:1:", "query:SET SESSION sql_mode = CONCAT(@@sql_mode, ',PIPES_AS_CONCAT,ANSI_QUOTES')",
                     "query:SELECT 'a' || 'b' AS v", "close:1", "prepare:SELECT 'a' || 'b' AS v", "execute:2:"}));
    RUVIA_CHECK_EQ(peer.maria_session_setups(), std::size_t{1});
}

// While the session's client character set is not the utf8mb4 the
// connection opened with, nothing runs prepared: the binary protocol converts
// parameters otherwise than the text protocol does literals. Back on utf8mb4,
// statements are prepared again.
RUVIA_TEST(db_mariadb_statements_run_as_text_while_the_client_character_set_differs) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::vector<std::pair<std::string, std::string>> gbk{
        {"character_set_client", "gbk"}, {"character_set_connection", "gbk"}, {"character_set_results", "gbk"}};
    const std::vector<std::pair<std::string, std::string>> utf8mb4{{"character_set_client", "utf8mb4"},
        {"character_set_connection", "utf8mb4"}, {"character_set_results", "utf8mb4"}};
    std::vector<reply> replies{{.column_ = "", .tracked_ = gbk}};
    replies.insert(replies.end(), 3, reply{.rows_ = {"v"}});
    replies.push_back({.column_ = "", .tracked_ = utf8mb4});
    replies.insert(replies.end(), 2, reply{.rows_ = {"v"}});
    peer_type peer(std::move(replies), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 7> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::execute(handle, "SET NAMES gbk", outcomes[0]);
        for (std::size_t index = 1; index < 4; ++index) {
            co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[index]);
        }
        co_await pipeline_test::execute(handle, "SET NAMES utf8mb4", outcomes[4]);
        for (std::size_t index = 5; index < 7; ++index) {
            co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[index]);
        }
    });
    for (const std::size_t index : {1, 2, 3, 5, 6}) {
        RUVIA_CHECK(outcomes[index].value_ == "v");
    }
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SET NAMES gbk", "query:SELECT v FROM t", "query:SELECT v FROM t",
                     "query:SELECT v FROM t", "query:SET NAMES utf8mb4", "query:SELECT v FROM t",
                     "prepare:SELECT v FROM t", "execute:1:"}));
}

// SET and USE change the session every cached statement was prepared in, so
// they run as text however often they are used; the change a SET reports
// drops the cached statements as any other does.
RUVIA_TEST(db_mariadb_set_and_use_run_as_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer({{.rows_ = {"a"}}, {.rows_ = {"b"}}, {.column_ = ""}, {.column_ = ""}, {.column_ = ""},
                       {.column_ = "", .tracked_ = {{"sql_mode", "PIPES_AS_CONCAT"}}}, {.rows_ = {"c"}},
                       {.rows_ = {"d"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 8> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[1]);
        co_await pipeline_test::execute(handle, "USE other", outcomes[2]);
        co_await pipeline_test::execute(handle, "USE other", outcomes[3]);
        co_await execute_with(handle, "SET SESSION sql_mode = ?", outcomes[4], std::string_view("STRICT_TRANS_TABLES"));
        co_await execute_with(handle, "SET SESSION sql_mode = ?", outcomes[5], std::string_view("PIPES_AS_CONCAT"));
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[6]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[7]);
    });
    for (std::size_t index = 2; index < 6; ++index) {
        RUVIA_CHECK(!outcomes[index].error_.has_value());
    }
    RUVIA_CHECK(outcomes[6].value_ == "c");
    RUVIA_CHECK(outcomes[7].value_ == "d");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT v FROM t", "prepare:SELECT v FROM t", "execute:1:",
                     "query:USE other", "query:USE other", "query:SET SESSION sql_mode = 'STRICT_TRANS_TABLES'",
                     "query:SET SESSION sql_mode = 'PIPES_AS_CONCAT'", "query:SELECT v FROM t", "close:1",
                     "prepare:SELECT v FROM t", "execute:2:"}));
}

// Either of character_set_client and character_set_connection alone off
// utf8mb4 keeps every statement on text: the binary protocol converts a
// parameter's invalid bytes otherwise than the text protocol does a literal's.
RUVIA_TEST(db_mariadb_one_character_set_variable_off_utf8mb4_runs_statements_as_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::string invalid(
        "a\xff"
        "b");
    for (const std::string variable : {"character_set_client", "character_set_connection"}) {
        std::vector<reply> replies{{.rows_ = {"a"}}, {.rows_ = {"b"}}, {.column_ = "", .tracked_ = {{variable, "gbk"}}}};
        replies.insert(replies.end(), 3, reply{.rows_ = {"x"}});
        replies.push_back({.column_ = "", .tracked_ = {{variable, "utf8mb4"}}});
        replies.insert(replies.end(), 2, reply{.rows_ = {"y"}});
        peer_type peer(std::move(replies), peer_type::after_startup::idle);
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), config(peer));
        std::array<pipeline_test::outcome, 5> outcomes;
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
            pipeline_test::outcome ignored;
            co_await pipeline_test::query(handle, "SELECT v FROM t", ignored);
            co_await pipeline_test::query(handle, "SELECT v FROM t", ignored);
            co_await pipeline_test::execute(handle, "SET SESSION " + variable + " = gbk", ignored);
            for (std::size_t index = 0; index < 3; ++index) {
                co_await query_with(handle, "SELECT ? AS v", outcomes[index], std::string_view(invalid));
            }
            co_await pipeline_test::execute(handle, "SET SESSION " + variable + " = utf8mb4", ignored);
            for (std::size_t index = 3; index < 5; ++index) {
                co_await query_with(handle, "SELECT ? AS v", outcomes[index], std::string_view(invalid));
            }
        });
        for (std::size_t index = 0; index < 3; ++index) {
            RUVIA_CHECK(outcomes[index].value_ == "x");
        }
        RUVIA_CHECK(outcomes[3].value_ == "y");
        RUVIA_CHECK(outcomes[4].value_ == "y");
        const auto text =
            "query:SELECT 'a\xff"
            "b' AS v";
        RUVIA_CHECK((peer.maria_commands() ==
                     std::vector<std::string>{"query:SELECT v FROM t", "prepare:SELECT v FROM t", "execute:1:",
                         "query:SET SESSION " + variable + " = gbk", text, text, text,
                         "query:SET SESSION " + variable + " = utf8mb4", text, "close:1", "prepare:SELECT ? AS v",
                         "execute:2:s:a\xff"
                         "b"}));
    }
}

// A server that does not track its session, or a session whose tracking a
// statement turned off (the server does not report that change), runs nothing
// prepared: a later change of its state could not be heard.
RUVIA_TEST(db_mariadb_untracked_session_runs_nothing_prepared) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    {
        peer_type peer(std::vector<reply>(3, reply{.rows_ = {"v"}}), peer_type::after_startup::idle, 1, false);
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), config(peer));
        std::array<pipeline_test::outcome, 3> outcomes;
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
            for (auto& result : outcomes) {
                co_await pipeline_test::query(handle, "SELECT v FROM t", result);
            }
        });
        for (const auto& result : outcomes) {
            RUVIA_CHECK(result.value_ == "v");
        }
        RUVIA_CHECK(peer.maria_session_setups() == 0);
        RUVIA_CHECK((peer.maria_commands() == std::vector<std::string>(3, "query:SELECT v FROM t")));
    }
    {
        std::vector<reply> replies{{.rows_ = {"v"}}, {.rows_ = {"v"}}, {.column_ = ""}};
        replies.insert(replies.end(), 2, reply{.rows_ = {"v"}});
        peer_type peer(std::move(replies), peer_type::after_startup::idle);
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), config(peer));
        std::array<pipeline_test::outcome, 5> outcomes;
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
            co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[0]);
            co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[1]);
            co_await pipeline_test::execute(handle, "SET SESSION session_track_system_variables = ''", outcomes[2]);
            co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[3]);
            co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[4]);
        });
        RUVIA_CHECK(!outcomes[2].error_.has_value());
        RUVIA_CHECK(outcomes[3].value_ == "v");
        RUVIA_CHECK(outcomes[4].value_ == "v");
        RUVIA_CHECK((peer.maria_commands() ==
                     std::vector<std::string>{"query:SELECT v FROM t", "prepare:SELECT v FROM t", "execute:1:",
                         "query:SET SESSION session_track_system_variables = ''", "query:SELECT v FROM t",
                         "query:SELECT v FROM t"}));
    }
}

// A statement that never runs prepared is remembered as such without taking
// one of the cache's places, also on its second use: a full cache of
// prepared statements outlives any number of CALLs, none evicted or closed.
// The first prepared execution after them reads the tracked variables back,
// since a procedure may have changed them unreported.
RUVIA_TEST(db_mariadb_statements_that_run_as_text_take_no_cache_places) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::size_t cached = 128;
    constexpr std::size_t calls = 130;
    std::vector<reply> replies(cached * 2 + calls * 2 + 1 + cached, reply{.rows_ = {"v"}});
    replies[cached * 2 + calls * 2] = {.rows_ = {std::string(server_tracked_list)}};
    peer_type peer(std::move(replies), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::size_t failures = 0;
    const auto select = [](std::size_t index) { return "SELECT v FROM t" + std::to_string(index); };
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        const auto run = [&](std::string sql) -> task<void> {
            pipeline_test::outcome result;
            co_await pipeline_test::query(handle, sql, result);
            failures += result.value_ == "v" ? 0 : 1;
        };
        for (std::size_t index = 0; index < cached; ++index) {
            co_await run(select(index));
            co_await run(select(index));
        }
        for (std::size_t index = 0; index < calls; ++index) {
            co_await run("CALL p" + std::to_string(index) + "()");
            co_await run("CALL p" + std::to_string(index) + "()");
        }
        for (std::size_t index = 0; index < cached; ++index) {
            co_await run(select(index));
        }
    });
    RUVIA_CHECK_EQ(failures, std::size_t{0});
    const auto commands = peer.maria_commands();
    RUVIA_CHECK_EQ(commands.size(), std::size_t{cached * 3 + calls * 2 + 1 + cached});
    RUVIA_CHECK(std::ranges::none_of(commands, [](const std::string& command) { return command.starts_with("close:"); }));
    RUVIA_CHECK(commands[cached * 3 + calls * 2] == "query:SELECT @@session_track_system_variables");
    std::size_t unexpected = 0;
    for (std::size_t index = 0; index < cached; ++index) {
        unexpected += commands[cached * 3 + calls * 2 + 1 + index] == "execute:" + std::to_string(index + 1) + ":" ? 0 : 1;
    }
    RUVIA_CHECK_EQ(unexpected, std::size_t{0});
}

// Reports of variables no statement depends on leave the cache alone: with
// binary logging on, the server reports last_gtid after every committed
// write, and a cache dropped on each would close and prepare its statements
// anew all the time.
RUVIA_TEST(db_mariadb_reports_of_other_variables_keep_the_cached_statements) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer({{.rows_ = {"a"}}, {.rows_ = {"b"}},
                       {.column_ = "", .affected_rows_ = 1, .tracked_ = {{"last_gtid", "0-1-5"}}},
                       {.column_ = "", .tracked_ = {{"autocommit", "OFF"}}}, {.rows_ = {"c"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 5> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[1]);
        co_await pipeline_test::execute(handle, "INSERT INTO t (v) VALUES ('w')", outcomes[2]);
        co_await pipeline_test::execute(handle, "SET autocommit = 0", outcomes[3]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[4]);
    });
    RUVIA_CHECK(outcomes[2].affected_rows_ == 1);
    RUVIA_CHECK(outcomes[4].value_ == "c");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT v FROM t", "prepare:SELECT v FROM t", "execute:1:",
                     "query:INSERT INTO t (v) VALUES ('w')", "query:SET autocommit = 0", "execute:1:"}));
}

// A stored procedure may turn session tracking off without a report, so the
// first prepared execution after a CALL reads the tracked variables back:
// still tracked, the cached statement runs; no longer, every statement runs
// as text on that connection.
RUVIA_TEST(db_mariadb_call_has_the_tracked_variables_read_back) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer({{.rows_ = {"a"}}, {.rows_ = {"b"}}, {.column_ = ""}, {.rows_ = {std::string(server_tracked_list)}},
                       {.rows_ = {"c"}}, {.column_ = ""}, {.rows_ = {"time_zone"}}, {.rows_ = {"d"}}, {.rows_ = {"e"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 7> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[1]);
        co_await pipeline_test::execute(handle, "CALL p()", outcomes[2]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[3]);
        co_await pipeline_test::execute(handle, "CALL p()", outcomes[4]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[5]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[6]);
    });
    RUVIA_CHECK(outcomes[3].value_ == "c");
    RUVIA_CHECK(outcomes[5].value_ == "d");
    RUVIA_CHECK(outcomes[6].value_ == "e");
    RUVIA_CHECK((peer.maria_commands() ==
                 std::vector<std::string>{"query:SELECT v FROM t", "prepare:SELECT v FROM t", "execute:1:",
                     "query:CALL p()", "query:SELECT @@session_track_system_variables", "execute:1:", "query:CALL p()",
                     "query:SELECT @@session_track_system_variables", "query:SELECT v FROM t",
                     "query:SELECT v FROM t"}));
}
#endif
