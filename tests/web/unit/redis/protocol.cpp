#include "test_harness.h"

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

#include <hiredis/hiredis.h>

#include "ruvia/web/detail/redis/RedisConfigValidation.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisProtocol.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace {

using ruvia::RedisValue;
using ruvia::detail::appendRedisScanOptions;
using ruvia::detail::appendRespCommand;
using ruvia::detail::hiredisReplyToValue;
using ruvia::detail::parseRedisBlockingPopReply;
using ruvia::detail::parseRedisXReadGroupReply;
using ruvia::detail::parseRedisHashScanResult;
using ruvia::detail::parseRedisKeyValueArray;
using ruvia::detail::parseRedisScanResult;
using ruvia::detail::parseRedisScoredArray;
using ruvia::detail::RedisTypesAccess;
using ruvia::detail::redisValueArray;
using ruvia::detail::redisValueIntegerBool;
using ruvia::detail::redisValueInteger;
using ruvia::detail::redisValueString;
using ruvia::detail::respCommandSerializedSize;
using ruvia::detail::validateRedisPooledCommand;

RedisValue toNilValue() {
    return RedisTypesAccess::nullValue(std::pmr::get_default_resource());
}

// Build a bulk-string hiredis reply pointing at `text` (borrowed, must outlive use).
redisReply stringReply(std::string_view text) {
    redisReply reply{};
    reply.type = REDIS_REPLY_STRING;
    reply.str = const_cast<char*>(text.data());
    reply.len = text.size();
    return reply;
}

// Build an array hiredis reply over `elements` (borrowed).
redisReply arrayReply(redisReply** elements, std::size_t count) {
    redisReply reply{};
    reply.type = REDIS_REPLY_ARRAY;
    reply.elements = count;
    reply.element = elements;
    return reply;
}

// Convert a constructed hiredis array reply into a RedisValue for the parsers.
RedisValue toValue(redisReply** elements, std::size_t count) {
    const auto reply = arrayReply(elements, count);
    return hiredisReplyToValue(reply, 0, 32, std::pmr::get_default_resource());
}

template <typename Fn>
bool throwsOn(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

// True only if fn throws ruvia::RedisError specifically. A std::logic_error (the
// raw accessors' exception) returns false, so this distinguishes "honors the
// RedisError contract" from "escapes it".
template <typename Fn>
bool throwsRedisError(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const ruvia::RedisError&) {
        return true;
    } catch (...) {
        return false;
    }
}

std::span<const std::string_view> asSpan(const std::vector<std::string_view>& args) {
    return std::span<const std::string_view>(args.data(), args.size());
}

std::string encode(const std::vector<std::string_view>& args) {
    std::pmr::string out(std::pmr::get_default_resource());
    appendRespCommand(out, asSpan(args));
    return std::string(out.data(), out.size());
}

}  // namespace

RUVIA_TEST(resp_command_encodes_multibulk_form) {
    // RESP2 multi-bulk: *<n> then $<len>\r\n<arg>\r\n per argument.
    RUVIA_CHECK_EQ(encode({"SET", "key", "val"}), std::string("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$3\r\nval\r\n"));
    // A single-argument command and a zero-length argument.
    RUVIA_CHECK_EQ(encode({"PING"}), std::string("*1\r\n$4\r\nPING\r\n"));
    RUVIA_CHECK_EQ(encode({"GET", ""}), std::string("*2\r\n$3\r\nGET\r\n$0\r\n\r\n"));
}

RUVIA_TEST(redis_set_options_build_one_valid_command_shape) {
    auto* resource = std::pmr::get_default_resource();

    const auto plain = ruvia::detail::redisSetArgs("key", "value", ruvia::RedisSetOptions{}, resource);
    RUVIA_CHECK_EQ(plain.size(), std::size_t{3});
    RUVIA_CHECK_EQ(std::string_view(plain[0]), std::string_view("SET"));
    RUVIA_CHECK_EQ(std::string_view(plain[1]), std::string_view("key"));
    RUVIA_CHECK_EQ(std::string_view(plain[2]), std::string_view("value"));

    ruvia::RedisSetOptions expiring;
    expiring.condition = ruvia::RedisSetCondition::kIfAbsent;
    expiring.expiration = ruvia::RedisSetExpiration::expiresAfter(std::chrono::milliseconds(1500));
    expiring.returnPrevious = true;
    const auto expiringArgs = ruvia::detail::redisSetArgs("key", "value", expiring, resource);
    constexpr std::array<std::string_view, 7> expectedExpiring{"SET", "key", "value", "PX", "1500", "NX", "GET"};
    RUVIA_CHECK_EQ(expiringArgs.size(), std::size_t{7});
    for (std::size_t i = 0; i < expiringArgs.size(); ++i) {
        RUVIA_CHECK_EQ(std::string_view(expiringArgs[i]), expectedExpiring[i]);
    }

    ruvia::RedisSetOptions preserving;
    preserving.condition = ruvia::RedisSetCondition::kIfPresent;
    preserving.expiration = ruvia::RedisSetExpiration::keepExisting();
    const auto preservingArgs = ruvia::detail::redisSetArgs("key", "value", preserving, resource);
    constexpr std::array<std::string_view, 5> expectedPreserving{"SET", "key", "value", "XX", "KEEPTTL"};
    RUVIA_CHECK_EQ(preservingArgs.size(), expectedPreserving.size());
    for (std::size_t i = 0; i < preservingArgs.size(); ++i) {
        RUVIA_CHECK_EQ(std::string_view(preservingArgs[i]), expectedPreserving[i]);
    }
}

RUVIA_TEST(redis_set_options_reject_invalid_condition) {
    ruvia::RedisSetOptions options;
    options.condition = static_cast<ruvia::RedisSetCondition>(42);
    RUVIA_CHECK(throwsOn([&] { (void)ruvia::detail::redisSetArgs("key", "value", options, std::pmr::get_default_resource()); }));
}

RUVIA_TEST(resp_serialized_size_matches_written_output) {
    // The size hint is used to reserve buffer space, so it must exactly equal the
    // bytes appendRespCommand writes -- including multi-digit length prefixes and
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
        const auto span = asSpan(args);
        const auto hinted = respCommandSerializedSize(span);
        appendRespCommand(out, span);
        RUVIA_CHECK_EQ(hinted, out.size());
    }
}

RUVIA_TEST(resp_serialized_size_rejects_wrapped_argument_length) {
    const std::array<std::string_view, 1> args{std::string_view("x", std::numeric_limits<std::size_t>::max())};
    bool lengthError = false;
    try {
        (void)respCommandSerializedSize(args);
    } catch (const std::length_error&) {
        lengthError = true;
    } catch (...) {
    }
    RUVIA_CHECK(lengthError);
}

RUVIA_TEST(redis_parse_key_value_array_pairs_and_rejects_odd_length) {
    auto* resource = std::pmr::get_default_resource();
    redisReply k1 = stringReply("field1");
    redisReply v1 = stringReply("value1");
    redisReply k2 = stringReply("field2");
    redisReply v2 = stringReply("value2");

    // An even-length array (e.g. HGETALL) yields the field/value pairs in order.
    redisReply* even[] = {&k1, &v1, &k2, &v2};
    const auto pairs = parseRedisKeyValueArray(toValue(even, 4), resource, "hgetall");
    RUVIA_CHECK_EQ(pairs.size(), std::size_t{2});
    RUVIA_CHECK_EQ(pairs[0].key(), std::string_view("field1"));
    RUVIA_CHECK_EQ(pairs[0].value(), std::string_view("value1"));
    RUVIA_CHECK_EQ(pairs[1].key(), std::string_view("field2"));
    RUVIA_CHECK_EQ(pairs[1].value(), std::string_view("value2"));

    // An odd-length reply is malformed and must be rejected, not truncated.
    redisReply* odd[] = {&k1, &v1, &k2};
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisKeyValueArray(toValue(odd, 3), resource, "hgetall"); }));
}

RUVIA_TEST(redis_parse_scored_array_parses_scores_and_rejects_odd_length) {
    auto* resource = std::pmr::get_default_resource();
    redisReply m1 = stringReply("member1");
    redisReply s1 = stringReply("1.5");
    redisReply m2 = stringReply("member2");
    redisReply s2 = stringReply("-2");

    // ZSCAN/ZRANGE WITHSCORES: member/score pairs; the score text becomes a double.
    redisReply* even[] = {&m1, &s1, &m2, &s2};
    const auto scored = parseRedisScoredArray(toValue(even, 4), resource);
    RUVIA_CHECK_EQ(scored.size(), std::size_t{2});
    RUVIA_CHECK_EQ(scored[0].value(), std::string_view("member1"));
    RUVIA_CHECK_EQ(scored[0].score(), 1.5);
    RUVIA_CHECK_EQ(scored[1].score(), -2.0);

    // Odd length -> rejected. A non-numeric score -> rejected.
    redisReply* odd[] = {&m1, &s1, &m2};
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisScoredArray(toValue(odd, 3), resource); }));
    redisReply badScore = stringReply("notanumber");
    redisReply* bad[] = {&m1, &badScore};
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisScoredArray(toValue(bad, 2), resource); }));
    redisReply infiniteScore = stringReply("inf");
    redisReply* infinite[] = {&m1, &infiniteScore};
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisScoredArray(toValue(infinite, 2), resource); }));
    redisReply nanScore = stringReply("nan");
    redisReply* nan[] = {&m1, &nanScore};
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisScoredArray(toValue(nan, 2), resource); }));
}

RUVIA_TEST(redis_parse_scan_result_reads_cursor_and_values) {
    auto* resource = std::pmr::get_default_resource();
    redisReply key1 = stringReply("key1");
    redisReply key2 = stringReply("key2");
    redisReply* innerElems[] = {&key1, &key2};
    redisReply inner = arrayReply(innerElems, 2);

    // A SCAN reply is a 2-element array: [cursor-string, [elements...]].
    redisReply cursor = stringReply("10");
    redisReply* root[] = {&cursor, &inner};
    const auto reply = arrayReply(root, 2);
    const auto scan = parseRedisScanResult(hiredisReplyToValue(reply, 0, 32, resource), resource);
    RUVIA_CHECK(!scan.done());
    RUVIA_CHECK_EQ(scan.nextCursor(), ruvia::detail::RedisTypesAccess::scanCursor(10));
    RUVIA_CHECK_EQ(scan.values().size(), std::size_t{2});
    RUVIA_CHECK_EQ(scan.values()[0], std::string_view("key1"));
    RUVIA_CHECK_EQ(scan.values()[1], std::string_view("key2"));

    redisReply terminalCursor = stringReply("0");
    redisReply* terminalRoot[] = {&terminalCursor, &inner};
    const auto terminalReply = arrayReply(terminalRoot, 2);
    const auto terminal = parseRedisScanResult(hiredisReplyToValue(terminalReply, 0, 32, resource), resource);
    RUVIA_CHECK(terminal.done());
    RUVIA_CHECK(!terminal.nextCursor().has_value());

    // A non-numeric cursor is a protocol error (guards parseRedisCursor).
    redisReply badCursor = stringReply("notacursor");
    redisReply* badRoot[] = {&badCursor, &inner};
    const auto badReply = arrayReply(badRoot, 2);
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisScanResult(hiredisReplyToValue(badReply, 0, 32, resource), resource); }));

    // A root array that is not exactly two elements is rejected.
    redisReply* shortRoot[] = {&cursor};
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisScanResult(toValue(shortRoot, 1), resource); }));
}

RUVIA_TEST(redis_parse_hash_scan_result_reads_field_value_pairs) {
    auto* resource = std::pmr::get_default_resource();
    redisReply f1 = stringReply("field1");
    redisReply v1 = stringReply("value1");
    redisReply f2 = stringReply("field2");
    redisReply v2 = stringReply("value2");
    redisReply* innerElems[] = {&f1, &v1, &f2, &v2};
    redisReply inner = arrayReply(innerElems, 4);

    // HSCAN reply: [cursor, [field, value, field, value, ...]].
    redisReply cursor = stringReply("7");
    redisReply* root[] = {&cursor, &inner};
    const auto reply = arrayReply(root, 2);
    const auto hscan = parseRedisHashScanResult(hiredisReplyToValue(reply, 0, 32, resource), resource);
    RUVIA_CHECK(!hscan.done());
    RUVIA_CHECK_EQ(hscan.nextCursor(), ruvia::detail::RedisTypesAccess::scanCursor(7));
    RUVIA_CHECK_EQ(hscan.entries().size(), std::size_t{2});
    RUVIA_CHECK_EQ(hscan.entries()[0].key(), std::string_view("field1"));
    RUVIA_CHECK_EQ(hscan.entries()[0].value(), std::string_view("value1"));
    RUVIA_CHECK_EQ(hscan.entries()[1].key(), std::string_view("field2"));

    // An odd-length inner array (a field with no value) is malformed.
    redisReply* oddInner[] = {&f1, &v1, &f2};
    redisReply oddArr = arrayReply(oddInner, 3);
    redisReply* oddRoot[] = {&cursor, &oddArr};
    const auto oddReply = arrayReply(oddRoot, 2);
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisHashScanResult(hiredisReplyToValue(oddReply, 0, 32, resource), resource); }));
}

RUVIA_TEST(redis_parse_blocking_pop_reply_handles_timeout_and_pair) {
    auto* resource = std::pmr::get_default_resource();
    // A nil reply is a BLPOP/BRPOP timeout -> nullopt, not an error.
    RUVIA_CHECK(!parseRedisBlockingPopReply(toNilValue(), resource).has_value());

    // A [list-key, popped-value] pair yields the key/value.
    redisReply key = stringReply("mylist");
    redisReply item = stringReply("item");
    redisReply* pair[] = {&key, &item};
    const auto popped = parseRedisBlockingPopReply(toValue(pair, 2), resource);
    RUVIA_CHECK(popped.has_value());
    RUVIA_CHECK_EQ(popped->key(), std::string_view("mylist"));
    RUVIA_CHECK_EQ(popped->value(), std::string_view("item"));

    // A non-nil reply that is not a 2-element array is malformed.
    redisReply* single[] = {&key};
    RUVIA_CHECK(throwsOn([&] { (void)parseRedisBlockingPopReply(toValue(single, 1), resource); }));
}

RUVIA_TEST(resp_command_rejects_empty_argument_list) {
    std::pmr::string out(std::pmr::get_default_resource());
    const std::span<const std::string_view> empty;
    bool threw = false;
    try {
        appendRespCommand(out, empty);
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(redis_scan_count_distinguishes_absence_from_configured_zero) {
    auto* resource = std::pmr::get_default_resource();
    std::pmr::vector<std::pmr::string> args(resource);

    appendRedisScanOptions(args, ruvia::RedisScanOptions{}, resource);
    RUVIA_CHECK(args.empty());

    ruvia::RedisScanOptions configured;
    configured.count = 25;
    appendRedisScanOptions(args, configured, resource);
    RUVIA_CHECK_EQ(args.size(), std::size_t{2});
    RUVIA_CHECK_EQ(std::string_view(args[0]), std::string_view("COUNT"));
    RUVIA_CHECK_EQ(std::string_view(args[1]), std::string_view("25"));

    ruvia::RedisScanOptions zero;
    zero.match = "user:*";
    zero.count = 0;
    const auto sizeBeforeFailure = args.size();
    RUVIA_CHECK(throwsOn([&] { appendRedisScanOptions(args, zero, resource); }));
    RUVIA_CHECK_EQ(args.size(), sizeBeforeFailure);
}

RUVIA_TEST(redis_config_validation_checks_every_field) {
    using ruvia::RedisConfig;
    using ruvia::detail::validateRedisConfig;
    using std::chrono::milliseconds;

    static_assert(std::same_as<decltype(RedisConfig{}.connectTimeout), std::optional<milliseconds>>);
    static_assert(std::same_as<decltype(RedisConfig{}.commandTimeout), std::optional<milliseconds>>);
    static_assert(std::same_as<decltype(RedisConfig{}.acquireTimeout), std::optional<milliseconds>>);
    static_assert(std::same_as<decltype(RedisConfig{}.maxReplyBytes), std::optional<std::size_t>>);

    // A default config is valid; absent timeouts are disabled explicitly.
    RUVIA_CHECK(!throwsOn([] { validateRedisConfig(RedisConfig{}); }));

    // Host, port, both pool sizes and max array depth each have a
    // required-value guard.
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.host.clear();
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.port = 0;
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.poolSizePerWorker = 0;
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.blockingPoolSizePerWorker = 0;
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.maxArrayDepth = 0;
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.maxReplyBytes = 0;
        validateRedisConfig(c);
    }));

    // Every configured timeout must be positive. Zero cannot silently recover the
    // former sentinel convention, and the whole fold must validate every field.
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.connectTimeout = milliseconds(0);
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.commandTimeout = milliseconds(0);
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.acquireTimeout = milliseconds(0);
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.connectTimeout = milliseconds(-1);
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.commandTimeout = milliseconds(-1);
        validateRedisConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        RedisConfig c;
        c.acquireTimeout = milliseconds(-1);
        validateRedisConfig(c);
    }));
}

RUVIA_TEST(resp_command_bulk_strings_are_binary_safe) {
    // RESP multi-bulk length-prefixes every argument ($<len>\r\n<bytes>\r\n), so an
    // argument containing CRLF is carried verbatim inside its byte count -- it cannot
    // terminate the bulk early or inject a second command. This is the RESP
    // command-injection defense for attacker-influenced keys and values.
    RUVIA_CHECK_EQ(encode({"SET", "k", "a\r\nb"}), std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4\r\na\r\nb\r\n"));
    // An embedded NUL is likewise just another length-counted byte.
    RUVIA_CHECK_EQ(encode({"SET", "k", std::string_view("a\0b", 3)}), std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\na\0b\r\n", 29));
}

RUVIA_TEST(redis_wrong_reply_type_throws_RedisError_not_logic_error) {
    // A reply's RESP type is chosen by the (untrusted) server, so a type mismatch is
    // a protocol condition the caller catches via RedisError -- not a std::logic_error
    // that escapes `catch (const RedisError&)` and can std::terminate a coroutine.
    auto* res = std::pmr::get_default_resource();
    const auto str = RedisTypesAccess::stringValue("foo", res);
    const auto err = RedisTypesAccess::errorValue("ERR command failed", res);
    const auto num = RedisTypesAccess::integerValue(5, res);

    RUVIA_CHECK(throwsRedisError([&] { (void)redisValueInteger(str); }));  // INCR answered with a string
    RUVIA_CHECK(throwsRedisError([&] { (void)redisValueArray(num); }));    // MGET answered with an integer
    RUVIA_CHECK(throwsRedisError([&] { (void)redisValueString(num); }));   // GET answered with an integer
    RUVIA_CHECK(throwsRedisError([&] { (void)redisValueString(err); }));   // GET answered with an error
    RUVIA_CHECK(throwsOn([&] { (void)err.string(); }));
    RUVIA_CHECK(throwsOn([&] { (void)str.error(); }));
    RUVIA_CHECK_EQ(err.error(), std::string_view("ERR command failed"));

    // End-to-end: a SCAN reply whose second element is an integer, not the value array.
    std::pmr::vector<RedisValue> root(res);
    root.push_back(RedisTypesAccess::stringValue("0", res));
    root.push_back(RedisTypesAccess::integerValue(7, res));
    const auto badScan = RedisTypesAccess::arrayValue(std::move(root), res);
    RUVIA_CHECK(throwsRedisError([&] { (void)parseRedisScanResult(badScan, res); }));

    // Correct types must still pass (no false rejections).
    RUVIA_CHECK(!throwsRedisError([&] { (void)redisValueInteger(num); }));
    RUVIA_CHECK(!throwsRedisError([&] { (void)redisValueString(str); }));
}

RUVIA_TEST(redis_error_uses_runtime_error_message_and_stable_code) {
    const ruvia::RedisError error(ruvia::RedisError::Code::kTimeout, "redis timed out");
    RUVIA_CHECK(error.code() == ruvia::RedisError::Code::kTimeout);
    RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("redis timed out"));
}

RUVIA_TEST(hiredis_reply_to_value_rejects_nonempty_null_string_storage) {
    redisReply reply{};
    reply.type = REDIS_REPLY_STRING;
    reply.str = nullptr;
    reply.len = 1;

    RUVIA_CHECK(throwsRedisError([&] { (void)hiredisReplyToValue(reply, 0, 32, std::pmr::get_default_resource()); }));

    reply.len = 0;
    const auto empty = hiredisReplyToValue(reply, 0, 32, std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(empty.string(), std::string_view{});
}

RUVIA_TEST(redis_integer_bool_rejects_non_boolean_integers) {
    auto* resource = std::pmr::get_default_resource();

    RUVIA_CHECK(!redisValueIntegerBool(RedisTypesAccess::integerValue(0, resource)));
    RUVIA_CHECK(redisValueIntegerBool(RedisTypesAccess::integerValue(1, resource)));

    RUVIA_CHECK(throwsRedisError([&] { (void)redisValueIntegerBool(RedisTypesAccess::integerValue(2, resource)); }));
    RUVIA_CHECK(throwsRedisError([&] { (void)redisValueIntegerBool(RedisTypesAccess::integerValue(-1, resource)); }));
}

RUVIA_TEST(redis_blocking_pop_uses_the_shared_block_wait_type) {
    using ruvia::detail::redisBlockingPopArgs;

    const std::array<std::string_view, 1> keys{"queue"};
    const auto finite = redisBlockingPopArgs(
        "BLPOP", keys, ruvia::RedisBlockWait::forDuration(std::chrono::milliseconds(1500)), std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string_view(finite.back()), std::string_view("1.500"));
    const auto infinite = redisBlockingPopArgs(
        "BRPOP", keys, ruvia::RedisBlockWait::indefinitely(), std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string_view(infinite.back()), std::string_view("0"));
}

RUVIA_TEST(redis_xreadgroup_builds_group_block_and_parallel_stream_arguments) {
    auto* resource = std::pmr::get_default_resource();
    const std::array streams{
        ruvia::RedisStreamReadView{.stream = "orders", .id = ">"},
        ruvia::RedisStreamReadView{.stream = "retries", .id = "0"},
    };
    ruvia::RedisXReadGroupOptions options;
    options.count = 25;
    options.block = ruvia::RedisBlockWait::forDuration(std::chrono::milliseconds(1500));
    options.noAck = true;
    const auto args = ruvia::detail::redisXReadGroupArgs("workers", "consumer-1", streams, options, resource);
    constexpr std::array<std::string_view, 14> expected{
        "XREADGROUP", "GROUP", "workers", "consumer-1", "COUNT", "25", "BLOCK", "1500", "NOACK", "STREAMS", "orders", "retries", ">", "0",
    };
    RUVIA_CHECK_EQ(args.size(), expected.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        RUVIA_CHECK_EQ(std::string_view(args[i]), expected[i]);
    }

    options.block = ruvia::RedisBlockWait::indefinitely();
    const auto infinite = ruvia::detail::redisXReadGroupArgs("workers", "consumer-1", streams, options, resource);
    RUVIA_CHECK_EQ(std::string_view(infinite[7]), std::string_view("0"));
}

RUVIA_TEST(redis_raw_xreadgroup_block_detection_skips_group_and_consumer_names) {
    constexpr std::array<std::string_view, 7> groupNamedBlock{
        "XREADGROUP", "GROUP", "BLOCK", "consumer", "STREAMS", "orders", ">",
    };
    RUVIA_CHECK(!validateRedisPooledCommand(groupNamedBlock, true));

    constexpr std::array<std::string_view, 7> consumerNamedBlock{
        "XREADGROUP", "GROUP", "workers", "BLOCK", "STREAMS", "orders", ">",
    };
    RUVIA_CHECK(!validateRedisPooledCommand(consumerNamedBlock, true));
    RUVIA_CHECK(!throwsOn([&] { (void)validateRedisPooledCommand(consumerNamedBlock, false); }));

    constexpr std::array<std::string_view, 9> blocking{
        "XREADGROUP", "GROUP", "workers", "consumer", "BLOCK", "0", "STREAMS", "orders", ">",
    };
    RUVIA_CHECK(validateRedisPooledCommand(blocking, true));
    RUVIA_CHECK(throwsOn([&] { (void)validateRedisPooledCommand(blocking, false); }));
}

RUVIA_TEST(redis_xreadgroup_parser_owns_nested_stream_entries) {
    auto* resource = std::pmr::get_default_resource();
    std::pmr::vector<RedisValue> fields(resource);
    fields.push_back(RedisTypesAccess::stringValue("type", resource));
    fields.push_back(RedisTypesAccess::stringValue("created", resource));

    std::pmr::vector<RedisValue> entry(resource);
    entry.push_back(RedisTypesAccess::stringValue("1710000000000-0", resource));
    entry.push_back(RedisTypesAccess::arrayValue(std::move(fields), resource));
    std::pmr::vector<RedisValue> entries(resource);
    entries.push_back(RedisTypesAccess::arrayValue(std::move(entry), resource));

    std::pmr::vector<RedisValue> stream(resource);
    stream.push_back(RedisTypesAccess::stringValue("orders", resource));
    stream.push_back(RedisTypesAccess::arrayValue(std::move(entries), resource));
    std::pmr::vector<RedisValue> root(resource);
    root.push_back(RedisTypesAccess::arrayValue(std::move(stream), resource));

    const auto parsed = parseRedisXReadGroupReply(RedisTypesAccess::arrayValue(std::move(root), resource), resource);
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK_EQ(parsed->streams().size(), std::size_t{1});
    RUVIA_CHECK_EQ(parsed->streams()[0].stream(), std::string_view("orders"));
    RUVIA_CHECK_EQ(parsed->streams()[0].entries().size(), std::size_t{1});
    RUVIA_CHECK_EQ(parsed->streams()[0].entries()[0].id(), std::string_view("1710000000000-0"));
    RUVIA_CHECK_EQ(parsed->streams()[0].entries()[0].fields()[0].key(), std::string_view("type"));
    RUVIA_CHECK_EQ(parsed->streams()[0].entries()[0].fields()[0].value(), std::string_view("created"));
    RUVIA_CHECK(!parseRedisXReadGroupReply(toNilValue(), resource).has_value());
}
