#include <hiredis.h>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/redis/redis_types.h"

#include "redis/redis_config_storage.h"
#include "redis/redis_config_validation.h"
#include "redis/redis_handle_helpers.h"
#include "redis/redis_protocol.h"
#include "redis/redis_types_access.h"
#include "test_harness.h"

namespace {

using ruvia::redis_value;
using ruvia::detail::append_redis_scan_options;
using ruvia::detail::append_resp_command;
using ruvia::detail::hiredis_reply_to_value;
using ruvia::detail::parse_redis_blocking_pop_reply;
using ruvia::detail::parse_redis_hash_scan_result;
using ruvia::detail::parse_redis_key_value_array;
using ruvia::detail::parse_redis_scan_result;
using ruvia::detail::parse_redis_scored_array;
using ruvia::detail::parse_redis_x_read_group_reply;
using ruvia::detail::redis_types_access;
using ruvia::detail::redis_value_array;
using ruvia::detail::redis_value_integer;
using ruvia::detail::redis_value_integer_bool;
using ruvia::detail::redis_value_string;
using ruvia::detail::resp_command_serialized_size;
using ruvia::detail::validate_redis_pooled_command;
using ruvia::testing::throws_on;

redis_value to_nil_value() {
    return redis_types_access::null_value(std::pmr::get_default_resource());
}

// Build a bulk-string hiredis reply pointing at `text` (borrowed, must outlive use).
redisReply string_reply(std::string_view text) {
    redisReply reply{};
    reply.type = REDIS_REPLY_STRING;
    reply.str = const_cast<char*>(text.data());
    reply.len = text.size();
    return reply;
}

// Build an array hiredis reply over `elements` (borrowed).
redisReply array_reply(redisReply** elements, std::size_t count) {
    redisReply reply{};
    reply.type = REDIS_REPLY_ARRAY;
    reply.elements = count;
    reply.element = elements;
    return reply;
}

// Convert a constructed hiredis array reply into a redis_value for the parsers.
redis_value to_value(redisReply** elements, std::size_t count) {
    const auto reply = array_reply(elements, count);
    return hiredis_reply_to_value(reply, 0, 32, std::pmr::get_default_resource());
}

// True only if fn throws ruvia::redis_error specifically. A std::logic_error (the
// raw accessors' exception) returns false, so this distinguishes "honors the
// redis_error contract" from "escapes it".
template <typename fn_type>
bool throws_redis_error(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const ruvia::redis_error&) {
        return true;
    } catch (...) {
        return false;
    }
}

std::span<const std::string_view> as_span(const std::vector<std::string_view>& args) {
    return std::span<const std::string_view>(args.data(), args.size());
}

std::string encode(const std::vector<std::string_view>& args) {
    std::pmr::string out(std::pmr::get_default_resource());
    append_resp_command(out, as_span(args));
    return std::string(out.data(), out.size());
}

}  // namespace

RUVIA_TEST(resp_command_encodes_multibulk_form) {
    // RESP2 multi-bulk: *<n> then $<len>\r\n<arg>\r\n per argument.
    RUVIA_CHECK_EQ(encode({"SET", "key", "val"}),
        std::string("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$3\r\nval\r\n"));
    // A single-argument command and a zero-length argument.
    RUVIA_CHECK_EQ(encode({"PING"}), std::string("*1\r\n$4\r\nPING\r\n"));
    RUVIA_CHECK_EQ(encode({"GET", ""}), std::string("*2\r\n$3\r\nGET\r\n$0\r\n\r\n"));
}

RUVIA_TEST(redis_set_options_build_one_valid_command_shape) {
    auto* resource = std::pmr::get_default_resource();

    const auto plain =
        ruvia::detail::redis_set_args("key", "value", ruvia::redis_set_options{}, resource);
    RUVIA_CHECK_EQ(plain.size(), std::size_t{3});
    RUVIA_CHECK_EQ(std::string_view(plain[0]), std::string_view("SET"));
    RUVIA_CHECK_EQ(std::string_view(plain[1]), std::string_view("key"));
    RUVIA_CHECK_EQ(std::string_view(plain[2]), std::string_view("value"));

    ruvia::redis_set_options expiring;
    expiring.condition_ = ruvia::redis_set_condition::if_absent;
    expiring.expiration_ = ruvia::redis_set_expiration::expires_after(std::chrono::milliseconds(1500));
    expiring.previous_value_ = ruvia::redis_set_previous_value_policy::return_value;
    const auto expiring_args = ruvia::detail::redis_set_args("key", "value", expiring, resource);
    constexpr std::array<std::string_view, 7> expected_expiring{
        "SET", "key", "value", "PX", "1500", "NX", "GET"};
    RUVIA_CHECK_EQ(expiring_args.size(), std::size_t{7});
    for (std::size_t i = 0; i < expiring_args.size(); ++i) {
        RUVIA_CHECK_EQ(std::string_view(expiring_args[i]), expected_expiring[i]);
    }

    ruvia::redis_set_options preserving;
    preserving.condition_ = ruvia::redis_set_condition::if_present;
    preserving.expiration_ = ruvia::redis_set_expiration::keep_existing();
    const auto preserving_args = ruvia::detail::redis_set_args("key", "value", preserving, resource);
    constexpr std::array<std::string_view, 5> expected_preserving{
        "SET", "key", "value", "XX", "KEEPTTL"};
    RUVIA_CHECK_EQ(preserving_args.size(), expected_preserving.size());
    for (std::size_t i = 0; i < preserving_args.size(); ++i) {
        RUVIA_CHECK_EQ(std::string_view(preserving_args[i]), expected_preserving[i]);
    }
}

RUVIA_TEST(redis_set_options_reject_invalid_condition) {
    ruvia::redis_set_options options;
    options.condition_ = static_cast<ruvia::redis_set_condition>(42);
    RUVIA_CHECK(throws_on([&] {
        (void)ruvia::detail::redis_set_args(
            "key", "value", options, std::pmr::get_default_resource());
    }));
}

RUVIA_TEST(redis_set_options_reject_invalid_previous_value_policy) {
    ruvia::redis_set_options options;
    options.previous_value_ = static_cast<ruvia::redis_set_previous_value_policy>(42);
    RUVIA_CHECK(throws_on([&] {
        (void)ruvia::detail::redis_set_args(
            "key", "value", options, std::pmr::get_default_resource());
    }));
}

RUVIA_TEST(resp_serialized_size_matches_written_output) {
    // The size hint is used to reserve buffer space, so it must exactly equal the
    // bytes append_resp_command writes -- including multi-digit length prefixes and
    // the empty-argument case.
    const std::string mid(42, 'y');   // two-digit bulk length
    const std::string big(150, 'x');  // three-digit bulk length
    const std::vector<std::vector<std::string_view>> cases = {
        {"PING"},
        {"SET", "key", "value"},
        {"GET", ""},
        {"MSET", "k1", "v1", "k2", "v2"},
        {"SETEX", "k", mid, big},
    };
    for (const auto& args : cases) {
        std::pmr::string out(std::pmr::get_default_resource());
        const auto span = as_span(args);
        const auto hinted = resp_command_serialized_size(span);
        append_resp_command(out, span);
        RUVIA_CHECK_EQ(hinted, out.size());
    }
}

RUVIA_TEST(resp_serialized_size_rejects_wrapped_argument_length) {
    const std::array<std::string_view, 1> args{
        std::string_view("x", std::numeric_limits<std::size_t>::max())};
    bool length_error = false;
    try {
        (void)resp_command_serialized_size(args);
    } catch (const std::length_error&) {
        length_error = true;
    } catch (...) {
    }
    RUVIA_CHECK(length_error);
}

RUVIA_TEST(redis_parse_key_value_array_pairs_and_rejects_odd_length) {
    auto* resource = std::pmr::get_default_resource();
    redisReply k1 = string_reply("field1");
    redisReply v1 = string_reply("value1");
    redisReply k2 = string_reply("field2");
    redisReply v2 = string_reply("value2");

    // An even-length array (e.g. HGETALL) yields the field/value pairs in order.
    redisReply* even[] = {&k1, &v1, &k2, &v2};
    const auto pairs = parse_redis_key_value_array(to_value(even, 4), resource, "hgetall");
    RUVIA_CHECK_EQ(pairs.size(), std::size_t{2});
    RUVIA_CHECK_EQ(pairs[0].key(), std::string_view("field1"));
    RUVIA_CHECK_EQ(pairs[0].value(), std::string_view("value1"));
    RUVIA_CHECK_EQ(pairs[1].key(), std::string_view("field2"));
    RUVIA_CHECK_EQ(pairs[1].value(), std::string_view("value2"));

    // An odd-length reply is malformed and must be rejected, not truncated.
    redisReply* odd[] = {&k1, &v1, &k2};
    RUVIA_CHECK(
        throws_on([&] { (void)parse_redis_key_value_array(to_value(odd, 3), resource, "hgetall"); }));
}

RUVIA_TEST(redis_parse_scored_array_parses_scores_and_rejects_odd_length) {
    auto* resource = std::pmr::get_default_resource();
    redisReply m1 = string_reply("member1");
    redisReply s1 = string_reply("1.5");
    redisReply m2 = string_reply("member2");
    redisReply s2 = string_reply("-2");

    // ZSCAN/ZRANGE WITHSCORES: member/score pairs; the score text becomes a double.
    redisReply* even[] = {&m1, &s1, &m2, &s2};
    const auto scored = parse_redis_scored_array(to_value(even, 4), resource);
    RUVIA_CHECK_EQ(scored.size(), std::size_t{2});
    RUVIA_CHECK_EQ(scored[0].value(), std::string_view("member1"));
    RUVIA_CHECK_EQ(scored[0].score(), 1.5);
    RUVIA_CHECK_EQ(scored[1].score(), -2.0);

    // Odd length -> rejected. A non-numeric score -> rejected.
    redisReply* odd[] = {&m1, &s1, &m2};
    RUVIA_CHECK(throws_on([&] { (void)parse_redis_scored_array(to_value(odd, 3), resource); }));
    redisReply bad_score = string_reply("notanumber");
    redisReply* bad[] = {&m1, &bad_score};
    RUVIA_CHECK(throws_on([&] { (void)parse_redis_scored_array(to_value(bad, 2), resource); }));
    redisReply infinite_score = string_reply("inf");
    redisReply* infinite[] = {&m1, &infinite_score};
    RUVIA_CHECK(throws_on([&] { (void)parse_redis_scored_array(to_value(infinite, 2), resource); }));
    redisReply nan_score = string_reply("nan");
    redisReply* nan[] = {&m1, &nan_score};
    RUVIA_CHECK(throws_on([&] { (void)parse_redis_scored_array(to_value(nan, 2), resource); }));
}

RUVIA_TEST(redis_parse_scan_result_reads_cursor_and_values) {
    auto* resource = std::pmr::get_default_resource();
    redisReply key1 = string_reply("key1");
    redisReply key2 = string_reply("key2");
    redisReply* inner_elems[] = {&key1, &key2};
    redisReply inner = array_reply(inner_elems, 2);

    // A SCAN reply is a 2-element array: [cursor-string, [elements...]].
    redisReply cursor_value = string_reply("10");
    redisReply* root[] = {&cursor_value, &inner};
    const auto reply = array_reply(root, 2);
    const auto scan = parse_redis_scan_result(hiredis_reply_to_value(reply, 0, 32, resource), resource);
    RUVIA_CHECK(!scan.done());
    RUVIA_CHECK_EQ(scan.next_cursor(), ruvia::detail::redis_types_access::scan_cursor(10));
    RUVIA_CHECK_EQ(scan.values().size(), std::size_t{2});
    RUVIA_CHECK_EQ(scan.values()[0], std::string_view("key1"));
    RUVIA_CHECK_EQ(scan.values()[1], std::string_view("key2"));

    redisReply terminal_cursor = string_reply("0");
    redisReply* terminal_root[] = {&terminal_cursor, &inner};
    const auto terminal_reply = array_reply(terminal_root, 2);
    const auto terminal =
        parse_redis_scan_result(hiredis_reply_to_value(terminal_reply, 0, 32, resource), resource);
    RUVIA_CHECK(terminal.done());
    RUVIA_CHECK(!terminal.next_cursor().has_value());

    // A non-numeric cursor is a protocol error (guards parse_redis_cursor).
    redisReply bad_cursor = string_reply("notacursor");
    redisReply* bad_root[] = {&bad_cursor, &inner};
    const auto bad_reply = array_reply(bad_root, 2);
    RUVIA_CHECK(throws_on([&] {
        (void)parse_redis_scan_result(hiredis_reply_to_value(bad_reply, 0, 32, resource), resource);
    }));

    // A root array that is not exactly two elements is rejected.
    redisReply* short_root[] = {&cursor_value};
    RUVIA_CHECK(throws_on([&] { (void)parse_redis_scan_result(to_value(short_root, 1), resource); }));
}

RUVIA_TEST(redis_parse_hash_scan_result_reads_field_value_pairs) {
    auto* resource = std::pmr::get_default_resource();
    redisReply f1 = string_reply("field1");
    redisReply v1 = string_reply("value1");
    redisReply f2 = string_reply("field2");
    redisReply v2 = string_reply("value2");
    redisReply* inner_elems[] = {&f1, &v1, &f2, &v2};
    redisReply inner = array_reply(inner_elems, 4);

    // HSCAN reply: [cursor, [field, value, field, value, ...]].
    redisReply cursor_value = string_reply("7");
    redisReply* root[] = {&cursor_value, &inner};
    const auto reply = array_reply(root, 2);
    const auto hscan =
        parse_redis_hash_scan_result(hiredis_reply_to_value(reply, 0, 32, resource), resource);
    RUVIA_CHECK(!hscan.done());
    RUVIA_CHECK_EQ(hscan.next_cursor(), ruvia::detail::redis_types_access::scan_cursor(7));
    RUVIA_CHECK_EQ(hscan.entries().size(), std::size_t{2});
    RUVIA_CHECK_EQ(hscan.entries()[0].key(), std::string_view("field1"));
    RUVIA_CHECK_EQ(hscan.entries()[0].value(), std::string_view("value1"));
    RUVIA_CHECK_EQ(hscan.entries()[1].key(), std::string_view("field2"));

    // An odd-length inner array (a field with no value) is malformed.
    redisReply* odd_inner[] = {&f1, &v1, &f2};
    redisReply odd_arr = array_reply(odd_inner, 3);
    redisReply* odd_root[] = {&cursor_value, &odd_arr};
    const auto odd_reply = array_reply(odd_root, 2);
    RUVIA_CHECK(throws_on([&] {
        (void)parse_redis_hash_scan_result(hiredis_reply_to_value(odd_reply, 0, 32, resource), resource);
    }));
}

RUVIA_TEST(redis_parse_blocking_pop_reply_handles_timeout_and_pair) {
    auto* resource = std::pmr::get_default_resource();
    // A nil reply is a BLPOP/BRPOP timeout -> nullopt, not an error.
    RUVIA_CHECK(!parse_redis_blocking_pop_reply(to_nil_value(), resource).has_value());

    // A [list-key, popped-value] pair yields the key/value.
    redisReply key = string_reply("mylist");
    redisReply item = string_reply("item");
    redisReply* pair[] = {&key, &item};
    const auto popped = parse_redis_blocking_pop_reply(to_value(pair, 2), resource);
    RUVIA_CHECK(popped.has_value());
    RUVIA_CHECK_EQ(popped->key(), std::string_view("mylist"));
    RUVIA_CHECK_EQ(popped->value(), std::string_view("item"));

    // A non-nil reply that is not a 2-element array is malformed.
    redisReply* single[] = {&key};
    RUVIA_CHECK(throws_on([&] { (void)parse_redis_blocking_pop_reply(to_value(single, 1), resource); }));
}

RUVIA_TEST(resp_command_rejects_empty_argument_list) {
    std::pmr::string out(std::pmr::get_default_resource());
    const std::span<const std::string_view> empty;
    bool threw = false;
    try {
        append_resp_command(out, empty);
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(redis_scan_count_distinguishes_absence_from_configured_zero) {
    auto* resource = std::pmr::get_default_resource();
    std::pmr::vector<std::pmr::string> args(resource);

    append_redis_scan_options(args, ruvia::redis_scan_options{}, resource);
    RUVIA_CHECK(args.empty());

    ruvia::redis_scan_options configured;
    configured.count_ = 25;
    append_redis_scan_options(args, configured, resource);
    RUVIA_CHECK_EQ(args.size(), std::size_t{2});
    RUVIA_CHECK_EQ(std::string_view(args[0]), std::string_view("COUNT"));
    RUVIA_CHECK_EQ(std::string_view(args[1]), std::string_view("25"));

    ruvia::redis_scan_options zero;
    zero.match_ = "user:*";
    zero.count_ = 0;
    const auto size_before_failure = args.size();
    RUVIA_CHECK(throws_on([&] { append_redis_scan_options(args, zero, resource); }));
    RUVIA_CHECK_EQ(args.size(), size_before_failure);
}

RUVIA_TEST(redis_config_validation_checks_every_field) {
    using ruvia::redis_config;
    using ruvia::detail::validate_redis_config;
    using std::chrono::milliseconds;

    // A default config is valid; absent timeouts are disabled explicitly.
    RUVIA_CHECK(!throws_on([] { validate_redis_config(redis_config{}); }));

    // Host, port, both pool sizes and max array depth each have a
    // required-value guard.
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.host_.clear();
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.port_ = 0;
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.pool_size_per_worker_ = 0;
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.blocking_pool_size_per_worker_ = 0;
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.max_array_depth_ = 0;
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.max_reply_bytes_ = 0;
        validate_redis_config(c);
    }));

    // Every configured timeout must be positive. Zero cannot silently recover the
    // former sentinel convention, and the whole fold must validate every field.
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.connect_timeout_ = milliseconds(0);
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.command_timeout_ = milliseconds(0);
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.acquire_timeout_ = milliseconds(0);
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.connect_timeout_ = milliseconds(-1);
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.command_timeout_ = milliseconds(-1);
        validate_redis_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        redis_config c;
        c.acquire_timeout_ = milliseconds(-1);
        validate_redis_config(c);
    }));
}

RUVIA_TEST(resp_command_bulk_strings_are_binary_safe) {
    // RESP multi-bulk length-prefixes every argument ($<len>\r\n<bytes>\r\n), so an
    // argument containing CRLF is carried verbatim inside its byte count -- it cannot
    // terminate the bulk early or inject a second command. This is the RESP
    // command-injection defense for attacker-influenced keys and values.
    RUVIA_CHECK_EQ(encode({"SET", "k", "a\r\nb"}),
        std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4\r\na\r\nb\r\n"));
    // An embedded NUL is likewise just another length-counted byte.
    RUVIA_CHECK_EQ(encode({"SET", "k", std::string_view("a\0b", 3)}),
        std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\na\0b\r\n", 29));
}

RUVIA_TEST(redis_wrong_reply_type_throws_redis_error_not_logic_error) {
    // A reply's RESP type is chosen by the (untrusted) server, so a type mismatch is
    // a protocol condition the caller catches via redis_error -- not a std::logic_error
    // that escapes `catch (const redis_error&)` and can std::terminate a coroutine.
    auto* res = std::pmr::get_default_resource();
    const auto str = redis_types_access::string_value("foo", res);
    const auto err = redis_types_access::error_value("ERR command failed", res);
    const auto num = redis_types_access::integer_value(5, res);

    RUVIA_CHECK(
        throws_redis_error([&] { (void)redis_value_integer(str); }));  // INCR answered with a string
    RUVIA_CHECK(
        throws_redis_error([&] { (void)redis_value_array(num); }));  // MGET answered with an integer
    RUVIA_CHECK(
        throws_redis_error([&] { (void)redis_value_string(num); }));  // GET answered with an integer
    RUVIA_CHECK(
        throws_redis_error([&] { (void)redis_value_string(err); }));  // GET answered with an error
    RUVIA_CHECK(throws_on([&] { (void)err.string(); }));
    RUVIA_CHECK(throws_on([&] { (void)str.error(); }));
    RUVIA_CHECK_EQ(err.error(), std::string_view("ERR command failed"));

    // End-to-end: a SCAN reply whose second element is an integer, not the value array.
    std::pmr::vector<redis_value> root(res);
    root.push_back(redis_types_access::string_value("0", res));
    root.push_back(redis_types_access::integer_value(7, res));
    const auto bad_scan = redis_types_access::array_value(std::move(root), res);
    RUVIA_CHECK(throws_redis_error([&] { (void)parse_redis_scan_result(bad_scan, res); }));

    // Correct types must still pass (no false rejections).
    RUVIA_CHECK(!throws_redis_error([&] { (void)redis_value_integer(num); }));
    RUVIA_CHECK(!throws_redis_error([&] { (void)redis_value_string(str); }));
}

RUVIA_TEST(redis_error_uses_runtime_error_message_and_stable_code) {
    const ruvia::redis_error error(ruvia::redis_error::code_type::timeout, "redis timed out");
    RUVIA_CHECK(error.code() == ruvia::redis_error::code_type::timeout);
    RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("redis timed out"));
}

RUVIA_TEST(hiredis_reply_to_value_rejects_nonempty_null_string_storage) {
    redisReply reply{};
    reply.type = REDIS_REPLY_STRING;
    reply.str = nullptr;
    reply.len = 1;

    RUVIA_CHECK(throws_redis_error(
        [&] { (void)hiredis_reply_to_value(reply, 0, 32, std::pmr::get_default_resource()); }));

    reply.len = 0;
    const auto empty = hiredis_reply_to_value(reply, 0, 32, std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(empty.string(), std::string_view{});
}

RUVIA_TEST(redis_integer_bool_rejects_non_boolean_integers) {
    auto* resource = std::pmr::get_default_resource();

    RUVIA_CHECK(!redis_value_integer_bool(redis_types_access::integer_value(0, resource)));
    RUVIA_CHECK(redis_value_integer_bool(redis_types_access::integer_value(1, resource)));

    RUVIA_CHECK(throws_redis_error(
        [&] { (void)redis_value_integer_bool(redis_types_access::integer_value(2, resource)); }));
    RUVIA_CHECK(throws_redis_error(
        [&] { (void)redis_value_integer_bool(redis_types_access::integer_value(-1, resource)); }));
}

RUVIA_TEST(redis_blocking_pop_uses_the_shared_block_wait_type) {
    using ruvia::detail::redis_blocking_pop_args;

    const std::array<std::string_view, 1> keys{"queue"};
    const auto finite = redis_blocking_pop_args("BLPOP", keys,
        ruvia::redis_block_wait::for_duration(std::chrono::milliseconds(1500)),
        std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string_view(finite.back()), std::string_view("1.500"));
    const auto infinite = redis_blocking_pop_args(
        "BRPOP", keys, ruvia::redis_block_wait::indefinitely(), std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string_view(infinite.back()), std::string_view("0"));
}

RUVIA_TEST(redis_xreadgroup_builds_group_block_and_parallel_stream_arguments) {
    auto* resource = std::pmr::get_default_resource();
    const std::array streams{
        ruvia::redis_stream_read_view{.stream_ = "orders", .id_ = ">"},
        ruvia::redis_stream_read_view{.stream_ = "retries", .id_ = "0"},
    };
    ruvia::redis_x_read_group_options options;
    options.count_ = 25;
    options.block_ = ruvia::redis_block_wait::for_duration(std::chrono::milliseconds(1500));
    options.acknowledgement_ = ruvia::redis_x_read_group_acknowledgement_policy::no_ack;
    const auto args =
        ruvia::detail::redis_x_read_group_args("workers", "consumer-1", streams, options, resource);
    constexpr std::array<std::string_view, 14> expected{
        "XREADGROUP",
        "GROUP",
        "workers",
        "consumer-1",
        "COUNT",
        "25",
        "BLOCK",
        "1500",
        "NOACK",
        "STREAMS",
        "orders",
        "retries",
        ">",
        "0",
    };
    RUVIA_CHECK_EQ(args.size(), expected.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        RUVIA_CHECK_EQ(std::string_view(args[i]), expected[i]);
    }

    options.block_ = ruvia::redis_block_wait::indefinitely();
    const auto infinite =
        ruvia::detail::redis_x_read_group_args("workers", "consumer-1", streams, options, resource);
    RUVIA_CHECK_EQ(std::string_view(infinite[7]), std::string_view("0"));
}

RUVIA_TEST(redis_xreadgroup_rejects_invalid_acknowledgement_policy) {
    auto* resource = std::pmr::get_default_resource();
    const std::array streams{ruvia::redis_stream_read_view{.stream_ = "orders", .id_ = ">"}};
    ruvia::redis_x_read_group_options options;
    options.acknowledgement_ = static_cast<ruvia::redis_x_read_group_acknowledgement_policy>(42);
    RUVIA_CHECK(throws_on([&] {
        (void)ruvia::detail::redis_x_read_group_args(
            "workers", "consumer-1", streams, options, resource);
    }));
}

RUVIA_TEST(redis_raw_xreadgroup_block_detection_skips_group_and_consumer_names) {
    constexpr std::array<std::string_view, 7> group_named_block{
        "XREADGROUP",
        "GROUP",
        "BLOCK",
        "consumer",
        "STREAMS",
        "orders",
        ">",
    };
    RUVIA_CHECK(!validate_redis_pooled_command(group_named_block, true));

    constexpr std::array<std::string_view, 7> consumer_named_block{
        "XREADGROUP",
        "GROUP",
        "workers",
        "BLOCK",
        "STREAMS",
        "orders",
        ">",
    };
    RUVIA_CHECK(!validate_redis_pooled_command(consumer_named_block, true));
    RUVIA_CHECK(!throws_on([&] { (void)validate_redis_pooled_command(consumer_named_block, false); }));

    constexpr std::array<std::string_view, 9> blocking{
        "XREADGROUP",
        "GROUP",
        "workers",
        "consumer",
        "BLOCK",
        "0",
        "STREAMS",
        "orders",
        ">",
    };
    RUVIA_CHECK(validate_redis_pooled_command(blocking, true));
    RUVIA_CHECK(throws_on([&] { (void)validate_redis_pooled_command(blocking, false); }));
}

RUVIA_TEST(redis_xreadgroup_parser_owns_nested_stream_entries) {
    auto* resource = std::pmr::get_default_resource();
    std::pmr::vector<redis_value> fields(resource);
    fields.push_back(redis_types_access::string_value("type", resource));
    fields.push_back(redis_types_access::string_value("created", resource));

    std::pmr::vector<redis_value> entry(resource);
    entry.push_back(redis_types_access::string_value("1710000000000-0", resource));
    entry.push_back(redis_types_access::array_value(std::move(fields), resource));
    std::pmr::vector<redis_value> entries(resource);
    entries.push_back(redis_types_access::array_value(std::move(entry), resource));

    std::pmr::vector<redis_value> stream(resource);
    stream.push_back(redis_types_access::string_value("orders", resource));
    stream.push_back(redis_types_access::array_value(std::move(entries), resource));
    std::pmr::vector<redis_value> root(resource);
    root.push_back(redis_types_access::array_value(std::move(stream), resource));

    const auto parsed_value = parse_redis_x_read_group_reply(
        redis_types_access::array_value(std::move(root), resource), resource);
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK_EQ(parsed_value->streams().size(), std::size_t{1});
    RUVIA_CHECK_EQ(parsed_value->streams()[0].stream(), std::string_view("orders"));
    RUVIA_CHECK_EQ(parsed_value->streams()[0].entries().size(), std::size_t{1});
    RUVIA_CHECK_EQ(parsed_value->streams()[0].entries()[0].id(), std::string_view("1710000000000-0"));
    RUVIA_CHECK_EQ(parsed_value->streams()[0].entries()[0].fields()[0].key(), std::string_view("type"));
    RUVIA_CHECK_EQ(
        parsed_value->streams()[0].entries()[0].fields()[0].value(), std::string_view("created"));
    RUVIA_CHECK(!parse_redis_x_read_group_reply(to_nil_value(), resource).has_value());
}

RUVIA_TEST(redis_tls_configuration_requires_complete_credentials_and_owns_them) {
    using namespace ruvia;
    redis_config config;
    RUVIA_CHECK(config.tls_.mode_ == client_tls_mode::verify_identity);
    config.tls_.certificate_file_ = "client.pem";
    RUVIA_CHECK(testing::throws_on([&] { detail::validate_redis_config(config); }));
    config.tls_.private_key_file_ = "client.key";
    config.tls_.ca_file_ = "ca.pem";
    config.tls_.server_name_ = "redis.example.test";
    RUVIA_CHECK(!testing::throws_on([&] { detail::validate_redis_config(config); }));
    std::pmr::unsynchronized_pool_resource resource;
    detail::redis_config_storage storage(config, &resource);
    config.tls_.server_name_.assign("changed.test");
    config.tls_.ca_file_.clear();
    RUVIA_CHECK_EQ(std::string_view(storage.tls_.server_name_), std::string_view("redis.example.test"));
    RUVIA_CHECK_EQ(std::string_view(storage.tls_.ca_file_), std::string_view("ca.pem"));
    RUVIA_CHECK(storage.tls_.ca_file_.get_allocator().resource() == &resource);
    config.tls_.mode_ = client_tls_mode::disabled;
    RUVIA_CHECK(testing::throws_on([&] { detail::validate_redis_config(config); }));
    config.tls_ = {.mode_ = client_tls_mode::disabled};
    RUVIA_CHECK(!testing::throws_on([&] { detail::validate_redis_config(config); }));
}
