#pragma once

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

struct redisReader;
struct redisReadTask;
struct redisReplyObjectFunctions;

#include "redis_session.h"

namespace ruvia::detail {

// Serializes a command into RESP multi-bulk form, appending directly to the
// reused connection write buffer. Both overloads avoid the extra heap
// allocation + copy that hiredis' redisFormatCommandArgv would impose; the
// std::string overload also lets owned-argument paths skip building an
// intermediate string_view vector.
[[nodiscard]] std::size_t resp_header_serialized_size(std::size_t arg_count);
[[nodiscard]] std::size_t resp_bulk_serialized_size(std::size_t arg_bytes);
void add_resp_serialized_size(std::size_t& current, std::size_t amount);
void append_resp_header(std::string& output, std::size_t arg_count);
void append_resp_bulk(std::string& output, std::string_view arg);
void serialize_resp_command(std::string& output, std::span<const std::string_view> args);
void serialize_resp_command(std::string& output, std::span<const std::string> args);
void serialize_resp_command(std::string& output, std::string_view first, std::span<const std::string_view> rest);
void append_resp_command(std::string& output, std::span<const std::string_view> args);
void append_resp_command(std::string& output, std::span<const std::string> args);
void append_resp_command(std::string& output, std::string_view first, std::span<const std::string_view> rest);
// Throws std::invalid_argument when args is empty, or std::length_error when
// the complete RESP command size cannot be represented by std::size_t.
[[nodiscard]] std::size_t resp_command_serialized_size(std::span<const std::string_view> args);
[[nodiscard]] std::size_t resp_command_serialized_size(std::span<const std::string> args);
[[nodiscard]] std::size_t resp_command_serialized_size(std::string_view first, std::span<const std::string_view> rest);
[[nodiscard]] const char* hiredis_reader_error(const redisReader& reader_value) noexcept;

// The only RESP reply construction path: hiredis' reader calls these object
// functions while parsing. The root lives in the connection's builder;
// payload and nested storage are owned std::string / std::vector nodes. One
// builder serves one reader for its whole life and enforces the configured
// reply budget: total array elements (one per three reply bytes) and array
// nesting depth. A rejected or failed node makes the reader fail; the reader
// then frees the partial tree through the builder.
class redis_reply_builder final {
public:
    redis_reply_builder(std::optional<std::size_t> max_reply_bytes, std::size_t max_depth) noexcept;

    redis_reply_builder(const redis_reply_builder&) = delete;
    redis_reply_builder& operator=(const redis_reply_builder&) = delete;

    // Creates a reader whose replies this builder constructs; the builder must
    // outlive it. Throws redis_error when hiredis cannot allocate the reader.
    [[nodiscard]] redisReader* create_reader();

    // Starts reading one reply: restores the per-reply budget.
    void begin() noexcept;

    // After the reader failed, reports a rejection raised by this builder:
    // the budget's redis_error or the exception a node construction threw.
    // Returns when the failure was the reader's own protocol error.
    void throw_if_rejected();
    void clear() noexcept;
    // Takes ownership of a complete reply returned by redisReaderGetReply.
    [[nodiscard]] static redis_value take(void* reply) noexcept;

private:
    enum class rejection_type : std::uint8_t { none,
        elements,
        depth,
        failed };

    struct root_slot final {
        root_slot(redis_reply_builder& owner, redis_value&& value) noexcept :
            owner_(&owner),
            value_(std::move(value)) {}
        redis_reply_builder* owner_;
        redis_value value_;
    };

    template <typename factory_type>
    void* place(const redisReadTask* task_value, factory_type&& factory) noexcept;

    static void* create_string(const redisReadTask* task_value, char* data, std::size_t size) noexcept;
    static void* create_array(const redisReadTask* task_value, std::size_t elements) noexcept;
    static void* create_integer(const redisReadTask* task_value, long long value) noexcept;
    static void* create_double(const redisReadTask* task_value, double value, char* data, std::size_t size) noexcept;
    static void* create_nil(const redisReadTask* task_value) noexcept;
    static void* create_bool(const redisReadTask* task_value, int value) noexcept;
    static void free_object(void* reply) noexcept;

    static redisReplyObjectFunctions functions_;

    // One root may be under construction until hiredis completes or rejects it.
    std::optional<root_slot> root_;
    redisReader* reader_{nullptr};
    std::optional<std::size_t> max_reply_bytes_;
    std::size_t maximum_elements_;
    std::size_t remaining_elements_;
    std::size_t max_depth_;
    rejection_type rejection_{rejection_type::none};
    std::exception_ptr failure_;
};

inline constexpr std::size_t redis_score_scratch_size = 64;

// Redis accepts "+inf"/"-inf" as sorted-set scores and range bounds, and
// replies them back, so the client round-trips infinities. NaN has no Redis
// spelling and is rejected.
[[nodiscard]] inline std::string_view format_redis_score(std::span<char, redis_score_scratch_size> scratch, double value) {
    if (std::isinf(value)) {
        return value > 0 ? "+inf" : "-inf";
    }
    if (!std::isfinite(value)) {
        throw std::invalid_argument("redis sorted set score must not be NaN");
    }
    const auto [ptr, ec] = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("redis sorted set score is invalid");
    }
    return {scratch.data(), static_cast<std::size_t>(ptr - scratch.data())};
}

// Encodes the arguments `enumerate_arguments(emit)` produces, in order, as one
// RESP command. The enumeration runs twice, once to size the request and once
// to append it, so the argument layout is written in exactly one place.
template <typename enumerate_type>
[[nodiscard]] redis_owned_command encode_redis_command(const enumerate_type& enumerate_arguments) {
    std::size_t arg_count = 0;
    std::size_t bulk_bytes = 0;
    enumerate_arguments([&](std::string_view arg) {
        ++arg_count;
        add_resp_serialized_size(bulk_bytes, resp_bulk_serialized_size(arg.size()));
    });
    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, bulk_bytes);
    redis_owned_command command{std::string()};
    command.wire_.reserve(size);
    append_resp_header(command.wire_, arg_count);
    enumerate_arguments([&](std::string_view arg) {
        append_resp_bulk(command.wire_, arg);
    });
    return command;
}

[[nodiscard]] redis_owned_command make_owned_redis_command(std::initializer_list<std::string_view> args);

[[nodiscard]] bool redis_set_returns_previous(redis_set_previous_value_policy policy);

[[nodiscard]] redis_owned_command redis_command_with_keys(std::string_view command, std::span<const std::string_view> keys);
[[nodiscard]] redis_owned_command redis_mset_args(std::span<const std::pair<std::string_view, std::string_view>> items);
[[nodiscard]] redis_owned_command redis_set_args(std::string_view key, std::string_view value, const redis_set_options& options);
[[nodiscard]] redis_owned_command redis_hset_fields_args(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields_value);
[[nodiscard]] redis_owned_command redis_command_with_key_fields(std::string_view command, std::string_view key, std::span<const std::string_view> fields_value);
[[nodiscard]] redis_owned_command redis_eval_args(std::string_view command, std::string_view script, std::span<const std::string_view> keys, std::span<const std::string_view> argv);
[[nodiscard]] redis_owned_command redis_blocking_pop_args(std::string_view command, std::span<const std::string_view> keys, redis_block_wait wait);
[[nodiscard]] redis_owned_command redis_x_read_group_args(std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams, const redis_x_read_group_options& options);

struct redis_types_access final {
    [[nodiscard]] static redis_set_result set_result(bool applied, std::optional<std::string> previous = std::nullopt) {
        return redis_set_result(applied, std::move(previous));
    }

    [[nodiscard]] static constexpr redis_scan_cursor scan_cursor(std::uint64_t value) noexcept {
        return redis_scan_cursor(value);
    }

    [[nodiscard]] static constexpr std::uint64_t cursor_value(redis_scan_cursor cursor_value) noexcept {
        return cursor_value.value_;
    }

    [[nodiscard]] static redis_key_value key_value(std::string&& key, std::string&& value) noexcept {
        return redis_key_value(std::move(key), std::move(value));
    }

    [[nodiscard]] static redis_scored_value scored_value(std::string&& value, double score) noexcept {
        return redis_scored_value(std::move(value), score);
    }

    [[nodiscard]] static redis_scan_result scan_result() {
        return redis_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(redis_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<std::string>& values(redis_scan_result& result_value) noexcept {
        return result_value.values_;
    }

    [[nodiscard]] static redis_hash_scan_result hash_scan_result() {
        return redis_hash_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(redis_hash_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<redis_key_value>& entries(redis_hash_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_z_scan_result z_scan_result() {
        return redis_z_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(redis_z_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<redis_scored_value>& entries(redis_z_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_stream_entry stream_entry(std::string&& id) noexcept {
        return redis_stream_entry(std::move(id));
    }

    [[nodiscard]] static std::vector<redis_key_value>& fields(redis_stream_entry& entry_value) noexcept {
        return entry_value.fields_;
    }

    [[nodiscard]] static redis_stream_read_result stream_read_result(std::string&& stream) noexcept {
        return redis_stream_read_result(std::move(stream));
    }

    [[nodiscard]] static std::vector<redis_stream_entry>& entries(redis_stream_read_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_x_read_group_result xread_group_result() {
        return redis_x_read_group_result{};
    }

    [[nodiscard]] static std::vector<redis_stream_read_result>& streams(redis_x_read_group_result& result_value) noexcept {
        return result_value.streams_;
    }

    [[nodiscard]] static constexpr redis_ttl ttl(redis_ttl_state state_value, std::optional<std::chrono::milliseconds> remaining = std::nullopt) noexcept {
        return redis_ttl(state_value, remaining);
    }

    [[nodiscard]] static redis_pubsub_event pubsub_confirmation(redis_pubsub_confirmation_kind kind, std::optional<std::string_view> name, std::int64_t subscription_count) noexcept {
        return redis_pubsub_event(redis_pubsub_confirmation(kind, name, subscription_count));
    }
    [[nodiscard]] static redis_pubsub_event pubsub_message(std::string_view channel, std::optional<std::string_view> pattern, std::string_view payload) noexcept {
        return redis_pubsub_event(redis_pubsub_message(channel, pattern, payload));
    }

    [[nodiscard]] static redis_value null_value() {
        return redis_value::null_value();
    }

    [[nodiscard]] static redis_value string_value(std::string_view value) {
        return redis_value::string_value(value);
    }

    [[nodiscard]] static redis_value error_value(std::string_view value) {
        return redis_value::error_value(value);
    }

    [[nodiscard]] static redis_value integer_value(std::int64_t value) {
        return redis_value::integer_value(value);
    }

    [[nodiscard]] static redis_value array_value(std::size_t capacity) {
        return redis_value::array_value(capacity);
    }

    [[nodiscard]] static std::vector<redis_value>& elements(redis_value& value) noexcept {
        return value.array_;
    }

    [[nodiscard]] static std::string& text(redis_value& value) noexcept {
        return value.string_;
    }
};

// Moves the reply's string storage out after checking it is a string reply.
// Never returns a borrow into the discarded reply.
[[nodiscard]] std::string take_redis_string(redis_value&& value);
[[nodiscard]] bool redis_value_integer_bool(const redis_value& value);
[[nodiscard]] std::span<redis_value> redis_value_array(redis_value& value);
void throw_if_redis_transaction_reply_error(const redis_value& value, std::size_t index);

// Reply decoders. Those taking only (redis_value&&) are mappers for
// map_redis_command; results own their storage.
[[nodiscard]] std::optional<std::string> parse_redis_optional_string(redis_value&& value);
[[nodiscard]] std::int64_t parse_redis_integer(redis_value&& value);
[[nodiscard]] bool parse_redis_integer_bool(redis_value&& value);
// Accept only the OK / PONG status reply, compared case-insensitively.
void parse_redis_ok(redis_value&& value);
void parse_redis_pong(redis_value&& value);
[[nodiscard]] std::vector<bool> parse_redis_bool_array(redis_value&& value);
// Every element must be a string; a nil element is a protocol error.
[[nodiscard]] std::vector<std::string> parse_redis_string_array(redis_value&& value);
[[nodiscard]] std::vector<std::optional<std::string>> parse_redis_optional_string_array(redis_value&& value);
[[nodiscard]] std::vector<redis_scored_value> parse_redis_scored_array(redis_value&& value);
[[nodiscard]] redis_scan_result parse_redis_scan_result(redis_value&& value);
[[nodiscard]] redis_hash_scan_result parse_redis_hash_scan_result(redis_value&& value);
[[nodiscard]] redis_z_scan_result parse_redis_z_scan_result(redis_value&& value);
[[nodiscard]] std::optional<redis_key_value> parse_redis_blocking_pop_reply(redis_value&& value);
[[nodiscard]] std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(redis_value&& value);

[[nodiscard]] double parse_redis_double(std::string_view value, std::string_view context_value);
[[nodiscard]] std::optional<double> parse_redis_optional_double(redis_value&& value, std::string_view context_value);
[[nodiscard]] std::vector<redis_key_value> parse_redis_key_value_array(redis_value&& value, std::string_view context_value);
[[nodiscard]] redis_set_result parse_redis_set_reply(redis_value&& value, const redis_set_options& options);
// TTL/PTTL: -2 is a missing key, -1 a key without expiry.
[[nodiscard]] redis_ttl parse_redis_ttl(redis_value&& value, bool seconds_precision);
// A pushed reply of a subscribed connection. Views borrow `value`.
[[nodiscard]] redis_pubsub_event parse_redis_pubsub_event(const redis_value& value);
// The reply to an argument-less PING on a subscription connection.
[[nodiscard]] bool is_redis_subscription_pong(const redis_value& value);

}  // namespace ruvia::detail
