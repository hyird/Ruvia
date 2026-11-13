#if defined(RUVIA_ENABLE_REDIS)
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

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/error.hpp>
#include <asio/fwd.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_server.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/streaming.h"

#include "connection_lifecycle.h"
#include "context_access.h"
#include "http_client_pool.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db_pool.h"
#endif
#ifdef RUVIA_ENABLE_DATABASE
#include "db_pool.h"
#endif

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

namespace ruvia::detail {

struct redis_types_access final {
    [[nodiscard]] static redis_set_result set_result(bool applied,
        std::optional<std::string> previous = std::nullopt) {
        return redis_set_result(applied, std::move(previous));
    }

    [[nodiscard]] static constexpr redis_scan_cursor scan_cursor(std::uint64_t value) noexcept {
        return redis_scan_cursor(value);
    }

    [[nodiscard]] static constexpr std::uint64_t cursor_value(redis_scan_cursor cursor_value) noexcept {
        return cursor_value.value_;
    }

    [[nodiscard]] static redis_key_value key_value(
        std::string&& key, std::string&& value) noexcept {
        return redis_key_value(std::move(key), std::move(value));
    }

    [[nodiscard]] static redis_scored_value scored_value(
        std::string&& value, double score) noexcept {
        return redis_scored_value(std::move(value), score);
    }

    [[nodiscard]] static redis_scan_result scan_result() {
        return redis_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<std::string>& values(
        redis_scan_result& result_value) noexcept {
        return result_value.values_;
    }

    [[nodiscard]] static redis_hash_scan_result hash_scan_result() {
        return redis_hash_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_hash_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<redis_key_value>& entries(
        redis_hash_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_z_scan_result z_scan_result() {
        return redis_z_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_z_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<redis_scored_value>& entries(
        redis_z_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_stream_entry stream_entry(
        std::string&& id) noexcept {
        return redis_stream_entry(std::move(id));
    }

    [[nodiscard]] static std::vector<redis_key_value>& fields(redis_stream_entry& entry_value) noexcept {
        return entry_value.fields_;
    }

    [[nodiscard]] static redis_stream_read_result stream_read_result(
        std::string&& stream) noexcept {
        return redis_stream_read_result(std::move(stream));
    }

    [[nodiscard]] static std::vector<redis_stream_entry>& entries(
        redis_stream_read_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_x_read_group_result xread_group_result() {
        return redis_x_read_group_result{};
    }

    [[nodiscard]] static std::vector<redis_stream_read_result>& streams(
        redis_x_read_group_result& result_value) noexcept {
        return result_value.streams_;
    }

    [[nodiscard]] static constexpr redis_ttl ttl(redis_ttl_state state_value,
        std::optional<std::chrono::milliseconds> remaining = std::nullopt) noexcept {
        return redis_ttl(state_value, remaining);
    }

    [[nodiscard]] static redis_pubsub_event pubsub_confirmation(redis_pubsub_confirmation_kind kind,
        std::optional<std::string_view> name, std::int64_t subscription_count) noexcept {
        return redis_pubsub_event(redis_pubsub_confirmation(kind, name, subscription_count));
    }
    [[nodiscard]] static redis_pubsub_event pubsub_message(std::string_view channel,
        std::optional<std::string_view> pattern, std::string_view payload) noexcept {
        return redis_pubsub_event(redis_pubsub_message(channel, pattern, payload));
    }

    [[nodiscard]] static redis_value null_value() {
        return redis_value::null_value();
    }

    [[nodiscard]] static redis_value string_value(
        std::string_view value) {
        return redis_value::string_value(value);
    }

    [[nodiscard]] static redis_value error_value(
        std::string_view value) {
        return redis_value::error_value(value);
    }

    [[nodiscard]] static redis_value integer_value(
        std::int64_t value) {
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

    // The resource every allocation of `value` comes from.
};

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

// Consumes reply storage when its resource matches; otherwise owns a copy in
// the destination resource. Never returns a borrow into the discarded reply.
[[nodiscard]] std::string take_redis_string(redis_value&& value);
[[nodiscard]] bool redis_value_integer_bool(const redis_value& value);
[[nodiscard]] std::span<redis_value> redis_value_array(redis_value& value);
void throw_if_redis_transaction_reply_error(const redis_value& value, std::size_t index);

// Reply decoders. Those taking only (redis_value&&, memory_resource*) are
// mappers for map_redis_command; results own storage in `resource`.
[[nodiscard]] std::optional<std::string> parse_redis_optional_string(
    redis_value&& value);
[[nodiscard]] std::int64_t parse_redis_integer(redis_value&& value);
[[nodiscard]] bool parse_redis_integer_bool(redis_value&& value);
// Accept only the OK / PONG status reply, compared case-insensitively.
void parse_redis_ok(redis_value&& value);
void parse_redis_pong(redis_value&& value);
[[nodiscard]] std::vector<bool> parse_redis_bool_array(redis_value&& value);
// Every element must be a string; a nil element is a protocol error.
[[nodiscard]] std::vector<std::string> parse_redis_string_array(
    redis_value&& value);
[[nodiscard]] std::vector<std::optional<std::string>> parse_redis_optional_string_array(
    redis_value&& value);
[[nodiscard]] std::vector<redis_scored_value> parse_redis_scored_array(
    redis_value&& value);
[[nodiscard]] redis_scan_result parse_redis_scan_result(redis_value&& value);
[[nodiscard]] redis_hash_scan_result parse_redis_hash_scan_result(
    redis_value&& value);
[[nodiscard]] redis_z_scan_result parse_redis_z_scan_result(redis_value&& value);
[[nodiscard]] std::optional<redis_key_value> parse_redis_blocking_pop_reply(
    redis_value&& value);
[[nodiscard]] std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(
    redis_value&& value);

[[nodiscard]] double parse_redis_double(std::string_view value, std::string_view context_value);
[[nodiscard]] std::optional<double> parse_redis_optional_double(redis_value&& value, std::string_view context_value);
[[nodiscard]] std::vector<redis_key_value> parse_redis_key_value_array(
    redis_value&& value, std::string_view context_value);
[[nodiscard]] redis_set_result parse_redis_set_reply(
    redis_value&& value, const redis_set_options& options);
// TTL/PTTL: -2 is a missing key, -1 a key without expiry.
[[nodiscard]] redis_ttl parse_redis_ttl(redis_value&& value, bool seconds_precision);
// A pushed reply of a subscribed connection. Views borrow `value`.
[[nodiscard]] redis_pubsub_event parse_redis_pubsub_event(const redis_value& value);
// The reply to an argument-less PING on a subscription connection.
[[nodiscard]] bool is_redis_subscription_pong(const redis_value& value);

}  // namespace ruvia::detail

namespace ruvia::detail {

inline void validate_redis_config(const redis_config& config) {
    validate_client_tls_config(config.tls_);
    validate_client_origin_host(config.host_, "redis host must not be empty", "redis host is invalid");
    detail::ensure_non_zero_port(config.port_.value(), "redis port must not be zero");
    detail::ensure_positive_size(config.pool_size_per_worker_, "redis pool size must be greater than zero");
    detail::ensure_positive_size(
        config.blocking_pool_size_per_worker_, "redis blocking pool size must be greater than zero");
    detail::ensure_positive_optional_durations("configured redis timeouts must be greater than zero",
        config.connect_timeout_, config.command_timeout_, config.acquire_timeout_);
    detail::ensure_positive_size(config.max_array_depth_, "redis max array depth must be greater than zero");
    detail::ensure_positive_optional_size(
        config.max_reply_bytes_, "configured redis reply byte limit must be greater than zero");
    validate_client_tcp_config(config.tcp_);
    detail::ensure_positive_optional_duration(
        config.subscription_ping_interval_, "redis subscription ping interval must be greater than zero");
}

// PMR-owned runtime form of the ordinary public redis_config value.
struct redis_config_storage final {
    redis_config_storage(const redis_config& source_value)
        : redis_config_storage(
              validated_config_tag_type{}, validate(source_value)) {}

    redis_config_storage(const redis_config_storage& source_value)
        : redis_config_storage(validated_config_tag_type{}, source_value) {}

    std::string host_;
    std::uint16_t port_;
    std::string username_;
    std::string password_;
    client_tls_config_storage tls_;
    std::uint32_t database_;
    std::size_t pool_size_per_worker_;
    std::size_t blocking_pool_size_per_worker_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> command_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<std::size_t> max_reply_bytes_;
    std::size_t max_array_depth_;
    client_tcp_config tcp_;
    std::optional<timeout_duration> subscription_ping_interval_;

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static const redis_config& validate(const redis_config& source_value) {
        validate_redis_config(source_value);
        return source_value;
    }

    [[nodiscard]] static std::uint16_t resolved_port(const redis_config& source) noexcept {
        return source.port_.value();
    }
    [[nodiscard]] static std::uint16_t resolved_port(const redis_config_storage& source) noexcept {
        return source.port_;
    }

    // Public configuration parses the TLS files every pool's context
    // installs; allocator-rebound copies share them.
    template <typename source_type>
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const source_type& source_value) {
        if constexpr (std::same_as<source_type, redis_config>) {
            return {source_value.tls_, client_tls_files::parse};
        } else {
            return {source_value.tls_};
        }
    }

    // Public configuration validation and allocator-rebound copies enter the
    // same owning construction path; no intermediate string copies or views.
    template <typename source_type>
        requires(std::same_as<source_type, redis_config> ||
                    std::same_as<source_type, redis_config_storage>)
    redis_config_storage(
        validated_config_tag_type, const source_type& source_value)
        : host_(source_value.host_),
          port_(resolved_port(source_value)),
          username_(source_value.username_),
          password_(source_value.password_),
          tls_(tls_storage(source_value)),
          database_(source_value.database_),
          pool_size_per_worker_(source_value.pool_size_per_worker_),
          blocking_pool_size_per_worker_(source_value.blocking_pool_size_per_worker_),
          connect_timeout_(source_value.connect_timeout_),
          command_timeout_(source_value.command_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          max_reply_bytes_(source_value.max_reply_bytes_),
          max_array_depth_(source_value.max_array_depth_),
          tcp_(source_value.tcp_),
          subscription_ping_interval_(source_value.subscription_ping_interval_) {}
};

using redis_definition_type = named_capability_definition<redis_config_storage>;

}  // namespace ruvia::detail

struct redisReader;

namespace ruvia::detail {

struct redis_reader_deleter {
    void operator()(redisReader* reader) const noexcept;
};

class redis_reply_builder;

inline constexpr std::size_t redis_read_buffer_bytes = 8192;
// Serialized request capacity retained after a write's transport borrow ends.
// Larger buffers return to their PMR resource, including on failure.
inline constexpr std::size_t redis_retained_write_buffer_bytes = std::size_t{64} * 1024;

// A credential request (AUTH) is wiped from the write buffer once written, so
// the retained buffer never holds a password after the write completes.
enum class redis_request_content : bool { ordinary,
    credential };

// One Redis connection: the shared outbound transport plus the RESP request
// and reply buffers. Pool slots and subscriptions each own one. Address-stable:
// the transport's pending I/O and timers and the reader's object functions
// borrow it. The owner serializes requests into write_buffer(), sends them
// with write_request() and reads replies one at a time; a subscription keeps
// reading pushed replies without writing.
//
// Every failed transport result is mapped to redis_error here. An owner
// abort (cancelled or closing) is recorded on the transport and checked
// after each wait, so cancellation and shutdown never surface as I/O errors;
// an aborted result without an owner reason is an expired deadline.
class redis_connection final {
public:
    redis_connection(asio::io_context& io_context, asio::ssl::context& tls_context, const worker_ref& worker_value,
        const redis_config_storage& config, client_wire_counters& counters);
    redis_connection(asio::io_context&, asio::ssl::context&, worker_ref&&, const redis_config_storage&,
        client_wire_counters&) = delete;
    ~redis_connection();

    redis_connection(const redis_connection&) = delete;
    redis_connection& operator=(const redis_connection&) = delete;

    [[nodiscard]] bool connected() const noexcept {
        return connected_;
    }

    // Connects when closed. A connected connection whose peer closed it or
    // sent unsolicited bytes would desynchronize the next reply, so it is
    // replaced. The optional operation deadline bounds establishment together
    // with connect_timeout; AUTH and SELECT replies also honor command_timeout.
    task<void> connect(const operation_deadline* operation_timeout_value,
        std::optional<timeout_duration> command_timeout);
    // Serialized request storage: append RESP commands, then write_request().
    [[nodiscard]] std::string& write_buffer() noexcept {
        return write_buffer_;
    }
    // Sends and then releases the serialized request, wiping it first when it
    // carries a credential, whether or not the write succeeded.
    task<void> write_request(
        const operation_deadline& timeout, redis_request_content content = redis_request_content::ordinary);
    // Reads one complete reply, allocating its nodes from `resource`. Throws
    // redis_error(io_error) when the connection is or becomes closed. A
    // non-null `input_time` receives the time of every socket read that
    // delivered bytes, so a subscription's idle clock restarts on partial
    // replies too.
    task<redis_value> read_reply(const operation_deadline& timeout,
        std::chrono::steady_clock::time_point* input_time = nullptr);

    void close() noexcept;
    // Cancellation/shutdown: records the reason so a resolve completing after
    // this point starts no connect or handshake, then closes the connection.
    void abort(client_abort_reason reason) noexcept;
    // Throws the redis_error of a recorded owner abort (cancelled or closing).
    void throw_if_aborted() const;

private:
    void ensure_reader();
    task<void> authenticate(const operation_deadline& connect_timeout,
        std::optional<timeout_duration> command_timeout);
    // One connection-setup command (AUTH, SELECT) and its reply.
    task<redis_value> exchange_control_command(
        std::span<const std::string_view> args, operation_deadline deadline, redis_request_content content);

    const redis_config_storage& config_;
    client_tcp_transport transport_;
    std::string write_buffer_;
    std::array<char, redis_read_buffer_bytes> read_buffer_;
    // Created on first connect; the reader's object functions address the
    // builder, so it outlives every reader.
    std::unique_ptr<redis_reply_builder> reply_builder_;
    std::unique_ptr<redisReader, redis_reader_deleter> reader_;
    std::size_t reply_bytes_{0};
    bool connected_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

struct redis_command_args_view final {
    std::span<const std::string> args_;
};

class redis_pool final {
public:
    // `tls_context` is borrowed from the owning runtime and shared by every
    // pool of the alias; it is configured only when TLS is enabled.
    redis_pool(asio::io_context& io_context, const redis_config_storage& config, asio::ssl::context& tls_context,
        std::optional<timeout_duration> command_timeout, std::size_t pool_size,
        const worker_ref& worker_value);
    redis_pool(asio::io_context&, redis_config_storage&&, asio::ssl::context&, std::optional<timeout_duration>,
        std::size_t, const worker_ref&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, const worker_ref&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, worker_ref&&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, const worker_ref&&) = delete;
    ~redis_pool();

    redis_pool(const redis_pool&) = delete;
    redis_pool& operator=(const redis_pool&) = delete;

    task<void> connect();
    void close_now() noexcept;
    // `reply_timeout` starts when the command is written, so a server-side
    // block window is not consumed by slot acquisition or a lazy connect.
    // `io_metrics` is the request's counter block, or null outside a request.
    task<redis_value> execute_owned(redis_owned_command command, operation_options options = {},
        std::optional<timeout_duration> reply_timeout = std::nullopt,
        request_io_metrics* io_metrics = nullptr, std::optional<operation_deadline> deadline = std::nullopt);
    task<redis_value> execute_owned(std::vector<std::string> args, operation_options options = {},
        std::optional<timeout_duration> reply_timeout = std::nullopt,
        request_io_metrics* io_metrics = nullptr, std::optional<operation_deadline> deadline = std::nullopt);
    // The caller keeps `args` alive until the operation completes. `deadline`
    // is copied, so repeated commands share the operation's absolute bound.
    task<redis_value> execute_borrowed(redis_command_args_view args,
        operation_options options, operation_deadline deadline);
    task<std::vector<redis_value>> execute_pipeline(
        std::vector<redis_owned_command> commands, operation_options options, bool wrap_transaction = false);

private:
    // Every deadline belongs to a connection's transport; the slot pool only
    // leases slots and routes cancellation.
    struct slot_policy final {
        using owner_type = redis_pool;
        enum class deadline_kind_type : std::uint8_t { none };

        [[noreturn]] static void throw_acquire_failure(const redis_pool& pool, worker_wait_status status);
        [[noreturn]] static void throw_aborted(const redis_pool& pool, pool_slot_abort_reason reason);
        static void expire_slot(redis_pool&, std::size_t, deadline_kind_type) noexcept {}
        static void cancel_slot(redis_pool& pool, std::size_t slot) noexcept;
    };
    using slot_pool_type = slot_pool<slot_policy>;
    using connection_owner_type = std::unique_ptr<redis_connection>;

    template <typename result_type, typename command_source_type>
    task<result_type> execute_commands(command_source_type source, operation_options options,
        std::optional<timeout_duration> reply_timeout,
        std::optional<operation_deadline> deadline, request_io_metrics* io_metrics = nullptr,
        bool wrap_transaction = false);

    const redis_config_storage& config_;
    std::optional<timeout_duration> command_timeout_;
    client_wire_counters wire_counters_;
    slot_pool_type slots_;
    // Declared after the slot pool, whose worker every transport borrows.
    std::vector<connection_owner_type> connections_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class redis_subscription_set;

// The connection behind one redis_subscription. Address-stable and
// worker-affine; the set links every live connection so shutdown and the
// subscription's stop token can abort it, and the connection unlinks itself
// when destroyed. Closing is terminal: a failed, aborted or cancelled
// connection never reconnects.
//
// While read() waits, subscription_ping_interval drives an idle probe: an
// interval without input sends PING unless a request written since the last
// input still awaits its reply, and a further interval without input closes
// the connection with reason timeout. The probe timer and every PING write
// belong to the read that started them and end before it returns.
class redis_subscription_connection final {
public:
    redis_subscription_connection(redis_subscription_set& owner, operation_options options);
    ~redis_subscription_connection();

    redis_subscription_connection(const redis_subscription_connection&) = delete;
    redis_subscription_connection& operator=(const redis_subscription_connection&) = delete;

    // Writes one subscription command, connecting first when needed. The
    // write is bound by the operation timeout and command_timeout, and waits
    // for a PING write of the idle probe that holds the transport's write lane.
    task<void> send(redis_owned_command command);
    // Reads the next pushed reply, consuming PING replies. Views of the
    // returned event borrow current_reply_ until the next read() releases it.
    // `operation` tightens the read deadline; its stop request closes the
    // connection and fails this read with cancelled.
    task<std::optional<redis_pubsub_event>> read(operation_options operation);
    void abort(client_abort_reason reason) noexcept;

    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] bool has_active_operations() const noexcept {
        return read_active_ || write_active_;
    }

private:
    friend class redis_subscription_set;

    using clock_type = std::chrono::steady_clock;

    void throw_if_closed() const;
    void fail() noexcept;
    // Idle probe of one read(); `ping_writes` is that read's scope and
    // `read_timeout` its overall deadline, both borrowed until
    // end_idle_probe().
    void begin_idle_probe(task_scope& ping_writes, const operation_deadline& read_timeout) noexcept;
    void end_idle_probe() noexcept;
    void arm_idle_probe(clock_type::time_point deadline) noexcept;
    void idle_probe_expired() noexcept;
    // Records the exception being handled for the read, then closes.
    void fail_idle_probe() noexcept;
    // Bounded by `timeout`: one interval, capped by the joining read's deadline.
    task<void> write_ping(operation_deadline timeout);
    static void cancel_read(void* connection) noexcept;

    redis_subscription_set& owner_;
    operation_options options_;
    redis_connection connection_;
    redis_value current_reply_;
    redis_subscription_connection* previous_{nullptr};
    redis_subscription_connection* next_{nullptr};
    std::uint64_t cancellation_id_{0};
    bool read_active_{false};
    bool write_active_{false};
    // The first subscribe() has authenticated the connection.
    bool ready_{false};
    bool closed_{false};
    // A PING write holds the transport's write lane; send() waits on
    // ping_written_ until it ends.
    bool ping_writing_{false};
    // PING replies still owed by the server, consumed by read() in wire order.
    std::size_t pending_pings_{0};
    // The read()'s scope its PING writes join and that read's deadline; null
    // while no read waits.
    task_scope* ping_writes_{nullptr};
    const operation_deadline* read_timeout_{nullptr};
    // Idle clock: the latest received bytes, the start of the waiting read,
    // the latest request written (a command or PING whose reply is owed when
    // it is newer than the input) and the start of the current probe.
    clock_type::time_point last_input_{};
    clock_type::time_point read_started_{};
    clock_type::time_point last_request_{};
    std::optional<clock_type::time_point> probe_started_{};
    // An internal probe failure no caller observed yet; the waiting read
    // rethrows it.
    std::exception_ptr probe_failure_{};
    worker_signal ping_written_;
    // Why the connection closed: none for a failed operation, timeout for an
    // unanswered idle probe.
    client_abort_reason closed_reason_{client_abort_reason::none};
    // Armed only while a read() waits and cancelled before it returns; its
    // completion addresses this connection.
    worker_timer_registration idle_probe_timer_;
    // Routes the stop request of the waiting read().
    operation_interrupt_source read_stops_;
    // Declared last: it is armed with the stop token once the connection is
    // linked and is released before any other member is destroyed.
    worker_cancellation_registration<worker_cancellation_target<redis_subscription_set>> cancellation_;
};

// The live Pub/Sub connections of one alias on one worker. Owned by
// redis_client_runtime next to the command pools; close_now() aborts every
// connection, and each connection is joined by the scope that owns its
// redis_subscription. Destroying the set with live connections terminates.
class redis_subscription_set final {
public:
    redis_subscription_set(asio::io_context& io_context, asio::ssl::context& tls_context,
        const redis_config_storage& config, const worker_ref& worker_value);
    redis_subscription_set(asio::io_context&, asio::ssl::context&, const redis_config_storage&, worker_ref&&) = delete;
    ~redis_subscription_set();

    redis_subscription_set(const redis_subscription_set&) = delete;
    redis_subscription_set& operator=(const redis_subscription_set&) = delete;

    using connection_owner_type =
        std::unique_ptr<redis_subscription_connection>;

    // A closed set opens connections that are already closing, so a late
    // subscription observes shutdown instead of connecting.
    [[nodiscard]] connection_owner_type open(operation_options options);
    void close_now() noexcept;

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    // worker_cancellation_target hook; runs on the worker.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

private:
    friend class redis_subscription_connection;

    void link(redis_subscription_connection& connection) noexcept;
    void unlink(redis_subscription_connection& connection) noexcept;

    asio::io_context& io_context_;
    asio::ssl::context& tls_context_;
    const redis_config_storage& config_;
    worker_ref worker_;
    client_wire_counters wire_counters_;
    std::shared_ptr<worker_cancellation_target<redis_subscription_set>> cancellation_target_;
    redis_subscription_connection* head_{nullptr};
    bool closing_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The single backend owner used by standalone clients and application registrations.
// Both pools and the Pub/Sub connections borrow this stable owner's normalized
// configuration, TLS context and worker.
class redis_client_runtime final {
public:
    redis_client_runtime(asio::io_context& io_context, const worker_ref& worker_value,
        redis_config_storage config);
    redis_client_runtime(asio::io_context&, worker_ref&&, redis_config_storage) = delete;
    ~redis_client_runtime();

    redis_client_runtime(const redis_client_runtime&) = delete;
    redis_client_runtime& operator=(const redis_client_runtime&) = delete;

    [[nodiscard]] task<void> connect();
    void close_now() noexcept;
    // An application-registry handle; `io_metrics` is the request's counter
    // block, or null outside a request.
    [[nodiscard]] redis_session session(::ruvia::operation_scope& scope, request_io_metrics* io_metrics);
    // A standalone client handle, which never belongs to a request.
    [[nodiscard]] redis_session session(::ruvia::operation_scope& scope, operation_options options);

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] const redis_config_storage& config() const noexcept {
        return config_;
    }
    // The configuration of the runtime behind an active handle, for a
    // consumer that sizes its own use of that store (the DB query cache).
    [[nodiscard]] static const redis_config_storage& handle_config(const redis_session& handle) noexcept {
        return handle.runtime_->config();
    }
    [[nodiscard]] redis_pool& general() noexcept {
        return general_;
    }
    [[nodiscard]] redis_pool& blocking() noexcept {
        return blocking_;
    }
    [[nodiscard]] redis_subscription_set& subscriptions() noexcept {
        return subscriptions_;
    }

private:
    redis_config_storage config_;
    worker_ref worker_;
    // One client TLS context per alias, borrowed by every connection of both
    // pools and every subscription; configured only when TLS is enabled.
    asio::ssl::context tls_context_;
    redis_pool general_;
    redis_pool blocking_;
    redis_subscription_set subscriptions_;
};

}  // namespace ruvia::detail

#ifdef RUVIA_ENABLE_REDIS

namespace ruvia::detail {

class redis_registry final {
public:
    redis_registry(asio::io_context& io_context,
        std::span<const redis_definition_type> redis, worker_ref worker_value);
    ~redis_registry();

    redis_registry(const redis_registry&) = delete;
    redis_registry& operator=(const redis_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] redis_session get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;
    [[nodiscard]] redis_session get(
        std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

private:
    using entry_type = std::unique_ptr<redis_client_runtime>;

    std::vector<entry_type> runtimes_;
    named_capability_index alias_index_;
};

}  // namespace ruvia::detail

#endif

#endif
#pragma once

#ifdef RUVIA_ENABLE_REDIS
#endif
#ifdef RUVIA_ENABLE_REDIS
#endif

namespace ruvia::detail {

class router;

// What a registered route runs, as one closed set of alternatives. Each
// alternative carries the handler's invoke thunk together with the route
// metadata only that shape may have, so a route cannot claim a streaming or
// websocket mode while holding a buffered handler. The handler's target is
// worker-bound and lives in each worker's route_table, never here.

class route_endpoint;

class buffered_route_endpoint final {
public:
    [[nodiscard]] route_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] detail::request_body_mode request_body_mode() const noexcept {
        return request_body_mode_;
    }

private:
    friend class route_endpoint;

    buffered_route_endpoint(route_handler::invoke_type invoke,
        detail::request_body_mode request_body_mode_value) noexcept
        : invoke_(invoke),
          request_body_mode_(request_body_mode_value) {}

    route_handler::invoke_type invoke_;
    detail::request_body_mode request_body_mode_;
};

class response_stream_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] detail::request_body_mode request_body_mode() const noexcept {
        return request_body_mode_;
    }

private:
    friend class route_endpoint;

    explicit response_stream_route_endpoint(route_stream_handler::invoke_type invoke,
        detail::request_body_mode request_body_mode_value = detail::request_body_mode::buffered) noexcept
        : invoke_(invoke),
          request_body_mode_(request_body_mode_value) {}

    route_stream_handler::invoke_type invoke_;
    detail::request_body_mode request_body_mode_;
};

class tunnel_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }
    [[nodiscard]] std::string_view protocol() const noexcept {
        return protocol_;
    }
    [[nodiscard]] const http_tunnel_route_config& config() const noexcept {
        return config_;
    }

private:
    friend class route_endpoint;
    tunnel_route_endpoint(route_stream_handler::invoke_type invoke,
        std::string_view protocol, http_tunnel_route_config config)
        : invoke_(invoke),
          protocol_(protocol),
          config_(config) {}
    route_stream_handler::invoke_type invoke_;
    std::string protocol_;
    http_tunnel_route_config config_;
};

class websocket_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] std::span<const std::string_view> subprotocols() const noexcept {
        return subprotocols_;
    }

    [[nodiscard]] websocket_handshake_validation_options handshake_validation() const noexcept {
        return {.supported_subprotocols_ = subprotocols_, .require_subprotocol_ = require_subprotocol_};
    }

    [[nodiscard]] const websocket_lifecycle_options& lifecycle() const noexcept {
        return lifecycle_;
    }

    [[nodiscard]] const websocket_deflate_config& deflate() const noexcept {
        return deflate_;
    }
    [[nodiscard]] protocol_byte_limit message_limit(std::size_t server_limit) const {
        const auto local = max_message_bytes_.maximum();
        return protocol_byte_limit::limited(local && *local < server_limit ? *local : server_limit);
    }

private:
    friend class route_endpoint;

    websocket_route_endpoint(route_stream_handler::invoke_type invoke,
        const websocket_route_config& options)
        : invoke_(invoke),
          require_subprotocol_(options.require_subprotocol_),
          lifecycle_(options.lifecycle_),
          deflate_(options.deflate_),
          max_message_bytes_(options.max_message_bytes_) {
        if (!options.subprotocols_.empty()) {
            std::size_t total_bytes = 0;
            for (const auto& subprotocol : options.subprotocols_) {
                total_bytes += subprotocol.size();
            }
            subprotocol_storage_.reserve(total_bytes);
            subprotocols_.reserve(options.subprotocols_.size());
            for (const auto& subprotocol : options.subprotocols_) {
                const auto offset = subprotocol_storage_.size();
                subprotocol_storage_.insert(
                    subprotocol_storage_.end(), subprotocol.begin(), subprotocol.end());
                subprotocols_.emplace_back(
                    subprotocol_storage_.data() + offset, subprotocol.size());
            }
        }
    }

    route_stream_handler::invoke_type invoke_;
    std::vector<char> subprotocol_storage_;
    std::vector<std::string_view> subprotocols_;
    bool require_subprotocol_;
    websocket_lifecycle_options lifecycle_;
    websocket_deflate_config deflate_;
    protocol_byte_limit max_message_bytes_;
};

// Startup-built endpoint contract, validated once when the process route plan
// is compiled. The handler shape and its only legal route metadata live in the
// same alternative.
class route_endpoint final {
public:
    route_endpoint(const route_endpoint&) = delete;
    route_endpoint& operator=(const route_endpoint&) = delete;
    route_endpoint(route_endpoint&&) noexcept = default;
    route_endpoint& operator=(route_endpoint&&) = delete;

    // Handlers come from the controller macros' bind()/bind_stream(), which
    // always supply an invoker; body modes and stream kinds are the builder's
    // own enumerators.
    [[nodiscard]] static route_endpoint buffered(route_handler::invoke_type invoke,
        request_body_mode request_body_mode_value) {
        return route_endpoint(buffered_route_endpoint(invoke, request_body_mode_value));
    }

    [[nodiscard]] static route_endpoint response_stream(
        route_stream_handler::invoke_type invoke,
        request_body_mode request_body_mode_value = request_body_mode::buffered) {
        return route_endpoint(response_stream_route_endpoint(invoke, request_body_mode_value));
    }

    [[nodiscard]] static route_endpoint websocket(route_stream_handler::invoke_type invoke, websocket_route_config options) {
        options.lifecycle_ = normalize_websocket_lifecycle_options(options.lifecycle_);
        validate_websocket_deflater_config(options.deflate_.deflater_);
        ruvia::websocket_subprotocol_set subprotocols;
        for (const auto& subprotocol : options.subprotocols_) {
            if (!subprotocols.append(subprotocol)) {
                throw std::invalid_argument(
                    "websocket subprotocols must contain at most 64 unique HTTP tokens");
            }
        }
        if (options.require_subprotocol_ && options.subprotocols_.empty()) {
            throw std::invalid_argument("a websocket route requiring a subprotocol must list subprotocols");
        }
        return route_endpoint(
            websocket_route_endpoint(invoke, options));
    }

    [[nodiscard]] static http_tunnel_route_config validate_tunnel_config(
        bool upgrade_protocol, bool udp, http_tunnel_route_config config) {
        if (udp) {
            config.datagrams_ = true;
        }
        if (config.datagrams_ && !upgrade_protocol) {
            throw std::invalid_argument("HTTP Datagrams require an upgrade-protocol tunnel route");
        }
        if (config.peer_transport_fin_timeout_.value() <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("CONNECT peer transport FIN timeout must be greater than zero");
        }
        return config;
    }

    [[nodiscard]] static route_endpoint tunnel(route_stream_handler::invoke_type invoke, std::string_view protocol, http_tunnel_route_config config) {
        if (!protocol.empty() && !is_valid_http_header_name(protocol)) {
            throw std::invalid_argument("CONNECT protocol must be an HTTP token");
        }
        config = validate_tunnel_config(!protocol.empty(), protocol == "connect-udp", config);
        return route_endpoint(
            tunnel_route_endpoint(invoke, protocol, config));
    }

    [[nodiscard]] const tunnel_route_endpoint* tunnel() const& noexcept {
        return std::get_if<tunnel_route_endpoint>(&value_);
    }
    const tunnel_route_endpoint* tunnel() const&& = delete;

    [[nodiscard]] const buffered_route_endpoint* buffered() const& noexcept {
        return std::get_if<buffered_route_endpoint>(&value_);
    }
    [[nodiscard]] const buffered_route_endpoint* buffered() const&& = delete;

    [[nodiscard]] const response_stream_route_endpoint* response_stream() const& noexcept {
        return std::get_if<response_stream_route_endpoint>(&value_);
    }
    [[nodiscard]] const response_stream_route_endpoint* response_stream() const&& = delete;

    [[nodiscard]] const websocket_route_endpoint* websocket() const& noexcept {
        return std::get_if<websocket_route_endpoint>(&value_);
    }
    [[nodiscard]] const websocket_route_endpoint* websocket() const&& = delete;

    [[nodiscard]] ruvia::detail::request_body_mode request_body_mode() const noexcept {
        if (const auto* endpoint = buffered()) {
            return endpoint->request_body_mode();
        }
        if (const auto* endpoint = response_stream()) {
            return endpoint->request_body_mode();
        }
        return request_body_mode::buffered;
    }

    // The streaming handler thunk, or null for a buffered-response endpoint.
    [[nodiscard]] route_stream_handler::invoke_type stream_invoke() const noexcept {
        if (const auto* endpoint = response_stream()) {
            return endpoint->invoke();
        }
        if (const auto* endpoint = websocket()) {
            return endpoint->invoke();
        }
        if (const auto* endpoint = tunnel()) {
            return endpoint->invoke();
        }
        return nullptr;
    }

private:
    using value_type =
        std::variant<buffered_route_endpoint, response_stream_route_endpoint, websocket_route_endpoint, tunnel_route_endpoint>;

    template <typename endpoint_type>
    explicit route_endpoint(endpoint_type endpoint) noexcept
        : value_(std::move(endpoint)) {}

    value_type value_;
};

class route_entry final {
public:
    struct init_type final {
        std::size_t index_{0};
        // Registration-order slot of the controller whose instance is the
        // handler target on every worker.
        std::size_t controller_{0};
        http_known_method method_;
        // Non-empty only for an extension-method route, where it is the exact,
        // case-sensitive wire token (RFC 9110 9.1) and `method` is unknown.
        // Known methods keep the enum as their identity so the routing fast
        // path never compares strings.
        std::string_view method_token_{};
        std::string_view path_;
        route_endpoint endpoint_;
        bool dynamic_{false};
        // 0 = no route-declared ceiling; the server default applies.
        std::size_t max_request_body_bytes_{0};
    };

    route_entry(init_type init)
        : index_(init.index_),
          controller_(init.controller_),
          method_(init.method_),
          method_token_(init.method_token_),
          path_(init.path_),
          endpoint_(std::move(init.endpoint_)),
          dynamic_(init.dynamic_),
          max_request_body_bytes_(init.max_request_body_bytes_) {}
    route_entry(const route_entry&) = delete;
    route_entry& operator=(const route_entry&) = delete;
    route_entry(route_entry&&) noexcept = default;
    route_entry& operator=(route_entry&&) = delete;

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }

    [[nodiscard]] std::size_t controller() const noexcept {
        return controller_;
    }

    [[nodiscard]] http_known_method method() const noexcept {
        return method_;
    }

    // Empty unless this is an extension-method route.
    [[nodiscard]] std::string_view method_token() const noexcept {
        return method_token_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] const route_endpoint& endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] bool dynamic() const noexcept {
        return dynamic_;
    }

    // A ceiling declared by one of this route's middlewares, or 0 for none.
    // Read before the body is accepted, so it bounds what is buffered rather
    // than what a handler later sees.
    [[nodiscard]] std::size_t max_request_body_bytes() const noexcept {
        return max_request_body_bytes_;
    }

    [[nodiscard]] std::span<const std::string_view> param_names() const noexcept {
        return param_names_;
    }

    [[nodiscard]] std::size_t middleware_offset() const noexcept {
        return middleware_offset_;
    }

    [[nodiscard]] std::size_t middleware_count() const noexcept {
        return middleware_count_;
    }

    [[nodiscard]] bool has_middleware() const noexcept {
        return middleware_count_ != 0;
    }

    // True when at least one frame in this route's range is a path-scoped
    // middleware whose scope covers only part of the route's request paths;
    // dispatch then checks the request path before running such a frame.
    [[nodiscard]] bool has_conditional_middleware() const noexcept {
        return conditional_middleware_;
    }

private:
    friend class router;

    std::size_t index_;
    std::size_t controller_;
    http_known_method method_;
    std::string method_token_;
    std::string path_;
    route_endpoint endpoint_;
    bool dynamic_{false};
    std::size_t max_request_body_bytes_{0};
    std::span<const std::string_view> param_names_{};
    std::size_t middleware_offset_{0};
    std::size_t middleware_count_{0};
    bool conditional_middleware_{false};
};
class route_table;

// The one immutable route plan of a process run, compiled once at startup and
// shared by every worker: every route with its endpoint metadata, limits and
// parameter names, the middleware frame layout, the lookup indices, and the
// error/not-found fallbacks. Workers bind only their own handler targets and
// middleware instances to it (route_table), by route and frame index.
class compiled_route_plan final {
public:
    compiled_route_plan()
        : routes_(),
          middleware_instances_(),
          middleware_frame_instances_(),
          middleware_scopes_(),
          param_names_(),
          extension_route_indices_(),
          extension_paths_(),
          extension_methods_(),
          connect_route_indices_(),
          connect_protocols_(),
          static_slots_(),
          dynamic_node_arena_(),
          url_patterns_(),
          prefix_error_handlers_(),
          prefix_not_found_handlers_() {}

    compiled_route_plan(const compiled_route_plan&) = delete;
    compiled_route_plan& operator=(const compiled_route_plan&) = delete;
    compiled_route_plan(compiled_route_plan&&) = delete;
    compiled_route_plan& operator=(compiled_route_plan&&) = delete;

private:
    friend class route_table;
    friend class router;

    static constexpr std::size_t routable_method_count = 7;
    static constexpr std::size_t no_route_index = std::numeric_limits<std::size_t>::max();

    struct static_route_slot final {
        std::uint64_t hash_{};
        std::size_t route_index_{no_route_index};
    };

    struct extension_path_type final {
        std::string_view path_;
        std::span<const std::string_view> methods_;
    };

    struct dynamic_node_type;

    struct dynamic_static_child_type final {
        std::string_view segment_;
        dynamic_node_type* node_{nullptr};
    };

    struct dynamic_node_type final {
        dynamic_node_type()
            : static_children_() {}

        std::vector<dynamic_static_child_type> static_children_;
        dynamic_node_type* param_child_{nullptr};
        std::size_t route_index_{no_route_index};
        std::size_t wildcard_route_index_{no_route_index};
    };

    struct connect_protocol_index_type final {
        explicit connect_protocol_index_type(std::string_view value)
            : protocol_(value) {}
        std::string protocol_;
        dynamic_node_type root_;
    };

    // Normalized prefix, deepest first: selection is a first-match scan.
    template <typename handler_type>
    struct prefix_handler_type final {
        prefix_handler_type(std::string_view prefix_value,
            handler_type handler_value)
            : prefix_(prefix_value),
              handler_(handler_value) {}

        std::string prefix_;
        handler_type handler_{nullptr};
    };

    std::vector<route_entry> routes_;
    // One descriptor per middleware instance a worker materializes. Frames
    // name their instance by index, so an app-wide middleware is one instance
    // shared by every route of a worker.
    std::vector<controller_middleware_descriptor> middleware_instances_;
    std::vector<std::size_t> middleware_frame_instances_;
    // Parallel to middleware_frame_instances_: the path scope a conditional
    // frame checks at dispatch, or empty for an unconditional frame. Only
    // routes flagged has_conditional_middleware() read it. Views borrow the
    // registration-owned use_at prefixes, which outlive every plan.
    std::vector<std::string_view> middleware_scopes_;
    // A contiguous block at the end of the frames: the app-wide middlewares
    // that declared they also run when no route matched.
    std::size_t unmatched_middleware_offset_{0};
    std::size_t unmatched_middleware_count_{0};
    std::vector<std::string_view> param_names_;
    // Sorted by case-sensitive method token, then canonical path.
    std::vector<std::size_t> extension_route_indices_;
    // Sorted distinct canonical paths, naming every registered token for
    // 405/OPTIONS Allow in registration order. The spans slice
    // extension_methods_ and borrow the routes' own path and token storage.
    std::vector<extension_path_type> extension_paths_;
    std::vector<std::string_view> extension_methods_;
    std::vector<std::size_t> connect_route_indices_;
    std::vector<connect_protocol_index_type> connect_protocols_;
    std::vector<static_route_slot> static_slots_;
    std::array<dynamic_node_type, routable_method_count> dynamic_roots_;
    std::vector<dynamic_node_type> dynamic_node_arena_;
    // Distinct registered route patterns, sorted, so url_for finds its pattern
    // by binary search.
    std::vector<std::string_view> url_patterns_;
    http_error_handler_ref error_handler_{nullptr};
    http_not_found_handler_ref not_found_handler_{nullptr};
    std::vector<prefix_handler_type<http_error_handler_ref>> prefix_error_handlers_;
    std::vector<prefix_handler_type<http_not_found_handler_ref>> prefix_not_found_handlers_;
    std::uint32_t static_method_mask_{0};
    std::uint32_t dynamic_method_mask_{0};
    std::size_t static_slot_mask_{};
    bool has_route_rate_limit_{false};
};

using compiled_route_plan_ptr_type =
    std::unique_ptr<compiled_route_plan>;

}  // namespace ruvia::detail

// Lightweight, self-contained route-resolution result types. A materialized
// resolution is exactly one of resolved, method-not-allowed, not-found, or
// rejected; the payload for another alternative is not observable.

namespace ruvia::detail {

inline constexpr std::size_t max_route_params = 16;
class route_entry;

class route_match final {
public:
    constexpr route_match() noexcept = default;

    constexpr route_match(const route_match& other) noexcept
        : param_count_(other.param_count_) {
        for (std::size_t i = 0; i < param_count_; ++i) {
            param_values_[i] = other.param_values_[i];
        }
    }

    constexpr route_match& operator=(const route_match& other) noexcept {
        param_count_ = other.param_count_;
        for (std::size_t i = 0; i < param_count_; ++i) {
            param_values_[i] = other.param_values_[i];
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::string_view> values() const& noexcept {
        return std::span<const std::string_view>(param_values_.data(), param_count_);
    }
    [[nodiscard]] std::span<const std::string_view> values() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return param_count_;
    }

    void clear() noexcept {
        param_count_ = 0;
    }

    void truncate(std::size_t count) noexcept {
        param_count_ = count <= param_count_ ? count : param_count_;
    }

    [[nodiscard]] bool add(std::string_view value) noexcept {
        if (param_count_ >= param_values_.size()) {
            return false;
        }

        param_values_[param_count_++] = value;
        return true;
    }

private:
    std::array<std::string_view, max_route_params> param_values_{};
    std::size_t param_count_{0};
};

class route_not_found final {
private:
    friend class route_resolution;
    constexpr route_not_found() noexcept = default;
};

// A target that names no resource at all (rootless or dot-segment path): no
// route and no document-root file may answer it, only the not-found path.
class route_rejected final {
private:
    friend class route_resolution;
    constexpr route_rejected() noexcept = default;
};

class route_method_not_allowed final {
public:
    [[nodiscard]] constexpr std::uint32_t allowed_methods() const noexcept {
        return allowed_methods_;
    }
    // The distinct extension tokens registered on the resource, for Allow.
    // Borrowed from the compiled route plan.
    [[nodiscard]] constexpr std::span<const std::string_view> extension_methods() const noexcept {
        return extension_methods_;
    }

private:
    friend class route_resolution;

    constexpr route_method_not_allowed(
        std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept
        : allowed_methods_(allowed_methods),
          extension_methods_(extension_methods) {}

    std::uint32_t allowed_methods_;
    std::span<const std::string_view> extension_methods_;
};

class resolved_route final {
public:
    // Process-invariant route data from the compiled plan.
    [[nodiscard]] const route_entry& route() const noexcept {
        return *route_;
    }

    // The resolving worker's handler. handler() requires a buffered-response
    // endpoint; stream_handler() a response-stream, websocket or tunnel one.
    [[nodiscard]] route_handler handler() const noexcept;
    [[nodiscard]] route_stream_handler stream_handler() const noexcept;

    // Static routes carry an empty match; dynamic routes carry their captured
    // values in the same value object. Callers never need a nullable match side
    // channel.
    [[nodiscard]] const route_match& match() const& noexcept {
        return match_;
    }
    [[nodiscard]] const route_match& match() const&& = delete;

private:
    friend class route_resolution;

    resolved_route(const route_entry& route, void* handler_target) noexcept
        : route_(&route),
          handler_target_(handler_target) {}

    resolved_route(const route_entry& route, void* handler_target, const route_match& match) noexcept
        : route_(&route),
          handler_target_(handler_target),
          match_(match) {}

    const route_entry* route_;
    void* handler_target_;
    route_match match_;
};

class route_resolution final {
public:
    route_resolution() noexcept
        : value_(route_not_found{}) {}

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, void* handler_target) noexcept {
        return route_resolution(resolved_route(route, handler_target));
    }

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, void* handler_target, const route_match& match) noexcept {
        return route_resolution(resolved_route(route, handler_target, match));
    }

    // The mask spans only classified methods, so it can be empty for a resource
    // that has extension-method routes. A non-empty `extension_methods` alone
    // still makes the resource exist, so an extension-only path answers 405
    // with an Allow built from its tokens instead of collapsing to 404.
    [[nodiscard]] static route_resolution method_not_allowed(
        std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept {
        if (allowed_methods == 0 && extension_methods.empty()) {
            return route_resolution();
        }
        return route_resolution(route_method_not_allowed(allowed_methods, extension_methods));
    }

    [[nodiscard]] static route_resolution reject() noexcept {
        return route_resolution(route_rejected{});
    }

    [[nodiscard]] const resolved_route* resolved() const& noexcept {
        return std::get_if<resolved_route>(&value_);
    }
    [[nodiscard]] const resolved_route* resolved() const&& = delete;

    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const& noexcept {
        return std::get_if<route_method_not_allowed>(&value_);
    }
    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const&& = delete;

    [[nodiscard]] const route_rejected* rejected() const& noexcept {
        return std::get_if<route_rejected>(&value_);
    }
    [[nodiscard]] const route_rejected* rejected() const&& = delete;

private:
    using value_type = std::variant<route_not_found, route_method_not_allowed, resolved_route, route_rejected>;

    template <typename alternative_type>
    explicit route_resolution(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class route_table;

struct middleware_chain final {
    using terminal_type = callback_ref<task<void>(context&)>;
    using failure_type = task<void> (*)(const route_table&, context&, std::exception_ptr);

    const route_table* table_{nullptr};
    std::span<const route_middleware> frames_;
    std::span<const std::string_view> scopes_;
    terminal_type terminal_;
    failure_type failure_{nullptr};
};

[[nodiscard]] task<void> run_chain(const middleware_chain& chain, context& context_value);
struct route_request_view final {
    http_known_method known_method_{http_known_method::unknown};
    std::string_view method_{};
    std::string_view path_{};
    std::string_view connect_target_{};
    std::string_view extended_protocol_{};
};

class controller_store;
class router;

// The framework's own answer to a failed request, consulting no application
// error handler: the status an http_error or protocol failure carries, else a
// generic 500 that names nothing internal. An inbound transport failure
// propagates instead.
struct default_exception_response final {
    http_response response_;
    // The failure maps to no HTTP error: a fault for the caller to report.
    bool unexpected_;
};
[[nodiscard]] default_exception_response make_default_exception_response(const std::exception_ptr& exception);

// One worker's binding of the process route plan: the worker's controller
// instance behind every route and the worker's middleware instance behind
// every frame, indexed exactly like the plan. Everything else -- lookup,
// endpoint metadata, limits, fallbacks -- is read from the shared plan.
class route_table final {
public:
    // The plan and the controllers must outlive the table. Middleware
    // instances are created here and destroyed with the table.
    route_table(const compiled_route_plan& plan, const controller_store& controllers);
    route_table(const compiled_route_plan& plan, std::span<void* const> targets);
    ~route_table();
    route_table(const route_table&) = delete;
    route_table& operator=(const route_table&) = delete;
    route_table(route_table&&) = delete;
    route_table& operator=(route_table&&) = delete;
    // Owner thread, before this worker admits its first routed listener.
    // Both plans and controller stores retain their existing lifetimes.
    void swap(route_table& other) noexcept;

    [[nodiscard]] bool has_route_rate_limit() const noexcept {
        return plan_->has_route_rate_limit_;
    }
    // Builds a request path from a registered route pattern: ":name" segments
    // take the next value (percent-encoded, non-empty), a trailing "*" takes
    // the final value (slashes preserved, may be empty). The pattern is the
    // route's identity -- an unregistered pattern, a value-count mismatch, a
    // "."/".." value, or a pattern starting with an empty segment (a "//"
    // network-path reference) is a programming error and throws
    // std::invalid_argument. A root "*" value's leading slash becomes "%2F".
    [[nodiscard]] std::string url_for(std::string_view pattern, std::span<const std::string_view> values) const;
    [[nodiscard]] route_resolution resolve(const http_request& request) const noexcept;
    [[nodiscard]] route_resolution resolve(route_request_view request) const noexcept;

    // Whether ANY route in the table uses this exact token. RFC 9110 15.5.6
    // makes 405 conditional on the method being "known by the origin server",
    // so a token nobody registered is 501 no matter what the target path holds.
    [[nodiscard]] bool recognizes_method_token(std::string_view method_token) const noexcept;
    // Canonical buffered-response application dispatch for unresolved requests
    // and callers that need it to own the route context. An unresolved route
    // first consults the configured document root, then falls through to
    // 404/405/OPTIONS handling. Any failure escaping the routing machinery
    // becomes an error response, except an inbound transport failure, which
    // propagates. Connection persistence and wire framing remain the HTTP/1
    // driver's responsibility. Pass document_root_binding::none() when no root
    // is configured, or the worker's configured(generation) lease otherwise.
    // The caller owns response and keeps it alive until dispatch completes;
    // successful completion assigns the selected final response directly.
    task<void> dispatch_buffered_response(const http_request& request,
        const route_resolution& resolution, document_root_binding document_root,
        const context_services& services, static_file_selection_mode static_file_mode,
        http_response& response) const;
    // The one matched-route invocation every transport awaits: builds the
    // context in this frame, runs the route chain, applies the response policy
    // for routes without middleware, finalizes a failure, and falls back to the
    // request-only handler when the context cannot be built or finalization
    // fails. `body_failure`, when given, is a request-body failure the transport
    // records outside the handler's reads (HTTP/2 DATA): a handler that returns
    // without surfacing it gets that failure instead. Returns whether the
    // response accounts for that failure. The context dies before this
    // returns, so body completion and the request arena outlive it.
    [[nodiscard]] task<bool> dispatch_resolved_route(const http_request& request,
        const resolved_route& resolved, const context_services& services,
        const std::optional<http_request_body_failure>* body_failure, http_response& response) const;
    task<http_response> handle_error(const http_request& request,
        http_error_info error, const context_services& services) const;
    task<http_response> handle_exception(const http_request& request,
        std::exception_ptr exception, const context_services& services) const;
    // Absence means the bound output handled the request; a value is the one
    // buffered response produced before a response-stream commit.
    task<std::optional<http_response>> dispatch_response_stream(const http_request& request,
        const resolved_route& route, response_stream_writer& response_stream,
        const context_services& services) const;
    // A CONNECT tunnel or WebSocket upgrade: `handler` is the terminal that
    // commits the upgrade. Absence means the terminal took the request.
    task<std::optional<http_response>> dispatch_upgrade(const http_request& request,
        const resolved_route& route, const route_stream_handler& handler,
        const context_services& services) const;
    // The not-found handler that applies to `path`: the tightest prefix scope
    // covering it, else the plan-wide one; null when neither is configured.
    [[nodiscard]] http_not_found_handler_ref not_found_handler_for(std::string_view path) const noexcept;
    // Reuses the transport's already-bound context and response writer.
    task<std::optional<http_response>> dispatch_bound_stream(const resolved_route& resolved,
        context& context_value, route_stream_handler handler) const;
    task<http_response> dispatch_bound_buffered(const resolved_route& resolved, context& context_value,
        const std::optional<http_request_body_failure>* body_failure = nullptr) const;

private:
    friend class router;

    [[nodiscard]] task<http_response> invoke_route(const resolved_route& resolved, context& context) const;
    // Failed buffered/stream invocations preserve only the explicit policy;
    // discarded representation fields never enter the replacement error.
    // If finalization fails, destroy the context before the request-only fallback.
    [[nodiscard]] task<http_response> finalize_failure(context& context, std::exception_ptr exception) const;

    [[nodiscard]] route_resolution resolve(
        http_known_method method, std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolve_connect(std::string_view protocol, std::string_view target) const noexcept;
    // Extension-method routing, kept off every enum-indexed structure. The
    // request's exact token is compared against a small cold list, which costs
    // a known-method request nothing: the HTTP/1 driver only reaches it when
    // classify_http_method() returned unknown.
    [[nodiscard]] route_resolution resolve_extension_method(
        std::string_view method_token, std::string_view path) const noexcept;
    // Distinct tokens of the extension routes on `path`, precomputed in the
    // plan; empty when none is registered there.
    [[nodiscard]] std::span<const std::string_view> extension_methods_for(std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolved(const route_entry& route) const noexcept;
    [[nodiscard]] route_resolution resolved(const route_entry& route, const route_match& match) const noexcept;

    static constexpr std::size_t routable_method_count = compiled_route_plan::routable_method_count;
    static constexpr std::size_t no_route_index = compiled_route_plan::no_route_index;
    using dynamic_node_type = compiled_route_plan::dynamic_node_type;
    using dynamic_static_child_type = compiled_route_plan::dynamic_static_child_type;

    [[nodiscard]] static std::size_t method_index(http_known_method method) noexcept;
    [[nodiscard]] static bool is_routable_method(http_known_method method) noexcept;
    [[nodiscard]] static bool supports_head_fallback(const route_entry& route) noexcept;
    [[nodiscard]] static std::uint64_t path_hash(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t route_hash(
        http_known_method method, std::uint64_t hash) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node(
        const dynamic_node_type& node, std::string_view path, route_match& match) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node_no_params(
        const dynamic_node_type& node, std::string_view path) noexcept;
    [[nodiscard]] static const dynamic_static_child_type* find_dynamic_static_child(
        const dynamic_node_type& node, std::string_view segment) noexcept;

    [[nodiscard]] const route_entry* find_static_route(
        http_known_method method, std::string_view path, std::uint64_t hash) const noexcept;
    [[nodiscard]] const route_entry* find_dynamic_route(
        http_known_method method, std::string_view path, route_match& match) const noexcept;
    [[nodiscard]] std::uint32_t allowed_methods(std::string_view path, std::uint64_t hash,
        std::uint32_t excluded_mask = 0, std::uint32_t initial_mask = 0) const noexcept;
    // The handler is copied into the coroutine frame: callers may pass one
    // bound from the resolved route as a temporary.
    [[nodiscard]] task<std::optional<http_response>> dispatch_stream_route(const http_request& request,
        const resolved_route& route, route_stream_handler handler,
        const context_services& services, response_stream_writer* response_stream = nullptr) const;

    // A route's own frames and, only when some are conditional, their scopes.
    [[nodiscard]] middleware_chain route_chain(const route_entry& route) const noexcept;
    // Runs `frames` around `terminal`, whose response becomes the result; the
    // completion check shared by matched buffered routes and the unmatched
    // chain.
    [[nodiscard]] task<http_response> respond_through_chain(
        middleware_chain chain, context& context, route_handler terminal) const;
    // Runs the unmatched-request middleware chain around `terminal`, which
    // produces the 404/405/501 response. Falls straight through to the terminal
    // when nothing declared itself for unmatched requests.
    [[nodiscard]] task<http_response> run_unmatched_chain(context& context, route_handler terminal) const;
    [[nodiscard]] static task<void> contain_failure(
        const route_table& table, context& context, std::exception_ptr exception);
    [[nodiscard]] task<http_response> handle_not_found(
        const http_request& request, const context_services& services) const;
    [[nodiscard]] task<http_response> handle_exception(
        context& context, std::exception_ptr exception) const;
    [[nodiscard]] task<http_response> handle_exception(context& context, std::exception_ptr exception,
        http_error_handler_ref error_handler) const;
    [[nodiscard]] task<http_response> handle_exception(const http_request& request,
        std::exception_ptr exception, const context_services& services,
        http_error_handler_ref error_handler) const;

    [[nodiscard]] http_error_handler_ref error_handler_for(std::string_view path) const noexcept;
    explicit route_table(const compiled_route_plan& plan);
    void build_middleware_instances();

    const compiled_route_plan* plan_;
    // Worker-bound, indexed like the plan: the controller instance behind each
    // route, the middleware instances this table owns, and each frame bound to
    // its instance.
    std::vector<void*> handler_targets_;
    std::vector<void*> middleware_instances_;
    std::vector<route_middleware> middleware_frames_;
};

}  // namespace ruvia::detail

namespace ruvia {

[[nodiscard]] inline context make_route_context(const http_request& request,
    const detail::resolved_route& resolved, const detail::context_services& services) {
    const auto& route = resolved.route();
    const auto values = resolved.match().values();
    return detail::context_access::make(request, route.path(), route.param_names().data(),
        values.data(), values.size(), reinterpret_cast<std::uintptr_t>(&route), services);
}

}  // namespace ruvia

namespace ruvia::detail {

class app_configuration;

// Builds the process route plan once per run: every controller instance in
// `controllers` (one per registrar, registrar order) declares its routes, then
// `configuration` applies the app-wide fallbacks and middleware. Route
// conflicts and invalid registrations throw here, before any worker starts.
[[nodiscard]] compiled_route_plan_ptr_type compile_route_plan(
    std::span<const controller_registrar> registrars, const controller_store& controllers,
    const app_configuration& configuration);

// Startup builder shared by controller declarations and owned public routes.
class router final {
public:
    router();
    ~router();

    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

    void set_error_handler(http_error_handler_ref handler) noexcept;
    void set_not_found_handler(http_not_found_handler_ref handler) noexcept;
    // Copies normalized, de-duplicated prefixes into the plan, deepest first.
    // Callback views borrow their app_configuration owners until the plan retires.
    void set_prefix_error_handlers(std::span<const std::pair<std::string, callback_registration<http_error_handler>>> handlers);
    void set_prefix_not_found_handlers(std::span<const std::pair<std::string, callback_registration<http_not_found_handler>>> handlers);
    // Application-wide middleware, prepended to every route's chain. Each
    // descriptor becomes exactly one instance per worker, serving all routes.
    void set_global_middlewares(std::span<const controller_middleware_descriptor> descriptors);

    // Runs every registrar against its controller instance, in slot order.
    void declare_controllers(
        std::span<const controller_registrar> registrars, const controller_store& controllers);
    // The single registration path. `handler_target` must be the instance of
    // the controller currently declaring its routes. `method_token` is non-empty
    // only for an extension method, with `method` unknown.
    void register_endpoint(http_known_method method, std::string_view method_token, std::string_view path,
        route_endpoint endpoint, void* handler_target,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    // Explicit, owned handler slots use the same registration and compilation
    // path without a controller declaration in progress.
    void register_owned_endpoint(std::size_t slot, http_known_method method, std::string_view method_token,
        std::string_view path, route_endpoint endpoint,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);

    [[nodiscard]] compiled_route_plan_ptr_type compile();

private:
    // A route's own (controller + route) middleware descriptors, as a slice of
    // route_middleware_descriptors_. Parallel to the plan's routes.
    struct route_middleware_range_type final {
        std::size_t offset_{0};
        std::size_t count_{0};
    };

    void validate_route_target(http_known_method method, std::string_view method_token, std::string_view path,
        const route_endpoint& endpoint) const;
    void validate_no_dynamic_route_conflict() const;
    [[nodiscard]] std::size_t append_route_middlewares(
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    void build_middleware_frames();
    void build_static_index();
    void build_static_method_mask();
    void build_extension_index();
    void build_dynamic_routes();
    void append_dynamic_param_name(route_entry& route, std::string_view name);
    void insert_dynamic(compiled_route_plan::dynamic_node_type& root, route_entry& route);
    void build_url_patterns();

    [[nodiscard]] static bool is_dynamic_path(std::string_view path) noexcept;

    compiled_route_plan_ptr_type plan_;
    std::vector<std::uint64_t> route_hashes_;
    std::vector<controller_middleware_descriptor> route_middleware_descriptors_;
    std::vector<route_middleware_range_type> route_middleware_ranges_;
    std::vector<controller_middleware_descriptor> global_middleware_descriptors_;
    std::size_t controller_slot_{0};
    void* controller_target_{nullptr};
};

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP2)

namespace ruvia::detail {

class route_table;

// What an HTTP/2 session needs from the server that accepted the connection.
// context_services is copied per session; the remaining dependencies are stable
// server-owned borrows that cannot be absent or rebound.
class http2_sans_io_session_context final {
public:
    http2_sans_io_session_context(context_services services, const http_server_options& options,
        connection_scanner::entry& scanner_entry)
        : services_(std::move(services)),
          options_(options),
          scanner_entry_(scanner_entry) {}

    [[nodiscard]] const http_server_options& options() const& noexcept RUVIA_LIFETIMEBOUND {
        return options_;
    }
    const http_server_options& options() const&& = delete;

    [[nodiscard]] connection_scanner::entry& scanner_entry() const& noexcept RUVIA_LIFETIMEBOUND {
        return scanner_entry_;
    }
    connection_scanner::entry& scanner_entry() const&& = delete;

    [[nodiscard]] const context_services& services() const& noexcept RUVIA_LIFETIMEBOUND {
        return services_;
    }
    const context_services& services() const&& = delete;

private:
    context_services services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
};

// HTTP/2 supports exactly the two transports owned by the Web server. Keeping
// these concrete overloads out of headers prevents every server/test consumer
// from instantiating the complete session coroutine again. The result is
// notify when the peer ended its input cleanly, linger when the session ended
// with GOAWAY (a completed drain or a connection error), otherwise abort.
[[nodiscard]] task<http_connection_close> run_http2_sans_io_session(inbound_socket_stream& stream,
    const route_table& routes_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes = {});

[[nodiscard]] task<http_connection_close> run_http2_sans_io_session(server_tls_stream& stream,
    const route_table& routes_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes = {});

}  // namespace ruvia::detail

#endif

// Shared admission for HTTP/1 and in-process subrequests. The HTTP/1 session
// reads and frames messages; admission decides coding, client identity,
// expectations, rate limits, body ceilings and endpoint kind.

namespace ruvia::detail {

class http1_deferred_access_log;
class route_table;

// The deployment limits that shape admission. Servers build it from their
// options; a subrequest builds it from its parent (no rate limit and no
// target-origin check: the parent request already passed both).
struct http_request_admission_policy final {
    std::optional<std::size_t> max_stream_body_bytes_{};
    std::size_t max_buffered_body_bytes_{};
    bool apply_rate_limit_{true};
    // RFC 9110 §7.4: answer 421 when the connection cannot carry the target
    // (decide_http_target_origin against the services' transport facts).
    bool verify_target_origin_{true};
    [[nodiscard]] static http_request_admission_policy server(const http_server_options& options) noexcept;
};

struct http_request_admission_input final {
    const http_request& request_;
    const route_resolution& resolution_;
    // The parser or subrequest supplies the response-coding selection.
    http_response_coding_selection coding_;
    // Request arena for copied rejection diagnostics.
    // The protocol's Expect plan, when it defers the rejection to Web.
    const http_server_expectation_plan* expectation_{};
    // Start of the request for the access-log duration.
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
    // The arrival instant rate-limit decisions use, pinned by an owner that
    // replays requests at chosen instants (test_app); empty reads the steady
    // clock at each decision.
    std::optional<std::chrono::steady_clock::time_point> pinned_arrival_{};
};

// Effective request-body ceiling. A route may only tighten the deployment limit.
[[nodiscard]] protocol_byte_limit http_request_body_limit(const route_resolution& resolution,
    request_body_mode mode, const http_request_admission_policy& policy) noexcept;

// A server-layer rejection decided before any handler ran. The diagnostic is
// borrowed from static storage or the request arena.
class http_request_rejection final {
public:
    [[nodiscard]] static constexpr http_request_rejection error(http_error_info error) noexcept {
        return http_request_rejection(error, std::nullopt);
    }

    [[nodiscard]] static http_request_rejection rate_limited(rate_limit_rejection rejection) noexcept;

    [[nodiscard]] constexpr const http_error_info& error() const& noexcept RUVIA_LIFETIMEBOUND {
        return error_;
    }
    const http_error_info& error() const&& = delete;

    [[nodiscard]] constexpr const rate_limit_rejection* rate_limit() const& noexcept RUVIA_LIFETIMEBOUND {
        return rate_limit_ ? &*rate_limit_ : nullptr;
    }
    const rate_limit_rejection* rate_limit() const&& = delete;

private:
    constexpr http_request_rejection(http_error_info error, std::optional<rate_limit_rejection> rate_limit) noexcept
        : error_(error),
          rate_limit_(rate_limit) {}

    http_error_info error_;
    std::optional<rate_limit_rejection> rate_limit_;
};

// Answers a rejection through the application's error handler, adding the
// rate-limit headers when that is why the request was refused. The lazy task
// owns this snapshot; its diagnostic text still borrows static/request storage.
[[nodiscard]] task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request, http_request_rejection rejection, const context_services& services);

// Result of the synchronous first preparation pass. ready() is null only when
// a blocking-pool encode or the application error handler must be awaited
// through http_response_tail::finish_prepare().
class http_response_preparation final {
public:
    [[nodiscard]] const prepared_application_response* ready() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<prepared_application_response>(&state_);
    }
    const prepared_application_response* ready() const&& = delete;

    http_response_preparation(const http_response_preparation&) = default;
    http_response_preparation(http_response_preparation&&) = default;
    http_response_preparation& operator=(const http_response_preparation&) = delete;
    http_response_preparation& operator=(http_response_preparation&&) = delete;

private:
    friend class http_response_tail;

    struct deferred_type final {
        http_response_coding_policy policy_;
        buffered_response_recovery recovery_;
        // Without a saved recovery step, encode on the blocking pool first.
        std::optional<buffered_response_recovery_step> pending_step_;
    };

    explicit http_response_preparation(prepared_application_response ready) noexcept
        : state_(ready) {}
    explicit http_response_preparation(http_response_coding_policy policy) noexcept
        : state_(deferred_type{policy, {}, std::nullopt}) {}
    http_response_preparation(http_response_coding_policy policy, buffered_response_recovery recovery,
        buffered_response_recovery_step pending_step) noexcept
        : state_(deferred_type{policy, recovery, pending_step}) {}

    std::variant<prepared_application_response, deferred_type> state_;
};

// The post-response tail: final representation preparation (CORS,
// compression, 406/500 recovery through the application error handler) and
// the access-log record of the committed status.
class http_response_tail final {
public:
    http_response_tail(const http_request& request, http_response_coding_policy coding_policy,
        ip_address remote_address, ip_address client_address,
        std::chrono::steady_clock::time_point start) noexcept
        : request_(&request),
          coding_policy_(coding_policy),
          remote_address_(remote_address),
          client_address_(client_address),
          start_(start) {}

    [[nodiscard]] const http_response_coding_policy& coding_policy() const& noexcept RUVIA_LIFETIMEBOUND {
        return coding_policy_;
    }
    const http_response_coding_policy& coding_policy() const&& = delete;

    [[nodiscard]] request_io_metrics& io_metrics() const& noexcept RUVIA_LIFETIMEBOUND {
        return io_metrics_;
    }
    request_io_metrics& io_metrics() const&& = delete;

    // A generated terminal response must not inherit the negotiated coding
    // promise (and must not be transformed again).
    void disable_response_coding() noexcept {
        coding_policy_ = http_response_coding_policy::disabled();
    }
    void bind_addresses(ip_address remote_address, ip_address client_address) noexcept {
        remote_address_ = remote_address;
        client_address_ = client_address;
    }
    // The listener's Alt-Svc value (RFC 7838 §3), added to every prepared
    // response that does not carry its own; it borrows listener storage.
    void bind_alternative_service(std::string_view value) noexcept {
        alternative_service_ = value;
    }
    // The CORS policy the response streams and buffered responses of this
    // request apply; null when none does.
    [[nodiscard]] static const http_cors_policy* cors(const http_server_options& options) noexcept {
        return options.cors_.has_value() ? &*options.cors_ : nullptr;
    }

    // All arguments borrow the current request owner through completion. Only
    // an uncommitted response may enter. The common response (no blocking-pool
    // encode, no recovery) completes here without a coroutine frame.
    [[nodiscard]] http_response_preparation prepare(
        http_response& response, const http_server_options& options, const worker_ref& worker) const;

    // Completes a preparation whose ready() is null, with the arguments given
    // to prepare(). The application error handler runs at most once here.
    [[nodiscard]] task<prepared_application_response> finish_prepare(const http_response_preparation& preparation,
        http_response& response, const http_server_options& options, const route_table& routes, const context_services& services) const;

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response,
        const http_server_options& options, const worker_ref& worker, const route_table& routes, const context_services& services) const {
        const auto first = prepare(response, options, worker);
        if (const auto* ready = first.ready()) {
            co_return *ready;
        }
        co_return co_await finish_prepare(first, response, options, routes, services);
    }

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response,
        const http_server_options& options, const route_table& routes,
        const context_services& services) const {
        return prepare_or_finish(response, options, services.worker(), routes, services);
    }

    // Records a terminal outcome whose final status was committed. A single
    // null check when no access log is configured.
    void record_access(const access_log_sink& access_log, http_status_code status) const noexcept;

    // Keeps the record of a pipelined response held back for a coalesced
    // write whose head completes at output offset `head_end`; the log reports
    // it once that write commits the head. A no-op without an access log.
    void defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred, std::size_t head_end,
        http_status_code status) const;

private:
    const http_request* request_;
    http_response_coding_policy coding_policy_;
    ip_address remote_address_{};
    ip_address client_address_{};
    std::chrono::steady_clock::time_point start_;
    std::string_view alternative_service_;
    mutable request_io_metrics io_metrics_{};
};

enum class http_request_endpoint : std::uint8_t {
    // Rejections: answer rejection() through respond_http_request_rejection.
    rejection,
    // Admitted endpoints (resolved() is non-null for all but buffered).
    websocket,
    tunnel,
    response_stream,
    stream_body,
    buffered,
};

class http_request_admission final {
public:
    [[nodiscard]] http_request_endpoint endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] const http_request_rejection* rejection() const& noexcept RUVIA_LIFETIMEBOUND {
        return rejection_ ? &*rejection_ : nullptr;
    }
    const http_request_rejection* rejection() const&& = delete;

    // Null for unresolved requests (404/405/OPTIONS/document root).
    [[nodiscard]] const resolved_route* resolved() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolved_;
    }
    const resolved_route* resolved() const&& = delete;

    // CONNECT-UDP (RFC 9298) tunnel; meaningful for endpoint() == tunnel.
    [[nodiscard]] bool udp_tunnel() const noexcept {
        return udp_tunnel_;
    }

    // The connection's request services after applying admission policy.
    [[nodiscard]] const context_services& services() const& noexcept RUVIA_LIFETIMEBOUND {
        return services_;
    }
    const context_services& services() const&& = delete;

    [[nodiscard]] protocol_byte_limit body_limit() const noexcept {
        return body_limit_;
    }

    [[nodiscard]] static_file_selection_mode static_file_mode() const noexcept {
        return services_.precompressed_static_files() ? static_file_selection_mode::precompressed
                                                      : static_file_selection_mode::identity_only;
    }

    [[nodiscard]] http_response_tail& tail() & noexcept RUVIA_LIFETIMEBOUND {
        return tail_;
    }
    http_response_tail& tail() && = delete;

    http_request_admission(const http_request_admission& other) noexcept
        : tail_(other.tail_),
          services_(other.services_),
          rejection_(other.rejection_),
          resolved_(other.resolved_),
          body_limit_(other.body_limit_),
          endpoint_(other.endpoint_),
          udp_tunnel_(other.udp_tunnel_) {
        if (services_.io_metrics() == &other.tail_.io_metrics()) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }

    http_request_admission& operator=(const http_request_admission& other) noexcept {
        if (this != &other) {
            tail_ = other.tail_;
            services_ = other.services_;
            if (services_.io_metrics() == &other.tail_.io_metrics()) {
                services_.bind_io_metrics(&tail_.io_metrics());
            }
            rejection_ = other.rejection_;
            resolved_ = other.resolved_;
            body_limit_ = other.body_limit_;
            endpoint_ = other.endpoint_;
            udp_tunnel_ = other.udp_tunnel_;
        }
        return *this;
    }

    http_request_admission(http_request_admission&& other) noexcept
        : tail_(std::move(other.tail_)),
          services_(other.services_),
          rejection_(std::move(other.rejection_)),
          resolved_(other.resolved_),
          body_limit_(other.body_limit_),
          endpoint_(other.endpoint_),
          udp_tunnel_(other.udp_tunnel_) {
        if (services_.io_metrics() == &other.tail_.io_metrics()) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }

    http_request_admission& operator=(http_request_admission&& other) noexcept {
        if (this != &other) {
            tail_ = std::move(other.tail_);
            services_ = other.services_;
            if (services_.io_metrics() == &other.tail_.io_metrics()) {
                services_.bind_io_metrics(&tail_.io_metrics());
            }
            rejection_ = std::move(other.rejection_);
            resolved_ = other.resolved_;
            body_limit_ = other.body_limit_;
            endpoint_ = other.endpoint_;
            udp_tunnel_ = other.udp_tunnel_;
        }
        return *this;
    }

private:
    friend http_request_admission admit_http_request(const http_request_admission_input& input,
        const http_request_admission_policy& policy, const context_services& services);

    http_request_admission(http_response_tail tail, const context_services& services) noexcept
        : tail_(tail),
          services_(services) {
        if (services_.io_metrics() == nullptr) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }
    http_response_tail tail_;
    context_services services_;
    std::optional<http_request_rejection> rejection_;
    const resolved_route* resolved_{};
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http_request_endpoint endpoint_{http_request_endpoint::buffered};
    bool udp_tunnel_{false};
};

// Runs the admission policy once for one request. `services` are the
// connection's request services (stop token, interim output, trailers, ...).
// Admission borrows the input only during this synchronous call and owns its
// request-local service view; binding it never changes the connection's view.
[[nodiscard]] http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, const context_services& services);

// Admits a buffered in-memory request and dispatches it through the route
// table or the rejection handler.
[[nodiscard]] task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes,
    const http_request_admission_input& input, const http_request_admission_policy& policy,
    const context_services& services, http_response& response);

}  // namespace ruvia::detail

namespace ruvia {
class context;  // only used as context* in a type-erased bind thunk; web supplies the definition
}

namespace ruvia::detail {

// A normal response writer may reject a tunnel but may never establish one.
// Establishment transfers ownership through the protocol's acceptance driver.
inline void validate_response_stream_status(const http_request& request, const http_response& response) {
    if (request.known_method() == http_known_method::connect && response.status().is_successful()) {
        throw std::logic_error("a 2xx response to CONNECT establishes the tunnel; use accept_tunnel()");
    }
    if (response.status() == http_status::switching_protocols) {
        throw std::logic_error("a 101 response requires accept_tunnel() or accept_websocket()");
    }
}

// Plain forwarding functions, not coroutines: each returns the sink's own task,
// so a streamed chunk costs exactly the sink's frame and no wrapper frame.
template <typename sink>
task<stream_commit_result> response_stream_commit_thunk(void* target) {
    return static_cast<sink*>(target)->commit();
}

template <typename sink>
task<void> response_stream_write_thunk(void* target, std::string_view chunk) {
    return static_cast<sink*>(target)->write(chunk);
}

template <typename sink>
task<void> response_stream_end_thunk(void* target, std::span<const http_header_view> trailers) {
    return static_cast<sink*>(target)->end(trailers);
}

template <typename sink>
task<timer_sleep_result> response_stream_sleep_thunk(
    void* target, std::chrono::milliseconds duration, const stop_token& stop_token_value) {
    return static_cast<sink*>(target)->sleep(duration, stop_token_value);
}

template <typename sink>
bool response_stream_aborted_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->aborted();
}

template <typename sink>
void response_stream_bind_context_thunk(void* target, context* context_value) {
    static_cast<sink*>(target)->bind_context(context_value);
}

template <typename sink>
void response_stream_release_context_thunk(void* target) noexcept {
    static_cast<sink*>(target)->release_context();
}

template <typename sink>
bool response_stream_committed_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->committed();
}

template <typename sink>
[[nodiscard]] response_stream_writer make_response_stream_writer(sink& sink_value) noexcept {
    return streaming_access::make_response_stream_writer(&sink_value, sink_value.output_interrupts(), &response_stream_commit_thunk<sink>, &response_stream_write_thunk<sink>,
        &response_stream_end_thunk<sink>, &response_stream_sleep_thunk<sink>,
        &response_stream_bind_context_thunk<sink>, &response_stream_release_context_thunk<sink>,
        &response_stream_committed_thunk<sink>, &response_stream_aborted_thunk<sink>);
}

class response_stream_completed final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;

    explicit constexpr response_stream_completed(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

class response_stream_aborted_before_commit final {
private:
    friend class response_stream_dispatch_result;
    constexpr response_stream_aborted_before_commit() noexcept = default;
};

class response_stream_aborted_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;
    explicit constexpr response_stream_aborted_after_commit(http_status_code status) noexcept
        : status_(status) {}
    http_status_code status_;
};

// The handler failed after its head was already on the wire. The status is what
// the peer was told; the exception is why it will never receive the rest. The
// status alone cannot be reported to anyone -- it says 200 -- so the exception
// travels with it to the transport, which is the last owner able to report it.
class response_stream_failed_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // Never null.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend class response_stream_dispatch_result;

    response_stream_failed_after_commit(http_status_code status, std::exception_ptr exception) noexcept
        : status_(status),
          exception_(std::move(exception)) {}

    http_status_code status_;
    std::exception_ptr exception_;
};

class response_stream_route_response final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_route_response(http_response response) noexcept
        : response_(std::move(response)) {}

    http_response response_;
};

class response_stream_recovered_failure final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_recovered_failure(http_response response) noexcept
        : response_(std::move(response)) {}

    http_response response_;
};

// Each route terminal is one alternative. Commit-bearing alternatives own the
// exact wire status; response-bearing alternatives own the movable response.
// No separate outcome discriminator is needed.
class response_stream_dispatch_result final {
public:
    [[nodiscard]] static response_stream_dispatch_result make_completed(
        http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_completed(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_aborted_before_commit() noexcept {
        return response_stream_dispatch_result(response_stream_aborted_before_commit{});
    }

    [[nodiscard]] static response_stream_dispatch_result make_aborted_after_commit(http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_aborted_after_commit(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_failed_after_commit(
        http_status_code status, std::exception_ptr exception) noexcept {
        return response_stream_dispatch_result(
            response_stream_failed_after_commit(status, std::move(exception)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_route_response(
        http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_route_response(std::move(response)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_recovered_failure(
        http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_recovered_failure(std::move(response)));
    }

    [[nodiscard]] const response_stream_completed* completed() const& noexcept {
        return std::get_if<response_stream_completed>(&value_);
    }
    const response_stream_completed* completed() const&& = delete;

    [[nodiscard]] const response_stream_aborted_before_commit* aborted_before_commit() const& noexcept {
        return std::get_if<response_stream_aborted_before_commit>(&value_);
    }
    const response_stream_aborted_before_commit* aborted_before_commit() const&& = delete;

    [[nodiscard]] const response_stream_aborted_after_commit* aborted_after_commit() const& noexcept {
        return std::get_if<response_stream_aborted_after_commit>(&value_);
    }
    const response_stream_aborted_after_commit* aborted_after_commit() const&& = delete;

    [[nodiscard]] const response_stream_failed_after_commit* failed_after_commit() const& noexcept {
        return std::get_if<response_stream_failed_after_commit>(&value_);
    }
    const response_stream_failed_after_commit* failed_after_commit() const&& = delete;

    [[nodiscard]] response_stream_route_response* route_response() & noexcept {
        return std::get_if<response_stream_route_response>(&value_);
    }
    response_stream_route_response* route_response() && = delete;

    [[nodiscard]] response_stream_recovered_failure* recovered_failure() & noexcept {
        return std::get_if<response_stream_recovered_failure>(&value_);
    }
    response_stream_recovered_failure* recovered_failure() && = delete;

    [[nodiscard]] std::optional<http_status_code> committed_status() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = aborted_after_commit()) {
            return value->status();
        }
        if (const auto* value = failed_after_commit()) {
            return value->status();
        }
        return std::nullopt;
    }

private:
    using value_type = std::variant<response_stream_completed, response_stream_aborted_before_commit,
        response_stream_aborted_after_commit, response_stream_failed_after_commit,
        response_stream_route_response, response_stream_recovered_failure>;

    template <typename alternative_type>
    explicit response_stream_dispatch_result(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

// Only called once sink_value.committed() holds.
template <typename sink>
[[nodiscard]] http_status_code committed_response_stream_status(const sink& sink_value) {
    return sink_value.commit_plan()->response_status();
}

struct response_stream_no_peer_abort final {
    [[nodiscard]] constexpr bool operator()() const noexcept {
        return false;
    }
};

// Drives a response-stream route over an already-constructed sink. Connection
// persistence remains a transport concern when a buffered response is returned.
template <typename sink, typename peer_aborted_type = response_stream_no_peer_abort>
task<response_stream_dispatch_result> dispatch_response_stream_with(sink& sink_value, const route_table& routes_value,
    const http_request& request, const resolved_route& route,
    const context_services& services, peer_aborted_type peer_aborted = {}) {
    auto response_stream = make_response_stream_writer(sink_value);

    std::exception_ptr exception;
    try {
        auto result_value = co_await routes_value.dispatch_response_stream(
            request, route, response_stream, services);
        if (services.connection_aborted() || services.request_reset() || peer_aborted()) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_aborted_after_commit(
                committed_response_stream_status(sink_value));
        }
        if (!result_value.has_value()) {
            if (!sink_value.committed()) {
                throw std::logic_error("handled response stream has no committed protocol plan");
            }
            co_return response_stream_dispatch_result::make_completed(
                committed_response_stream_status(sink_value));
        }
        if (sink_value.committed()) {
            throw std::logic_error("buffered stream result followed a committed response head");
        }
        co_return response_stream_dispatch_result::make_route_response(std::move(*result_value));
    } catch (const inbound_transport_error&) {
        if (!services.request_reset() && (services.connection_aborted() || peer_aborted())) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_aborted_after_commit(
                committed_response_stream_status(sink_value));
        }
        exception = std::current_exception();
    } catch (...) {
        exception = std::current_exception();
    }
    if (services.connection_aborted() || services.request_reset()) {
        auto result = sink_value.committed()
                          ? response_stream_dispatch_result::make_aborted_after_commit(
                                committed_response_stream_status(sink_value))
                          : response_stream_dispatch_result::make_aborted_before_commit();
        // The transport is already closed, but a real callback failure still
        // belongs to its configured outlet. Snapshot before invoking user code.
        const auto failure = services.connection_failure();
        const auto remote = services.remote_address();
        failure.invoke(remote, exception);
        co_return result;
    }

    if (sink_value.committed()) {
        // Past the point of no return: the status cannot be changed and no
        // error body can be appended, so the exception is handed to the
        // transport rather than dropped with the connection.
        co_return response_stream_dispatch_result::make_failed_after_commit(
            committed_response_stream_status(sink_value), std::move(exception));
    }
    auto response = co_await routes_value.handle_exception(request, exception, services);
    co_return response_stream_dispatch_result::make_recovered_failure(std::move(response));
}

}  // namespace ruvia::detail

// listener_dispatch::request_stream: what a request stream connection resolves
// every request to in place of the route table, and the one run of the
// application's request_stream callback every transport reaches through it.

namespace ruvia::detail {

class route_entry;

// A request stream route runs no middleware and declares no body limit. A
// tunnel request -- CONNECT, extended CONNECT, or an HTTP/1.1 Upgrade without
// content -- resolves to an upgrade route whose callback commits the tunnel
// itself (accept_tunnel()); any other request to a response stream route with
// a streamed body.
[[nodiscard]] route_resolution resolve_request_stream_route(bool tunnel) noexcept;

// Whether `route` is the tunnel route resolve_request_stream_route() yields.
[[nodiscard]] bool is_request_stream_tunnel_route(const route_entry& route) noexcept;

// Whether an HTTP/1 request of a request stream connection is a tunnel
// request; `content` is whether it carries request content.
[[nodiscard]] bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept;

// Runs the connection's callback for a tunnel request in the context its
// upgrade route built. `accept` commits the success head and establishes the
// tunnel; a refusal writes through the protocol's ordinary response stream,
// which the upgrade driver must bind before dispatching this context.
[[nodiscard]] task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept,
    const request_stream_websocket_accept* accept_websocket = nullptr);

}  // namespace ruvia::detail

namespace ruvia::detail {

// The single writer of every HTTP/1 final head that hands the connection to
// another protocol: a WebSocket or CONNECT-UDP 101 and a CONNECT 2xx. RFC 9110
// section 7.8: when the head is 101 (Switching Protocols) and the request
// carried Expect: 100-continue, the HTTP-owned
// http_request_expectations::continue_before_switching_protocols() decision
// puts a 100 (Continue) head before it in the same write, even when no content
// follows. Any other status is written alone. `head` is the complete encoded
// final head; it must stay valid until the returned task completes.
template <typename stream_type>
[[nodiscard]] task<std::error_code> write_http1_upgrade_response_head(stream_type& stream,
    const http_request_expectations& expectations, http_status_code status, std::string_view head,
    connection_scanner::entry& scanner_entry) {
    constexpr std::size_t continue_head_capacity = 64;
    std::array<char, continue_head_capacity> continue_bytes;
    std::string_view continue_head;
    if (status == http_status::switching_protocols && expectations.continue_before_switching_protocols()) {
        const auto prepared = prepare_http1_interim_response_head(
            http_interim_response_head(http_status::continue_value), std::span<char>(continue_bytes));
        const auto* prepared_head = std::get_if<prepared_http1_interim_response>(&prepared);
        if (prepared_head == nullptr) {
            throw std::logic_error("cannot prepare HTTP/1 100 Continue head");
        }
        continue_head = prepared_head->head();
    }
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(continue_head), asio::buffer(head)};
    const connection_scanner::entry::write_scope write_stall_clock(scanner_entry);
    const auto written = co_await ruvia::async_asio<std::size_t>([&stream, &buffers, &scanner_entry](auto handler) {
        asio::async_write(stream, buffers, connection_write_progress(scanner_entry), std::move(handler));
    });
    co_return written.error_code();
}

// The context retains header storage through handshake construction. The
// protocol plan copies the views before this temporary pointer table is freed.
[[nodiscard]] inline inline_http_header_views<> websocket_response_headers(context& context_value) {
    return inline_http_header_views<>(
        context_access::handshake_headers(context_value));
}

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
// The shared HTTP/1 upgrade-head writer applies the request `expectations`
// (100 Continue before 101, RFC 9110 section 7.8).
template <typename stream_type>
task<std::error_code> write_websocket_handshake(stream_type& stream,
    const websocket_server_handshake& handshake, const http_request_expectations& expectations, connection_scanner::entry& scanner_entry) {
    http_response_head_buffer head;
    handshake.for_each_response_part([&](std::string_view part) {
        head.append(part);
    });
    co_return co_await write_http1_upgrade_response_head(
        stream, expectations, http_status::switching_protocols, head.view(), scanner_entry);
}

}  // namespace ruvia::detail

// The shared shape of every upgrade-capable route (CONNECT / CONNECT-UDP
// tunnels and WebSocket upgrades) across HTTP/1 and HTTP/2: run the
// application chain with a terminal that commits the upgrade and
// owns the resulting session; once a session exists it is always finished,
// otherwise a handler failure propagates and an uncommitted response falls
// back to the ordinary buffered tail.

namespace ruvia::detail {

[[nodiscard]] inline websocket_connection_options make_websocket_session_connection_options(
    const http_server_options& options, websocket_compression negotiated,
    const websocket_deflate_config& deflate, protocol_byte_limit receive_limit) noexcept {
    // Directions without context takeover borrow the worker's codecs, which
    // outlive every connection, so they allocate no codec state here.
    return websocket_connection_options{
        .receive_message_limit_ = receive_limit,
        .compression_ = negotiated,
        .deflater_ = deflate.deflater_,
        .shared_deflate_ = &options.codecs_->websocket_deflate(),
    };
}

// Selects the refusal response the server sends after the callback returns.
[[nodiscard]] inline websocket_rejection reject_request_stream_websocket(
    context& context_value, const websocket_handshake_failure& failure) {
    const auto error = failure.protocol_error();
    auto response = context_value.error(copy_http_protocol_error_info(error));
    failure.apply_required_response_headers(response);
    const websocket_rejection rejection{response.status(), error.code()};
    context_access::set_response(context_value, std::move(response));
    return rejection;
}

struct websocket_accept_access final {
    [[nodiscard]] static websocket_accept_result accepted(websocket& session) noexcept {
        return websocket_accept_result(session);
    }
    [[nodiscard]] static websocket_accept_result rejected(websocket_rejection rejection) noexcept {
        return websocket_accept_result(rejection);
    }
};

// `datagrams` is the route's http_tunnel_route_config::datagrams_, which
// CONNECT-UDP (`udp`) always sets.
[[nodiscard]] inline http_response prepare_tunnel_upgrade_response_head(
    context& context_value, http_protocol_version version, bool udp, bool datagrams) {
    auto head_response = context_access::streaming_head(context_value);
    if (udp) {
        auto negotiated = prepare_http_connect_udp_response(std::move(head_response), version);
        if (negotiated.index() != 0) {
            throw std::invalid_argument("invalid CONNECT-UDP response metadata");
        }
        head_response = std::move(std::get<0>(negotiated));
    } else if (datagrams && head_response.status().is_successful()) {
        // RFC 9297 section 3.4: the route's protocol uses the Capsule Protocol,
        // so its successful response says so with exactly one ?1.
        head_response.header_stable_view("Capsule-Protocol", "?1");
    }
    return head_response;
}

// `establish(context&) -> task<void>` commits the upgrade response head,
// emplaces `session` and runs the route's stream handler on it.
// `finish(session_type&, std::exception_ptr) -> task<void>` closes an
// established session and reports a handler failure; it always runs once a
// session exists, before any handler exception is surfaced.
// Returns a buffered response only when the application answered without
// upgrading; an established or abandoned upgrade returns std::nullopt.
template <typename session_type, typename establish_type, typename finish_type>
[[nodiscard]] task<std::optional<http_response>> run_http_upgrade_route(
    const route_table& routes, const http_request& request, const resolved_route& resolved,
    const context_services& services, std::optional<session_type>& session, establish_type& establish,
    finish_type& finish) {
    const auto terminal = callback_access::bind<task<void>(context&)>(establish);
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await routes.dispatch_upgrade(request, resolved, terminal, services);
    } catch (...) {
        failure = std::current_exception();
    }
    if (session.has_value()) {
        co_await finish(*session, failure);
        co_return std::nullopt;
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return buffered;
}

template <typename stream_type>
class websocket_upgrade_commit final {
public:
    static constexpr bool native_stream = websocket_session_connection<stream_type>::native_stream;
    using stream_argument_type = typename websocket_session_connection<stream_type>::stream_argument_type;

    websocket_upgrade_commit(stream_argument_type stream, websocket_compression compression,
        std::string_view subprotocol)
        : stream_(hold_stream(std::forward<stream_argument_type>(stream))),
          compression_(compression) {
        try {
            subprotocol_.assign(subprotocol);
        } catch (...) {
            abort();
            throw;
        }
    }
    [[nodiscard]] stream_argument_type release_stream() noexcept {
        if constexpr (native_stream) {
            return *stream_;
        } else {
            return std::move(stream_);
        }
    }

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::string&& release_subprotocol() noexcept {
        return std::move(subprotocol_);
    }

    void abort() noexcept {
        if constexpr (native_stream) {
            websocket_session_connection<stream_type>::close_stream(*stream_);
        } else {
            stream_.abort();
        }
    }

private:
    using held_stream_type = std::conditional_t<native_stream, stream_type*, stream_type>;

    [[nodiscard]] static held_stream_type hold_stream(stream_argument_type stream) noexcept(native_stream) {
        if constexpr (native_stream) {
            return &stream;
        } else {
            return std::move(stream);
        }
    }

    held_stream_type stream_;
    websocket_compression compression_{};
    std::string subprotocol_;
};

// A committed upgrade, or the refusal its handshake check selected.
template <typename stream_type>
using websocket_upgrade_attempt = std::variant<websocket_upgrade_commit<stream_type>, websocket_rejection>;

template <typename transport_type, typename commit_upgrade_type, typename commit_websocket_type>
struct tunnel_upgrade_session_operation final {
    using session_type = http_tunnel_session<transport_type>;

    const http_request& request_;
    const resolved_route& resolved_;
    const tunnel_route_endpoint& endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    inbound_buffer_account& inbound_;
    bool udp_;
    std::string_view pending_;
    commit_upgrade_type& commit_upgrade_;
    commit_websocket_type& commit_websocket_;
    std::optional<session_type>& session_;
    http_tunnel_route_config config_{endpoint_.config()};
    std::optional<websocket_session_connection<transport_type>> websocket_session_{};
    struct websocket_borrow final {
        explicit websocket_borrow(websocket_session_connection<transport_type>& connection)
            : value_(make_websocket_facade(connection)) {}
        websocket value_;
    };
    std::optional<websocket_borrow> websocket_{};

    // Commits the upgrade head the context's draft describes and opens the
    // session over the transport that head hands over.
    [[nodiscard]] task<void> establish(context& context_value) {
        auto head_response = prepare_tunnel_upgrade_response_head(
            context_value, request_.protocol_version(), udp_, config_.datagrams_);
        std::optional<transport_type> transport;
        if constexpr (requires { commit_upgrade_(context_value, head_response).operator co_await(); }) {
            transport.emplace(co_await commit_upgrade_(context_value, head_response));
        } else {
            transport.emplace(commit_upgrade_(context_value, head_response));
        }
        try {
            session_.emplace(std::move(*transport), services_.worker(), &inbound_,
                config_.datagrams_, pending_);
        } catch (...) {
            transport->abort();
            throw;
        }
        // A synchronous commit leaves no co_await in this instantiation;
        // co_return keeps the body a coroutine for every transport.
        co_return;
    }

    [[nodiscard]] task<http_tunnel*> accept_request_stream_tunnel(context& context_value, http_tunnel_route_config config) {
        config_ = route_endpoint::validate_tunnel_config(
            !request_.protocol().empty() || request_.known_method() != http_known_method::connect, udp_, config);
        co_await establish(context_value);
        scanner_entry_.enter_long_lived();
        co_return &session_->tunnel();
    }

    [[nodiscard]] task<websocket_accept_result> accept_request_stream_websocket(
        context& context_value, const websocket_route_endpoint& websocket_endpoint) {
        const auto headers = websocket_response_headers(context_value);
        auto attempt = co_await commit_websocket_(context_value, websocket_endpoint, headers);
        if (const auto* rejection = std::get_if<websocket_rejection>(&attempt)) {
            co_return websocket_accept_access::rejected(*rejection);
        }
        auto* committed = std::get_if<websocket_upgrade_commit<transport_type>>(&attempt);
        try {
            websocket_session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_,
                websocket_endpoint.lifecycle(),
                protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_),
                pending_,
                make_websocket_session_connection_options(options_, committed->compression(), websocket_endpoint.deflate(),
                    websocket_endpoint.message_limit(options_.server_.max_websocket_message_bytes_)),
                committed->release_subprotocol(), &inbound_);
        } catch (...) {
            committed->abort();
            throw;
        }
        websocket_.emplace(*websocket_session_);
        scanner_entry_.enter_long_lived();
        co_return websocket_accept_access::accepted(websocket_->value_);
    }

    [[nodiscard]] task<void> operator()(context& context_value) {
        if (is_request_stream_tunnel_route(resolved_.route())) {
            auto accept = [this, &context_value](http_status_code, http_tunnel_route_config config) {
                return accept_request_stream_tunnel(context_value, config);
            };
            const auto accept_ref = callback_access::bind<task<http_tunnel*>(http_status_code, http_tunnel_route_config)>(accept);
            auto accept_websocket = [this, &context_value](const websocket_route_endpoint& endpoint) {
                return accept_request_stream_websocket(context_value, endpoint);
            };
            const auto websocket_accept_ref =
                callback_access::bind<task<websocket_accept_result>(const websocket_route_endpoint&)>(accept_websocket);
            std::exception_ptr failure;
            try {
                co_await serve_request_stream_tunnel(context_value, accept_ref, &websocket_accept_ref);
            } catch (...) {
                failure = std::current_exception();
            }
            // Retire borrowed operations before draining the owned connection,
            // exactly as the route handler's stack facade does.
            websocket_.reset();
            if (websocket_session_) {
                co_await finish_websocket_session(*websocket_session_, failure,
                    options_.connection_failure_, services_.remote_address());
            } else if (failure) {
                std::rethrow_exception(failure);
            }
            co_return;
        }
        co_await establish(context_value);
        co_await invoke_tunnel_handler(
            *session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_tunnel_session(session_value, std::move(failure),
            options_.connection_failure_, services_.remote_address(),
            scanner_entry_, config_.peer_transport_fin_timeout_);
    }
};

template <typename transport_type, typename commit_upgrade_type, typename commit_websocket_type>
[[nodiscard]] task<std::optional<http_response>> execute_tunnel_upgrade_session(
    const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, const http_server_options& options,
    connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound, bool udp,
    std::string_view pending, commit_upgrade_type commit_upgrade, commit_websocket_type commit_websocket) {
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    using operation_type = tunnel_upgrade_session_operation<transport_type, commit_upgrade_type, commit_websocket_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .request_ = request,
        .resolved_ = resolved,
        .endpoint_ = endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .udp_ = udp,
        .pending_ = pending,
        .commit_upgrade_ = commit_upgrade,
        .commit_websocket_ = commit_websocket,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(
        routes, request, resolved, services, session, operation, operation);
}

template <typename stream_type, typename commit_upgrade_type>
struct websocket_upgrade_session_operation final {
    using session_type = websocket_session_connection<stream_type>;

    const resolved_route& resolved_;
    const websocket_route_endpoint& websocket_endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    inbound_buffer_account& inbound_;
    std::string_view pending_frames_;
    commit_upgrade_type& commit_upgrade_;
    std::optional<session_type>& session_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        const auto response_headers_value = websocket_response_headers(context_value);
        std::optional<websocket_upgrade_commit<stream_type>> committed;
        if constexpr (requires { commit_upgrade_(context_value, response_headers_value).operator co_await(); }) {
            committed.emplace(co_await commit_upgrade_(context_value, response_headers_value));
        } else {
            committed.emplace(commit_upgrade_(context_value, response_headers_value));
        }
        try {
            session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_,
                websocket_endpoint_.lifecycle(),
                protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_),
                pending_frames_,
                make_websocket_session_connection_options(
                    options_, committed->compression(), websocket_endpoint_.deflate(),
                    websocket_endpoint_.message_limit(options_.server_.max_websocket_message_bytes_)),
                committed->release_subprotocol(), &inbound_);
        } catch (...) {
            committed->abort();
            throw;
        }
        co_await invoke_websocket_handler(
            *session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_websocket_session(session_value, std::move(failure),
            options_.connection_failure_, services_.remote_address());
    }
};

template <typename stream_type, typename commit_upgrade_type>
[[nodiscard]] task<std::optional<http_response>> execute_websocket_upgrade_session(
    const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, const http_server_options& options,
    connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound,
    std::string_view pending_frames, commit_upgrade_type commit_upgrade) {
    const auto& websocket_endpoint = *resolved.route().endpoint().websocket();
    using operation_type = websocket_upgrade_session_operation<stream_type, commit_upgrade_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .resolved_ = resolved,
        .websocket_endpoint_ = websocket_endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .pending_frames_ = pending_frames,
        .commit_upgrade_ = commit_upgrade,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(
        routes, request, resolved, services, session, operation, operation);
}

}  // namespace ruvia::detail

namespace ruvia {

class context;

namespace detail {

class response_stream_state final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return commit_plan() != nullptr;
    }

    [[nodiscard]] bool ended() const noexcept {
        return std::holds_alternative<ended_type>(state_);
    }

    [[nodiscard]] bool aborted() const noexcept {
        return std::holds_alternative<aborted_before_commit_type>(state_) ||
               std::holds_alternative<aborted_after_commit_type>(state_);
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        if (const auto* value = std::get_if<body_open_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<ended_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<aborted_after_commit_type>(&state_)) {
            return &value->plan_;
        }
        return nullptr;
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    void bind_context(context* context_value) {
        if (!std::holds_alternative<unbound_type>(state_)) {
            throw std::logic_error("response stream context is already bound");
        }
        if (context_value == nullptr) {
            throw std::invalid_argument("response stream context binding is incomplete");
        }
        state_.emplace<bound_type>(context_value);
    }

    void release_context() noexcept {
        if (std::holds_alternative<bound_type>(state_)) {
            state_.emplace<detached_type>();
        }
    }

    [[nodiscard]] context& bound_context() const {
        const auto* bound = std::get_if<bound_type>(&state_);
        if (bound == nullptr) {
            if (committed()) {
                throw std::logic_error("response stream is already committed");
            }
            throw std::logic_error("response stream context is not bound");
        }
        return *bound->context_;
    }

    void mark_committed(http_response_stream_commit_plan plan) {
        if (committed() || aborted()) {
            throw std::logic_error("response stream is already committed");
        }
        switch (plan.head_disposition()) {
            case http_response_stream_head_disposition::body_open:
                state_.emplace<body_open_type>(plan);
                break;
            case http_response_stream_head_disposition::message_ended:
                state_.emplace<ended_type>(plan);
                break;
        }
    }

    void mark_ended() {
        if (ended()) {
            return;
        }
        if (auto* value = std::get_if<body_open_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<ended_type>(plan);
            return;
        }
        throw std::logic_error("response stream is not committed");
    }

    void mark_aborted() noexcept {
        if (aborted()) {
            return;
        }
        if (const auto* committed_plan = commit_plan()) {
            // A body-suppressed commit is already logically ended, but its head
            // can still fail during transport delivery. Preserve that commit.
            const auto plan = *committed_plan;
            state_.emplace<aborted_after_commit_type>(plan);
            return;
        }
        state_.emplace<aborted_before_commit_type>();
    }

    void ensure_body_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<body_open_type>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    // Trailers are optional metadata (RFC 9110 6.5): framing that cannot carry
    // them discards them, so only a status that can never carry content is a
    // handler error here. The caller writes trailers only if trailers_available().
    void ensure_trailers_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        const auto* plan = commit_plan();
        if (plan == nullptr || !plan->status_permits_trailers()) {
            throw std::logic_error("response status does not permit trailers");
        }
    }

private:
    struct unbound_type final {};

    struct detached_type final {};

    struct bound_type final {
        explicit bound_type(context* bound_context) noexcept
            : context_(bound_context) {}

        context* context_;
    };

    struct body_open_type final {
        explicit body_open_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct ended_type final {
        explicit ended_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct aborted_before_commit_type final {};

    struct aborted_after_commit_type final {
        explicit aborted_after_commit_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    using state_type = std::variant<unbound_type, bound_type, detached_type, body_open_type, ended_type,
        aborted_before_commit_type, aborted_after_commit_type>;

    state_type state_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

// Owns the response-coding lifecycle of HTTP/1 and HTTP/2 stream sinks.
// The transport only delivers encoded slices; it never reconstructs whether a
// coding was selected, whether the body is suppressed, or whether an encoder
// exists. The encoder itself is borrowed from the worker's codecs for the
// stream's lifetime, not constructed per response.
class http_streaming_response_compression final {
public:
    // Application input encoded per step. It bounds the encoded buffer and the
    // codec work between two transport writes independently of the size of
    // one application write.
    static constexpr std::size_t input_slice_bytes = 16 * 1024;

    // `compression` is the server's compression policy, or nullptr when the
    // server compresses nothing.
    http_streaming_response_compression(worker_codecs& codecs,
        http_response_coding_selection selection, const compression_config* compression) noexcept
        : codecs_(codecs),
          selection_(selection),
          compression_(compression) {}
    http_streaming_response_compression(worker_codecs&&,
        http_response_coding_selection, const compression_config*) = delete;

    http_streaming_response_compression(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression& operator=(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression(http_streaming_response_compression&&) = delete;
    http_streaming_response_compression& operator=(http_streaming_response_compression&&) = delete;

    // Applies the representation headers before the protocol-owned stream head
    // is committed. A forbidden identity fallback, including a negotiation the
    // selection already reports as failed, is rejected here, while the encoder
    // itself is delayed until activate() has a final body plan.
    void prepare(const http_request& request, http_response& response) {
        if (!std::holds_alternative<unprepared_type>(state_)) {
            throw std::logic_error("streaming response compression is already prepared");
        }
        const auto request_method = request.known_method();
        const auto decision = prepare_response_compression(selection_, request_method, response,
            http_response_compression_source::stream,
            compression_ != nullptr ? http_response_coding_availability::identity_and_compression
                                    : http_response_coding_availability::identity_only);
        if (decision != http_response_compression_decision::encode || !stream_length_allows_encoding(response)) {
            if (http_response_coding_fallback_forbidden(selection_, request_method, response, &request)) {
                throw http_error(http_not_acceptable_coding_error());
            }
            state_.emplace<inactive_type>();
            return;
        }

        response.apply_content_encoding(http_content_coding_token(selection_.coding()));
        state_.emplace<pending_type>();
    }

    // Binds encoder ownership to the protocol body plan. Body-suppressed
    // responses retain their selected representation metadata but never create
    // an encoder for bytes that the protocol will not send.
    void activate(http_response_body_plan body_plan) {
        if (std::holds_alternative<inactive_type>(state_)) {
            return;
        }
        const auto* pending = std::get_if<pending_type>(&state_);
        if (pending == nullptr) {
            throw std::logic_error("streaming response compression is not pending");
        }
        if (body_plan.body_suppressed()) {
            state_.emplace<inactive_type>();
            return;
        }
        state_.emplace<active_type>(codecs_.acquire_stream_encoder(selection_.coding()));
    }

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<active_type>(state_);
    }

    // A stream-head transaction may prepare representation metadata before the
    // protocol layer accepts the head. If that later preparation fails, the
    // sink is terminal before any body byte can be retried; keep the encoder
    // lifecycle in the same terminal state instead of leaving pending/inactive
    // behind for a second commit attempt.
    void abort() noexcept {
        encoded_chunk_.clear();
        state_.emplace<failed_type>();
    }

    // The one slicing loop of both protocols: encodes one application write in
    // input_slice_bytes steps and hands every non-empty encoded slice to
    // `deliver` (the sink's framed write, returning task<void>). Only the last
    // slice flushes, so each application write ends with exactly one codec
    // flush, which keeps SSE events prompt without flushing every slice.
    template <typename deliver_type>
    task<void> write(std::string_view chunk, deliver_type deliver) {
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            const auto count = (std::min)(input_slice_bytes, chunk.size() - offset);
            const auto slice = chunk.substr(offset, count);
            offset += count;
            encode_slice(slice, offset == chunk.size());
            if (!encoded_chunk_.empty()) {
                co_await deliver(std::string_view(encoded_chunk_));
            }
            release_output();
        }
    }

    void finish() {
        encoded_chunk_.clear();
        if (std::holds_alternative<finished_type>(state_)) {
            return;
        }
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            throw std::logic_error("streaming response encoder is not active");
        }
        try {
            active_state->lease_.encoder().finish(encoded_chunk_);
            state_.emplace<finished_type>();
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    [[nodiscard]] std::string_view output() const& noexcept {
        return encoded_chunk_;
    }
    std::string_view output() const&& = delete;

    // A sent chunk no longer has a transport borrower. Keep ordinary stream
    // buffers reusable without retaining unusually large writes for the life
    // of a long-running handler.
    void release_output() {
        constexpr std::size_t retained_bytes = 64 * 1024;
        http_clear_string_retaining_small(encoded_chunk_, retained_bytes);
    }

private:
    struct unprepared_type final {};
    // Identity output and protocol-suppressed bodies both need no encoder.
    struct inactive_type final {};
    struct pending_type final {};
    struct finished_type final {};
    struct failed_type final {};

    struct active_type final {
        explicit active_type(stream_encoder_lease lease) noexcept
            : lease_(std::move(lease)) {}

        stream_encoder_lease lease_;
    };

    using state_type = std::variant<unprepared_type, inactive_type, pending_type, finished_type, failed_type, active_type>;

    // A length the head declares sizes the stream like a buffered body; an
    // undeclared length leaves the decision to the negotiated coding.
    [[nodiscard]] bool stream_length_allows_encoding(const http_response& response) const noexcept {
        const auto declared = declared_content_length(response);
        return !declared.has_value() || declared_length_permits_encoding(selection_, *compression_, *declared);
    }

    void encode_slice(std::string_view input, bool flush) {
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            encoded_chunk_.clear();
            throw std::logic_error("streaming response encoder is not active");
        }
        encoded_chunk_.clear();
        try {
            active_state->lease_.encoder().write(input, encoded_chunk_, flush);
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    // A codec that failed mid-stream is never returned to the worker pool.
    void fail_encoder(active_type& active_state) noexcept {
        active_state.lease_.discard();
        abort();
    }

    worker_codecs& codecs_;
    http_response_coding_selection selection_;
    const compression_config* compression_;
    std::string encoded_chunk_;
    state_type state_;
};

}  // namespace ruvia::detail

namespace ruvia {
class context;
}

namespace ruvia::detail {

// Inactivity belongs to the connection; request bodies and output waits have
// independent per-stream deadlines.
[[nodiscard]] inline connection_scanner::phase multiplexed_inactivity_phase(bool peer_settings_received,
    bool header_block_in_progress, bool stream_active, bool tunnel_active) noexcept {
    if (!peer_settings_received || header_block_in_progress) {
        return connection_scanner::phase::reading_initial;
    }
    if (tunnel_active) {
        return connection_scanner::phase::long_lived;
    }
    return stream_active ? connection_scanner::phase::idle : connection_scanner::phase::awaiting_stream;
}

class multiplexed_request_body_deadline final {
public:
    void open(std::int64_t now_ms) noexcept {
        open_ = true;
        opened_ms_ = now_ms;
        progress_ms_ = now_ms;
    }
    void progress(std::int64_t now_ms) noexcept {
        progress_ms_ = now_ms;
    }
    void close() noexcept {
        open_ = false;
    }
    [[nodiscard]] bool overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return open_ &&
               ((inactivity.has_value() && now_ms - progress_ms_ >= inactivity->value().count()) ||
                   (completion.has_value() && now_ms - opened_ms_ >= completion->value().count()));
    }

private:
    std::int64_t opened_ms_{0};
    std::int64_t progress_ms_{0};
    bool open_{false};
};

// A timeout drain must end the connection at its bound; an ordinary drain only
// finalizes GOAWAY. Protocol owners keep their own wake/close ordering.
class multiplexed_drain_timer final {
public:
    void request_timeout() noexcept {
        timeout_drain_ = true;
    }
    [[nodiscard]] bool needed(bool terminated, bool final_boundary) const noexcept {
        return !timer_.registered() && !terminated && (!final_boundary || timeout_drain_);
    }
    void mark_expired() noexcept {
        expired_ = timeout_drain_;
    }
    [[nodiscard]] bool expired() const noexcept {
        return expired_;
    }
    void cancel() noexcept {
        timer_.cancel();
    }

    template <typename duration_type, typename expired_type, typename failure_type>
    void arm(const worker_ref& worker_value, duration_type bound,
        expired_type on_expired, failure_type on_failure) noexcept {
        try {
            const auto status = worker_value.schedule_timer(timer_,
                deadline_after(bound), [on_expired](worker_timer_outcome outcome) {
                    // Cancellation belongs to worker shutdown, which closes the session.
                    if (outcome == worker_timer_outcome::expired) {
                        on_expired();
                    }
                });
            if (status == worker_timer_schedule_status::worker_stopping) {
                on_expired();
            }
        } catch (...) {
            on_failure();
        }
    }

private:
    bool timeout_drain_{false};
    bool expired_{false};
    // Unregister before the state captured by its completion can retire.
    worker_timer_registration timer_;
};

// Connection-owned terminal state shared by every Web capability derived from
// one multiplexed HTTP/2 or HTTP/3 session. Stream waits that must end on
// termination combine their own stop token with token().
class multiplexed_session_termination final {
public:
    multiplexed_session_termination() noexcept = default;
    multiplexed_session_termination(const multiplexed_session_termination&) = delete;
    multiplexed_session_termination& operator=(const multiplexed_session_termination&) = delete;

    [[nodiscard]] bool terminated() const noexcept {
        return static_cast<bool>(error_);
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return error_;
    }

    [[nodiscard]] bool terminate(std::error_code error) noexcept {
        if (terminated()) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::connection_aborted);
        if (stop_source_) {
            stop_source_->request_stop();
        }
        return true;
    }

    // Allocated on first use; already stopped once the session terminated.
    [[nodiscard]] stop_token token() {
        if (!stop_source_) {
            stop_source_.emplace();
            if (terminated()) {
                stop_source_->request_stop();
            }
        }
        return stop_source_->token();
    }

private:
    std::error_code error_;
    std::optional<stop_source> stop_source_;
};

// The one asynchronous wake primitive of a dispatched multiplexed stream. Body
// readers and writers waiting for send window or output budget share it and
// always re-check their own readiness after a wake.
class multiplexed_stream_signal final {
public:
    class output_wait_guard final {
    public:
        explicit output_wait_guard(multiplexed_stream_signal& signal) noexcept
            : signal_(signal) {
            signal_.set_waiting_output(true);
        }
        ~output_wait_guard() noexcept {
            signal_.set_waiting_output(false);
        }
        output_wait_guard(const output_wait_guard&) = delete;
        output_wait_guard& operator=(const output_wait_guard&) = delete;

    private:
        multiplexed_stream_signal& signal_;
    };

    multiplexed_stream_signal(const worker_ref& worker_value, multiplexed_session_termination& termination)
        : signal_(worker_value),
          termination_(termination) {}
    multiplexed_stream_signal(worker_ref&&, multiplexed_session_termination&) = delete;

    void wake() noexcept {
        signal_.notify();
    }

    // The stream ended abnormally (peer reset, local reset): every wait ends.
    void stop() noexcept {
        stopped_ = true;
        if (stop_source_) {
            stop_source_->request_stop();
        }
        wake();
    }

    [[nodiscard]] bool stopped() const noexcept {
        return stopped_ || termination_.terminated();
    }

    [[nodiscard]] stop_token terminal_stop_token() {
        if (!stop_source_) {
            stop_source_.emplace();
        }
        if (stopped()) {
            stop_source_->request_stop();
        }
        return stop_source_->token();
    }

    [[nodiscard]] bool terminated() const noexcept {
        return termination_.terminated();
    }

    [[nodiscard]] std::error_code terminal_error() const noexcept {
        return termination_.error();
    }

    [[nodiscard]] multiplexed_session_termination& termination() & noexcept RUVIA_LIFETIMEBOUND {
        return termination_;
    }
    multiplexed_session_termination& termination() && = delete;

    // Spans one whole wait for peer window / acknowledgements to free output
    // capacity; the connection scanner times it against write_timeout_.
    void set_waiting_output(bool waiting) noexcept {
        waiting_output_ = waiting;
        output_wait_observed_ms_.reset();
        output_wait_expired_ = false;
    }

    [[nodiscard]] bool waiting_output() const noexcept {
        return waiting_output_;
    }

    // True once per wait, when it has been observed for `timeout` in the
    // scanner's clock. The first observation starts the clock.
    [[nodiscard]] bool output_wait_expired(std::int64_t now_ms, timeout_duration timeout) noexcept {
        if (!waiting_output_ || output_wait_expired_) {
            return false;
        }
        if (!output_wait_observed_ms_.has_value()) {
            output_wait_observed_ms_ = now_ms;
        }
        output_wait_expired_ = now_ms - *output_wait_observed_ms_ >= timeout.value().count();
        return output_wait_expired_;
    }

    [[nodiscard]] worker_signal::wait_operation wait() {
        return signal_.wait();
    }

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_.worker();
    }
    const worker_ref& worker() const&& = delete;

private:
    worker_signal signal_;
    multiplexed_session_termination& termination_;
    std::optional<stop_source> stop_source_;
    bool stopped_{false};
    bool waiting_output_{false};
    bool output_wait_expired_{false};
    std::optional<std::int64_t> output_wait_observed_ms_;
};

// Streams of one multiplexed connection that may open or read a file body on
// the blocking pool at once; the rest wait holding no read buffer and no pool
// task.
inline constexpr std::size_t multiplexed_file_read_slots = 4;

struct multiplexed_buffered_response_write_result final {
    // The final status, once the response head was submitted.
    std::optional<http_status_code> committed_status_{};
    // Why a file body failed. Without committed_status_ nothing of the
    // response was sent and the stream still awaits one.
    std::optional<response_file_failure> file_failure_{};
};

class multiplexed_file_read_limiter final {
public:
    class slot final {
    public:
        slot(slot&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)) {}
        ~slot() noexcept {
            if (owner_ != nullptr) {
                auto* const owner = std::exchange(owner_, nullptr);
                --owner->active_;
                owner->released_.notify();
            }
        }

        slot(const slot&) = delete;
        slot& operator=(const slot&) = delete;
        slot& operator=(slot&&) = delete;

    private:
        friend class multiplexed_file_read_limiter;

        explicit slot(multiplexed_file_read_limiter& owner) noexcept
            : owner_(&owner) {}

        multiplexed_file_read_limiter* owner_;
    };

    explicit multiplexed_file_read_limiter(
        const worker_ref& worker_value, std::size_t max_slots = multiplexed_file_read_slots) noexcept
        : released_(worker_value),
          max_slots_(max_slots) {}
    multiplexed_file_read_limiter(worker_ref&&, std::size_t = multiplexed_file_read_slots) = delete;
    multiplexed_file_read_limiter(const multiplexed_file_read_limiter&) = delete;
    multiplexed_file_read_limiter& operator=(const multiplexed_file_read_limiter&) = delete;

    // Admission and its return obligation travel as one move-only value.
    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<slot>> acquire(stream_ended_predicate stream_ended) {
        while (active_ >= max_slots_) {
            co_await released_.wait();
            if (stream_ended()) {
                co_return std::nullopt;
            }
        }
        ++active_;
        co_return std::optional<slot>(slot(*this));
    }

    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<multiplexed_buffered_response_write_result>> open_file_body(
        std::optional<response_file_source>& source, const http_response& response,
        const response_file_io& file_io, stream_ended_predicate stream_ended) {
        source.emplace(response, file_io);
        bool opened = false;
        {
            const auto held = co_await acquire(stream_ended);
            if (!held) {
                co_return multiplexed_buffered_response_write_result{};
            }
            opened = co_await source->open_first();
        }
        if (stream_ended()) {
            co_return multiplexed_buffered_response_write_result{};
        }
        if (!opened) {
            co_return multiplexed_buffered_response_write_result{.file_failure_ = *source->failure()};
        }
        co_return std::nullopt;
    }

private:
    worker_signal released_;
    std::size_t max_slots_;
    std::size_t active_{0};
};

// Shared state and lifecycle for HTTP/2 and HTTP/3 streaming response sinks.
// Derived sinks supply the protocol-specific head submission, encoded DATA
// write, terminal finish, and abort check.
template <typename derived_type, typename stream_type>
class multiplexed_response_stream_sink_base {
public:
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted() || (!state_.ended() && (derived().stream_aborted() || signal_.terminated()));
    }

    // Routes the stop requests of the writer's output operations.
    [[nodiscard]] operation_interrupt_source& output_interrupts() & noexcept RUVIA_LIFETIMEBOUND {
        return output_interrupts_;
    }
    operation_interrupt_source& output_interrupts() && = delete;

    task<stream_commit_result> commit() {
        if (state_.committed()) {
            throw std::logic_error("response stream is already committed");
        }
        co_await commit_head(http_response_trailer_intent::none);
        co_return state_.ended() ? stream_commit_result::completed : stream_commit_result::body;
    }

    void bind_context(context* context_value) {
        interim_output_ = context_value != nullptr ? context_access::interim_output(*context_value) : nullptr;
        state_.bind_context(context_value);
    }

    void release_context() noexcept {
        state_.release_context();
    }

    task<void> write(std::string_view chunk) {
        throw_if_terminated();
        throw_if_body_failed();
        state_.ensure_body_allowed();
        if (chunk.empty()) {
            co_return;
        }
        if (compression_.active()) {
            try {
                co_await compression_.write(
                    chunk, [this](std::string_view encoded) { return write_encoded(encoded); });
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_return;
        }
        co_await write_encoded(chunk);
    }

    task<void> write_encoded(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        try {
            co_await derived().write_encoded_data(chunk);
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
    }

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        // Connection termination wakes the sleep through the combined token and
        // then fails it; the caller's own stop still reports stop_requested.
        const stop_group stop_owner(stop_token_value, signal_.termination().token());
        const auto result = co_await sleep_for(signal_.worker(), duration, stop_owner.token());
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::sleep_terminated_error_message);
        }
        if (derived().stream_aborted()) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), derived_type::sleep_reset_error_message);
        }
        co_return result;
    }

    task<void> end(std::span<const http_header_view> trailers) {
        // An empty terminal call is idempotent once the content ended.
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }
        throw_if_terminated();
        throw_if_body_failed();

        const auto trailer_section = validate_http_response_trailers(trailers);
        const auto trailer_intent = response_trailer_intent(trailer_section);
        if (!state_.committed()) {
            co_await commit_head(trailer_intent);
        }
        throw_if_body_failed();
        if (state_.ended()) {
            co_return;
        }
        if (!trailer_section.empty()) {
            state_.ensure_trailers_allowed();
        }
        if (compression_.active()) {
            try {
                compression_.finish();
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_await write_encoded(compression_.output());
            compression_.release_output();
        }
        try {
            if constexpr (std::is_void_v<decltype(derived().finish_stream_response(trailer_section))>) {
                derived().finish_stream_response(trailer_section);
            } else {
                co_await derived().finish_stream_response(trailer_section);
            }
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
        state_.mark_ended();
    }

protected:
    multiplexed_response_stream_sink_base(stream_type& stream, multiplexed_stream_signal& signal, http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding, const compression_config* compression,
        const http_cors_policy* cors) noexcept
        : stream_(stream),
          signal_(signal),
          request_method_(request_method),
          cors_(cors),
          compression_(codecs, response_coding, compression) {}

    void throw_if_terminated() const {
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
        }
    }

    void throw_if_body_failed() {
        if (const auto& body_failure = stream_.body_failure(); body_failure.has_value()) {
            if (state_.committed()) {
                state_.mark_aborted();
            }
            if (*body_failure == http_request_body_failure::buffer_limit) {
                throw inbound_buffer_limit_error{};
            }
            throw http_request_body_protocol_error(*body_failure);
        }
    }

    stream_type& stream_;
    multiplexed_stream_signal& signal_;
    response_stream_state state_;
    http_interim_response_output* interim_output_{};
    http_known_method request_method_{http_known_method::unknown};
    const http_cors_policy* cors_;
    http_streaming_response_compression compression_;
    operation_interrupt_source output_interrupts_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }

    task<void> commit_head(http_response_trailer_intent trailer_intent) {
        throw_if_terminated();
        throw_if_body_failed();
        try {
            if (context_access::output_committed(state_.bound_context())) {
                throw std::logic_error("the request's tunnel is already established");
            }
            auto response = co_await context_access::commit_streaming_head(state_.bound_context());
            validate_response_stream_status(context_access::request(state_.bound_context()), response);
            if (cors_ != nullptr) {
                (void)cors_->apply_response_headers(
                    context_access::request(state_.bound_context()), response);
            }
            throw_if_terminated();
            throw_if_body_failed();
            compression_.prepare(context_access::request(state_.bound_context()), response);
            const auto commit_body_plan = plan_http_response_body(request_method_, response.status());
            compression_.activate(commit_body_plan);
            const auto submitted_head = derived().submit_streaming_head(std::move(response), trailer_intent);
            if (interim_output_ != nullptr) {
                interim_output_->commit_final();
            }
            stream_.mark_response_committed();
            state_.mark_committed(submitted_head);
            derived().wake_writer();
        } catch (...) {
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
    }
};

// Shared streaming request-body reader loop for HTTP/2 and HTTP/3 streams.
// Derived readers provide protocol-specific body failure, reset check, chunk
// extraction (returning flow-control credit), and end-of-body check.
template <typename derived_type>
class multiplexed_request_body_reader_base {
public:
    // An interrupted read leaves queued chunks and flow-control credit for the
    // next read.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) {
        operation_interrupt interrupt(interrupts_, signal_.worker(), operation, &signal_, &wake_signal,
            "request body read");
        throw_if_body_failed();
        if (auto* continue_output = std::exchange(continue_output_, nullptr)) {
            co_await continue_output->send_continue();
        }
        for (;;) {
            interrupt.throw_if_interrupted();
            throw_if_body_failed();
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
            }
            if (signal_.stopped() || derived().stream_aborted()) {
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), derived_type::reset_error_message);
            }
            const auto chunk = derived().pop_chunk();
            if (!chunk.empty()) {
                co_return ::ruvia::as_bytes(chunk);
            }
            if (derived().body_ended()) {
                co_return std::nullopt;
            }
            co_await signal_.wait();
        }
    }

protected:
    multiplexed_request_body_reader_base(
        multiplexed_stream_signal& signal, http_interim_response_output* continue_output) noexcept
        : signal_(signal),
          continue_output_(continue_output) {}

    multiplexed_stream_signal& signal_;
    http_interim_response_output* continue_output_;
    operation_interrupt_source interrupts_;

private:
    static void wake_signal(void* raw) noexcept {
        static_cast<multiplexed_stream_signal*>(raw)->wake();
    }
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
    void throw_if_body_failed() const {
        if (const auto& failure = derived().body_failure(); failure.has_value()) {
            if (*failure == http_request_body_failure::buffer_limit) {
                throw inbound_buffer_limit_error{};
            }
            throw http_request_body_protocol_error(*failure);
        }
    }
};

// Shared duplex CONNECT tunnel read loop and WebSocket write disposition
// forwarding for HTTP/2 and HTTP/3 streams.
template <typename derived_type>
class multiplexed_tunnel_transport_base {
public:
    template <typename buffer_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer) {
        return read_more(buffer, []() noexcept {}, asio::cancellation_slot{});
    }

    // A cancellation emitted on `slot` wakes the wait; `check` runs before
    // every wait and may throw to abandon it. Input that has not been
    // consumed stays queued for the next read_more().
    template <typename buffer_type, typename check_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer, check_type check, asio::cancellation_slot slot) {
        const wake_assignment assignment(signal_, slot);
        for (;;) {
            derived().require_live();
            if (derived().consume_tunnel_chunk(buffer)) {
                co_return true;
            }
            if (derived().receive_ended()) {
                co_return false;
            }
            derived().require_live();
            check();
            co_await signal_.wait();
        }
    }

protected:
    explicit multiplexed_tunnel_transport_base(multiplexed_stream_signal& signal) noexcept
        : signal_(signal) {}

    // While assigned, a cancellation emitted on the slot wakes this stream's waits.
    class wake_assignment final {
    public:
        wake_assignment(multiplexed_stream_signal& signal, asio::cancellation_slot slot)
            : slot_(slot) {
            if (slot_.is_connected()) {
                slot_.assign([&signal](asio::cancellation_type) noexcept { signal.wake(); });
            }
        }
        ~wake_assignment() {
            if (slot_.is_connected()) {
                slot_.clear();
            }
        }
        wake_assignment(const wake_assignment&) = delete;
        wake_assignment& operator=(const wake_assignment&) = delete;

    private:
        asio::cancellation_slot slot_;
    };

    multiplexed_stream_signal& signal_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
};

// `request_stream`: the connection's listener is a request stream one, whose
// every request streams its body (a CONNECT is a tunnel request).
[[nodiscard]] inline std::pair<route_resolution, request_body_mode> resolve_multiplexed_stream_route(
    const route_table& routes, bool request_stream, std::string_view method_token, std::string_view target_path,
    std::string_view authority, std::string_view protocol) noexcept {
    const auto method = classify_http_method(method_token);
    if (request_stream) {
        return {resolve_request_stream_route(method == http_known_method::connect), request_body_mode::stream};
    }
    const auto path = target_path.substr(0, target_path.find('?'));
    const auto extended_protocol = http_ascii_equals_ignore_case(protocol, "websocket")
                                       ? std::string_view("websocket")
                                       : protocol;
    auto resolution = routes.resolve(route_request_view{
        method, method_token, path, authority, extended_protocol});
    auto body_mode = request_body_mode::buffered;
    if (const auto* resolved = resolution.resolved()) {
        body_mode = resolved->route().endpoint().request_body_mode();
        if ((method == http_known_method::connect && resolved->route().endpoint().websocket() != nullptr) ||
            resolved->route().endpoint().tunnel() != nullptr) {
            body_mode = request_body_mode::stream;
        }
    }
    return {std::move(resolution), body_mode};
}

[[nodiscard]] inline task<http_response> respond_http_request_body_failure(const route_table& routes,
    const http_request& request, http_request_body_failure failure,
    const context_services& services, http_response_tail& tail) {
    tail.disable_response_coding();
    return respond_http_request_rejection(routes, request,
        http_request_rejection::error(
            [&] { const auto err = http_request_body_protocol_error(failure); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()),
        services);
}

// Completes a multiplexed response-stream route result. Returns true when the
// stream already terminated (peer abort before commit or committed head), or
// false when `response` holds a buffered route/recovered response to finish.
template <typename reset_type>
[[nodiscard]] inline bool complete_multiplexed_response_stream_dispatch(
    response_stream_dispatch_result& result_value, bool request_body_failed,
    const http_server_options& options, std::string_view remote_address,
    http_response_tail& tail, http_response& response, bool& body_failure_handled,
    reset_type&& reset_stream) {
    if (result_value.aborted_before_commit() != nullptr) {
        return true;
    }
    if (const auto committed_status = result_value.committed_status()) {
        if (const auto* failed = result_value.failed_after_commit()) {
            reset_stream(request_body_failed);
            if (!request_body_failed) {
                options.connection_failure_.invoke(remote_address, failed->exception());
            }
        } else if (request_body_failed) {
            reset_stream(true);
        }
        tail.record_access(options.access_log_, *committed_status);
        return true;
    }
    if (auto* route_response = result_value.route_response()) {
        response = std::move(*route_response).take_response();
    } else if (auto* recovered = result_value.recovered_failure()) {
        body_failure_handled = request_body_failed;
        response = std::move(*recovered).take_response();
    } else {
        throw std::logic_error("response stream dispatch returned no terminal alternative");
    }
    return false;
}

template <typename stream_id_type, typename writer_type, typename reset_type>
[[nodiscard]] task<void> finish_multiplexed_buffered_response(stream_id_type stream_id,
    const std::optional<http_request_body_failure>& body_failure, bool body_failure_handled,
    http_response& response, const http_request& request, const route_table& routes, const context_services& request_services,
    const http_server_options& options, std::string_view remote_address,
    http_response_tail& tail, writer_type& writer, reset_type reset_stream) {
    if (body_failure.has_value() && !body_failure_handled) {
        body_failure_handled = true;
        response = co_await respond_http_request_body_failure(
            routes, request, *body_failure, request_services, tail);
    }
    auto preparation = co_await tail.prepare_or_finish(
        response, options, routes, request_services);
    if (body_failure.has_value() && !body_failure_handled) {
        response = co_await respond_http_request_body_failure(
            routes, request, *body_failure, request_services, tail);
        preparation = co_await tail.prepare_or_finish(
            response, options, routes, request_services);
    }
    if (request_services.get_stop_token().stop_requested()) {
        reset_stream(true);
        co_return;
    }
    auto written = co_await writer.write(stream_id, response, preparation.write_plan_);
    if (written.file_failure_ && !written.committed_status_) {
        // Nothing of the response was sent: the stream gets the error
        // response instead of a reset.
        options.connection_failure_.invoke(remote_address, written.file_failure_->fault());
        tail.disable_response_coding();
        response = co_await routes.handle_exception(
            request, written.file_failure_->response_error(), request_services);
        preparation = co_await tail.prepare_or_finish(
            response, options, routes, request_services);
        written = co_await writer.write(stream_id, response, preparation.write_plan_);
        if (written.file_failure_ && !written.committed_status_) {
            reset_stream(false);
        }
    }
    if (written.file_failure_) {
        options.connection_failure_.invoke(remote_address, written.file_failure_->fault());
    }
    if (written.committed_status_) {
        tail.record_access(options.access_log_, *written.committed_status_);
    }
}

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP3)

// Server-chosen QUIC connection IDs (RFC 9000 §5.1) that route themselves
// without exposing a linkable worker prefix on the wire (RFC 9000 §9.5).
// Every worker of a POSIX HTTP/3 listener binds the UDP port with
// SO_REUSEPORT (on Linux steered in the kernel by SO_ATTACH_REUSEPORT_CBPF),
// and on Windows the acceptor receives every datagram. Each connection ID
// this server issues names the worker that owns the connection, masked and
// authenticated by the listener's shared secret and the ID's random suffix so
// that sibling IDs of one connection share no fixed bytes and a client-chosen
// ID only passes the tag check by chance (one in 2^16):
//
//   bytes 0..1   (owner worker index) ^ mask, big endian
//   bytes 2..3   tag: keyed 16-bit authentication tag over worker index and random bytes
//   bytes 4..19  random (16 CSPRNG bytes)
//
// A datagram whose destination ID names another worker is steered to it (by
// the kernel cBPF filter on Linux, or handed through the worker directory).
// Any other unknown ID (a client's first Initial) is hashed by its connection
// ID and the listener secret.

namespace ruvia::detail {

inline constexpr std::size_t http3_connection_id_length = 20;
using http3_secret = std::array<std::uint8_t, 32>;

// 32-bit mixing primes shared with the Linux SO_ATTACH_REUSEPORT_CBPF program.
inline constexpr std::uint32_t http3_cid_mix_prime_1 = 0x85ebca6bu;
inline constexpr std::uint32_t http3_cid_mix_prime_2 = 0xc2b2ae35u;

[[nodiscard]] constexpr std::uint16_t read_http3_be16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] constexpr std::uint32_t read_http3_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

// A connection ID as a map key: up to NGTCP2_MAX_CIDLEN bytes.
struct http3_cid_key final {
    std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> bytes_{};
    std::uint8_t size_{0};

    [[nodiscard]] static http3_cid_key from(std::span<const std::uint8_t> value) noexcept {
        http3_cid_key key;
        key.size_ = static_cast<std::uint8_t>(value.size() < key.bytes_.size() ? value.size() : key.bytes_.size());
        std::memcpy(key.bytes_.data(), value.data(), key.size_);
        return key;
    }

    [[nodiscard]] static http3_cid_key from(const ngtcp2_cid& cid) noexcept {
        return from(std::span<const std::uint8_t>(cid.data, cid.datalen));
    }

    friend bool operator==(const http3_cid_key& left, const http3_cid_key& right) noexcept {
        return left.size_ == right.size_ && std::memcmp(left.bytes_.data(), right.bytes_.data(), left.size_) == 0;
    }
};

struct http3_cid_key_hash final {
    [[nodiscard]] std::size_t operator()(const http3_cid_key& key) const noexcept;
};

// Fills `cid` with a fresh ID owned by `worker_index`. False when the CSPRNG
// failed.
[[nodiscard]] bool make_http3_connection_id(
    const http3_secret& secret, std::size_t worker_index, ngtcp2_cid& cid) noexcept;

// The worker a destination connection ID names, or nullopt when it is not an
// ID of this listener (wrong length, bad tag) or names no running worker.
[[nodiscard]] std::optional<std::size_t> http3_connection_id_owner(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;
// The worker that opens the connection whose client Initial carries the
// client-chosen destination ID `cid`: a hash of it keyed by the listener
// secret. Retransmissions of that Initial carry the same ID, so they reach
// the same worker without per-connection acceptor state, and a client that
// does not know the secret cannot choose its worker.
[[nodiscard]] std::size_t http3_initial_worker(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;

// The stateless reset token of `cid` (RFC 9000 §10.3.2), derived from the
// listener secret so every worker computes the same token.
[[nodiscard]] bool make_http3_stateless_reset_token(
    const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

// The largest UDP payload (65535 minus the UDP header): a receive buffer of
// this size truncates no datagram.
inline constexpr std::size_t http3_max_udp_payload_bytes = 65527;

// A receive error that concerns a single datagram, not the socket: ICMP
// errors of earlier sends (Windows reports them on later receives) and
// truncation. The socket keeps receiving.
[[nodiscard]] inline bool http3_ignorable_receive_error(const std::error_code& error) noexcept {
    return error == asio::error::connection_refused || error == asio::error::connection_reset ||
           error == asio::error::message_size || error == asio::error::network_unreachable ||
           error == asio::error::host_unreachable;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class http3_endpoint;

#if defined(_WIN32)
class windows_acceptor;
#endif

// One datagram handed to a worker that did not receive it. It owns its bytes
// because it crosses threads.
struct http3_handoff_datagram final {
    std::vector<std::byte> bytes_;
    asio::ip::udp::endpoint remote_;
    std::uint32_t ecn_{0};
    asio::ip::udp::endpoint local_;
    std::uint32_t interface_index_{};
};

// Slot identities outlive their worker-local endpoint. Another worker can
// still own a connection issued under that slot's secret after this one has
// released its last connection and socket.
struct http3_endpoint_registration final {
    http3_endpoint* endpoint_;
    asio::ip::udp::endpoint local_;
    std::shared_ptr<const http3_secret> secret_;
    std::size_t listener_index_;
};

// Datagrams in flight from one producer to one worker, product limit. A
// datagram past it is dropped, as UDP may drop it anyway; QUIC loss recovery
// retransmits what matters.
inline constexpr std::size_t http3_max_datagrams_in_flight = 256;

// One handed-off datagram's place in its in-flight bound: reserved before the
// datagram is posted, released when the posted job ran or was abandoned, on
// whichever thread that is.
class http3_in_flight_token final {
public:
    // Empty when the bound is full.
    [[nodiscard]] static http3_in_flight_token reserve(std::atomic<std::size_t>& count) noexcept {
        if (count.fetch_add(1, std::memory_order_relaxed) >= http3_max_datagrams_in_flight) {
            count.fetch_sub(1, std::memory_order_relaxed);
            return http3_in_flight_token(nullptr);
        }
        return http3_in_flight_token(&count);
    }
    http3_in_flight_token(http3_in_flight_token&& other) noexcept
        : count_(std::exchange(other.count_, nullptr)) {}
    http3_in_flight_token(const http3_in_flight_token&) = delete;
    http3_in_flight_token& operator=(const http3_in_flight_token&) = delete;
    http3_in_flight_token& operator=(http3_in_flight_token&&) = delete;
    ~http3_in_flight_token() {
        if (count_ != nullptr) {
            count_->fetch_sub(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return count_ != nullptr;
    }

private:
    explicit http3_in_flight_token(std::atomic<std::size_t>* count) noexcept
        : count_(count) {}

    std::atomic<std::size_t>* count_;
};

// The HTTP/3 rendezvous of every worker of one application run. A datagram
// for a connection a worker owns (see http3_connection_id.h) is handed to
// that worker here when another thread received it: on POSIX another worker
// whose SO_REUSEPORT socket the kernel chose, on Windows the acceptor that
// receives every datagram. It travels on the owner's bounded worker queue,
// at most http3_max_datagrams_in_flight per owner at a time, and is dropped
// when either bound is full or the owner no longer serves.
class http3_worker_directory final {
public:
    using receive_type = void (*)(void* target, std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;

    explicit http3_worker_directory(std::size_t worker_count);
    http3_worker_directory(const http3_worker_directory&) = delete;
    ~http3_worker_directory();
    http3_worker_directory& operator=(const http3_worker_directory&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return slots_.size();
    }

    // Construction thread, before any worker serves -- for a directory
    // application::add_listener() creates, before any worker uses it.
    void bind_worker(std::size_t index, const worker_ref& worker_value);
    // The owning worker's thread: opens and closes its receiving side.
    void attach(std::size_t index, void* target, receive_type receive) noexcept;
    void detach(std::size_t index) noexcept;

    // Worker-local endpoint registrations share packet routing across removed
    // and re-added listeners bound to the same UDP endpoint.
    void register_endpoint(std::size_t worker_index, http3_endpoint& endpoint,
        const asio::ip::udp::endpoint& local, std::shared_ptr<const http3_secret> secret,
        std::size_t listener_index);
    void unregister_endpoint(std::size_t worker_index, http3_endpoint& endpoint) noexcept;
    [[nodiscard]] std::span<const http3_endpoint_registration> endpoints(std::size_t worker_index) const noexcept {
        return slots_[worker_index]->endpoints_;
    }

#if !defined(_WIN32)
    // The first call for `listener_index` binds every worker's SO_REUSEPORT
    // socket in worker index order 0..N-1 -- or takes `inherited`'s UDP
    // sockets, in that order already, when it holds them -- and attaches
    // SO_ATTACH_REUSEPORT_CBPF on Linux; each call assigns `worker_index`'s
    // socket to `socket`.
    void open_listener_socket(std::size_t worker_index, std::size_t listener_index,
        const asio::ip::udp::endpoint& configured_endpoint, const http3_secret& secret,
        inherited_listener_sockets* inherited, asio::ip::udp::socket& socket, std::optional<bool> ipv6_only);
    // Closes the sockets of `listener_index` no worker claimed, for a listener
    // whose addition was rolled back.
    void close_unclaimed_listener_sockets(std::size_t listener_index) noexcept;
#endif
    // Any thread. False when the datagram was dropped.
    [[nodiscard]] bool hand_off(std::size_t owner, std::size_t listener_index,
        std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index,
        std::uint32_t ecn = 0) noexcept;

private:
    struct slot final {
        worker_ref worker_;
        // Read and written only on the owning worker's thread.
        void* target_{nullptr};
        receive_type receive_{nullptr};
        std::atomic<std::size_t> in_flight_{0};
        std::vector<http3_endpoint_registration> endpoints_{};
    };

    std::vector<std::unique_ptr<slot>> slots_;
    std::mutex bind_mutex_;
    std::vector<std::vector<int>> listener_sockets_;
};

// Where one worker's HTTP/3 endpoints sit among the application's workers:
// the directory (null without an HTTP/3 listener) and this worker's index in
// it, which the connection IDs it issues carry.
struct http3_worker_placement final {
    http3_worker_directory* directory_;
    std::size_t worker_index_;
#if defined(_WIN32)
    // Sends this worker's datagrams from the listener's one UDP socket.
    windows_acceptor* acceptor_;
#endif
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class http3_session;
class route_table;
class worker_capabilities;

// The worker-owned services every HTTP/3 connection of a worker borrows.
// Each outlives the worker's connections, which their listeners' task scopes
// join.
struct http3_worker_services final {
    asio::io_context& io_;
    const worker_ref& worker_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    connection_scanner& scanner_;
    connection_count& connection_count_;
    connection_id_sequence& connection_ids_;
    connection_registry& registry_;
    const stop_token& stop_token_;
    const http_server_worker_state& state_;
    std::atomic<std::size_t>& refused_;
    // Where this worker sits among the application's workers. Its directory
    // (and on Windows its acceptor) is set whenever an endpoint exists.
    http3_worker_placement placement_;
};

// One worker's QUIC endpoint of one HTTP/3 listener: the table from
// connection ID to this worker's connections, connection admission, packet
// steering and the datagram path.
//
// On POSIX the endpoint owns a UDP socket bound with SO_REUSEPORT. On Windows
// the application's acceptor owns the listener's only socket: every datagram
// arrives here handed over through the directory, and every datagram this
// worker sends leaves through the acceptor's bounded queue, so every worker
// serves HTTP/3 alike.
//
// A datagram is delivered to the connection its destination ID names in the
// table. Otherwise an ID that names another worker (http3_connection_id.h)
// is handed to it through the directory; an unknown ID with a valid client
// Initial opens a connection on this worker, bounded like TCP by
// max_connections_per_worker_ (the connection idle longest is evicted
// first); an unsupported version gets Version Negotiation (RFC 9000 §6); a
// short header naming a retired ID of this worker gets a rate-limited
// Stateless Reset (RFC 9000 §10.3); any other datagram is dropped.
class http3_endpoint final {
public:
    // `listener_index` names this listener's endpoint on every worker (and in
    // the Windows acceptor), for datagrams handed between them. Its receive
    // loop and connections run in `tasks`, its listener's scope.
    http3_endpoint(const http3_worker_services& services, http_server_session_config& session,
        asio::ip::udp::endpoint endpoint, std::size_t listener_index, task_scope& tasks, connection_count& listener_connections);
    ~http3_endpoint();
    http3_endpoint(const http3_endpoint&) = delete;
    http3_endpoint& operator=(const http3_endpoint&) = delete;

    // POSIX binds the UDP socket (SO_REUSEADDR and SO_REUSEPORT), or claims
    // it from `inherited` when that holds the service's sockets; Windows,
    // whose acceptor binds it, only prepares the QUIC backend.
    void open(inherited_listener_sockets* inherited);
    // Starts admitting connections, and on POSIX receiving.
    void start() noexcept;
    // The listener was removed: no connection is admitted any more, while
    // established ones are served. After the last one ends the POSIX socket
    // closes; on Windows the acceptor learns that this worker no longer
    // needs the listener's socket.
    void retire() noexcept;
    // The worker stops: no connection is admitted any more, and the POSIX
    // socket closes. Its connections were already closed by the scanner.
    void close() noexcept;
#if !defined(_WIN32)
    // A dup() of the open socket, for application::duplicate_listener_sockets().
    [[nodiscard]] native_socket duplicate_socket();
#endif

    // A datagram another thread received for this worker.
    void receive_handoff(http3_handoff_datagram& datagram) noexcept;

    // Session interface.
    [[nodiscard]] const http3_worker_services& services() const noexcept {
        return services_;
    }
    [[nodiscard]] http_server_session_config& session_config() const noexcept {
        return session_;
    }
    [[nodiscard]] const http3_secret& secret() const noexcept {
        return *session_.http3_->secret_;
    }
    [[nodiscard]] const asio::ip::udp::endpoint& local_endpoint() const noexcept {
        return local_endpoint_;
    }
    // Sends one datagram without blocking. On POSIX would_block means the
    // socket's send buffer is full (then wait with wait_writable()); on
    // Windows the datagram is queued to the acceptor, or lost when that
    // bounded queue is full (no_buffer_space).
    [[nodiscard]] std::error_code send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
#if !defined(_WIN32)
    // Completes once the socket can send again, or with the error that ended
    // waiting (the socket closed).
    [[nodiscard]] task<std::error_code> wait_writable();
#endif
    // False when another connection already holds the ID.
    [[nodiscard]] bool register_connection_id(const http3_cid_key& key, http3_session& session);
    void unregister_connection_id(const http3_cid_key& key) noexcept;

private:
    using session_pointer = std::unique_ptr<http3_session>;

#if !defined(_WIN32)
    task<void> run_receive_loop();
    worker_signal activated_;
#endif
    task<void> serve(session_pointer session);
    void receive(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_version_negotiation(const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_retry(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote,
        std::size_t max_packet_bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_stateless_connection_close(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote,
        std::uint64_t error_code, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_stateless_reset(std::span<const std::uint8_t> destination_cid, std::size_t trigger_bytes,
        const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    [[nodiscard]] bool consume_stateless_reset_budget() noexcept;
    // A retiring endpoint whose last connection ended.
    void release_after_last_connection() noexcept;

    const http3_worker_services& services_;
    http_server_session_config& session_;
    asio::ip::udp::endpoint configured_endpoint_;
    asio::ip::udp::endpoint local_endpoint_;
    std::size_t listener_index_;
    task_scope& tasks_;
    connection_count& listener_connections_;
#if !defined(_WIN32)
    asio::ip::udp::socket socket_;
    // One received datagram at a time; a UDP payload never exceeds it.
    std::vector<std::byte> receive_buffer_;
#endif
    std::unordered_map<http3_cid_key, http3_session*, http3_cid_key_hash> connections_;
    std::chrono::steady_clock::time_point stateless_reset_refill_at_{};
    std::size_t stateless_reset_tokens_{32};
    // Sessions serve() is running.
    std::size_t sessions_{0};
    // Between open() and close(): datagrams are processed.
    bool open_{false};
    bool admitting_{false};
    bool registered_{false};
    bool retiring_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Resumes the coroutine on a later turn of its worker, letting other
// connections run between bursts of one connection's output.
struct worker_turn final {
    explicit worker_turn(const worker_ref& worker_value) noexcept
        : worker_(worker_value) {}
    worker_turn(worker_ref&&) = delete;
    const worker_turn* operator&() const = delete;
    worker_turn* operator&() = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const noexcept {
        worker_.defer_resume(continuation);
    }
    void await_resume() const noexcept {}

    const worker_ref& worker_;
};

struct free_tls final {
    void operator()(SSL* tls) const noexcept {
        SSL_free(tls);
    }
};

struct free_crypto final {
    void operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept {
        ngtcp2_crypto_ossl_ctx_del(crypto);
    }
};

inline void fill_random(std::uint8_t* destination, std::size_t size, const ngtcp2_rand_ctx*) noexcept {
    // ngtcp2 cannot fail this callback; packet protection without randomness
    // must not continue.
    if (!fill_random_bytes(std::span<unsigned char>(destination, size))) {
        std::terminate();
    }
}

// The ngtcp2_crypto_ossl adapter callbacks both roles share; the client adds
// client_initial_ and recv_retry_, the server recv_client_initial_.
[[nodiscard]] inline http3_quic_crypto_callbacks make_http3_quic_crypto_callbacks() noexcept {
    return {
        .recv_crypto_data_ = ngtcp2_crypto_recv_crypto_data_cb,
        .encrypt_ = ngtcp2_crypto_encrypt_cb,
        .decrypt_ = ngtcp2_crypto_decrypt_cb,
        .hp_mask_ = ngtcp2_crypto_hp_mask_cb,
        .update_key_ = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx_ = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx_ = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data_ = ngtcp2_crypto_get_path_challenge_data2_cb,
        .version_negotiation_ = ngtcp2_crypto_version_negotiation_cb,
        .rand_ = fill_random,
    };
}

[[nodiscard]] inline ngtcp2_tstamp http3_quic_now() noexcept {
    return static_cast<ngtcp2_tstamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] inline ngtcp2_duration http3_quic_duration(timeout_duration value) noexcept {
    const auto milliseconds = static_cast<std::uint64_t>((std::max)(value.value().count(), std::int64_t{0}));
    return milliseconds > UINT64_MAX / NGTCP2_MILLISECONDS ? UINT64_MAX : milliseconds * NGTCP2_MILLISECONDS;
}

[[nodiscard]] inline ngtcp2_duration http3_quic_duration(const std::optional<timeout_duration>& value) noexcept {
    return value.has_value() ? http3_quic_duration(*value) : 0;
}

[[nodiscard]] inline std::chrono::steady_clock::time_point http3_quic_steady_time(ngtcp2_tstamp timestamp) noexcept {
    using nanoseconds = std::chrono::nanoseconds;
    if (timestamp >= static_cast<ngtcp2_tstamp>((std::numeric_limits<nanoseconds::rep>::max)())) {
        return (std::chrono::steady_clock::time_point::max)();
    }
    return std::chrono::steady_clock::time_point(
        std::chrono::ceil<std::chrono::steady_clock::duration>(nanoseconds(static_cast<nanoseconds::rep>(timestamp))));
}

[[nodiscard]] inline std::chrono::nanoseconds http3_quic_nanoseconds(ngtcp2_duration duration) noexcept {
    return std::chrono::nanoseconds(
        static_cast<std::int64_t>((std::min)(duration, static_cast<ngtcp2_duration>(INT64_MAX))));
}

[[nodiscard]] inline ngtcp2_path http3_quic_path(
    asio::ip::udp::endpoint& local, asio::ip::udp::endpoint& remote) noexcept {
    return {
        .local = {local.data(), static_cast<ngtcp2_socklen>(local.size())},
        .remote = {remote.data(), static_cast<ngtcp2_socklen>(remote.size())},
        .user_data = nullptr,
    };
}

[[nodiscard]] inline std::optional<asio::ip::udp::endpoint> http3_udp_endpoint(
    const ngtcp2_addr& address) noexcept {
    asio::ip::udp::endpoint endpoint;
    if (address.addr == nullptr || address.addrlen == 0 || address.addrlen > endpoint.capacity()) {
        return std::nullopt;
    }
    std::memcpy(endpoint.data(), address.addr, address.addrlen);
    endpoint.resize(address.addrlen);
    return endpoint;
}
}  // namespace ruvia::detail

// Web-side state of one HTTP/3 request stream. The QUIC/HTTP/3 core owns
// framing, QPACK and flow control; this runtime owns what a route needs: the
// validated head, the request body as the route consumes it (buffered or
// streamed, with flow-control credit returned only as bytes are consumed), the
// trailers, and the wake/stop signal a dispatched handler waits on.

namespace ruvia::detail {

enum class http3_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
    buffer_limit,
};

// Tunnel bytes taken together with the receive credit they hold; the reader
// drops the credit once it consumed the bytes.
struct http3_tunnel_chunk final {
    std::string bytes_;
    std::optional<http3_received_data_credit> credit_;
};

class http3_stream final {
public:
    http3_stream(http3_request_head head, route_resolution resolution, request_body_mode body_mode,
        inbound_buffer_account* inbound = nullptr) noexcept
        : head_(std::move(head)),
          resolution_(std::move(resolution)),
          body_mode_(body_mode),
          body_reservation_(inbound),
          active_reservation_(inbound) {}

    http3_stream(const http3_stream&) = delete;
    http3_stream& operator=(const http3_stream&) = delete;

    [[nodiscard]] std::int64_t id() const noexcept {
        return head_.stream_id();
    }
    [[nodiscard]] const http3_request_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http3_request_head& head() const&& = delete;
    [[nodiscard]] const route_resolution& resolution() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolution_;
    }
    const route_resolution& resolution() const&& = delete;
    [[nodiscard]] request_body_mode body_mode() const noexcept {
        return body_mode_;
    }
    [[nodiscard]] http_request_trailers& trailers() & noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    http_request_trailers& trailers() && = delete;
    [[nodiscard]] const std::optional<http_priority>& priority() const& noexcept RUVIA_LIFETIMEBOUND {
        return priority_;
    }
    const std::optional<http_priority>& priority() const&& = delete;
    void set_priority(std::optional<http_priority> priority) noexcept {
        priority_ = priority;
    }
    void defer_dispatch() noexcept {
        dispatch_pending_ = true;
    }
    [[nodiscard]] bool dispatch_pending() const noexcept {
        return dispatch_pending_ && !dispatched();
    }
    void defer_continue() noexcept {
        continue_pending_ = true;
    }
    [[nodiscard]] bool take_continue_pending() noexcept {
        return std::exchange(continue_pending_, false);
    }

    void store_datagram(std::string_view payload, std::size_t byte_limit) {
        if ((datagrams_ && datagrams_->size() >= 64) || datagram_bytes_ > byte_limit ||
            payload.size() > byte_limit - datagram_bytes_) {
            return;  // RFC 9221 permits loss; never backpressure reliable DATA.
        }
        try {
            if (!datagrams_) {
                datagrams_.emplace();
            }
            datagrams_->emplace_back(payload);
            datagram_bytes_ += payload.size();
        } catch (const inbound_buffer_limit_error&) {
            return;
        }
        if (signal_) {
            signal_->wake();
        }
    }
    [[nodiscard]] std::optional<std::string> pop_datagram() {
        if (!datagrams_ || datagrams_->empty()) {
            return std::nullopt;
        }
        auto payload = std::move(datagrams_->front());
        datagrams_->pop_front();
        datagram_bytes_ -= payload.size();
        return payload;
    }
    [[nodiscard]] http3_tunnel_chunk take_tunnel_bytes() {
        http3_tunnel_chunk result{std::string{}, std::exchange(body_credit_, std::nullopt)};
        result.bytes_.swap(body_);
        body_reservation_.clear();
        return result;
    }

    // Buffered body: the whole content, borrowed by the request at dispatch;
    // its credit returns as it is stored. Streaming body and tunnel: the bytes
    // queued for the reader, holding their credit until it takes them.
    [[nodiscard]] http3_body_store_status store(std::string_view data, http3_received_data_credit&& credit,
        protocol_byte_limit total_limit, std::size_t backlog_limit) {
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return http3_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream &&
            (body_.size() > backlog_limit || data.size() > backlog_limit - body_.size())) {
            return http3_body_store_status::backlog_overflow;
        }
        try {
            inbound_append(body_, body_reservation_, data);
        } catch (const inbound_buffer_limit_error&) {
            return http3_body_store_status::buffer_limit;
        }
        received_bytes_ += data.size();
        if (body_mode_ == request_body_mode::stream && credit.valid()) {
            if (body_credit_) {
                body_credit_->merge(std::move(credit));
            } else {
                body_credit_.emplace(std::move(credit));
            }
        }
        return http3_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    std::string_view buffered_body() const&& = delete;

    // Streaming reader: hands out every queued byte as one chunk, valid until
    // the next pop(). The previous chunk's flow-control credit returns now;
    // true when it held any, so the caller can let the writer send it.
    [[nodiscard]] std::pair<std::string_view, bool> pop() & RUVIA_LIFETIMEBOUND {
        const bool returned = active_credit_.has_value();
        active_credit_.reset();
        if (body_credit_) {
            active_credit_.emplace(std::move(*body_credit_));
            body_credit_.reset();
        }
        inbound_clear_retaining_small(active_chunk_, active_reservation_);
        active_chunk_.swap(body_);
        active_reservation_.swap(body_reservation_);
        return {active_chunk_, returned};
    }
    std::pair<std::string_view, bool> pop() && = delete;

    // The handler finished: the content's storage goes back to the
    // connection's inbound budget, and credit for content received but never
    // read returns so a stopped reader cannot strand connection-level credit.
    // True when any credit returned.
    bool discard_body() noexcept {
        const bool returned = body_credit_.has_value() || active_credit_.has_value();
        body_credit_.reset();
        active_credit_.reset();
        inbound_discard(body_, body_reservation_);
        inbound_discard(active_chunk_, active_reservation_);
        datagrams_.reset();
        datagram_bytes_ = 0;
        return returned;
    }

    void mark_body_failure(http_request_body_failure failure) noexcept {
        if (!body_failure_.has_value()) {
            body_failure_ = failure;
        }
    }
    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_failure_;
    }
    const std::optional<http_request_body_failure>& body_failure() const&& = delete;

    // The request content ended (the peer's FIN, or no content at all).
    void mark_request_ended() noexcept {
        request_ended_ = true;
        request_body_deadline_.close();
    }
    [[nodiscard]] bool request_ended() const noexcept {
        return request_ended_;
    }

    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_deadline_.open(now_ms);
    }
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_deadline_.progress(now_ms);
    }
    void mark_request_body_closed() noexcept {
        request_body_deadline_.close();
    }
    // The request body is still open and went `inactivity` without DATA, or
    // `completion` since it opened (request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return request_body_deadline_.overdue(now_ms, inactivity, completion);
    }

    void mark_tunnel() noexcept {
        tunnel_ = true;
    }
    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

    void mark_response_committed() noexcept {
        response_committed_ = true;
    }
    [[nodiscard]] bool response_committed() const noexcept {
        return response_committed_;
    }

    // The peer reset the stream, or this side did: no response byte may follow.
    void mark_aborted() noexcept {
        aborted_ = true;
        if (signal_) {
            signal_->stop();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }

    // The QUIC stream closed in both directions.
    void mark_closed() noexcept {
        closed_ = true;
    }
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        const worker_ref& worker_value, multiplexed_session_termination& termination) {
        if (signal_ || handler_done_) {
            return nullptr;
        }
        signal_.emplace(worker_value, termination);
        if (aborted_) {
            signal_->stop();
        }
        return &*signal_;
    }
    [[nodiscard]] bool dispatched() const noexcept {
        return signal_.has_value();
    }
    [[nodiscard]] multiplexed_stream_signal* signal() & noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    multiplexed_stream_signal* signal() && = delete;
    [[nodiscard]] const multiplexed_stream_signal* signal() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    const multiplexed_stream_signal* signal() const&& = delete;

    void mark_handler_done() noexcept {
        handler_done_ = true;
    }
    [[nodiscard]] bool handler_done() const noexcept {
        return handler_done_;
    }

private:
    http3_request_head head_;
    route_resolution resolution_;
    request_body_mode body_mode_;
    http_request_trailers trailers_;
    std::optional<http_priority> priority_{};
    // Received request content: the whole body (buffered), or the queue the
    // reader drains (streaming). active_chunk_ backs the last popped view.
    std::string body_;
    inbound_buffer_reservation body_reservation_;
    std::string active_chunk_;
    inbound_buffer_reservation active_reservation_;
    // Receive credit of body_ and active_chunk_ (streaming body or tunnel).
    std::optional<http3_received_data_credit> body_credit_;
    std::optional<http3_received_data_credit> active_credit_;
    std::optional<std::deque<std::string>> datagrams_;
    std::size_t datagram_bytes_{0};
    std::size_t received_bytes_{0};
    std::optional<http_request_body_failure> body_failure_{};
    std::optional<multiplexed_stream_signal> signal_;
    multiplexed_request_body_deadline request_body_deadline_;
    bool request_ended_{false};
    bool tunnel_{false};
    bool response_committed_{false};
    bool aborted_{false};
    bool closed_{false};
    bool handler_done_{false};
    bool dispatch_pending_{false};
    bool continue_pending_{false};
};

}  // namespace ruvia::detail

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {

class http3_session;
class http3_stream;
enum class http3_data_write_status : std::uint8_t {
    completed,
    // The stream was reset (by either side) or stopped.
    stream_aborted,
    // The connection ended.
    terminated,
};

// Submits all of `bytes` as DATA of a stream, at most the stream's output
// budget at a time (http3_connection::submit_data): when the budget is
// spent it waits for the peer's acknowledgements, timed by the session
// against write_timeout_. A Content-Length overrun throws std::length_error.
[[nodiscard]] task<http3_data_write_status> write_http3_stream_data(
    http3_session& session, std::int64_t stream_id, multiplexed_stream_signal& signal, std::string_view bytes);

// Writes a buffered or file response on one request stream: the head with its
// write plan's Content-Length, then the content within the stream's output
// budget. A file body is opened and validated before the head without being
// read; each later chunk is read only once output budget is free, at most that
// much, while holding one of multiplexed_file_read_slots through its submission. A
// stream it cannot complete is reset through the session.
class http3_buffered_response_writer final {
public:
    http3_buffered_response_writer(http3_session& session, response_file_io file_io) noexcept;

    http3_buffered_response_writer(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer& operator=(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer(http3_buffered_response_writer&&) = delete;
    http3_buffered_response_writer& operator=(http3_buffered_response_writer&&) = delete;

    [[nodiscard]] task<multiplexed_buffered_response_write_result> write(std::int64_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);

private:
    [[nodiscard]] task<multiplexed_buffered_response_write_result> write_file_body(
        std::int64_t stream_id, multiplexed_stream_signal& signal, response_file_source& source,
        http_status_code committed_status);
    [[nodiscard]] bool stream_ended(std::int64_t stream_id) noexcept;

    http3_session& session_;
    response_file_io file_io_;
    multiplexed_file_read_limiter file_reads_;
};

}  // namespace ruvia::detail

struct ngtcp2_crypto_ossl_ctx;

namespace ruvia::detail {

class http3_endpoint;
struct http3_worker_services;

// Per-stream DATA a connection holds between submission and the peer's
// acknowledgement (http3_connection_options::max_stream_output_bytes_).
inline constexpr std::size_t http3_stream_output_bytes = 256 * 1024;
// Largest UDP payload this server sends: fits a 1500-byte Ethernet MTU behind
// IPv6, UDP and common tunnel headers, without path MTU discovery.
inline constexpr std::size_t http3_max_datagram_bytes = 1350;

// One HTTP/3 connection on the worker that accepted it: its TLS session, its
// QUIC/HTTP/3 state (http3_quic_connection), its one QUIC timer, its writer
// and the dispatch of its request streams through the shared request spine
// (admission, routes, response tail), as the HTTP/2 session engine does.
//
// The endpoint feeds datagrams in with receive(); run() is the connection's
// task: it writes every datagram the connection produces, re-arms the timer to
// expiry(), and returns once the connection has ended and every handler has
// finished. All of it runs on the owning worker.
//
// Lifecycle: open -> (graceful drain on the request cap or a connection
// timeout: shutdown-notice GOAWAY, a bounded round trip, final GOAWAY,
// CONNECTION_CLOSE(H3_NO_ERROR) once no stream is left) -> closing period
// (RFC 9000 §10.2, three probe timeouts absorbing late packets) -> ended. A
// worker stop, eviction or a timeout past its grace ends it at once with
// CONNECTION_CLOSE(H3_NO_ERROR).
class http3_session final {
public:
    // Accepts the connection the client Initial `initial` opens (RFC 9000
    // §7.2): a server connection ID owned by this worker, the client's chosen
    // ID as original_destination_connection_id.
    http3_session(http3_endpoint& endpoint, connection_slot slot, const ngtcp2_pkt_hd& initial,
        const asio::ip::udp::endpoint& remote, const ngtcp2_cid& original_dcid,
        std::optional<ngtcp2_cid> retry_scid, std::span<const std::uint8_t> retry_token,
        connection_lifecycle lifecycle);
    ~http3_session();

    http3_session(const http3_session&) = delete;
    http3_session& operator=(const http3_session&) = delete;
    http3_session(http3_session&&) = delete;
    http3_session& operator=(http3_session&&) = delete;

    // One datagram for this connection, from `remote`.
    void receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    [[nodiscard]] task<void> run();
    // Ends the connection now: CONNECTION_CLOSE(H3_NO_ERROR), streams
    // cancelled, no closing period. The scanner's close action.
    void close_now() noexcept;

    // Stream I/O for the response writers, the streaming sink and the request
    // body reader of this connection's handlers.
    [[nodiscard]] http3_connection& codec() noexcept {
        return connection_->http3();
    }
    [[nodiscard]] http3_stream* find_stream(std::int64_t stream_id) noexcept;
    // Queues the stream's output for the writer.
    void wake_writer() noexcept;
    // Abandons both directions of a request stream (RFC 9114 §4.1.1) and
    // stops every wait of its handler.
    void reset_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] std::string_view remote_address() const& noexcept RUVIA_LIFETIMEBOUND {
        return remote_address_;
    }
    std::string_view remote_address() const&& = delete;
    [[nodiscard]] http3_quic_status submit_datagram(std::int64_t stream_id, std::span<const std::byte> payload);

private:
    using stream_pointer = std::unique_ptr<http3_stream>;

    enum class lifecycle_type : std::uint8_t {
        open,
        // The QUIC connection closed (either side, or idle): late packets
        // are absorbed until the closing period elapses.
        closing,
        ended,
    };

    static ngtcp2_conn* native_connection(ngtcp2_crypto_conn_ref* reference) noexcept;
    static int new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
        std::size_t size, void* user_data) noexcept;
    static int remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) noexcept;

    [[nodiscard]] static ngtcp2_tstamp now() noexcept;
    [[nodiscard]] ngtcp2_path path(asio::ip::udp::endpoint& remote) noexcept;
    void register_connection_id(const ngtcp2_cid& cid);
    void unregister_connection_ids() noexcept;

    // Connection events.
    void process_events() noexcept;
    void drain_events();
    void start_application();
    void on_request_head(http3_request_head_event& event);
    void on_data(http3_data_event& event);
    void on_datagram(const http3_datagram_event& event);
    void on_message_end(const http3_message_end_event& event);
    void on_stream_end(std::int64_t stream_id);
    void on_stream_abort(std::int64_t stream_id) noexcept;
    void on_stream_close(const http3_stream_close_event& event) noexcept;
    void wake_output_waiters() noexcept;

    // Requests.
    [[nodiscard]] bool admit_stream(std::int64_t stream_id);
    [[nodiscard]] task<void> dispatch_one(std::int64_t stream_id);
    void finish_stream_handler(std::int64_t stream_id) noexcept;
    void remove_stream_if_done(std::int64_t stream_id) noexcept;
    void set_inactivity_phase() noexcept;
    void expire_stream_deadlines(std::int64_t now_ms) noexcept;

    // Output and timers.
    [[nodiscard]] task<void> flush();
    [[nodiscard]] task<void> send_datagram();
    void arm_timer() noexcept;
    void timer_fired(worker_timer_outcome outcome) noexcept;
    void enter_closing() noexcept;
    void terminate(std::error_code error) noexcept;

    // Graceful drain (RFC 9114 §5.2).
    void request_drain() noexcept;
    void begin_drain() noexcept;
    void arm_drain_timer() noexcept;
    void drain_timer_expired() noexcept;
    void finish_drain_if_ready() noexcept;

    http3_endpoint& endpoint_;
    const http3_worker_services& services_;
    connection_slot slot_;
    connection_lifecycle connection_lifecycle_;
    // conn_info::connection_id() of this connection.
    std::uint64_t connection_id_;
    asio::ip::udp::endpoint local_;
    // The actual destination address of the Initial, including wildcard-bound
    // listeners. TLS identity selection borrows this stable address.
    asio::ip::address tls_local_address_;
    asio::ip::udp::endpoint remote_;
    // The sender of the datagram being read, for its ngtcp2 path.
    asio::ip::udp::endpoint path_remote_;
    asio::ip::udp::endpoint path_local_;
    std::uint32_t path_interface_index_;
    std::string remote_address_;
    // Present only with an on_tls_client_hello observer; tls_ borrows it, so
    // it is declared before tls_.
    std::string local_address_;
    std::optional<tls_client_hello_observation> client_hello_;
    // Keys this connection holds in the endpoint's table.
    std::vector<http3_cid_key> connection_ids_;
    std::unique_ptr<SSL, free_tls> tls_;
    std::array<unsigned char, 32> tls_resumption_context_{};
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref connection_reference_{};
    // Destroyed first: ngtcp2 still references the TLS objects above.
    std::optional<http3_quic_connection> connection_;
    http3_quic_datagram datagram_;
    std::string client_certificate_;
    std::optional<tls_served_certificate> served_certificate_;
    inbound_buffer_account inbound_buffers_;
    std::optional<context_services> base_services_;
    http_request_admission_policy admission_policy_;
    multiplexed_session_termination termination_;
    worker_signal write_signal_;
    worker_signal handler_finished_;
    // Declared after the inbound buffers their bodies borrow.
    std::unordered_map<std::int64_t, stream_pointer> streams_;
    http3_buffered_response_writer buffered_response_writer_;
    std::size_t active_handler_tasks_{0};
    std::size_t accepted_request_heads_{0};
    std::size_t tunnel_streams_{0};
    lifecycle_type lifecycle_{lifecycle_type::open};
    bool drain_begun_{false};
    // The deadline the expiry timer is armed for, in ngtcp2 time.
    ngtcp2_tstamp armed_expiry_{UINT64_MAX};
    connection_scanner::entry scanner_entry_;
    std::optional<connection_scanner::guard> scanner_guard_;
    connection_scanner::periodic_check_registration scanner_checks_;
    // Declared last: unregistered before the state their completions touch.
    multiplexed_drain_timer drain_timer_;
    worker_timer_registration timer_;
};

}  // namespace ruvia::detail

#endif

#if defined(RUVIA_ENABLE_HTTP3)
#endif

namespace ruvia::detail {

class http1_request_buffer_completion;
struct access_log_sink;

// Initial read capacity for an active connection. Growth is bounded by pending
// input, and destruction returns the buffer to the worker resource on idle.
inline constexpr std::size_t initial_read_buffer_bytes = std::size_t{4} * 1024;

// Bytes of the connection-resident buffer an idle connection reads into while
// it holds no work set. Sized so a typical request line arrives in one read;
// longer heads finish in the regular read loop after the work set is
// re-acquired. It lives in every connection's frame, so growing it to cover
// whole browser heads (400-800 bytes with cookies) costs that much per idle
// connection to save one immediately-completing read per such request.
inline constexpr std::size_t idle_resident_read_bytes = 256;

// A connection's buffered request input with its growth and trim policy.
// Consumption advances the window; reads use the writable tail first and
// compact only when the consumed prefix covers the remaining bytes.
class connection_read_buffer final {
public:
    connection_read_buffer() noexcept;

    // Unconsumed bytes, starting with the current request.
    [[nodiscard]] std::string_view pending() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    std::string_view pending() const&& = delete;
    [[nodiscard]] bool empty() const noexcept {
        return storage_.empty();
    }

    // Room for the next socket read, bounded by max_http_header_bytes minus
    // pending bytes. Uses spare storage first, growing only when none remains.
    // Empty when pending bytes already fill the header limit.
    [[nodiscard]] std::span<char> prepare_read() & RUVIA_LIFETIMEBOUND;
    std::span<char> prepare_read() && = delete;
    // Appends `bytes` read into the span prepare_read() returned.
    void commit_read(std::size_t bytes) noexcept {
        storage_.commit(bytes);
    }

    // Drops the first `bytes` pending bytes (a served request).
    void consume(std::size_t bytes) noexcept;
    // Replaces the pending bytes with a copy of `bytes`, which never aliases
    // this buffer (an idle landing pad or a body runtime's over-read stash).
    void assign(std::string_view bytes);
    // Applies a reusable request's buffer completion. Installing a handed-over
    // pipeline can grow the storage, so this is the one completion step that
    // may allocate.
    void complete_request(const http1_request_buffer_completion& completion);

    // Gives storage that a handed-over pipeline grew past max_http_header_bytes
    // back once the pending bytes fit the initial size.
    void trim();

private:
    http_io_buffer storage_;
};

// Access-log fields of pipelined responses held back for a coalesced write.
// The request storage they come from is reused before that write, so the
// logged fields are copied; nothing is kept without an access log.
class http1_deferred_access_log final {
public:
    http1_deferred_access_log() noexcept;

    // Keeps the record of a response whose head completes at output offset
    // `head_end`. Duration is measured when the record is reported.
    void defer(std::size_t head_end, const http_request& request, ip_address client_address,
        std::chrono::steady_clock::time_point start, http_status_code status,
        const request_io_metrics& io_metrics = {});
    // Reports every kept response whose head lies within the first `written`
    // output bytes (in output order), then forgets all of them.
    void complete(const access_log_sink& access_log, ip_address remote_address, std::size_t written) noexcept;
    // Forgets every kept response and releases grown storage.
    void reset() noexcept;

private:
    struct entry_type final {
        std::size_t head_end_;
        std::size_t text_begin_;
        std::size_t method_bytes_;
        std::size_t path_bytes_;
        ip_address client_address_;
        std::chrono::steady_clock::time_point start_;
        request_io_metrics io_metrics_{};
        http_known_method known_method_;
        http_protocol_version protocol_version_;
        http_status_code status_;
    };

    std::vector<entry_type> entries_;
    std::string text_;
};

// Heavy request storage exists only while the connection is serving requests.
// A pipelined burst reuses it; idle connections retain only their session frame.
// The work set and its configured initial arena block share one allocation.
struct connection_work_set final {
    connection_work_set() noexcept;

    connection_read_buffer read_buffer_;
    http_response_head_buffer response_head_;
    http1_deferred_access_log deferred_access_;
    http1_server_request_parse_state parsed_;
    route_resolution route_resolution_;
};

[[nodiscard]] std::unique_ptr<connection_work_set> make_connection_work_set();

// Owns one admitted socket and its connection_slot as one linear value. The
// slot is acquired only after socket configuration succeeds, then moves into
// the lazy session task before task_scope::spawn. If coroutine allocation or
// spawn throws, destroying that cold ownership chain closes the socket first
// and releases the count second.
class accepted_connection_lease final {
public:
    accepted_connection_lease(asio::ip::tcp::socket socket, connection_count& count,
        connection_lifecycle lifecycle) noexcept
        : slot_(count),
          socket_(std::move(socket)),
          lifecycle_(std::move(lifecycle)) {
        lifecycle_.bind_transport(connection_transport::tcp(socket_));
    }

    accepted_connection_lease(const accepted_connection_lease&) = delete;
    accepted_connection_lease& operator=(const accepted_connection_lease&) = delete;
    accepted_connection_lease(accepted_connection_lease&& other) noexcept
        : slot_(std::move(other.slot_)),
          socket_(std::move(other.socket_)),
          lifecycle_(std::move(other.lifecycle_)) {
        lifecycle_.bind_transport(connection_transport::tcp(socket_));
    }
    accepted_connection_lease& operator=(accepted_connection_lease&&) = delete;

    ~accepted_connection_lease() {
        // Session bodies may close their socket before deferred frame retirement.
        // Keep the count until retirement, and close cold tasks or early exits here.
        if (socket_.is_open()) {
            std::error_code ignored;
            socket_.close(ignored);
        }
    }

    [[nodiscard]] asio::ip::tcp::socket& socket() & noexcept {
        return socket_;
    }
    asio::ip::tcp::socket& socket() && = delete;
    [[nodiscard]] connection_lifecycle& lifecycle() noexcept {
        return lifecycle_;
    }

private:
    // Declared first: released after the socket closes.
    connection_slot slot_;
    asio::ip::tcp::socket socket_;
    connection_lifecycle lifecycle_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The connection is closing, so no bytes from this request can be reused.
class http1_request_buffer_discarded final {
private:
    friend class http1_session_request_completion;

    constexpr http1_request_buffer_discarded() noexcept = default;
};

// The session still owns an unshifted read buffer and must remove exactly this
// request prefix before parsing the next pipelined request.
class http1_request_buffer_compaction final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_request_buffer_compaction(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}

    std::size_t consumed_bytes_;
};

// A request-body runtime over-read the next pipelined request into its own
// buffer, which dies with the route, and handed those bytes over in
// request-scoped storage that outlives the completion. The session installs
// them at its single buffer-cleanup point, once the response is written and
// the access log is recorded. A suffix still in the read buffer is a
// compaction instead.
class http1_request_buffer_pipeline_restore final {
public:
    [[nodiscard]] constexpr std::string_view pipeline() const noexcept {
        return pipeline_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_request_buffer_pipeline_restore(std::string_view pipeline) noexcept
        : pipeline_(pipeline) {}

    std::string_view pipeline_;
};

class http1_request_buffer_completion final {
public:
    [[nodiscard]] constexpr const http1_request_buffer_discarded* discarded() const& noexcept {
        return std::get_if<http1_request_buffer_discarded>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_discarded* discarded() const&& = delete;

    [[nodiscard]] constexpr const http1_request_buffer_compaction* compaction() const& noexcept {
        return std::get_if<http1_request_buffer_compaction>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_compaction* compaction() const&& = delete;

    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore()
        const& noexcept {
        return std::get_if<http1_request_buffer_pipeline_restore>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore() const&& =
        delete;

private:
    friend class http1_session_request_completion;

    using value_type = std::variant<http1_request_buffer_discarded, http1_request_buffer_compaction,
        http1_request_buffer_pipeline_restore>;

    template <typename alternative_type>
    explicit constexpr http1_request_buffer_completion(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

// A buffered response still needs the session's scatter-gather writer.
class http1_buffered_response_ready final {
private:
    friend class http1_session_request_completion;

    constexpr http1_buffered_response_ready() noexcept = default;
};

// A response-stream route already committed its final head. Only this
// alternative exposes the exact wire status used by access logging.
class http1_committed_stream_response final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // The handler failed after the head was on the wire. A close-delimited or
    // TLS body ended by a clean close would read as complete (RFC 9112 §9.8),
    // so the connection is aborted rather than closed gracefully.
    [[nodiscard]] constexpr bool truncated() const noexcept {
        return truncated_;
    }

private:
    friend class http1_session_request_completion;

    constexpr http1_committed_stream_response(http_status_code status, bool truncated) noexcept
        : status_(status),
          truncated_(truncated) {}

    http_status_code status_;
    bool truncated_;
};

// A route's buffered response broke the HTTP/1 final-response rules, so it
// cannot be committed. The session replaces it through the exception handler
// on a closing connection before preparing the response for the wire.
class http1_rejected_final_response final {
public:
    [[nodiscard]] http1_response_head_error exception() const noexcept {
        return error_;
    }

private:
    friend class http1_session_request_completion;

    explicit http1_rejected_final_response(::ruvia::http1_response_head_error error) noexcept
        : error_(std::move(error)) {}

    ::ruvia::http1_response_head_error error_;
};

// One terminal result for a dispatched HTTP/1 request. Wire ownership,
// connection reuse, and read-buffer cleanup are committed together so the
// session never reconstructs them from an optional status plus scalar flags.
class http1_session_request_completion final {
public:
    [[nodiscard]] static http1_session_request_completion make_buffered_closing(
        http1_request_connection_plan connection_plan) noexcept {
        if (connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_unrestored(
        http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_pipeline_restore(
        http1_request_connection_plan connection_plan, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream(
        http1_request_connection_plan connection_plan, http_status_code status,
        std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status, false), connection_plan,
            unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream_pipeline_restore(
        http1_request_connection_plan connection_plan, http_status_code status, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_committed_stream_response(status, false), connection_plan,
            http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_truncated_stream(
        http1_request_connection_plan connection_plan, http_status_code status) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status, true),
            connection_plan.require_close(), http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_rejected_final_response(
        http1_request_connection_plan connection_plan, ::ruvia::http1_response_head_error error) noexcept {
        return http1_session_request_completion(http1_rejected_final_response(std::move(error)),
            connection_plan.require_close(), http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] constexpr const http1_buffered_response_ready* buffered_response() const& noexcept {
        return std::get_if<http1_buffered_response_ready>(&value_);
    }
    [[nodiscard]] constexpr const http1_buffered_response_ready* buffered_response() const&& = delete;

    [[nodiscard]] constexpr const http1_committed_stream_response* committed_stream() const& noexcept {
        return std::get_if<http1_committed_stream_response>(&value_);
    }
    [[nodiscard]] constexpr const http1_committed_stream_response* committed_stream() const&& = delete;

    [[nodiscard]] constexpr const http1_rejected_final_response* rejected_final_response() const& noexcept {
        return std::get_if<http1_rejected_final_response>(&value_);
    }
    [[nodiscard]] constexpr const http1_rejected_final_response* rejected_final_response() const&& = delete;

    [[nodiscard]] constexpr http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

    [[nodiscard]] constexpr const http1_request_buffer_completion& buffer_completion() const& noexcept {
        return buffer_completion_;
    }
    [[nodiscard]] constexpr const http1_request_buffer_completion& buffer_completion() const&& = delete;

    // A buffered response can be replaced by a pre-commit policy error after
    // representation preparation (for example a forbidden identity fallback).
    // Rebind the final response plan without losing the exact read-buffer
    // cleanup alternative already established by request-body dispatch.
    [[nodiscard]] http1_session_request_completion with_buffered_connection_plan(
        http1_request_connection_plan connection_plan) const& noexcept {
        if (std::get_if<http1_buffered_response_ready>(&value_) == nullptr) {
            std::terminate();
        }
        if (buffer_completion_.discarded() != nullptr) {
            return make_buffered_closing(connection_plan.require_close());
        }
        if (const auto* compaction = buffer_completion_.compaction()) {
            return make_buffered_unrestored(connection_plan, compaction->consumed_bytes());
        }
        if (const auto* pipeline = buffer_completion_.pipeline_restore()) {
            if (connection_plan.disposition() == http1_close_policy::close_after_response) {
                return make_buffered_closing(connection_plan);
            }
            return make_buffered_pipeline_restore(connection_plan, pipeline->pipeline());
        }
        std::terminate();
    }

private:
    using value_type = std::variant<http1_buffered_response_ready, http1_committed_stream_response,
        http1_rejected_final_response>;

    [[nodiscard]] static http1_request_buffer_completion unshifted_buffer_completion(
        http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        if (connection_plan.disposition() == http1_close_policy::close_after_response) {
            return http1_request_buffer_completion(http1_request_buffer_discarded{});
        }
        return http1_request_buffer_completion(http1_request_buffer_compaction(consumed_bytes));
    }

    template <typename alternative_type>
    http1_session_request_completion(alternative_type alternative, http1_request_connection_plan connection_plan,
        http1_request_buffer_completion buffer_completion) noexcept
        : value_(std::move(alternative)),
          connection_plan_(connection_plan),
          buffer_completion_(std::move(buffer_completion)) {}

    value_type value_;
    http1_request_connection_plan connection_plan_;
    http1_request_buffer_completion buffer_completion_;
};

}  // namespace ruvia::detail

#if defined(_WIN32)
#endif

namespace ruvia {
class worker_runtime;
}

namespace ruvia::detail {

using tcp_socket_type = asio::ip::tcp::socket;
class accepted_connection_lease;
class context_services;
template <typename stream_type>
struct http1_connection_session;
class route_table;
struct server_connection_fields;
class worker_capabilities;

// What serve_requests hands back to run_stream when the connection stays open:
// no input is left buffered (the next request is awaited without a work set),
// or a complete prior-knowledge HTTP/2 preface is pending in the work set's
// read buffer (RFC 9113 §3.3).
enum class http1_serve_handoff : std::uint8_t {
    await_request,
    cleartext_http2,
};

#if !defined(_WIN32)
// One worker's dup()s of a listener's sockets; udp_ is invalid without an
// HTTP/3 service.
struct listener_socket_duplicates final {
    native_socket tcp_{invalid_native_socket};
    native_socket udp_{invalid_native_socket};
};
#endif

// Worker-local listeners, connection resources and session assembly. The
// surrounding runtime owns lifecycle phases and the scope joining every task.
// Admission runs open -> stop, and stopping closes every connection at once:
// there is no drain. On POSIX each worker binds every TCP listener with
// SO_REUSEPORT and accepts on its own event loop; on Windows the
// application's acceptor accepts and hands each socket over (adopt_socket).
// A TLS listener with an HTTP/3 service also gets a QUIC endpoint
// (http3_endpoint), which admits and stops the same way.
//
// Listener slots are numbered alike on every worker (and in the Windows
// acceptor) and never reused within a run, so a hand-over queued for a
// removed listener cannot reach another. A listener added while running is
// prepared (bound, not admitting) on every worker before any opens it; a
// removed one stops admitting at once and retires -- slot emptied, its
// session configuration destroyed -- once every task it owns has ended.
class worker_connections final {
public:
    worker_connections(asio::io_context& io, const worker_ref& worker_value,
        const route_table& routes_value, worker_capabilities& capabilities,
        http_server_options& options, const stop_token& stop_token_value,
        http_server_worker_state& state_value, task_scope& tasks,
        std::span<const http_server_listener_definition> listeners, worker_failure_sink failure, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );
    worker_connections(const worker_connections&) = delete;
    worker_connections& operator=(const worker_connections&) = delete;
    void prepare();
    void open_admission() noexcept;
    // Worker thread, once the worker state left running: stops admission and
    // closes every connection at once.
    void stop() noexcept;
    void retire_tls() noexcept;
    // Worker thread: listener `listener_index` -- TLS over TCP and its
    // HTTP/3 service alike -- selects every later handshake's identity from
    // `identities`.
    void adopt_tls_identities(
        std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept;
    // Worker thread, the controls of application::add_listener(): prepares
    // `definition` in slot `index` as prepare() would -- TLS contexts, bound
    // or adopted sockets, the HTTP/3 endpoint in slot `http3_index` -- without
    // admitting, or throws with the slot left empty.
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition,
        bool externally_accepted = false
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory = nullptr
#endif
    );
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition, bool externally_accepted,
        prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory = nullptr
#endif
    );
    // 唤醒准备阶段已经创建的接入任务；提交不再分配监听资源。
    void open_listener(std::size_t index) noexcept;
    // Closes the sockets of a listener prepared but never opened and retires
    // it; an empty slot is left alone.
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept;
    // application::remove_listener(): stops admitting listener `index` and
    // closes its TCP socket at once, holding `receipt` until its accept loop
    // exited. Its connections are served on; its HTTP/3 endpoint stops
    // admitting and closes its socket after its last connection.
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt, listener_remove_options options = {}) noexcept;
#if !defined(_WIN32)
    // application::duplicate_listener_sockets(): this worker's sockets of
    // listener `index`, duplicated. Throws std::logic_error when it no longer
    // admits.
    [[nodiscard]] listener_socket_duplicates duplicate_listener(std::size_t index);
#endif
    // Takes an already accepted native socket, including ownership on failure.
    // It first binds that socket to this worker's execution context.
    void adopt_connection(std::size_t index, native_socket socket, adopt_options options = {});
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] std::shared_ptr<connection_count> listener_connections(const asio::ip::tcp::endpoint& endpoint) const;
    // Owning worker only; false for an unknown or retired connection ID.
    [[nodiscard]] bool close_connection(std::uint64_t id) noexcept;
#if defined(_WIN32)
    // This worker as the acceptor's hand-over target.
    [[nodiscard]] windows_acceptor::business_worker acceptor_target() noexcept;
    void bind_acceptor(windows_acceptor& acceptor) noexcept {
#if defined(RUVIA_ENABLE_HTTP3)
        http3_placement_.acceptor_ = &acceptor;
        http3_services_.placement_.acceptor_ = &acceptor;
#else
        (void)acceptor;
#endif
    }
#endif

private:
    struct listener final {
        listener(asio::io_context& io, const worker_ref& worker_value,
            const http_server_listener_definition& definition,
            prepared_http_server_session session = {})
            : session_owner_(std::move(session)),
              session_(session_owner_ ? *session_owner_ : inline_session_.emplace(definition)),
              endpoint_(definition.endpoint_),
              tasks_(worker_value),
              retired_(worker_value),
              activated_(worker_value)
#if !defined(_WIN32)
              ,
              socket_(io),
              inherited_(definition.inherited_sockets_)
#endif
        {
#if defined(_WIN32)
            (void)io;
#endif
        }

        prepared_http_server_session session_owner_;
        std::optional<http_server_session_config> inline_session_;
        http_server_session_config& session_;
        asio::ip::tcp::endpoint endpoint_;
        // Its accept loop, sessions and HTTP/3 endpoint tasks, all joined
        // before the listener is destroyed: they borrow session_.
        task_scope tasks_;
        // Notified once removal or the worker's stop ended admission.
        worker_signal retired_;
        worker_signal activated_;
        // 取消准备持有至该监听的全部任务已 join；强制移除不等待调用它的会话自身。
        std::shared_ptr<void> retirement_receipt_;
#if defined(RUVIA_ENABLE_HTTP3)
        std::optional<std::size_t> http3_index_;
#endif
#if !defined(_WIN32)
        asio::ip::tcp::acceptor socket_;
        // Until preparation claimed this worker's sockets.
        std::shared_ptr<inherited_listener_sockets> inherited_;
        // A removal's, until the accept loop exited.
        std::shared_ptr<void> removal_receipt_;
        bool accepting_{false};
#endif
        // From preparation on, a task awaits retired_ to retire the listener.
        bool prepared_{false};
        // From opening until removal or the worker's stop.
        bool admitting_{false};
        bool externally_accepted_{false};
    };
#if defined(RUVIA_ENABLE_HTTP3)
    void add_http3_endpoint(std::size_t listener_index, std::size_t http3_index);
    void receive_http3_handoff(std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;
#endif
    // Builds the listener's TLS contexts, binds or adopts its sockets and
    // starts the task that retires it.
    void prepare_listener_resources(std::size_t index);
    void start_listener(std::size_t index) noexcept;
    void close_listener_sockets(listener& configured) noexcept;
    // Awaits the listener's retired_, joins its tasks, then empties its slot.
    task<void> retire_listener(std::size_t index);
#if defined(_WIN32)
    // The worker thread: binds a socket the acceptor accepted to this
    // worker's io_context (its first IOCP association) and serves it.
    void adopt_socket(std::size_t listener_index, windows_accepted_socket accepted);
#else
    void close_listeners() noexcept;
    task<void> run_accept_loop(std::size_t index);
#endif
    void report_accept_failure(std::exception_ptr failure, bool fatal) noexcept;
    task<void> rotate_tls_ticket_keys();
    void accept_socket(std::size_t index, tcp_socket_type socket, adopt_options options = {});
    task<void> prepare_connection(std::size_t index, tcp_socket_type socket,
        std::vector<std::byte> prefix, std::optional<ip_endpoint> remote);
    void admit_socket(std::size_t index, tcp_socket_type socket, std::optional<ip_endpoint> remote,
        std::vector<std::byte> prefix, std::size_t bytes_read = 0);
    // `remote` is the peer accept_socket() read before the socket moved into
    // the lease; absent when the socket no longer knew its peer.
    task<void> run_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix);
    task<void> run_tls_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix);
    void report_session_failure(
        tcp_socket_type& socket, std::string_view remote_address, std::exception_ptr failure) noexcept;
    template <typename stream_type>
    task<http_connection_close> run_stream(http_server_session_config& listener_value, stream_type& stream, tcp_socket_type& socket, context_services services);
#if defined(RUVIA_ENABLE_HTTP2)
    template <typename stream_type>
    task<http_connection_close> run_http2(stream_type& stream, tcp_socket_type& socket, context_services services);
#endif
    // A listener_dispatch::connection listener's connection: application::on_connection
    // owns it until its task completes. `fields` names everything but the
    // local endpoint, which this reads from `socket`.
    template <typename stream_type>
    task<void> serve_connection(stream_type& stream, tcp_socket_type& socket, server_connection_fields fields);
    template <typename stream_type>
    task<std::variant<http1_serve_handoff, http_connection_close>> serve_requests(
        http1_connection_session<stream_type>& session, connection_work_set& work_set);
    asio::io_context& io_;
    const worker_ref& worker_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    const stop_token& stop_token_;
    http_server_worker_state& state_;
    task_scope& tasks_;
    worker_failure_sink failure_;
    // This worker's position among the application's workers: the inherited
    // socket of each listener it claims.
    std::size_t worker_index_;
    // Indexed by listener slot; empty once a listener retired or failed to
    // prepare.
    std::vector<std::unique_ptr<listener>> listeners_;
    connection_scanner scanner_;
    // Caller-thread statistics and the Windows acceptor's balancing read it.
    connection_count connection_count_;
    struct listener_gauge final {
        listener_gauge(const asio::ip::tcp::endpoint& endpoint, connection_count& total)
            : endpoint_(endpoint),
              count_(&total) {}
        asio::ip::tcp::endpoint endpoint_;
        connection_count count_;
        std::atomic<bool> serving_{false};
        std::atomic<bool> published_{false};
    };
    void prepare_listener_gauge(std::size_t index, const asio::ip::tcp::endpoint& endpoint);
    mutable std::mutex gauges_mutex_;
    std::vector<std::shared_ptr<listener_gauge>> gauges_{};
    connection_id_sequence connection_ids_;
    connection_registry registry_;
    std::atomic<std::size_t> refused_{};
    std::atomic<std::size_t> failures_{};
    std::atomic<std::size_t> accept_failures_{};
    bool rotating_ticket_keys_{false};
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_placement http3_placement_;
    http3_worker_services http3_services_;
    bool http3_attached_{false};
    // Indexed by HTTP/3 slot, like listeners_. Declared last: destroyed
    // before every service its connections borrow.
    std::vector<std::unique_ptr<http3_endpoint>> http3_endpoints_;
#endif
};

}  // namespace ruvia::detail

namespace ruvia {
class env;
}  // namespace ruvia

namespace ruvia::detail {

class context_services;
struct connection_failure_sink;

struct worker_capability_definitions final {
#ifdef RUVIA_ENABLE_DATABASE
    std::span<const db_definition> databases_{};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::span<const redis_definition_type> redis_{};
#endif
    std::span<const worker_state_definition> worker_states_{};
    std::span<const http_client_definition_type> http_clients_{};
};

struct worker_capability_options final {
    std::optional<rate_limit_rule> default_rate_limit_{};
    route_rate_limit_presence route_rate_limits_{route_rate_limit_presence::absent};
    std::size_t rate_limit_capacity_{default_rate_limit_capacity_per_worker};
    std::size_t max_decoded_body_bytes_{default_max_buffered_body_bytes};
    http_client_worker_config http_client_worker_{};
    const env* env_{nullptr};
    const http_client_ip_extractor* trusted_proxies_{nullptr};
    bool precompressed_static_files_{false};
    // The server write_timeout_ bounding a file response's open on the pool.
    std::optional<timeout_duration> file_io_timeout_{};
    // The worker's connection failure outlet; required.
    const connection_failure_sink* connection_failure_{nullptr};
};

// Owns every application capability attached to one worker. Construction,
// startup, request exposure, and shutdown all follow this single ownership
// boundary; none of these registries is process-global or shared by workers.
// start()/stop() are the one worker bring-up and retirement chain, shared by
// the server worker and test_app. Both run on the owning worker.
class worker_capabilities final {
public:
    worker_capabilities(asio::io_context& io_context, const worker_ref& worker_value,
        worker_capability_definitions definitions,
        worker_capability_options options);
    worker_capabilities(asio::io_context&, worker_ref&&,
        worker_capability_definitions, worker_capability_options) = delete;

    worker_capabilities(const worker_capabilities&) = delete;
    worker_capabilities& operator=(const worker_capabilities&) = delete;

    // Creates the per-worker state, then connects remote capabilities. A
    // failure retires everything this call started before rethrowing.
    [[nodiscard]] task<void> start();
    // Interrupts capability I/O; idempotent. Retirement completes in stop().
    void close_now() noexcept;
    // Closes capability I/O, joins in-flight client work, then destroys the
    // per-worker state. Safe after a failed or skipped start().
    [[nodiscard]] task<void> stop();

    [[nodiscard]] context_services make_context_services(const stop_token& stop_token);
    context_services make_context_services(stop_token&&) = delete;
    [[nodiscard]] worker_client_registry_view client_registries() noexcept;
    [[nodiscard]] const worker_state_registry& worker_states() const noexcept;

    // Owner thread, together with adoption of the first routed table.
    void adopt_route_rate_limiter(rate_limiter& prepared) noexcept {
        rate_limiter_.adopt_route_storage(prepared);
    }

private:
    const worker_ref& worker_;
#ifdef RUVIA_ENABLE_REDIS
    redis_registry redis_;
#endif
#ifdef RUVIA_ENABLE_DATABASE
    db_registry databases_;
#endif
    http_client_worker_services http_client_services_;
    http_client_registry http_clients_;
    worker_state_registry worker_states_;
    rate_limiter rate_limiter_;
    worker_capability_options options_;
    context_worker_services worker_services_;
};

}  // namespace ruvia::detail

namespace ruvia {

class http_client;
class worker_ref;
#ifdef RUVIA_ENABLE_DATABASE
class db_client;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_client;
#endif

namespace detail {

// Caller-owned standalone clients a router lends to the requests it
// dispatches. Every entry borrows its client, which must run on the router's
// worker and outlive every dispatch; aliases are unique per client kind. The
// set is sealed with its router, so request-time lookup only reads it.
class borrowed_clients final {
public:
    borrowed_clients();
    borrowed_clients(const borrowed_clients&) = delete;
    borrowed_clients& operator=(const borrowed_clients&) = delete;

    [[nodiscard]] bool empty() const noexcept;

    void bind_http_client(const worker_ref& worker, std::string_view alias, const http_client& client);
    [[nodiscard]] const http_client* http_clients(std::string_view alias) const noexcept;

#ifdef RUVIA_ENABLE_DATABASE
    void bind_database(const worker_ref& worker, std::string_view alias, const db_client& client);
    [[nodiscard]] const db_client* database(std::string_view alias) const noexcept;
#endif

#ifdef RUVIA_ENABLE_REDIS
    void bind_redis(const worker_ref& worker, std::string_view alias, const redis_client& client);
    [[nodiscard]] const redis_client* redis(std::string_view alias) const noexcept;
#endif

private:
    template <typename client_type>
    struct alias_entry final {
        std::string alias_;
        const client_type* client_;
    };

    std::vector<alias_entry<http_client>> http_clients_;
#ifdef RUVIA_ENABLE_DATABASE
    std::vector<alias_entry<db_client>> databases_;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::vector<alias_entry<redis_client>> redis_;
#endif
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

enum class app_lifecycle_state : std::uint8_t {
    stopped,
    preparing,
    starting,
    running,
    stopping,
};

enum class app_stop_request : std::uint8_t {
    ignored,
    // Startup was in progress: the runtime stops at once, rolling it back.
    startup,
    // The app was serving: the run() owner stops admission, then the runtime.
    serving,
};

// application owns the mutex protecting this state machine. application::run() is the sole
// lifecycle owner; other threads may only request the monotonic transition to
// stopping. A stop requested while stopped is latched and consumed by the next run.
class app_lifecycle final {
public:
    [[nodiscard]] bool active() const noexcept {
        return state_ != app_lifecycle_state::stopped;
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ == app_lifecycle_state::stopping;
    }

    // Serving, with no stop requested yet.
    [[nodiscard]] bool running() const noexcept {
        return state_ == app_lifecycle_state::running;
    }

    [[nodiscard]] bool begin_run() noexcept {
        if (state_ != app_lifecycle_state::stopped) {
            return false;
        }
        state_ = stop_pending_ ? app_lifecycle_state::stopping : app_lifecycle_state::preparing;
        stop_pending_ = false;
        return true;
    }

    [[nodiscard]] bool publish_runtime() noexcept {
        if (state_ != app_lifecycle_state::preparing) {
            return false;
        }
        state_ = app_lifecycle_state::starting;
        return true;
    }

    [[nodiscard]] bool mark_running() noexcept {
        if (state_ != app_lifecycle_state::starting) {
            return false;
        }
        state_ = app_lifecycle_state::running;
        return true;
    }

    [[nodiscard]] app_stop_request request_stop() noexcept {
        switch (state_) {
            case app_lifecycle_state::preparing:
            case app_lifecycle_state::starting:
                state_ = app_lifecycle_state::stopping;
                return app_stop_request::startup;
            case app_lifecycle_state::running:
                state_ = app_lifecycle_state::stopping;
                return app_stop_request::serving;
            case app_lifecycle_state::stopped:
                stop_pending_ = true;
                return app_stop_request::ignored;
            case app_lifecycle_state::stopping:
                return app_stop_request::ignored;
        }
        return app_stop_request::ignored;
    }

    void complete_run() noexcept {
        state_ = app_lifecycle_state::stopped;
    }

private:
    app_lifecycle_state state_{app_lifecycle_state::stopped};
    bool stop_pending_{};
};

struct app_runtime_graph;

struct app_document_root_config final {
    explicit app_document_root_config(static_root_config_storage configured_static_options)
        : static_options_(std::move(configured_static_options)) {}

    http_native_path_string_type root_;
    static_root_config_storage static_options_;
    std::chrono::milliseconds refresh_interval_{};
    static_root_precompression_options precompression_;
    static_root_resident_options resident_;
};

struct app_state final {
    app_state();
    ~app_state();

    std::vector<http_server_listener_definition> listeners_;
    callback_registration<access_log_callback> access_log_callback_;
    callback_registration<connection_failure_callback> connection_failure_callback_;
    callback_registration<tls_client_hello_callback> tls_client_hello_callback_;
    callback_registration<http2_input_callback> http2_input_callback_;
#if defined(RUVIA_ENABLE_HTTP2)
    callback_registration<http2_frame_callback> http2_frame_callback_;
    callback_registration<http2_headers_callback> http2_headers_callback_;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    callback_registration<http3_frame_callback> http3_frame_callback_;
    callback_registration<http3_field_callback> http3_field_callback_;
#endif
    callback_registration<connection_admission_callback> connection_admission_callback_;
    callback_registration<connection_open_callback> connection_open_callback_;
    callback_registration<connection_close_callback> connection_close_callback_;
    callback_registration<connection_callback> connection_callback_;
    callback_registration<request_stream_callback> request_stream_callback_;
    http_server_options options_{};
    std::optional<app_document_root_config> document_root_config_;
    app_configuration configuration_;
    std::optional<blocking_pool_options> blocking_pool_{std::in_place};
    std::vector<callback_registration<app_hook>> on_start_hooks_;
    std::vector<callback_registration<app_hook>> on_stop_hooks_;
#ifdef RUVIA_ENABLE_DATABASE
    std::vector<db_definition> databases_;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::vector<redis_definition_type> redis_;
#endif
    std::vector<http_client_definition_type> http_clients_;
    env env_;
    std::unique_ptr<app_runtime_graph> runtime_;

    mutable std::mutex mutex_;
    std::condition_variable lifecycle_changed_;
    app_lifecycle lifecycle_;
    // Controls never hold a mutex across suspension. The synchronous run()
    // owner keeps the published runtime and frozen configuration alive until
    // every admitted control (including its blocking preparation) has joined.
    runtime_control_gate runtime_control_gate_;
    std::size_t runtime_controls_{};
};

inline void ensure_app_not_running(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}

template <typename configure_type>
application& mutate_stopped_app(
    application& app, app_state& state_value, const char* running_message, configure_type&& configure) {
    std::lock_guard lock(state_value.mutex_);
    ensure_app_not_running(state_value.lifecycle_.active(), running_message);
    std::forward<configure_type>(configure)(state_value);
    return app;
}

void run_app(application& app, app_state& state_value);
// The one stop chain for a published runtime: closes admission on the acceptor
// and every worker. The caller holds state_value.mutex_; run_app owns retirement.
void request_runtime_stop(app_state& state_value) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct env_variable final {
    std::string name_;
    std::string value_;
};

struct env_state final {
    std::vector<env_variable> variables_;
    bool loaded_{false};
};

[[nodiscard]] std::vector<env_variable> read_dotenv_entries(
    std::istream& input, const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path dotenv_executable_directory();
// prefix followed by path in UTF-8. path::string() narrows a Windows wide path
// through the ANSI code page, which throws or yields non-UTF-8 error text.
[[nodiscard]] std::string dotenv_path_message(std::string_view prefix, const std::filesystem::path& path);

}  // namespace ruvia::detail

namespace ruvia::detail {

class worker_capabilities;
class worker_state_registry;

// The endpoint behind every web_worker_ref. Core owns admission: posts go
// through worker_ref::post_payload and accepted jobs start only while that
// worker accepts and its stop token is clear. Started jobs run in the worker's
// task_scope, which the server worker joins before retiring capabilities.
//
// Escaped handles share this object so their terminal queries (valid,
// accepting, id, stats) remain safe after the server is gone. The borrowed
// server state below is touched only by jobs started on the worker; core
// abandons every other accepted job without running it.
class web_worker_dispatch final {
public:
    using job_type = move_only_function<ruvia::task<void>(web_worker_context&)>;

    web_worker_dispatch(ruvia::worker_runtime& runtime,
        worker_capabilities& capabilities, task_scope& jobs, const stop_token& stop_token_value);

    web_worker_dispatch(const web_worker_dispatch&) = delete;
    web_worker_dispatch& operator=(const web_worker_dispatch&) = delete;

    [[nodiscard]] static web_worker_ref make_ref(std::shared_ptr<web_worker_dispatch> dispatch) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_post_result post(job_type job);
    [[nodiscard]] web_worker_stats stats() const noexcept;

    // Reconciles the outstanding reservation of an accepted job that core
    // destroys without running. Public only for the reservation deleter.
    void abandon() noexcept;

private:
    void start(job_type job);
    [[nodiscard]] ruvia::task<void> run(job_type job);

    // A stable copy: queries stay valid after the server's runtime is gone.
    worker_ref worker_;
    ruvia::worker_runtime& runtime_;
    worker_client_registry_view client_registries_;
    const worker_state_registry& worker_states_;
    task_scope& jobs_;
    const stop_token& stop_token_;
    std::atomic_uint64_t accepted_{0};
    std::atomic_uint64_t queue_full_{0};
    std::atomic_uint64_t worker_stopping_{0};
    std::atomic_uint64_t completed_{0};
    std::atomic_uint64_t failed_count_{0};
    std::atomic_size_t outstanding_{0};
};

}  // namespace ruvia::detail

namespace ruvia {
class worker_ref;
}  // namespace ruvia

namespace ruvia::detail {

struct http_server_options;

// The application's one document-root refresher. The first registered worker
// walks the root, reads small files into the snapshot and precompresses changed files
// once per interval; every worker adopts the published immutable snapshot on
// its own thread. The runtime graph owns this object, so it outlives every
// worker and their joined refresh tasks.
class document_root_publisher final {
public:
    // Builds the initial snapshot; a failure here fails application startup.
    document_root_publisher(const std::filesystem::path& root, const static_root_config_storage& config,
        std::chrono::milliseconds interval, static_root_precompression_options precompression,
        static_root_resident_options resident, std::size_t worker_count);
    ~document_root_publisher();

    document_root_publisher(const document_root_publisher&) = delete;
    document_root_publisher& operator=(const document_root_publisher&) = delete;

    [[nodiscard]] std::chrono::milliseconds interval() const noexcept {
        return interval_;
    }

    [[nodiscard]] std::size_t failures() const noexcept {
        return failures_.load(std::memory_order_relaxed);
    }

    // Counts a failed refresh pass or adoption and reports its cause through
    // the rate-limited unhandled-failure sink; the last complete snapshot
    // stays published.
    void record_failure(std::exception_ptr failure) noexcept;

    // One rebuild pass on the refresher. A changed tree is published; a
    // failed pass is counted and the last complete snapshot stays published.
    [[nodiscard]] task<void> refresh(
        const worker_ref& worker, const http_server_worker_state& state, const stop_token& stop = {});

private:
    friend class static_root_runtime;

    using publication_owner =
        std::unique_ptr<document_root_snapshot>;

    struct mailbox final {
        ~mailbox();

        // Exchange transfers exclusive ownership of the node, not a borrowed
        // shared_ptr source. Exactly one exchanging thread reclaims each node.
        std::atomic<document_root_snapshot*> pending_{nullptr};
    };

    static_assert(std::atomic<document_root_snapshot*>::is_always_lock_free);

    // The coordinator registers every worker before starting any of them.
    // The fixed mailbox storage then outlives all workers and joined tasks.
    // The first mailbox identifies the one refresher.
    [[nodiscard]] mailbox& register_worker() noexcept;

    void publish(document_root_snapshot snapshot);
    void commit_publication(document_root_snapshot snapshot,
        std::vector<publication_owner>& publication) noexcept;

    // The configured path, not the snapshot's canonical one: a refresh must
    // re-resolve symlinks so a retargeted root (atomic deploy) is followed.
    std::filesystem::path root_;
    std::chrono::milliseconds interval_;
    static_root_precompression_options precompression_;
    static_root_resident_options resident_;
    // Only the first registered worker reads or replaces current_ after startup.
    document_root_snapshot current_;
    std::vector<mailbox> mailboxes_;
    std::size_t registered_workers_{0};
    std::atomic<std::size_t> failures_{0};
};

// One worker's adopted document-root generations. Request bindings lease the
// current generation with a plain counter; a superseded generation stays until
// its leases drain, so a long request pins only the snapshot it borrows. The
// worker joins refresh() before teardown.
class static_root_runtime final {
public:
    static_root_runtime(http_server_options& options, const worker_ref& worker_value,
        const http_server_worker_state& state_value);
    ~static_root_runtime();
    static_root_runtime(const static_root_runtime&) = delete;
    static_root_runtime& operator=(const static_root_runtime&) = delete;

    [[nodiscard]] bool enabled() const noexcept {
        return publisher_ != nullptr;
    }

    // Once per interval while the worker admits work: reclaim drained
    // generations, run the application's rebuild if this worker claimed it,
    // then adopt the published snapshot.
    [[nodiscard]] task<void> refresh(const stop_token& stop = {});

private:
    using generation_owner =
        std::unique_ptr<document_root_generation>;

    void adopt_published();

    http_server_options& options_;
    document_root_publisher* publisher_;
    document_root_publisher::mailbox* mailbox_;
    const worker_ref& worker_;
    const http_server_worker_state& state_;
    // Retained locally if preparing a worker generation fails; a later
    // publication supersedes it instead of accumulating missed snapshots.
    document_root_publisher::publication_owner pending_;
    generation_owner current_;
    std::vector<generation_owner> retired_;
    const bool refreshes_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class route_table;
class validated_http_server_configuration;
class web_worker_dispatch;

// One server worker: its core worker_runtime plus the HTTP domain state that
// runtime's hooks compose. Core owns the generic lifecycle (launch, ready/serve
// barrier, stop admission, finalization, join, first failure); application
// sequences every independently listening worker through one core worker_group.
class web_worker_runtime final {
public:
    // `pool` is the application pool the worker's event loop borrows for
    // every offload and client host lookup; null when the application
    // disabled it. It must outlive the worker. `worker_index` is this
    // worker's position among the application's workers.
    web_worker_runtime(const validated_http_server_configuration& configuration, const route_table& routes_value,
        worker_capability_definitions capabilities, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );
    ~web_worker_runtime();

    web_worker_runtime(const web_worker_runtime&) = delete;
    web_worker_runtime& operator=(const web_worker_runtime&) = delete;

    [[nodiscard]] ruvia::worker_runtime& runtime() & noexcept {
        return runtime_;
    }
    ruvia::worker_runtime& runtime() && = delete;
    [[nodiscard]] stop_token cancellation() const noexcept {
        return stop_token_;
    }
    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] std::shared_ptr<connection_count> listener_connections(const asio::ip::tcp::endpoint& endpoint) const {
        return connections_.listener_connections(endpoint);
    }
    [[nodiscard]] web_worker_ref web_worker() const;
    // Owner thread, before admitting this run's first routed listener.
    void adopt_route_rate_limiter(rate_limiter& prepared) noexcept {
        capabilities_.adopt_route_rate_limiter(prepared);
    }
    // Owner thread only, from a control posted to runtime(): listener
    // `listener_index` -- TLS over TCP and its HTTP/3 service alike --
    // selects every later handshake's identity from `identities`.
    void adopt_tls_identities(
        std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept {
        connections_.adopt_tls_identities(listener_index, std::move(identities));
    }
    // Owner thread only, from controls posted to runtime(): the listener
    // transactions of application::add_listener() and remove_listener()
    // (see worker_connections).
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition, prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory
#endif
    ) {
        connections_.prepare_listener(index, http3_index, definition, false, std::move(session)
#if defined(RUVIA_ENABLE_HTTP3)
                                                                                 ,
            directory
#endif
        );
    }
    void open_listener(std::size_t index) noexcept {
        connections_.open_listener(index);
    }
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept {
        connections_.discard_listener(index, std::move(receipt));
    }
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt, listener_remove_options options = {}) noexcept {
        connections_.remove_listener(index, std::move(receipt), options);
    }
    [[nodiscard]] bool close_connection(std::uint64_t connection_id) noexcept {
        return connections_.close_connection(connection_id);
    }
#if !defined(_WIN32)
    [[nodiscard]] listener_socket_duplicates duplicate_listener(std::size_t index) {
        return connections_.duplicate_listener(index);
    }
#endif
#if defined(_WIN32)
    // This worker as the Windows acceptor's hand-over target.
    [[nodiscard]] windows_acceptor::business_worker acceptor_target() noexcept {
        return connections_.acceptor_target();
    }
#endif

private:
    web_worker_runtime(std::span<const http_server_listener_definition> listeners,
        const route_table& routes_value, worker_capability_definitions capabilities,
        http_server_options validated_options, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );

    void serve_on_context();
    void stop_admission_on_context() noexcept;
    void finalize_on_context() noexcept;
    task<void> run_worker();
    ruvia::worker_runtime runtime_;
    asio::io_context& io_context_;
    worker_signal finalize_signal_;
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    const route_table& routes_;
    // Every connection's inbound buffers charge this worker-wide budget.
    inbound_buffer_budget inbound_budget_;
    // Borrowed through options_.codecs_ by requests and connections, which
    // all end before it is destroyed.
    worker_codecs codecs_;
    task_scope background_tasks_;
    http_server_worker_state worker_state_{http_server_worker_state::fresh};
    http_server_options options_;
    static_root_runtime static_roots_;
    worker_capabilities capabilities_;
    worker_connections connections_;
    // Shared only with escaped public handles: their terminal queries outlive
    // this worker. Jobs run in background_tasks_ and are joined by run_worker.
    std::shared_ptr<web_worker_dispatch> web_worker_dispatch_;
    // Atomic because stats() reads them from the caller's thread while this
    // worker updates them. Relaxed: they are counters, and publish nothing.
    std::atomic<std::size_t> worker_failures_{0};
};

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP3)
#endif
#if defined(_WIN32)
#endif

namespace ruvia::detail {

struct app_worker_slot final {
    app_worker_slot(controller_store configured_controllers,
        std::unique_ptr<route_table> configured_routes,
        std::unique_ptr<web_worker_runtime> configured_runtime)
        : controllers_(std::move(configured_controllers)),
          routes_(std::move(configured_routes)),
          runtime_(std::move(configured_runtime)) {}

    app_worker_slot(app_worker_slot&&) noexcept = default;
    app_worker_slot& operator=(app_worker_slot&&) noexcept = default;
    app_worker_slot(const app_worker_slot&) = delete;
    app_worker_slot& operator=(const app_worker_slot&) = delete;

    // Destruction is intentionally reversed: runtime, routes, controllers.
    controller_store controllers_;
    std::unique_ptr<route_table> routes_;
    std::unique_ptr<web_worker_runtime> runtime_;
};

struct app_runtime_graph final {
    app_runtime_graph() = default;

    // Declared before workers so it is destroyed after suspended worker tasks.
    std::unique_ptr<blocking_pool> blocking_pool_;
    // The one document-root refresher; null without a document root. Workers
    // borrow it and adopt its snapshots, so it is declared before them.
    std::unique_ptr<document_root_publisher> document_root_;
    // Every worker's route_table binds its instances to this one immutable plan.
    compiled_route_plan_ptr_type route_plan_;
    // A non-routed run keeps its original empty plan alive if its first routed
    // listener is added while serving; a stopping worker may still borrow it.
    compiled_route_plan_ptr_type empty_route_plan_;
    bool routes_enabled_{false};
#if defined(RUVIA_ENABLE_HTTP3)
    // Hands QUIC datagrams to the worker that owns their connection; null
    // without an HTTP/3 listener. Declared before the workers and the
    // acceptor, whose queued hand-overs reference it.
    std::unique_ptr<http3_worker_directory> http3_directory_;
#endif
#if defined(_WIN32)
    // The one thread that owns every listener and hands sockets and datagrams
    // to the workers. Declared before them: they send their datagrams
    // through it until they joined, so it is destroyed after them.
    std::unique_ptr<windows_acceptor> acceptor_;
#endif
    std::vector<app_worker_slot> workers_;
    // The one lifecycle over every independently listening worker. It borrows
    // their core runtimes, so it is declared after them.
    worker_group group_;
    // Guarded by the application mutex. Worker handles are published only
    // once every worker is ready, so a posted job never meets a starting worker.
    bool workers_ready_{false};
    // Guarded by the application mutex. The TCP endpoint each listener slot
    // serves: slot i is listener i on every worker (and in the Windows
    // acceptor), nullopt once removed or while being added. Slots are never
    // reused, so a queued hand-over cannot reach another listener.
    std::vector<std::optional<asio::ip::tcp::endpoint>> listener_slots_;
    // HTTP/3 services are numbered alike, apart from the TCP listeners.
    std::size_t http3_slot_count_{0};
    // Where every TLS listener's ticket key epochs start, so a listener
    // added while the run serves rotates with the others.
    std::chrono::steady_clock::time_point tls_key_origin_{};
};

}  // namespace ruvia::detail
