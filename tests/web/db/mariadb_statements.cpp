#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "pipeline_support.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_MARIADB
namespace mariadb_statement_test {

using pipeline_test::peer_type;
using reply = peer_type::maria_reply;
using column = peer_type::maria_column;
using binary_row = std::pmr::vector<std::optional<std::pmr::string>>;

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

[[nodiscard]] inline std::pmr::string little(std::uint64_t value, int width) {
    std::pmr::string bytes{ruvia::process_resource()};
    for (int index = 0; index < width; ++index) {
        bytes.push_back(static_cast<char>((value >> (8 * index)) & 255));
    }
    return bytes;
}

[[nodiscard]] inline std::pmr::string real(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return little(bits, 8);
}

[[nodiscard]] inline std::pmr::string single(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return little(bits, 4);
}

[[nodiscard]] inline std::pmr::string text(std::string_view value) {
    std::pmr::string bytes(1, static_cast<char>(value.size()), ruvia::process_resource());
    bytes.append(value);
    return bytes;
}

// DATE, DATETIME and TIMESTAMP: a length, then year, month, day, hour,
// minute, second and microseconds as far as they are not zero.
[[nodiscard]] inline std::pmr::string datetime(unsigned year, unsigned month, unsigned day, unsigned hour = 0,
    unsigned minute = 0, unsigned second = 0, unsigned microsecond = 0) {
    if (year == 0 && month == 0 && day == 0 && hour == 0 && minute == 0 && second == 0 && microsecond == 0) {
        return std::pmr::string(1, '\0', ruvia::process_resource());
    }
    std::pmr::string bytes(little(year, 2) + static_cast<char>(month) + static_cast<char>(day), ruvia::process_resource());
    if (hour != 0 || minute != 0 || second != 0 || microsecond != 0) {
        bytes += std::pmr::string({static_cast<char>(hour), static_cast<char>(minute), static_cast<char>(second)}, ruvia::process_resource());
        if (microsecond != 0) {
            bytes += little(microsecond, 4);
        }
    }
    return static_cast<char>(bytes.size()) + bytes;
}

// TIME: a length, then the sign, days, hours, minutes, seconds and
// microseconds.
[[nodiscard]] inline std::pmr::string time(
    bool negative, unsigned days, unsigned hour, unsigned minute, unsigned second, unsigned microsecond = 0) {
    std::pmr::string bytes(1, negative ? '\1' : '\0', ruvia::process_resource());
    bytes += little(days, 4);
    bytes += std::pmr::string({static_cast<char>(hour), static_cast<char>(minute), static_cast<char>(second)}, ruvia::process_resource());
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

[[nodiscard]] inline std::pmr::vector<std::optional<std::pmr::string>> values(const ruvia::db_rows& rows) {
    std::pmr::vector<std::optional<std::pmr::string>> result{ruvia::process_resource()};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (std::size_t field = 0; field < rows[row].size(); ++field) {
            const auto value = rows[row][field].value();
            result.push_back(value.has_value() ? std::optional<std::pmr::string>(std::pmr::string(*value, ruvia::process_resource())) : std::nullopt);
        }
    }
    return result;
}

// The first row's fields, joined by '|'.
[[nodiscard]] inline std::pmr::string first_row(const ruvia::db_rows& rows) {
    if (rows.empty()) {
        return std::pmr::string("<no rows>", ruvia::process_resource());
    }
    std::pmr::string joined{ruvia::process_resource()};
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
TEST(Ruvia, db_mariadb_repeated_statement_runs_prepared_from_its_second_use) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}}, ruvia::process_resource()), peer_type::after_startup::idle);
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(outcomes[2].value_ == "c");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t WHERE id = 7", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t WHERE id = ?", ruvia::process_resource()),
                                                        std::pmr::string("execute:1:i:7", ruvia::process_resource()), std::pmr::string("execute:1:i:7", ruvia::process_resource())},
                     ruvia::process_resource())));
}

// Parameters travel in the binary protocol with the types their literals
// read as: an unsigned integer within INT64_MAX is a signed BIGINT, as its
// literal is.
TEST(Ruvia, db_mariadb_prepared_parameters_keep_their_types) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}}, ruvia::process_resource()), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<std::pmr::string, 2> results{std::pmr::string(ruvia::process_resource()), std::pmr::string(ruvia::process_resource())};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (auto& result : results) {
            const auto rows = co_await handle.query(
                "SELECT v FROM t WHERE a = ? AND b = ? AND c = ? AND d <=> ? "
                "AND e = ? AND f = ?",
                std::int64_t{42}, std::string_view("it's"), 1.5, nullptr, true, std::uint64_t{7});
            result = std::pmr::string(rows.front()["v"].value().value_or(""), ruvia::process_resource());
        }
    });
    EXPECT_TRUE(results[0] == "a");
    EXPECT_TRUE(results[1] == "b");
    const auto commands = peer.maria_commands();
    EXPECT_EQ(commands.size(), std::size_t{3});
    EXPECT_TRUE(commands.front() ==
                "query:SELECT v FROM t WHERE a = 42 AND b = 'it''s' AND c = 1.5e0 AND d <=> NULL "
                "AND e = 1 AND f = 7");
    EXPECT_TRUE(commands.back() == "execute:1:i:42,s:it's,d:1.5,null,t:1,i:7");
}

// Values read in the binary protocol become the text the text protocol sends
// for them. Each expected text below was read from MariaDB 11.8's text
// protocol for the same stored value.
TEST(Ruvia, db_mariadb_prepared_reals_read_as_the_text_protocol_sends_them) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::pmr::vector<std::pair<double, std::pmr::string>> doubles({{0.1, std::pmr::string("0.1", ruvia::process_resource())}, {1e20, std::pmr::string("1e20", ruvia::process_resource())}, {1e15, std::pmr::string("1e15", ruvia::process_resource())},
                                                                            {123456789012345.0, std::pmr::string("123456789012345", ruvia::process_resource())}, {1.5e-15, std::pmr::string("0.0000000000000015", ruvia::process_resource())}, {1e-5, std::pmr::string("0.00001", ruvia::process_resource())},
                                                                            {-0.000123, std::pmr::string("-0.000123", ruvia::process_resource())}, {1.0 / 3, std::pmr::string("0.3333333333333333", ruvia::process_resource())}, {1e-14, std::pmr::string("0.00000000000001", ruvia::process_resource())},
                                                                            {2.5e300, std::pmr::string("2.5e300", ruvia::process_resource())}, {12345678901234567890.0, std::pmr::string("1.2345678901234567e19", ruvia::process_resource())}, {1e16, std::pmr::string("1e16", ruvia::process_resource())},
                                                                            // dtoa reports an overflow for infinity and NaN, which the server
                                                                            // writes as 0 (VARIANCE() of values near DBL_MAX overflows).
                                                                            {std::numeric_limits<double>::infinity(), "0"}, {-std::numeric_limits<double>::infinity(), std::pmr::string("0", ruvia::process_resource())},
                                                                            {std::numeric_limits<double>::quiet_NaN(), std::pmr::string("0", ruvia::process_resource())}},
        ruvia::process_resource());
    const std::pmr::vector<std::pair<float, std::pmr::string>> floats({{0.1F, std::pmr::string("0.1", ruvia::process_resource())}, {3.14159265F, std::pmr::string("3.14159", ruvia::process_resource())},
                                                                          {16777217.0F, std::pmr::string("16777200", ruvia::process_resource())}, {-1.17549435e-38F, std::pmr::string("-1.17549e-38", ruvia::process_resource())}, {3.4e38F, std::pmr::string("3.4e38", ruvia::process_resource())}, {1e-5F, std::pmr::string("0.00001", ruvia::process_resource())},
                                                                          {0.33333334F, std::pmr::string("0.333333", ruvia::process_resource())}, {1e-7F, std::pmr::string("0.0000001", ruvia::process_resource())}, {1234567.0F, std::pmr::string("1234570", ruvia::process_resource())}, {1e10F, std::pmr::string("10000000000", ruvia::process_resource())},
                                                                          {123456.0F, std::pmr::string("123456", ruvia::process_resource())}, {2.5F, std::pmr::string("2.5", ruvia::process_resource())}, {std::numeric_limits<float>::infinity(), std::pmr::string("0", ruvia::process_resource())},
                                                                          {-std::numeric_limits<float>::infinity(), std::pmr::string("0", ruvia::process_resource())}, {std::numeric_limits<float>::quiet_NaN(), std::pmr::string("0", ruvia::process_resource())}},
        ruvia::process_resource());
    std::pmr::vector<binary_row> rows{ruvia::process_resource()};
    std::pmr::vector<std::optional<std::pmr::string>> expected{ruvia::process_resource()};
    for (std::size_t index = 0; index < doubles.size(); ++index) {
        rows.push_back(binary_row({real(doubles[index].first), single(floats[index].first)}, ruvia::process_resource()));
        expected.emplace_back(std::in_place, doubles[index].second, ruvia::process_resource());
        expected.emplace_back(std::in_place, floats[index].second, ruvia::process_resource());
    }
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("x", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>({{.name_ = std::pmr::string("d", ruvia::process_resource()), .type_ = type_double, .decimals_ = not_fixed, .length_ = 22},
                                                                                                                                                                           {.name_ = std::pmr::string("f", ruvia::process_resource()), .type_ = type_float, .decimals_ = not_fixed, .length_ = 12}},
                                                                                             ruvia::process_resource()),
                                                                                            .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>(rows, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
    EXPECT_TRUE(prepared_rows.has_value());
    EXPECT_TRUE((values(*prepared_rows) == expected));
}

TEST(Ruvia, db_mariadb_prepared_values_read_as_the_text_protocol_sends_them) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::pmr::vector<column> columns({{.name_ = std::pmr::string("dd", ruvia::process_resource()), .type_ = type_double, .decimals_ = 6, .length_ = 20},
                                               {.name_ = std::pmr::string("fd", ruvia::process_resource()), .type_ = type_float, .decimals_ = 3, .length_ = 9},
                                               {.name_ = std::pmr::string("z", ruvia::process_resource()), .type_ = type_long, .flags_ = unsigned_flag | zerofill_flag, .length_ = 8},
                                               {.name_ = std::pmr::string("dt3", ruvia::process_resource()), .type_ = type_datetime, .decimals_ = 3, .length_ = 23},
                                               {.name_ = std::pmr::string("t3", ruvia::process_resource()), .type_ = type_time, .decimals_ = 3, .length_ = 14},
                                               {.name_ = std::pmr::string("t", ruvia::process_resource()), .type_ = type_time, .length_ = 10},
                                               {.name_ = std::pmr::string("da", ruvia::process_resource()), .type_ = type_date, .length_ = 10},
                                               {.name_ = std::pmr::string("de", ruvia::process_resource()), .type_ = type_newdecimal, .decimals_ = 10, .length_ = 32},
                                               {.name_ = std::pmr::string("bu", ruvia::process_resource()), .type_ = type_longlong, .flags_ = unsigned_flag, .length_ = 20},
                                               {.name_ = std::pmr::string("b", ruvia::process_resource()), .type_ = type_longlong, .length_ = 20},
                                               {.name_ = std::pmr::string("bl", ruvia::process_resource()), .type_ = type_blob, .flags_ = 128, .length_ = 65535},
                                               {.name_ = std::pmr::string("n", ruvia::process_resource()), .type_ = type_double, .decimals_ = not_fixed, .length_ = 22}},
        ruvia::process_resource());
    const std::pmr::vector<binary_row> rows({binary_row({std::pmr::string(real(1.5), ruvia::process_resource()), std::pmr::string(single(1.5F), ruvia::process_resource()), std::pmr::string(little(42, 4), ruvia::process_resource()), std::pmr::string(datetime(2001, 2, 3, 4, 5, 6, 789000), ruvia::process_resource()), std::pmr::string(time(true, 34, 22, 59, 59, 500000), ruvia::process_resource()),
                                                            std::pmr::string(time(false, 1, 1, 0, 0), ruvia::process_resource()), std::pmr::string(datetime(0, 0, 0), ruvia::process_resource()), std::pmr::string(text("-1.5000000000"), ruvia::process_resource()), std::pmr::string(little(18446744073709551615ULL, 8), ruvia::process_resource()),
                                                            std::pmr::string(little(static_cast<std::uint64_t>(INT64_MIN), 8), ruvia::process_resource()), std::pmr::string(text(std::pmr::string("\0\xff\x10", 3, ruvia::process_resource())), ruvia::process_resource()), std::nullopt},
                                                 ruvia::process_resource()),
                                                binary_row({std::pmr::string(real(123456789.123457), ruvia::process_resource()), std::pmr::string(single(0.1F), ruvia::process_resource()), std::pmr::string(little(0, 4), ruvia::process_resource()), std::pmr::string(datetime(0, 0, 0), ruvia::process_resource()), std::pmr::string(time(true, 0, 0, 0, 0, 500000), ruvia::process_resource()),
                                                               std::pmr::string(time(false, 34, 22, 59, 59), ruvia::process_resource()), std::pmr::string(datetime(2024, 2, 29), ruvia::process_resource()), std::pmr::string(text("0.0000000001"), ruvia::process_resource()), std::pmr::string(little(0, 8), ruvia::process_resource()), std::pmr::string(little(7, 8), ruvia::process_resource()),
                                                               std::pmr::string(text(""), ruvia::process_resource()), std::pmr::string(real(0.1), ruvia::process_resource())},
                                                    ruvia::process_resource()),
                                                binary_row({std::pmr::string(real(1e10), ruvia::process_resource()), std::pmr::string(single(2.001F), ruvia::process_resource()), std::pmr::string(little(99999999, 4), ruvia::process_resource()), std::pmr::string(datetime(2038, 1, 19, 3, 14, 7, 999000), ruvia::process_resource()),
                                                               std::pmr::string(time(false, 0, 12, 34, 56, 1000), ruvia::process_resource()), std::pmr::string(time(false, 0, 0, 0, 1), ruvia::process_resource()), std::pmr::string(datetime(9999, 12, 31), ruvia::process_resource()),
                                                               std::pmr::string(text("12345678901234567890.0000000000"), ruvia::process_resource()), std::pmr::string(little(1, 8), ruvia::process_resource()), std::pmr::string(little(static_cast<std::uint64_t>(-1), 8), ruvia::process_resource()),
                                                               std::pmr::string(text("x"), ruvia::process_resource()), std::nullopt},
                                                    ruvia::process_resource())},
        ruvia::process_resource());
    const std::pmr::vector<std::optional<std::pmr::string>> expected({std::pmr::string("1.500000", ruvia::process_resource()), std::pmr::string("1.500", ruvia::process_resource()), std::pmr::string("00000042", ruvia::process_resource()), std::pmr::string("2001-02-03 04:05:06.789", ruvia::process_resource()),
                                                                         std::pmr::string("-838:59:59.500", ruvia::process_resource()), std::pmr::string("25:00:00", ruvia::process_resource()), std::pmr::string("0000-00-00", ruvia::process_resource()), std::pmr::string("-1.5000000000", ruvia::process_resource()), std::pmr::string("18446744073709551615", ruvia::process_resource()), std::pmr::string("-9223372036854775808", ruvia::process_resource()),
                                                                         std::pmr::string("\0\xff\x10", 3, ruvia::process_resource()), std::nullopt, std::pmr::string("123456789.123457", ruvia::process_resource()), std::pmr::string("0.100", ruvia::process_resource()), std::pmr::string("00000000", ruvia::process_resource()), std::pmr::string("0000-00-00 00:00:00.000", ruvia::process_resource()),
                                                                         std::pmr::string("-00:00:00.500", ruvia::process_resource()), std::pmr::string("838:59:59", ruvia::process_resource()), std::pmr::string("2024-02-29", ruvia::process_resource()), std::pmr::string("0.0000000001", ruvia::process_resource()), std::pmr::string("0", ruvia::process_resource()), std::pmr::string("7", ruvia::process_resource()), std::pmr::string("", ruvia::process_resource()), std::pmr::string("0.1", ruvia::process_resource()), std::pmr::string("10000000000.000000", ruvia::process_resource()), std::pmr::string("2.001", ruvia::process_resource()),
                                                                         std::pmr::string("99999999", ruvia::process_resource()), std::pmr::string("2038-01-19 03:14:07.999", ruvia::process_resource()), std::pmr::string("12:34:56.001", ruvia::process_resource()), std::pmr::string("00:00:01", ruvia::process_resource()), std::pmr::string("9999-12-31", ruvia::process_resource()),
                                                                         std::pmr::string("12345678901234567890.0000000000", ruvia::process_resource()), std::pmr::string("1", ruvia::process_resource()), std::pmr::string("-1", ruvia::process_resource()), std::pmr::string("x", ruvia::process_resource()), std::nullopt},
        ruvia::process_resource());
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("x", ruvia::process_resource())}, ruvia::process_resource())}, {.columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(columns, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>(rows, ruvia::process_resource())}}, ruvia::process_resource()), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::optional<db_rows> text_rows;
    std::optional<db_rows> prepared_rows;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        co_await twice(
            connected.handle({.timeout_ = std::chrono::seconds(5)}), "SELECT * FROM typed", text_rows, prepared_rows);
    });
    EXPECT_TRUE(prepared_rows.has_value());
    EXPECT_TRUE((values(*prepared_rows) == expected));
    EXPECT_TRUE((prepared_rows->front()["dt3"].value() == std::optional<std::string_view>("2001-02-03 04:05:06.789")));
}

// A statement the server will not prepare runs as text from then on.
TEST(Ruvia, db_mariadb_statement_the_server_will_not_prepare_runs_as_text) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.error_ = std::pmr::string("This command is not supported in the prepared statement protocol yet", ruvia::process_resource()),
                                                                                            .code_ = 1295,
                                                                                            .sql_state_ = std::pmr::string("HY000", ruvia::process_resource()),
                                                                                            .prepare_fails_ = true},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(outcomes[2].value_ == "c");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SHOW v", ruvia::process_resource()), std::pmr::string("prepare:SHOW v", ruvia::process_resource()), std::pmr::string("query:SHOW v", ruvia::process_resource()), std::pmr::string("query:SHOW v", ruvia::process_resource())}, ruvia::process_resource())));
}

// A statement the server no longer holds is prepared and run once more; the
// handle it dropped is closed on the server.
TEST(Ruvia, db_mariadb_stale_prepared_statement_is_prepared_again_once) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    const peer_type::maria_reply unknown{
        .error_ = "Unknown prepared statement handler", .code_ = 1243, .sql_state_ = "HY000"};
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, unknown, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}, unknown, unknown}, ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(outcomes[2].value_ == "c");
    // Stale again on its retry: the error reaches the caller.
    EXPECT_TRUE(outcomes[3].error_ == db_error::code_type::statement_failed);
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()),
                                                        std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("close:1", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource()), std::pmr::string("close:2", ruvia::process_resource()),
                                                        std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:3:", ruvia::process_resource())},
                     ruvia::process_resource())));
}

// A cached statement alone on its connection runs prepared; the statements
// arriving while it runs wait for its response, then go out together as
// pipelined text, since the binary protocol cannot share the connection. The
// peer holds the prepared execution's response until the others have
// arrived, so the first is still running when they do.
TEST(Ruvia, db_mariadb_cached_statements_arriving_together_run_as_pipelined_text) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource()), .hold_ = 1}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("d", ruvia::process_resource())}, ruvia::process_resource()), .batch_ = 2},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("e", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
        EXPECT_TRUE(outcomes[index].value_ == std::pmr::string(1, static_cast<char>('a' + index), ruvia::process_resource()));
    }
    EXPECT_TRUE((peer.maria_commands() == std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()),
                                                                                 std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()),
                                                                                 std::pmr::string("query:SELECT v FROM t", ruvia::process_resource())},
                                              ruvia::process_resource())));
}

// Inside a transaction a statement is prepared on its second use as well.
TEST(Ruvia, db_mariadb_transaction_statement_runs_prepared_from_its_second_use) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.column_ = std::pmr::string("", ruvia::process_resource()), .status_ = 3}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource()), .status_ = 3}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource()), .status_ = 3},
                                                                                        {.column_ = std::pmr::string("", ruvia::process_resource())}},
                       ruvia::process_resource()),
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::array<std::pmr::string, 2> results{std::pmr::string(ruvia::process_resource()), std::pmr::string(ruvia::process_resource())};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto transaction = co_await connected.handle({.timeout_ = std::chrono::seconds(5)}).begin_transaction();
        for (auto& result : results) {
            const auto rows = co_await transaction.query("SELECT v FROM t WHERE id = ?", std::int64_t{3});
            result = std::pmr::string(rows.front()["v"].value().value_or(""), ruvia::process_resource());
        }
        co_await transaction.commit();
    });
    EXPECT_TRUE(results[0] == "a");
    EXPECT_TRUE(results[1] == "b");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:START TRANSACTION", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t WHERE id = 3", ruvia::process_resource()),
                                                        std::pmr::string("prepare:SELECT v FROM t WHERE id = ?", ruvia::process_resource()), std::pmr::string("execute:1:i:3", ruvia::process_resource()), std::pmr::string("query:COMMIT", ruvia::process_resource())},
                     ruvia::process_resource())));
}

// The least recently used statement is closed on the server when a new one
// needs its room.
TEST(Ruvia, db_mariadb_least_recently_used_prepared_statement_is_closed_for_a_new_one) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    constexpr std::size_t statements = 129;
    std::pmr::vector<peer_type::maria_reply> replies(statements * 2, peer_type::maria_reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource());
    peer_type peer(std::move(replies), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_statement_test::config(peer));
    std::size_t failures = 0;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (std::size_t index = 0; index < statements; ++index) {
            const auto sql = "SELECT v FROM t" + std::pmr::string(std::to_string(index), ruvia::process_resource());
            for (int use = 0; use < 2; ++use) {
                pipeline_test::outcome result;
                co_await pipeline_test::query(handle, sql, result);
                failures += result.value_ == "v" ? 0 : 1;
            }
        }
    });
    EXPECT_EQ(failures, std::size_t{0});
    const auto commands = peer.maria_commands();
    EXPECT_EQ(commands.size(), statements * 3 + 1);
    // The first statement's handle goes before the last one is prepared.
    EXPECT_TRUE(commands[commands.size() - 3] == "close:1");
    EXPECT_TRUE(commands[commands.size() - 2] == "prepare:SELECT v FROM t128");
}

// A server error numbered above the client library's range (4025, a violated
// CHECK constraint) is the prepared statement's own failure, as it is for the
// statement run as text; the connection is kept.
TEST(Ruvia, db_mariadb_prepared_server_error_above_the_client_range_is_the_statements_own) {
    using namespace ruvia;
    using mariadb_statement_test::peer_type;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.column_ = std::pmr::string("", ruvia::process_resource()), .affected_rows_ = 1},
                                                                                        {.column_ = std::pmr::string("", ruvia::process_resource()),
                                                                                            .error_ = std::pmr::string("CONSTRAINT `t.v` failed for `db`.`t`", ruvia::process_resource()),
                                                                                            .code_ = 4025,
                                                                                            .sql_state_ = std::pmr::string("23000", ruvia::process_resource())},
                                                                                        {.column_ = std::pmr::string("", ruvia::process_resource()), .affected_rows_ = 1}},
                       ruvia::process_resource()),
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
    EXPECT_EQ(outcomes[0].affected_rows_, std::uint64_t{1});
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::statement_failed);
    EXPECT_TRUE(outcomes[1].sql_state_ == "23000");
    EXPECT_EQ(outcomes[2].affected_rows_, std::uint64_t{1});
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:INSERT INTO t (v) VALUES (1)", ruvia::process_resource()),
                                                        std::pmr::string("prepare:INSERT INTO t (v) VALUES (1)", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource())},
                     ruvia::process_resource())));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A call runs prepared only with parameters the server reads as it reads
// their literals, and a statement is prepared for each set of parameter
// types, since the server fixes result types when it prepares. A negative
// number reads as a literal's unary minus, which binds looser than COLLATE
// and turns -? of INT64_MIN into an overflow only as a parameter; an
// unsigned value above INT64_MAX makes arithmetic unsigned only as a
// parameter (3 - ? with 5 is -2 as text, an error prepared). Those calls run
// as text, while the statement stays prepared for the others.
TEST(Ruvia, db_mariadb_parameters_read_otherwise_than_their_literals_run_as_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::string_view sql = "SELECT v FROM t WHERE a = ?";
    peer_type peer(std::pmr::vector<reply>(10, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource()), peer_type::after_startup::idle);
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
        EXPECT_TRUE(result.value_ == "v");
    }
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t WHERE a = 5", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t WHERE a = ?", ruvia::process_resource()),
                                                        std::pmr::string("execute:1:i:6", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t WHERE a = -1", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT v FROM t WHERE a = -9223372036854775808", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT v FROM t WHERE a = 18446744073709551615", ruvia::process_resource()), std::pmr::string("execute:1:i:7", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT v FROM t WHERE a = 1.5e0", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t WHERE a = ?", ruvia::process_resource()), std::pmr::string("execute:2:d:2.5", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT v FROM t WHERE a = -2.5e0", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t WHERE a = -0e0", ruvia::process_resource())},
                     ruvia::process_resource())));
}

// Where the text protocol takes only an integer literal (LIMIT, OFFSET and
// FETCH counts), it refuses a string, a real, NULL or a negative number with
// a syntax error, while a parameter there would take them: such a call runs
// as text. Parameters elsewhere in the statement are not restricted.
TEST(Ruvia, db_mariadb_integer_only_placeholders_run_prepared_only_with_integers) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::string_view limit = "SELECT v FROM t LIMIT ?";
    constexpr std::string_view filtered = "SELECT v FROM t WHERE w = ? LIMIT ?, ?";
    constexpr std::string_view fetch = "SELECT v FROM t ORDER BY v OFFSET ? ROWS FETCH FIRST ? ROWS ONLY";
    peer_type peer(std::pmr::vector<reply>(19, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource()), peer_type::after_startup::idle);
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
        EXPECT_TRUE(result.value_ == "v");
    }
    const std::pmr::vector<std::pmr::string> expected({std::pmr::string("query:SELECT v FROM t LIMIT 2", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t LIMIT ?", ruvia::process_resource()),
                                                          std::pmr::string("execute:1:i:3", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t LIMIT '4'", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t LIMIT 1.5e0", ruvia::process_resource()),
                                                          std::pmr::string("query:SELECT v FROM t LIMIT NULL", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t LIMIT 1", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t LIMIT '4'", ruvia::process_resource()),
                                                          std::pmr::string("query:SELECT v FROM t LIMIT 1.5e0", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t LIMIT NULL", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t LIMIT 1", ruvia::process_resource()),
                                                          std::pmr::string("execute:1:i:5", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t WHERE w = 'x' LIMIT 1, 2", ruvia::process_resource()),
                                                          std::pmr::string("prepare:SELECT v FROM t WHERE w = ? LIMIT ?, ?", ruvia::process_resource()), std::pmr::string("execute:2:s:x,i:1,i:2", ruvia::process_resource()),
                                                          std::pmr::string("query:SELECT v FROM t WHERE w = 'x' LIMIT '1', 2", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t WHERE w = 'x' LIMIT '1', 2", ruvia::process_resource()),
                                                          std::pmr::string("query:SELECT v FROM t ORDER BY v OFFSET 1 ROWS FETCH FIRST 2 ROWS ONLY", ruvia::process_resource()),
                                                          std::pmr::string("prepare:SELECT v FROM t ORDER BY v OFFSET ? ROWS FETCH FIRST ? ROWS ONLY", ruvia::process_resource()), std::pmr::string("execute:3:i:1,i:2", ruvia::process_resource()),
                                                          std::pmr::string("query:SELECT v FROM t ORDER BY v OFFSET 1 ROWS FETCH FIRST 2e0 ROWS ONLY", ruvia::process_resource()),
                                                          std::pmr::string("query:SELECT v FROM t ORDER BY v OFFSET 1 ROWS FETCH FIRST 2e0 ROWS ONLY", ruvia::process_resource())},
        ruvia::process_resource());
    EXPECT_TRUE((peer.maria_commands() == expected));
}

// Statements the binary protocol cannot run as the text protocol does are
// never prepared, however they are written: a CALL (its OUT parameters come
// back as an extra result set only when prepared) also behind a comment, a
// statement with RETURNING (a re-run after its result changed shape would
// repeat its writes), one led by an executable comment, whose content the
// server runs, and a LOAD. A statement in parentheses is prepared as usual.
TEST(Ruvia, db_mariadb_statements_the_binary_protocol_runs_otherwise_are_never_prepared) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::array<std::string_view, 4> text_only{"/* note */ CALL p()",
        "INSERT INTO t (v) VALUES (1) RETURNING v", "/*!100000 SELECT v FROM t */", "LOAD XML INFILE 'x' INTO TABLE t"};
    // The tenth answers the read-back of the tracked variables after the
    // CALL and the executable comment.
    std::pmr::vector<reply> replies(12, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource());
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
        EXPECT_TRUE(result.value_ == "v");
    }
    std::pmr::vector<std::pmr::string> expected{ruvia::process_resource()};
    for (const auto sql : text_only) {
        expected.insert(expected.end(), 2, "query:" + std::pmr::string(sql, ruvia::process_resource()));
    }
    expected.insert(expected.end(), {"query:(SELECT v FROM t)", "query:SELECT @@session_track_system_variables",
                                        "prepare:(SELECT v FROM t)", "execute:1:", "execute:1:"});
    EXPECT_TRUE((peer.maria_commands() == expected));
}

// Values of a column without a fixed scale read as the text protocol sends
// them: a DATETIME or TIME of AUTO_SEC_PART_DIGITS shows six fractional
// digits when the value has a fraction and none otherwise; a non-finite
// DOUBLE, also of a fixed scale, is written as 0.
TEST(Ruvia, db_mariadb_prepared_values_without_a_fixed_scale_read_as_the_text_protocol_sends_them) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::uint8_t auto_digits = 39;
    const std::pmr::vector<column> columns({{.name_ = std::pmr::string("dd", ruvia::process_resource()), .type_ = type_double, .decimals_ = 6, .length_ = 20},
                                               {.name_ = std::pmr::string("dt", ruvia::process_resource()), .type_ = type_datetime, .decimals_ = auto_digits, .length_ = 26},
                                               {.name_ = std::pmr::string("t", ruvia::process_resource()), .type_ = type_time, .decimals_ = auto_digits, .length_ = 17}},
        ruvia::process_resource());
    const std::pmr::vector<binary_row> rows({binary_row({std::pmr::string(real(std::numeric_limits<double>::infinity()), ruvia::process_resource()), std::pmr::string(datetime(1970, 1, 1, 0, 0, 1, 500000), ruvia::process_resource()),
                                                            std::pmr::string(time(false, 0, 0, 0, 1, 500000), ruvia::process_resource())},
                                                 ruvia::process_resource()),
                                                binary_row({std::pmr::string(real(std::numeric_limits<double>::quiet_NaN()), ruvia::process_resource()), std::pmr::string(datetime(1970, 1, 1, 0, 0, 1), ruvia::process_resource()), std::pmr::string(time(false, 0, 1, 2, 3), ruvia::process_resource())}, ruvia::process_resource()),
                                                binary_row({std::pmr::string(real(-std::numeric_limits<double>::infinity()), ruvia::process_resource()), std::pmr::string(datetime(2020, 1, 1, 10, 0, 0, 10), ruvia::process_resource()),
                                                               std::pmr::string(time(true, 1, 10, 0, 0, 250000), ruvia::process_resource())},
                                                    ruvia::process_resource())},
        ruvia::process_resource());
    const std::pmr::vector<std::optional<std::pmr::string>> expected({std::pmr::string("0", ruvia::process_resource()), std::pmr::string("1970-01-01 00:00:01.500000", ruvia::process_resource()), std::pmr::string("00:00:01.500000", ruvia::process_resource()), std::pmr::string("0", ruvia::process_resource()),
                                                                         std::pmr::string("1970-01-01 00:00:01", ruvia::process_resource()), std::pmr::string("01:02:03", ruvia::process_resource()), std::pmr::string("0", ruvia::process_resource()), std::pmr::string("2020-01-01 10:00:00.000010", ruvia::process_resource()), std::pmr::string("-34:00:00.250000", ruvia::process_resource())},
        ruvia::process_resource());
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("x", ruvia::process_resource())}, ruvia::process_resource())}, {.columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(columns, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>(rows, ruvia::process_resource())}}, ruvia::process_resource()), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::optional<db_rows> text_rows;
    std::optional<db_rows> prepared_rows;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        co_await twice(
            connected.handle({.timeout_ = std::chrono::seconds(5)}), "SELECT * FROM auto", text_rows, prepared_rows);
    });
    EXPECT_TRUE(prepared_rows.has_value());
    EXPECT_TRUE((values(*prepared_rows) == expected));
}

// The text protocol pads a YEAR column's value to four digits when it comes
// from a table column and not when it comes from an expression, under the
// same metadata without ZEROFILL, and the metadata of the prepare is not that
// of the execution: a UNION of YEAR columns is prepared with ZEROFILL and
// runs without it. A statement whose prepared result holds a YEAR runs as
// text.
TEST(Ruvia, db_mariadb_statement_with_a_year_column_runs_as_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::pmr::vector<column> padded({{.name_ = std::pmr::string("y", ruvia::process_resource()), .type_ = type_year, .flags_ = unsigned_flag | zerofill_flag, .length_ = 4}}, ruvia::process_resource());
    const std::pmr::vector<column> plain({{.name_ = std::pmr::string("y", ruvia::process_resource()), .type_ = type_year, .flags_ = unsigned_flag, .length_ = 4}}, ruvia::process_resource());
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0000", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0000", ruvia::process_resource())}, ruvia::process_resource()), .columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(plain, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>({std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(little(0, 2), ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource()), .prepare_columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(padded, ruvia::process_resource())},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0000", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0", ruvia::process_resource())}, ruvia::process_resource()), .columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(plain, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
        EXPECT_TRUE(outcomes[index].value_ == "0000");
    }
    for (const std::size_t index : {3, 4, 5}) {
        EXPECT_TRUE(outcomes[index].value_ == "0");
    }
    EXPECT_TRUE((peer.maria_commands() == std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT y FROM t UNION ALL SELECT y FROM u", ruvia::process_resource()),
                                                                                 std::pmr::string("prepare:SELECT y FROM t UNION ALL SELECT y FROM u", ruvia::process_resource()),
                                                                                 std::pmr::string("query:SELECT y FROM t UNION ALL SELECT y FROM u", ruvia::process_resource()),
                                                                                 std::pmr::string("query:SELECT y FROM t UNION ALL SELECT y FROM u", ruvia::process_resource()),
                                                                                 std::pmr::string("query:SELECT MIN(y) AS y FROM t", ruvia::process_resource()), std::pmr::string("close:1", ruvia::process_resource()),
                                                                                 std::pmr::string("prepare:SELECT MIN(y) AS y FROM t", ruvia::process_resource()), std::pmr::string("query:SELECT MIN(y) AS y FROM t", ruvia::process_resource()),
                                                                                 std::pmr::string("query:SELECT MIN(y) AS y FROM t", ruvia::process_resource())},
                                              ruvia::process_resource())));
}

// A YEAR the prepare did not describe (the column was altered to YEAR since,
// and the server prepared the statement anew inside the execution): one
// with ZEROFILL reads padded, as the text protocol sends it; one without it
// cannot be told from its binary value, and the server has run the statement
// already, so it fails once and runs as text from its next use.
TEST(Ruvia, db_mariadb_year_column_appearing_at_execution_reads_padded_or_fails_once) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::pmr::vector<column> number({{.name_ = std::pmr::string("y", ruvia::process_resource()), .type_ = type_long, .length_ = 11}}, ruvia::process_resource());
    const std::pmr::vector<column> padded({{.name_ = std::pmr::string("y", ruvia::process_resource()), .type_ = type_year, .flags_ = unsigned_flag | zerofill_flag, .length_ = 4}}, ruvia::process_resource());
    const std::pmr::vector<column> plain({{.name_ = std::pmr::string("y", ruvia::process_resource()), .type_ = type_year, .flags_ = unsigned_flag, .length_ = 4}}, ruvia::process_resource());
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("x", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(padded, ruvia::process_resource()),
                                                                                            .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>({std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(little(0, 2), ruvia::process_resource())}, ruvia::process_resource()), std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(little(2155, 2), ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource()),
                                                                                            .prepare_columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(number, ruvia::process_resource())},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("y", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(plain, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>({std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(little(0, 2), ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource()), .prepare_columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(number, ruvia::process_resource())},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
    EXPECT_TRUE(padded_rows.has_value());
    EXPECT_TRUE((values(*padded_rows) == std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string("0000", ruvia::process_resource()), std::pmr::string("2155", ruvia::process_resource())}, ruvia::process_resource())));
    EXPECT_TRUE(outcomes[0].value_ == "y");
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::statement_failed);
    EXPECT_TRUE(outcomes[2].value_ == "0");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT y FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT y FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT y FROM u", ruvia::process_resource()), std::pmr::string("prepare:SELECT y FROM u", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource()), std::pmr::string("query:SELECT y FROM u", ruvia::process_resource())},
                     ruvia::process_resource())));
}

// A table that gained a column since the statement was prepared: the server
// prepares it anew, runs it, and its result has a column the handle was not
// prepared for (CR_NEW_STMT_METADATA). The server has run it, and running it
// again would repeat what it did (NEXTVAL() in a SELECT): the statement fails
// once, as a statement, with its result read past, and is prepared anew on
// its next use.
TEST(Ruvia, db_mariadb_statement_whose_result_changed_shape_fails_once_and_is_prepared_anew) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::pmr::vector<column> one({{.name_ = std::pmr::string("v", ruvia::process_resource())}}, ruvia::process_resource());
    const std::pmr::vector<column> two({{.name_ = std::pmr::string("v", ruvia::process_resource())}, {.name_ = std::pmr::string("w", ruvia::process_resource())}}, ruvia::process_resource());
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("1", ruvia::process_resource())}, ruvia::process_resource())}, {.columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(one, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>({std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(text("1"), ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(two, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>({std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(text("2"), ruvia::process_resource()), std::pmr::string(text("2"), ruvia::process_resource())}, ruvia::process_resource()), std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(text("3"), ruvia::process_resource()), std::pmr::string(text("4"), ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(two, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>({std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(text("3"), ruvia::process_resource()), std::pmr::string(text("2"), ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[0].value_ == "1");
    EXPECT_TRUE(outcomes[1].value_ == "1");
    EXPECT_TRUE(outcomes[2].error_ == db_error::code_type::statement_failed);
    EXPECT_TRUE(outcomes[2].value_.empty());
    EXPECT_TRUE(outcomes[3].value_ == "3|2");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT NEXTVAL(s), t.* FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT NEXTVAL(s), t.* FROM t", ruvia::process_resource()),
                                                        std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("close:1", ruvia::process_resource()), std::pmr::string("prepare:SELECT NEXTVAL(s), t.* FROM t", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource())},
                     ruvia::process_resource())));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// Inside a transaction the statement whose result changed shape fails as a
// statement, not as a lost connection: the transaction rolls back on the
// same connection, and the statement is prepared anew on its next use.
TEST(Ruvia, db_mariadb_statement_whose_result_changed_shape_fails_inside_a_transaction) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::pmr::vector<column> one({{.name_ = std::pmr::string("v", ruvia::process_resource())}}, ruvia::process_resource());
    const std::pmr::vector<column> two({{.name_ = std::pmr::string("v", ruvia::process_resource())}, {.name_ = std::pmr::string("w", ruvia::process_resource())}}, ruvia::process_resource());
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.column_ = std::pmr::string("", ruvia::process_resource()), .status_ = 3}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("1", ruvia::process_resource())}, ruvia::process_resource()), .status_ = 3},
                                                                                        {.status_ = 3, .columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(one, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>({std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(text("1"), ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.status_ = 3, .columns_ = std::pmr::vector<ruvia::test::backend_startup_peer::maria_column>(two, ruvia::process_resource()), .binary_rows_ = std::pmr::vector<std::pmr::vector<std::optional<std::pmr::string>>>({std::pmr::vector<std::optional<std::pmr::string>>({std::pmr::string(text("1"), ruvia::process_resource()), std::pmr::string(text("2"), ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.column_ = std::pmr::string("", ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("after", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::pmr::vector<std::pmr::string> results{ruvia::process_resource()};
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
    EXPECT_TRUE((results == std::pmr::vector<std::pmr::string>({std::pmr::string("1", ruvia::process_resource()), std::pmr::string("1", ruvia::process_resource())}, ruvia::process_resource())));
    EXPECT_TRUE(failure == db_error::code_type::statement_failed);
    EXPECT_TRUE(after.value_ == "after");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:START TRANSACTION", ruvia::process_resource()), std::pmr::string("query:SELECT * FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT * FROM t", ruvia::process_resource()),
                                                        std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("query:ROLLBACK", ruvia::process_resource()), std::pmr::string("close:1", ruvia::process_resource()), std::pmr::string("prepare:SELECT * FROM t", ruvia::process_resource()),
                                                        std::pmr::string("execute:2:", ruvia::process_resource())},
                     ruvia::process_resource())));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A prepare whose command is still being written when its statement is
// stopped is finished by the next statement on the connection, from the
// statement text the connection's cache owns: the stopped caller's text is
// gone by then. The peer reads nothing until the caller has left, so the
// write is suspended when it does.
TEST(Ruvia, db_mariadb_abandoned_prepare_is_finished_from_the_cached_statement_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    std::pmr::string sql("SELECT v FROM t /* ", ruvia::process_resource());
    // More than the socket buffers of both ends hold.
    sql.append(std::size_t{15} * 1024 * 1024, 'x');
    sql.append(" */");
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("text", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("second", ruvia::process_resource())}, ruvia::process_resource()), .read_hold_ = 1}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("third", ruvia::process_resource())}, ruvia::process_resource())}}, ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[0].value_ == "text");
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::cancelled);
    EXPECT_TRUE(outcomes[2].value_ == "second");
    EXPECT_TRUE(outcomes[3].value_ == "third");
    const auto commands = peer.maria_commands();
    EXPECT_EQ(commands.size(), std::size_t{5});
    if (commands.size() == 5) {
        EXPECT_TRUE(commands[1] == std::pmr::string("prepare:", ruvia::process_resource()) + sql);
        EXPECT_TRUE((std::pmr::vector<std::pmr::string>(commands.begin() + 2, commands.end(), ruvia::process_resource()) ==
                     std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource())}, ruvia::process_resource())));
    }
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A prepared statement whose rows end in an error (a DOUBLE out of range)
// fails alone and leaves nothing pending: it is reset to its prepared state,
// and another statement found stale afterwards is still closed on the server
// and prepared anew.
TEST(Ruvia, db_mariadb_prepared_statement_failing_in_its_rows_leaves_the_others_usable) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const reply unknown{.error_ = "Unknown prepared statement handler", .code_ = 1243, .sql_state_ = "HY000"};
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("1", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("2", ruvia::process_resource())}, ruvia::process_resource()),
                                                                                            .error_ = std::pmr::string("DOUBLE value is out of range", ruvia::process_resource()),
                                                                                            .code_ = 1690,
                                                                                            .sql_state_ = std::pmr::string("22003", ruvia::process_resource()),
                                                                                            .error_after_rows_ = true},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, unknown, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[0].value_ == "1");
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::statement_failed);
    EXPECT_TRUE(outcomes[1].sql_state_ == "22003");
    EXPECT_TRUE(outcomes[2].value_ == "b");
    EXPECT_TRUE(outcomes[3].value_ == "b");
    EXPECT_TRUE(outcomes[4].value_ == "b");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v * 3 FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v * 3 FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()),
                                                        std::pmr::string("reset:1", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM u", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM u", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource()),
                                                        std::pmr::string("close:2", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM u", ruvia::process_resource()), std::pmr::string("execute:3:", ruvia::process_resource())},
                     ruvia::process_resource())));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// The same prepared statement runs again after its rows failed (1242, a
// subquery returning more than one row): its next execution goes out and
// reads its own response, and the statements behind it get theirs, on the
// same connection.
TEST(Ruvia, db_mariadb_prepared_statement_runs_again_after_its_rows_failed) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("1", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("2", ruvia::process_resource())}, ruvia::process_resource()),
                                                                                            .error_ = std::pmr::string("Subquery returns more than 1 row", ruvia::process_resource()),
                                                                                            .code_ = 1242,
                                                                                            .sql_state_ = std::pmr::string("21000", ruvia::process_resource()),
                                                                                            .error_after_rows_ = true},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("3", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("f1", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("f2", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[0].value_ == "1");
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::statement_failed);
    EXPECT_TRUE(outcomes[1].sql_state_ == "21000");
    EXPECT_TRUE(outcomes[2].value_ == "3");
    EXPECT_TRUE(outcomes[3].value_ == "f1");
    EXPECT_TRUE(outcomes[4].value_ == "f2");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT (SELECT v FROM t)", ruvia::process_resource()), std::pmr::string("prepare:SELECT (SELECT v FROM t)", ruvia::process_resource()),
                                                        std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("reset:1", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM f1", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM f2", ruvia::process_resource())},
                     ruvia::process_resource())));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A cached statement keeps the session state it was prepared in, so a change
// the session reports (here sql_mode: PIPES_AS_CONCAT, ANSI_QUOTES) drops
// every cached statement: it is closed on the server, runs as text, and is
// prepared anew under the new state on its second use after the change.
TEST(Ruvia, db_mariadb_session_change_drops_the_cached_statements) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer(
        std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())},
                                                                             {.column_ = std::pmr::string("", ruvia::process_resource()), .tracked_ = std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>>({{std::pmr::string("sql_mode", ruvia::process_resource()), std::pmr::string("PIPES_AS_CONCAT,ANSI_QUOTES,STRICT_TRANS_TABLES", ruvia::process_resource())}}, ruvia::process_resource())},
                                                                             {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("d", ruvia::process_resource())}, ruvia::process_resource())}},
            ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(!outcomes[2].error_.has_value());
    EXPECT_TRUE(outcomes[3].value_ == "c");
    EXPECT_TRUE(outcomes[4].value_ == "d");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT 'a' || 'b' AS v", ruvia::process_resource()), std::pmr::string("prepare:SELECT 'a' || 'b' AS v", ruvia::process_resource()),
                                                        std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("query:SET SESSION sql_mode = CONCAT(@@sql_mode, ',PIPES_AS_CONCAT,ANSI_QUOTES')", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT 'a' || 'b' AS v", ruvia::process_resource()), std::pmr::string("close:1", ruvia::process_resource()), std::pmr::string("prepare:SELECT 'a' || 'b' AS v", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource())},
                     ruvia::process_resource())));
    EXPECT_EQ(peer.maria_session_setups(), std::size_t{1});
}

// A rowless stream still executed SET before its shape was refused. A
// non-transactional session flag must not retire the connection, and its
// tracked sql_mode change must invalidate statements prepared in the old mode.
TEST(Ruvia, db_mariadb_rowless_stream_preserves_session_changes_and_invalidates_prepared_statements) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer(
        std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("0", ruvia::process_resource())}, ruvia::process_resource())},
                                                                             {.column_ = std::pmr::string("", ruvia::process_resource()), .status_ = 514, .tracked_ = std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>>({{std::pmr::string("sql_mode", ruvia::process_resource()), std::pmr::string("PIPES_AS_CONCAT,NO_BACKSLASH_ESCAPES", ruvia::process_resource())}}, ruvia::process_resource())},
                                                                             {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("ab", ruvia::process_resource())}, ruvia::process_resource()), .status_ = 514}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("ab", ruvia::process_resource())}, ruvia::process_resource()), .status_ = 514}},
            ruvia::process_resource()),
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::array<pipeline_test::outcome, 4> outcomes;
    bool refused = false;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query(handle, "SELECT 'a' || 'b' AS v", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT 'a' || 'b' AS v", outcomes[1]);
        try {
            (void)co_await handle.query_stream("SET SESSION sql_mode = 'PIPES_AS_CONCAT,NO_BACKSLASH_ESCAPES'");
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        co_await pipeline_test::query(handle, "SELECT 'a' || 'b' AS v", outcomes[2]);
        co_await pipeline_test::query(handle, "SELECT 'a' || 'b' AS v", outcomes[3]);
    });
    EXPECT_TRUE(refused);
    EXPECT_TRUE(outcomes[0].value_ == "0");
    EXPECT_TRUE(outcomes[1].value_ == "0");
    EXPECT_TRUE(outcomes[2].value_ == "ab");
    EXPECT_TRUE(outcomes[3].value_ == "ab");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT 'a' || 'b' AS v", ruvia::process_resource()), std::pmr::string("prepare:SELECT 'a' || 'b' AS v", ruvia::process_resource()),
                                                        std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("query:SET SESSION sql_mode = 'PIPES_AS_CONCAT,NO_BACKSLASH_ESCAPES'", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT 'a' || 'b' AS v", ruvia::process_resource()), std::pmr::string("close:1", ruvia::process_resource()), std::pmr::string("prepare:SELECT 'a' || 'b' AS v", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource())},
                     ruvia::process_resource())));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// While the session's client character set is not the utf8mb4 the
// connection opened with, nothing runs prepared: the binary protocol converts
// parameters otherwise than the text protocol does literals. Back on utf8mb4,
// statements are prepared again.
TEST(Ruvia, db_mariadb_statements_run_as_text_while_the_client_character_set_differs) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> gbk({{std::pmr::string("character_set_client", ruvia::process_resource()), std::pmr::string("gbk", ruvia::process_resource())}, {std::pmr::string("character_set_connection", ruvia::process_resource()), std::pmr::string("gbk", ruvia::process_resource())}, {std::pmr::string("character_set_results", ruvia::process_resource()), std::pmr::string("gbk", ruvia::process_resource())}}, ruvia::process_resource());
    const std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> utf8mb4({{std::pmr::string("character_set_client", ruvia::process_resource()), std::pmr::string("utf8mb4", ruvia::process_resource())},
                                                                                      {std::pmr::string("character_set_connection", ruvia::process_resource()), std::pmr::string("utf8mb4", ruvia::process_resource())}, {std::pmr::string("character_set_results", ruvia::process_resource()), std::pmr::string("utf8mb4", ruvia::process_resource())}},
        ruvia::process_resource());
    std::pmr::vector<reply> replies({{.column_ = std::pmr::string("", ruvia::process_resource()), .tracked_ = std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>>(gbk, ruvia::process_resource())}}, ruvia::process_resource());
    replies.insert(replies.end(), 3, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())});
    replies.push_back({.column_ = "", .tracked_ = utf8mb4});
    replies.insert(replies.end(), 2, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())});
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
        EXPECT_TRUE(outcomes[index].value_ == "v");
    }
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SET NAMES gbk", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("query:SET NAMES utf8mb4", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()),
                                                        std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource())},
                     ruvia::process_resource())));
}

// SET and USE change the session every cached statement was prepared in, so
// they run as text however often they are used; the change a SET reports
// drops the cached statements as any other does.
TEST(Ruvia, db_mariadb_set_and_use_run_as_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, {.column_ = std::pmr::string("", ruvia::process_resource())}, {.column_ = std::pmr::string("", ruvia::process_resource())}, {.column_ = std::pmr::string("", ruvia::process_resource())},
                                                                                        {.column_ = std::pmr::string("", ruvia::process_resource()), .tracked_ = std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>>({{std::pmr::string("sql_mode", ruvia::process_resource()), std::pmr::string("PIPES_AS_CONCAT", ruvia::process_resource())}}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("d", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
        EXPECT_TRUE(!outcomes[index].error_.has_value());
    }
    EXPECT_TRUE(outcomes[6].value_ == "c");
    EXPECT_TRUE(outcomes[7].value_ == "d");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()),
                                                        std::pmr::string("query:USE other", ruvia::process_resource()), std::pmr::string("query:USE other", ruvia::process_resource()), std::pmr::string("query:SET SESSION sql_mode = 'STRICT_TRANS_TABLES'", ruvia::process_resource()),
                                                        std::pmr::string("query:SET SESSION sql_mode = 'PIPES_AS_CONCAT'", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("close:1", ruvia::process_resource()),
                                                        std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:2:", ruvia::process_resource())},
                     ruvia::process_resource())));
}

// Either of character_set_client and character_set_connection alone off
// utf8mb4 keeps every statement on text: the binary protocol converts a
// parameter's invalid bytes otherwise than the text protocol does a literal's.
TEST(Ruvia, db_mariadb_one_character_set_variable_off_utf8mb4_runs_statements_as_text) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    const std::pmr::string invalid(
        "a\xff"
        "b",
        ruvia::process_resource());
    for (const auto* variable : {"character_set_client", "character_set_connection"}) {
        std::pmr::vector<reply> replies({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, {.column_ = std::pmr::string("", ruvia::process_resource()), .tracked_ = std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>>({{std::pmr::string(variable, ruvia::process_resource()), std::pmr::string("gbk", ruvia::process_resource())}}, ruvia::process_resource())}}, ruvia::process_resource());
        replies.insert(replies.end(), 3, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("x", ruvia::process_resource())}, ruvia::process_resource())});
        replies.push_back({.column_ = "", .tracked_ = {{variable, "utf8mb4"}}});
        replies.insert(replies.end(), 2, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("y", ruvia::process_resource())}, ruvia::process_resource())});
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
            co_await pipeline_test::execute(handle, std::pmr::string("SET SESSION ", ruvia::process_resource()) + variable + " = gbk", ignored);
            for (std::size_t index = 0; index < 3; ++index) {
                co_await query_with(handle, "SELECT ? AS v", outcomes[index], std::string_view(invalid));
            }
            co_await pipeline_test::execute(handle, std::pmr::string("SET SESSION ", ruvia::process_resource()) + variable + " = utf8mb4", ignored);
            for (std::size_t index = 3; index < 5; ++index) {
                co_await query_with(handle, "SELECT ? AS v", outcomes[index], std::string_view(invalid));
            }
        });
        for (std::size_t index = 0; index < 3; ++index) {
            EXPECT_TRUE(outcomes[index].value_ == "x");
        }
        EXPECT_TRUE(outcomes[3].value_ == "y");
        EXPECT_TRUE(outcomes[4].value_ == "y");
        const auto text =
            "query:SELECT 'a\xff"
            "b' AS v";
        EXPECT_TRUE((peer.maria_commands() ==
                     std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()),
                                                            std::pmr::string("query:SET SESSION ", ruvia::process_resource()) + variable + " = gbk", std::pmr::string(text, ruvia::process_resource()), std::pmr::string(text, ruvia::process_resource()), std::pmr::string(text, ruvia::process_resource()),
                                                            std::pmr::string("query:SET SESSION ", ruvia::process_resource()) + variable + " = utf8mb4", std::pmr::string(text, ruvia::process_resource()), std::pmr::string("close:1", ruvia::process_resource()), std::pmr::string("prepare:SELECT ? AS v", ruvia::process_resource()),
                                                            std::pmr::string("execute:2:s:a\xff"
                                                                             "b",
                                                                ruvia::process_resource())},
                         ruvia::process_resource())));
    }
}

// A server that does not track its session, or a session whose tracking a
// statement turned off (the server does not report that change), runs nothing
// prepared: a later change of its state could not be heard.
TEST(Ruvia, db_mariadb_untracked_session_runs_nothing_prepared) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    {
        peer_type peer(std::pmr::vector<reply>(3, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource()), peer_type::after_startup::idle, 1, false);
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
            EXPECT_TRUE(result.value_ == "v");
        }
        EXPECT_TRUE(peer.maria_session_setups() == 0);
        EXPECT_TRUE((peer.maria_commands() == std::pmr::vector<std::pmr::string>(3, "query:SELECT v FROM t", ruvia::process_resource())));
    }
    {
        std::pmr::vector<reply> replies({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())}, {.column_ = std::pmr::string("", ruvia::process_resource())}}, ruvia::process_resource());
        replies.insert(replies.end(), 2, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())});
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
        EXPECT_TRUE(!outcomes[2].error_.has_value());
        EXPECT_TRUE(outcomes[3].value_ == "v");
        EXPECT_TRUE(outcomes[4].value_ == "v");
        EXPECT_TRUE((peer.maria_commands() ==
                     std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()),
                                                            std::pmr::string("query:SET SESSION session_track_system_variables = ''", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()),
                                                            std::pmr::string("query:SELECT v FROM t", ruvia::process_resource())},
                         ruvia::process_resource())));
    }
}

// A statement that never runs prepared is remembered as such without taking
// one of the cache's places, also on its second use: a full cache of
// prepared statements outlives any number of CALLs, none evicted or closed.
// The first prepared execution after them reads the tracked variables back,
// since a procedure may have changed them unreported.
TEST(Ruvia, db_mariadb_statements_that_run_as_text_take_no_cache_places) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    constexpr std::size_t cached = 128;
    constexpr std::size_t calls = 130;
    std::pmr::vector<reply> replies(cached * 2 + calls * 2 + 1 + cached, reply{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("v", ruvia::process_resource())}, ruvia::process_resource())}, ruvia::process_resource());
    replies[cached * 2 + calls * 2] = {.rows_ = {std::pmr::string(server_tracked_list, ruvia::process_resource())}};
    peer_type peer(std::move(replies), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config(peer));
    std::size_t failures = 0;
    const auto select = [](std::size_t index) { return "SELECT v FROM t" + std::pmr::string(std::to_string(index), ruvia::process_resource()); };
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        const auto run = [&](std::pmr::string sql) -> task<void> {
            pipeline_test::outcome result;
            co_await pipeline_test::query(handle, sql, result);
            failures += result.value_ == "v" ? 0 : 1;
        };
        for (std::size_t index = 0; index < cached; ++index) {
            co_await run(select(index));
            co_await run(select(index));
        }
        for (std::size_t index = 0; index < calls; ++index) {
            co_await run("CALL p" + std::pmr::string(std::to_string(index), ruvia::process_resource()) + "()");
            co_await run("CALL p" + std::pmr::string(std::to_string(index), ruvia::process_resource()) + "()");
        }
        for (std::size_t index = 0; index < cached; ++index) {
            co_await run(select(index));
        }
    });
    EXPECT_EQ(failures, std::size_t{0});
    const auto commands = peer.maria_commands();
    EXPECT_EQ(commands.size(), std::size_t{cached * 3 + calls * 2 + 1 + cached});
    EXPECT_TRUE(std::ranges::none_of(commands, [](const std::pmr::string& command) { return command.starts_with("close:"); }));
    EXPECT_TRUE(commands[cached * 3 + calls * 2] == "query:SELECT @@session_track_system_variables");
    std::size_t unexpected = 0;
    for (std::size_t index = 0; index < cached; ++index) {
        unexpected += commands[cached * 3 + calls * 2 + 1 + index] == "execute:" + std::pmr::string(std::to_string(index + 1), ruvia::process_resource()) + ":" ? 0 : 1;
    }
    EXPECT_EQ(unexpected, std::size_t{0});
}

// Reports of variables no statement depends on leave the cache alone: with
// binary logging on, the server reports last_gtid after every committed
// write, and a cache dropped on each would close and prepare its statements
// anew all the time.
TEST(Ruvia, db_mariadb_reports_of_other_variables_keep_the_cached_statements) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.column_ = std::pmr::string("", ruvia::process_resource()), .affected_rows_ = 1, .tracked_ = std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>>({{std::pmr::string("last_gtid", ruvia::process_resource()), std::pmr::string("0-1-5", ruvia::process_resource())}}, ruvia::process_resource())},
                                                                                        {.column_ = std::pmr::string("", ruvia::process_resource()), .tracked_ = std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>>({{std::pmr::string("autocommit", ruvia::process_resource()), std::pmr::string("OFF", ruvia::process_resource())}}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[2].affected_rows_ == 1);
    EXPECT_TRUE(outcomes[4].value_ == "c");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()),
                                                        std::pmr::string("query:INSERT INTO t (v) VALUES ('w')", ruvia::process_resource()), std::pmr::string("query:SET autocommit = 0", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource())},
                     ruvia::process_resource())));
}

// A stored procedure may turn session tracking off without a report, so the
// first prepared execution after a CALL reads the tracked variables back:
// still tracked, the cached statement runs; no longer, every statement runs
// as text on that connection.
TEST(Ruvia, db_mariadb_call_has_the_tracked_variables_read_back) {
    using namespace ruvia;
    using namespace mariadb_statement_test;
    peer_type peer(std::pmr::vector<ruvia::test::backend_startup_peer::maria_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, {.column_ = std::pmr::string("", ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string(server_tracked_list, ruvia::process_resource())}, ruvia::process_resource())},
                                                                                        {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}, {.column_ = std::pmr::string("", ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("time_zone", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("d", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("e", ruvia::process_resource())}, ruvia::process_resource())}},
                       ruvia::process_resource()),
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
    EXPECT_TRUE(outcomes[3].value_ == "c");
    EXPECT_TRUE(outcomes[5].value_ == "d");
    EXPECT_TRUE(outcomes[6].value_ == "e");
    EXPECT_TRUE((peer.maria_commands() ==
                 std::pmr::vector<std::pmr::string>({std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("prepare:SELECT v FROM t", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()),
                                                        std::pmr::string("query:CALL p()", ruvia::process_resource()), std::pmr::string("query:SELECT @@session_track_system_variables", ruvia::process_resource()), std::pmr::string("execute:1:", ruvia::process_resource()), std::pmr::string("query:CALL p()", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT @@session_track_system_variables", ruvia::process_resource()), std::pmr::string("query:SELECT v FROM t", ruvia::process_resource()),
                                                        std::pmr::string("query:SELECT v FROM t", ruvia::process_resource())},
                     ruvia::process_resource())));
}
#endif
