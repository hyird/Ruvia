#include "test_harness.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <hiredis/hiredis.h>

#include "redis/core/RedisConfigValidation.h"
#include "redis/core/RedisProtocol.h"
#include "ruvia/redis/RedisTypes.h"

namespace {

using ruvia::RedisValue;
using ruvia::detail::appendRespCommand;
using ruvia::detail::hiredisReplyToValue;
using ruvia::detail::parseRedisBlockingPopReply;
using ruvia::detail::parseRedisHashScanResult;
using ruvia::detail::parseRedisKeyValueArray;
using ruvia::detail::parseRedisScanResult;
using ruvia::detail::parseRedisScoredArray;
using ruvia::detail::respCommandSerializedSize;

// Build a nil hiredis reply (e.g. a BLPOP timeout).
redisReply nilReply() {
    redisReply reply{};
    reply.type = REDIS_REPLY_NIL;
    return reply;
}

RedisValue toNilValue() {
    const auto reply = nilReply();
    return hiredisReplyToValue(reply, 0, 32, std::pmr::get_default_resource());
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
    RUVIA_CHECK_EQ(encode({"SET", "key", "val"}),
                   std::string("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$3\r\nval\r\n"));
    // A single-argument command and a zero-length argument.
    RUVIA_CHECK_EQ(encode({"PING"}), std::string("*1\r\n$4\r\nPING\r\n"));
    RUVIA_CHECK_EQ(encode({"GET", ""}), std::string("*2\r\n$3\r\nGET\r\n$0\r\n\r\n"));
}

RUVIA_TEST(resp_serialized_size_matches_written_output) {
    // The size hint is used to reserve buffer space, so it must exactly equal the
    // bytes appendRespCommand writes -- including multi-digit length prefixes and
    // the empty-argument case.
    const std::string mid(42, 'y');    // two-digit bulk length
    const std::string big(150, 'x');   // three-digit bulk length
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
    const auto scan = parseRedisScanResult(
        hiredisReplyToValue(reply, 0, 32, resource), resource);
    RUVIA_CHECK_EQ(scan.cursor(), std::uint64_t{10});
    RUVIA_CHECK_EQ(scan.values().size(), std::size_t{2});
    RUVIA_CHECK_EQ(scan.values()[0], std::string_view("key1"));
    RUVIA_CHECK_EQ(scan.values()[1], std::string_view("key2"));

    // A non-numeric cursor is a protocol error (guards parseRedisCursor).
    redisReply badCursor = stringReply("notacursor");
    redisReply* badRoot[] = {&badCursor, &inner};
    const auto badReply = arrayReply(badRoot, 2);
    RUVIA_CHECK(throwsOn([&] {
        (void)parseRedisScanResult(hiredisReplyToValue(badReply, 0, 32, resource), resource);
    }));

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
    const auto hscan = parseRedisHashScanResult(
        hiredisReplyToValue(reply, 0, 32, resource), resource);
    RUVIA_CHECK_EQ(hscan.cursor(), std::uint64_t{7});
    RUVIA_CHECK_EQ(hscan.entries().size(), std::size_t{2});
    RUVIA_CHECK_EQ(hscan.entries()[0].key(), std::string_view("field1"));
    RUVIA_CHECK_EQ(hscan.entries()[0].value(), std::string_view("value1"));
    RUVIA_CHECK_EQ(hscan.entries()[1].key(), std::string_view("field2"));

    // An odd-length inner array (a field with no value) is malformed.
    redisReply* oddInner[] = {&f1, &v1, &f2};
    redisReply oddArr = arrayReply(oddInner, 3);
    redisReply* oddRoot[] = {&cursor, &oddArr};
    const auto oddReply = arrayReply(oddRoot, 2);
    RUVIA_CHECK(throwsOn([&] {
        (void)parseRedisHashScanResult(hiredisReplyToValue(oddReply, 0, 32, resource), resource);
    }));
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

RUVIA_TEST(redis_config_validation_checks_every_field) {
    using ruvia::RedisConfig;
    using ruvia::detail::validateRedisConfig;
    using std::chrono::milliseconds;

    // A default config is valid (localhost:6379, pool 4, timeouts 0, depth 64).
    RUVIA_CHECK(!throwsOn([] { validateRedisConfig(RedisConfig{}); }));

    // Host, port, pool size and max array depth each have a required-value guard.
    RUVIA_CHECK(throwsOn([] { RedisConfig c; c.host.clear(); validateRedisConfig(c); }));
    RUVIA_CHECK(throwsOn([] { RedisConfig c; c.port = 0; validateRedisConfig(c); }));
    RUVIA_CHECK(throwsOn([] { RedisConfig c; c.poolSizePerWorker = 0; validateRedisConfig(c); }));
    RUVIA_CHECK(throwsOn([] { RedisConfig c; c.maxArrayDepth = 0; validateRedisConfig(c); }));

    // Every one of the three timeouts must be non-negative -- a negative value in
    // any of them is rejected (verifies the whole fold is wired, not just one).
    RUVIA_CHECK(throwsOn([] { RedisConfig c; c.connectTimeout = milliseconds(-1); validateRedisConfig(c); }));
    RUVIA_CHECK(throwsOn([] { RedisConfig c; c.commandTimeout = milliseconds(-1); validateRedisConfig(c); }));
    RUVIA_CHECK(throwsOn([] { RedisConfig c; c.acquireTimeout = milliseconds(-1); validateRedisConfig(c); }));
}
