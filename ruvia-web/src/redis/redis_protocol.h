#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/redis/redis.h"

struct redisReader;
struct redisReadTask;
struct redisReplyObjectFunctions;

#include <charconv>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <system_error>

#include "ruvia/web/redis/redis_session.h"

namespace ruvia::detail {

// Serializes a command into RESP multi-bulk form, appending directly to the
// reused connection write buffer. Both overloads avoid the extra heap
// allocation + copy that hiredis' redisFormatCommandArgv would impose; the
// pmr::string overload also lets owned-argument paths skip building an
// intermediate string_view vector.
[[nodiscard]] std::size_t resp_header_serialized_size(std::size_t arg_count);
[[nodiscard]] std::size_t resp_bulk_serialized_size(std::size_t arg_bytes);
void add_resp_serialized_size(std::size_t& current, std::size_t amount);
void append_resp_header(std::string& output, std::size_t arg_count);
void append_resp_bulk(std::string& output, std::string_view arg);
void serialize_resp_command(std::string& output, std::span<const std::string_view> args);
void serialize_resp_command(std::string& output, std::span<const std::string> args);
void serialize_resp_command(
    std::string& output, std::string_view first, std::span<const std::string_view> rest);
void append_resp_command(std::string& output, std::span<const std::string_view> args);
void append_resp_command(std::string& output, std::span<const std::string> args);
void append_resp_command(
    std::string& output, std::string_view first, std::span<const std::string_view> rest);
// Throws std::invalid_argument when args is empty, or std::length_error when
// the complete RESP command size cannot be represented by std::size_t.
[[nodiscard]] std::size_t resp_command_serialized_size(std::span<const std::string_view> args);
[[nodiscard]] std::size_t resp_command_serialized_size(std::span<const std::string> args);
[[nodiscard]] std::size_t resp_command_serialized_size(
    std::string_view first, std::span<const std::string_view> rest);
[[nodiscard]] const char* hiredis_reader_error(const redisReader& reader_value) noexcept;

// The only RESP reply construction path: hiredis' reader calls these object
// functions while parsing. The root lives in the connection's builder;
// payload and nested storage use the resource of the read in progress. One
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

    // Starts reading one reply: its owned payload uses `resource` and the
    // per-reply budget is restored.
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
        root_slot(redis_reply_builder& owner, redis_value&& value) noexcept
            : owner_(&owner),
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

}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::size_t redis_int_scratch_size = 20;
inline constexpr std::size_t redis_score_scratch_size = 64;

[[nodiscard]] inline std::string_view format_redis_int(
    std::span<char, redis_int_scratch_size> scratch, std::int64_t value) noexcept {
    const auto end = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value).ptr;
    return {scratch.data(), static_cast<std::size_t>(end - scratch.data())};
}

// Redis accepts "+inf"/"-inf" as sorted-set scores and range bounds, and
// replies them back, so the client round-trips infinities. NaN has no Redis
// spelling and is rejected.
[[nodiscard]] inline std::string_view format_redis_score(
    std::span<char, redis_score_scratch_size> scratch, double value) {
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
[[nodiscard]] redis_owned_command encode_redis_command(
    const enumerate_type& enumerate_arguments) {
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
    enumerate_arguments([&](std::string_view arg) { append_resp_bulk(command.wire_, arg); });
    return command;
}

[[nodiscard]] redis_owned_command make_owned_redis_command(
    std::initializer_list<std::string_view> args);

[[nodiscard]] bool redis_set_returns_previous(redis_set_previous_value_policy policy);

[[nodiscard]] redis_owned_command redis_command_with_keys(std::string_view command,
    std::span<const std::string_view> keys);
[[nodiscard]] redis_owned_command redis_mset_args(
    std::span<const std::pair<std::string_view, std::string_view>> items);
[[nodiscard]] redis_owned_command redis_set_args(std::string_view key,
    std::string_view value, const redis_set_options& options);
[[nodiscard]] redis_owned_command redis_hset_fields_args(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value);
[[nodiscard]] redis_owned_command redis_command_with_key_fields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields_value);
[[nodiscard]] redis_owned_command redis_eval_args(std::string_view command,
    std::string_view script, std::span<const std::string_view> keys,
    std::span<const std::string_view> argv);
[[nodiscard]] redis_owned_command redis_blocking_pop_args(std::string_view command,
    std::span<const std::string_view> keys, redis_block_wait wait);
[[nodiscard]] redis_owned_command redis_x_read_group_args(std::string_view group,
    std::string_view consumer, std::span<const redis_stream_read_view> streams,
    const redis_x_read_group_options& options);

}  // namespace ruvia::detail
