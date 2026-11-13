#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "ruvia/core/operation_options.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/redis/redis_repository.h"

#include "integration/request_io_metrics.h"
#include "redis/redis_client_runtime.h"
#include "redis/redis_pool.h"
#include "redis/redis_protocol.h"
#include "redis/redis_reply_parsers.h"

namespace ruvia::detail {

// Classifies a raw pooled command before it is encoded. Connection-stateful
// commands are rejected; WAIT/WAITAOF are accepted only where a barrier is
// allowed. Returns whether the command needs the blocking pool; a blocking
// command throws when `allow_blocking` is false. An XREAD/XREADGROUP option
// this classifier does not know throws instead of being guessed non-blocking.
[[nodiscard]] bool validate_redis_pooled_command(
    std::span<const std::string_view> args, bool allow_blocking, bool allow_barrier = false);

// A wait the server may hold open indefinitely (a blocking command, a barrier,
// a subscription) must be bounded by its owner: a stop token or a finite
// operation timeout. Throws std::invalid_argument with `message` otherwise.
void require_bounded_redis_wait(const operation_options& options, const char* message);

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_redis_token(std::string_view value, std::string_view token) {
    return http_ascii_equals_ignore_case(value, token);
}

enum class redis_stream_read_kind : bool { read,
    read_group };

// Scans the options before STREAMS:
//   XREAD [COUNT n] [BLOCK ms] STREAMS ...
//   XREADGROUP GROUP g c [COUNT n] [BLOCK ms] [CLAIM min-idle-time] [NOACK] STREAMS ...
// https://redis.io/docs/latest/commands/xreadgroup/
// A token outside this grammar may change how the server waits, so it is
// rejected rather than routed as non-blocking.
[[nodiscard]] bool stream_read_options_block(
    std::span<const std::string_view> args, std::size_t first_option, redis_stream_read_kind kind) {
    const bool group = kind == redis_stream_read_kind::read_group;
    bool blocking = false;
    for (std::size_t i = first_option; i < args.size();) {
        const auto token = args[i];
        if (is_redis_token(token, "STREAMS")) {
            const auto remaining = args.size() - (i + 1);
            if (remaining < 2 || (remaining & 1U) != 0) {
                throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
            }
            return blocking;
        }
        if (is_redis_token(token, "BLOCK")) {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
            }
            blocking = true;
            i += 2;
        } else if (is_redis_token(token, "COUNT") || (group && is_redis_token(token, "CLAIM"))) {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
            }
            i += 2;
        } else if (group && is_redis_token(token, "NOACK")) {
            ++i;
        } else {
            throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
        }
    }
    throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
}

[[nodiscard]] bool xread_group_command_is_blocking(std::span<const std::string_view> args) {
    if (args.size() < 4 || !is_redis_token(args[1], "GROUP")) {
        throw std::invalid_argument("unsupported redis XREAD/XREADGROUP option");
    }
    return stream_read_options_block(args, 4, redis_stream_read_kind::read_group);
}

}  // namespace

bool validate_redis_pooled_command(std::span<const std::string_view> args, bool allow_blocking, bool allow_barrier) {
    if (args.empty() || args.front().empty()) {
        throw std::invalid_argument("redis command requires a command name");
    }
    const auto command = args.front();
    // Redis barriers use the current connection's last write offset, not the
    // client's other pooled connections: https://redis.io/docs/latest/commands/wait/
    if (is_redis_token(command, "WAIT") || is_redis_token(command, "WAITAOF")) {
        if (!allow_barrier) {
            throw std::invalid_argument(
                "redis WAIT and WAITAOF require a non-transaction pipeline containing the writes");
        }
        return true;
    }
    const auto reject_stateful = [] {
        throw std::invalid_argument(
            "stateful redis commands are not allowed through pooled command()");
    };
    bool blocking = false;
    switch (command.size()) {
        case 4:
            if (is_redis_token(command, "AUTH") || is_redis_token(command, "EXEC") ||
                is_redis_token(command, "QUIT") || is_redis_token(command, "SYNC")) {
                reject_stateful();
            }
            break;
        case 5:
            if (is_redis_token(command, "HELLO") || is_redis_token(command, "MULTI") ||
                is_redis_token(command, "PSYNC") || is_redis_token(command, "RESET") ||
                is_redis_token(command, "WATCH")) {
                reject_stateful();
            }
            if (is_redis_token(command, "BLPOP") || is_redis_token(command, "BRPOP")) {
                blocking = true;
            } else if (is_redis_token(command, "XREAD")) {
                blocking = stream_read_options_block(args, 1, redis_stream_read_kind::read);
            }
            break;
        case 6:
            if (is_redis_token(command, "ASKING") || is_redis_token(command, "CLIENT") ||
                is_redis_token(command, "SELECT")) {
                reject_stateful();
            }
            blocking = is_redis_token(command, "BLMOVE") || is_redis_token(command, "BLMPOP") ||
                       is_redis_token(command, "BZMPOP");
            break;
        case 7:
            if (is_redis_token(command, "DISCARD") || is_redis_token(command, "MONITOR") ||
                is_redis_token(command, "UNWATCH")) {
                reject_stateful();
            }
            break;
        case 8:
            if (is_redis_token(command, "READONLY") || is_redis_token(command, "REPLCONF")) {
                reject_stateful();
            }
            blocking = is_redis_token(command, "BZPOPMIN") || is_redis_token(command, "BZPOPMAX");
            break;
        case 9:
            if (is_redis_token(command, "READWRITE") || is_redis_token(command, "SUBSCRIBE")) {
                reject_stateful();
            }
            break;
        case 10:
            if (is_redis_token(command, "PSUBSCRIBE") || is_redis_token(command, "SSUBSCRIBE")) {
                reject_stateful();
            }
            if (is_redis_token(command, "BRPOPLPUSH")) {
                blocking = true;
            } else if (is_redis_token(command, "XREADGROUP")) {
                blocking = xread_group_command_is_blocking(args);
            }
            break;
        case 11:
            if (is_redis_token(command, "UNSUBSCRIBE")) {
                reject_stateful();
            }
            break;
        case 12:
            if (is_redis_token(command, "PUNSUBSCRIBE") || is_redis_token(command, "SUNSUBSCRIBE")) {
                reject_stateful();
            }
            break;
        default:
            return false;
    }
    if (blocking && !allow_blocking) {
        throw std::invalid_argument(
            "blocking redis command is not allowed in a pipeline or transaction");
    }
    return blocking;
}

void require_bounded_redis_wait(const operation_options& options, const char* message) {
    if (!options.stop_token_.stoppable() && !options.timeout_.has_value()) {
        throw std::invalid_argument(message);
    }
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

template <typename duration_type>
void require_positive_redis_ttl(duration_type ttl, const char* message) {
    if (ttl.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

[[nodiscard]] detail::redis_owned_command make_redis_scan_command(std::string_view command,
    std::optional<std::string_view> key, const redis_scan_options& options) {
    std::array<char, http_max_unsigned_decimal_size> cursor_scratch{};
    std::array<char, http_max_unsigned_decimal_size> count_scratch{};
    std::array<std::string_view, 7> args{};
    std::size_t count = 0;
    args[count++] = command;
    if (key.has_value()) {
        args[count++] = *key;
    }
    args[count++] = http_format_unsigned(
        cursor_scratch, options.cursor_.has_value() ? detail::redis_types_access::cursor_value(*options.cursor_) : 0);
    if (options.match_.has_value()) {
        args[count++] = "MATCH";
        args[count++] = *options.match_;
    }
    if (options.count_.has_value()) {
        args[count++] = "COUNT";
        args[count++] = http_format_unsigned(count_scratch, options.count_->value());
    }
    return detail::make_owned_redis_command(std::span<const std::string_view>(args.data(), count));
}

// The server-side wait starts when the command is written, so the reply
// deadline is armed at write time: slot acquisition and a lazy connect must not
// consume the block window. The grace covers the reply's network latency.
[[nodiscard]] std::optional<timeout_duration> redis_block_reply_timeout(
    const operation_options& options, redis_block_wait wait) {
    if (const auto duration = wait.duration(); duration.has_value()) {
        constexpr auto grace = std::chrono::seconds(1);
        if (*duration > std::chrono::milliseconds::max() - grace) {
            return std::chrono::milliseconds::max();
        }
        return *duration + grace;
    }
    detail::require_bounded_redis_wait(options, "infinite redis block requires a stop_token or finite operation timeout");
    return std::nullopt;
}

}  // namespace

redis_session::redis_session(detail::redis_client_runtime& runtime,
    ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics) noexcept
    : runtime_(&runtime),
      io_metrics_(io_metrics),
      registration_(operation_scope, this, &redis_session::expire_capability) {}

redis_session::redis_session(detail::redis_client_runtime& runtime,
    ::ruvia::operation_scope& operation_scope, operation_options options) noexcept
    : runtime_(&runtime),
      operation_options_(std::move(options)),
      registration_(operation_scope, this, &redis_session::expire_capability) {}

redis_session::redis_session(const redis_session& other) noexcept
    : runtime_(other.runtime_),
      io_metrics_(other.io_metrics_),
      operation_options_(other.operation_options_),
      registration_(other.registration_, this) {}

redis_session redis_session::with_options(operation_options options) const {
    registration_.require_active();
    redis_session configured(*this);
    configured.operation_options_ =
        override_operation_options(operation_options_, std::move(options));
    return configured;
}

const worker_ref& redis_session::worker() const& {
    registration_.require_active();
    return runtime_->worker();
}

detail::redis_pool& redis_session::pool() const noexcept {
    return runtime_->general();
}

detail::redis_pool& redis_session::blocking_pool() const noexcept {
    return runtime_->blocking();
}

task<redis_value> redis_session::execute(detail::redis_pool& pool_value, detail::redis_owned_command&& command,
    std::optional<timeout_duration> reply_timeout) const {
    return pool_value.execute_owned(std::move(command), operation_options_, reply_timeout, io_metrics_);
}

void redis_session::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<redis_session*>(target);
    handle.runtime_ = nullptr;
    handle.io_metrics_ = nullptr;
    handle.operation_options_ = {};
}

task<redis_value> redis_session::command(std::span<const std::string_view> args) const {
    registration_.require_active();
    const bool blocking = detail::validate_redis_pooled_command(args, true);
    if (blocking) {
        detail::require_bounded_redis_wait(
            operation_options_, "raw blocking redis command requires a stop_token or finite operation timeout");
    }
    return scoped(execute(blocking ? blocking_pool() : pool(), detail::make_owned_redis_command(args)));
}

task<redis_value> redis_session::command_owned(std::vector<std::string>&& args) const {
    registration_.require_active();
    return pool().execute_owned(std::vector<std::string>(args.begin(), args.end()),
        operation_options_, std::nullopt, io_metrics_);
}

task<redis_value> redis_session::script_owned(
    const detail::redis_orm_script& script, std::vector<std::string>&& args) const {
    registration_.require_active();
    return detail::execute_redis_orm_script(
        pool(), script, std::move(args), io_metrics_, operation_options_);
}

task<void> redis_session::ping() const {
    return mapped<void>({"PING"}, detail::parse_redis_pong);
}

task<std::string> redis_session::ping(std::string_view message) const {
    return mapped<std::string>({"PING", message}, detail::take_redis_string);
}

task<std::optional<std::string>> redis_session::get(std::string_view key) const {
    return mapped<std::optional<std::string>>({"GET", key}, detail::parse_redis_optional_string);
}

task<std::vector<std::optional<std::string>>> redis_session::mget(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::optional<std::string>>>(
        pool(), detail::redis_command_with_keys("MGET", keys), detail::parse_redis_optional_string_array);
}

task<redis_set_result> redis_session::set(
    std::string_view key, std::string_view value, redis_set_options options) const {
    registration_.require_active();
    auto command = detail::redis_set_args(key, value, options);
    return execute_mapped<redis_set_result>(pool(), std::move(command),
        [options = std::move(options)](redis_value&& reply) {
            return detail::parse_redis_set_reply(std::move(reply), options);
        });
}

task<void> redis_session::mset(
    std::span<const std::pair<std::string_view, std::string_view>> items) const {
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::redis_mset_args(items), detail::parse_redis_ok);
}

task<std::optional<std::string>> redis_session::get_del(std::string_view key) const {
    return mapped<std::optional<std::string>>({"GETDEL", key}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_session::append(
    std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"APPEND", key, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::strlen(std::string_view key) const {
    return mapped<std::int64_t>({"STRLEN", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::incr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::make_redis_integer_command("INCRBY", key, value), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::decr(std::string_view key) const {
    return mapped<std::int64_t>({"DECR", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::decr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::make_redis_integer_command("DECRBY", key, value), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::del(std::string_view key) const {
    return mapped<std::int64_t>({"DEL", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::del(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::redis_command_with_keys("DEL", keys), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::unlink(std::string_view key) const {
    return mapped<std::int64_t>({"UNLINK", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::unlink(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::redis_command_with_keys("UNLINK", keys), detail::parse_redis_integer);
}

task<bool> redis_session::exists(std::string_view key) const {
    return mapped<bool>({"EXISTS", key}, detail::parse_redis_integer_bool);
}

task<bool> redis_session::touch(std::string_view key) const {
    return mapped<bool>({"TOUCH", key}, detail::parse_redis_integer_bool);
}

task<std::string> redis_session::type(std::string_view key) const {
    return mapped<std::string>({"TYPE", key}, detail::take_redis_string);
}

task<void> redis_session::rename(std::string_view key, std::string_view new_key) const {
    return mapped<void>({"RENAME", key, new_key}, detail::parse_redis_ok);
}

task<bool> redis_session::rename_nx(std::string_view key, std::string_view new_key) const {
    return mapped<bool>({"RENAMENX", key, new_key}, detail::parse_redis_integer_bool);
}

task<bool> redis_session::expire(std::string_view key, std::chrono::seconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis expire TTL must be greater than zero");
    return execute_mapped<bool>(
        pool(), detail::make_redis_integer_command("EXPIRE", key, ttl.count()), detail::parse_redis_integer_bool);
}

task<bool> redis_session::pexpire(std::string_view key, std::chrono::milliseconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis pexpire TTL must be greater than zero");
    return execute_mapped<bool>(
        pool(), detail::make_redis_integer_command("PEXPIRE", key, ttl.count()), detail::parse_redis_integer_bool);
}

task<bool> redis_session::expire_at(
    std::string_view key, std::chrono::system_clock::time_point expires_at) const {
    registration_.require_active();
    const auto expires_ms =
        std::chrono::ceil<std::chrono::milliseconds>(expires_at.time_since_epoch()).count();
    return execute_mapped<bool>(
        pool(), detail::make_redis_integer_command("PEXPIREAT", key, expires_ms), detail::parse_redis_integer_bool);
}

task<bool> redis_session::persist(std::string_view key) const {
    return mapped<bool>({"PERSIST", key}, detail::parse_redis_integer_bool);
}

task<redis_ttl> redis_session::ttl(std::string_view key) const {
    return mapped<redis_ttl>({"TTL", key},
        [](redis_value&& reply) { return detail::parse_redis_ttl(std::move(reply), true); });
}

task<redis_ttl> redis_session::pttl(std::string_view key) const {
    return mapped<redis_ttl>({"PTTL", key},
        [](redis_value&& reply) { return detail::parse_redis_ttl(std::move(reply), false); });
}

task<std::int64_t> redis_session::incr(std::string_view key) const {
    return mapped<std::int64_t>({"INCR", key}, detail::parse_redis_integer);
}

task<std::optional<std::string>> redis_session::hget(
    std::string_view key, std::string_view field) const {
    return mapped<std::optional<std::string>>({"HGET", key, field}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_session::hset(
    std::string_view key, std::string_view field, std::string_view value) const {
    return mapped<std::int64_t>({"HSET", key, field, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::hset(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::redis_hset_fields_args(key, fields_value), detail::parse_redis_integer);
}

task<std::vector<std::optional<std::string>>> redis_session::hmget(
    std::string_view key, std::span<const std::string_view> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::optional<std::string>>>(
        pool(), detail::redis_command_with_key_fields("HMGET", key, fields_value),
        detail::parse_redis_optional_string_array);
}

task<std::vector<redis_key_value>> redis_session::hget_all(std::string_view key) const {
    return mapped<std::vector<redis_key_value>>({"HGETALL", key},
        [](redis_value&& reply) {
            return detail::parse_redis_key_value_array(std::move(reply), "unexpected redis hgetall reply");
        });
}

task<std::int64_t> redis_session::hdel(
    std::string_view key, std::string_view field) const {
    return mapped<std::int64_t>({"HDEL", key, field}, detail::parse_redis_integer);
}

task<bool> redis_session::hexists(std::string_view key, std::string_view field) const {
    return mapped<bool>({"HEXISTS", key, field}, detail::parse_redis_integer_bool);
}

task<std::int64_t> redis_session::hlen(std::string_view key) const {
    return mapped<std::int64_t>({"HLEN", key}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::hkeys(std::string_view key) const {
    return mapped<std::vector<std::string>>({"HKEYS", key}, detail::parse_redis_string_array);
}

task<std::vector<std::string>> redis_session::hvals(std::string_view key) const {
    return mapped<std::vector<std::string>>({"HVALS", key}, detail::parse_redis_string_array);
}

task<std::int64_t> redis_session::hincr_by(
    std::string_view key, std::string_view field, std::int64_t value) const {
    std::array<char, detail::redis_int_scratch_size> value_scratch{};
    return mapped<std::int64_t>(
        {"HINCRBY", key, field, detail::format_redis_int(value_scratch, value)}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::lpush(
    std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"LPUSH", key, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::rpush(
    std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"RPUSH", key, value}, detail::parse_redis_integer);
}

task<std::optional<std::string>> redis_session::lpop(std::string_view key) const {
    return mapped<std::optional<std::string>>({"LPOP", key}, detail::parse_redis_optional_string);
}

task<std::optional<std::string>> redis_session::rpop(std::string_view key) const {
    return mapped<std::optional<std::string>>({"RPOP", key}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_session::llen(std::string_view key) const {
    return mapped<std::int64_t>({"LLEN", key}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::lrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(
        pool(), detail::make_redis_range_command("LRANGE", key, start, stop), detail::parse_redis_string_array);
}

task<std::optional<std::string>> redis_session::lindex(
    std::string_view key, std::int64_t index) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::string>>(
        pool(), detail::make_redis_integer_command("LINDEX", key, index), detail::parse_redis_optional_string);
}

task<void> redis_session::lset(
    std::string_view key, std::int64_t index, std::string_view value) const {
    std::array<char, detail::redis_int_scratch_size> index_scratch{};
    return mapped<void>({"LSET", key, detail::format_redis_int(index_scratch, index), value}, detail::parse_redis_ok);
}

task<void> redis_session::ltrim(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<void>(
        pool(), detail::make_redis_range_command("LTRIM", key, start, stop), detail::parse_redis_ok);
}

task<std::int64_t> redis_session::lrem(
    std::string_view key, std::int64_t count, std::string_view value) const {
    std::array<char, detail::redis_int_scratch_size> count_scratch{};
    return mapped<std::int64_t>(
        {"LREM", key, detail::format_redis_int(count_scratch, count), value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::sadd(
    std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"SADD", key, member}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::srem(
    std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"SREM", key, member}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::smembers(
    std::string_view key) const {
    return mapped<std::vector<std::string>>({"SMEMBERS", key}, detail::parse_redis_string_array);
}

task<std::int64_t> redis_session::scard(std::string_view key) const {
    return mapped<std::int64_t>({"SCARD", key}, detail::parse_redis_integer);
}

task<bool> redis_session::sismember(std::string_view key, std::string_view member) const {
    return mapped<bool>({"SISMEMBER", key, member}, detail::parse_redis_integer_bool);
}

task<std::optional<std::string>> redis_session::spop(std::string_view key) const {
    return mapped<std::optional<std::string>>({"SPOP", key}, detail::parse_redis_optional_string);
}

task<std::optional<std::string>> redis_session::srand_member(
    std::string_view key) const {
    return mapped<std::optional<std::string>>({"SRANDMEMBER", key}, detail::parse_redis_optional_string);
}

task<std::vector<std::string>> redis_session::sinter(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(
        pool(), detail::redis_command_with_keys("SINTER", keys), detail::parse_redis_string_array);
}

task<std::vector<std::string>> redis_session::sunion(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(
        pool(), detail::redis_command_with_keys("SUNION", keys), detail::parse_redis_string_array);
}

task<std::vector<std::string>> redis_session::sdiff(
    std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(
        pool(), detail::redis_command_with_keys("SDIFF", keys), detail::parse_redis_string_array);
}

task<std::int64_t> redis_session::zadd(
    std::string_view key, double score, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(
        pool(), detail::make_redis_zadd_command(key, score, member), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::zrem(
    std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"ZREM", key, member}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::zrange(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(
        pool(), detail::make_redis_range_command("ZRANGE", key, start, stop), detail::parse_redis_string_array);
}

task<std::vector<redis_scored_value>> redis_session::zrange_with_scores(
    std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto command = detail::make_redis_range_command("ZRANGE", key, start, stop, true);
    return execute_mapped<std::vector<redis_scored_value>>(pool(), std::move(command), detail::parse_redis_scored_array);
}

task<std::optional<double>> redis_session::zscore(
    std::string_view key, std::string_view member) const {
    return mapped<std::optional<double>>({"ZSCORE", key, member},
        [](redis_value&& reply) {
            return detail::parse_redis_optional_double(std::move(reply), "invalid redis zscore reply");
        });
}

task<std::int64_t> redis_session::zcard(std::string_view key) const {
    return mapped<std::int64_t>({"ZCARD", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::zcount(
    std::string_view key, double min_value, double max_value) const {
    registration_.require_active();
    std::array<char, detail::redis_score_scratch_size> min_scratch{};
    std::array<char, detail::redis_score_scratch_size> max_scratch{};
    const std::string_view args[]{
        "ZCOUNT",
        key,
        detail::format_redis_score(min_scratch, min_value),
        detail::format_redis_score(max_scratch, max_value),
    };
    return execute_mapped<std::int64_t>(pool(), detail::make_owned_redis_command(args), detail::parse_redis_integer);
}

task<redis_scan_result> redis_session::scan(redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_scan_result>(
        pool(), make_redis_scan_command("SCAN", std::nullopt, options), detail::parse_redis_scan_result);
}

task<redis_hash_scan_result> redis_session::hscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_hash_scan_result>(
        pool(), make_redis_scan_command("HSCAN", key, options), detail::parse_redis_hash_scan_result);
}

task<redis_scan_result> redis_session::sscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_scan_result>(
        pool(), make_redis_scan_command("SSCAN", key, options), detail::parse_redis_scan_result);
}

task<redis_z_scan_result> redis_session::zscan(
    std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_z_scan_result>(
        pool(), make_redis_scan_command("ZSCAN", key, options), detail::parse_redis_z_scan_result);
}

task<redis_value> redis_session::eval(std::string_view script,
    std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(execute(pool(), detail::redis_eval_args("EVAL", script, keys, args)));
}

task<redis_value> redis_session::eval_sha(std::string_view sha1,
    std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(execute(pool(), detail::redis_eval_args("EVALSHA", sha1, keys, args)));
}

task<std::string> redis_session::script_load(std::string_view script) const {
    return mapped<std::string>({"SCRIPT", "LOAD", script}, detail::take_redis_string);
}

task<std::vector<bool>> redis_session::script_exists(
    std::span<const std::string_view> sha1s) const {
    registration_.require_active();
    return execute_mapped<std::vector<bool>>(
        pool(), detail::redis_command_with_key_fields("SCRIPT", "EXISTS", sha1s), detail::parse_redis_bool_array);
}

task<std::optional<redis_key_value>> redis_session::blpop(
    std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    const auto reply_timeout = redis_block_reply_timeout(operation_options_, wait);
    return execute_mapped<std::optional<redis_key_value>>(blocking_pool(),
        detail::redis_blocking_pop_args("BLPOP", keys, wait), detail::parse_redis_blocking_pop_reply,
        reply_timeout);
}

task<std::optional<redis_key_value>> redis_session::brpop(
    std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    const auto reply_timeout = redis_block_reply_timeout(operation_options_, wait);
    return execute_mapped<std::optional<redis_key_value>>(blocking_pool(),
        detail::redis_blocking_pop_args("BRPOP", keys, wait), detail::parse_redis_blocking_pop_reply,
        reply_timeout);
}

task<std::optional<redis_x_read_group_result>> redis_session::xread_group(
    std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams,
    redis_x_read_group_options options) const {
    registration_.require_active();
    auto args = detail::redis_x_read_group_args(group, consumer, streams, options);
    auto* selected_pool = &pool();
    std::optional<timeout_duration> reply_timeout;
    if (options.block_.has_value()) {
        selected_pool = &blocking_pool();
        reply_timeout = redis_block_reply_timeout(operation_options_, *options.block_);
    }
    return execute_mapped<std::optional<redis_x_read_group_result>>(
        *selected_pool, std::move(args), detail::parse_redis_x_read_group_reply, reply_timeout);
}

redis_pipeline redis_session::pipeline() const {
    registration_.require_active();
    return redis_pipeline(pool(), operation_options_, registration_.scope(), io_metrics_);
}

redis_transaction redis_session::transaction() const {
    registration_.require_active();
    return redis_transaction(pool(), operation_options_, registration_.scope(), io_metrics_);
}

task<std::int64_t> redis_session::publish(std::string_view channel, std::string_view payload) const {
    return mapped<std::int64_t>({"PUBLISH", channel, payload}, detail::parse_redis_integer);
}

redis_subscription redis_session::subscription() const {
    registration_.require_active();
    detail::require_bounded_redis_wait(
        operation_options_, "redis subscription requires a stop_token or finite operation timeout");
    return redis_subscription(runtime_->subscriptions(), registration_.scope(), operation_options_);
}

namespace detail {

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_pool& pool, operation_options options,
    operation_scope& scope, request_io_metrics* io_metrics) noexcept
    : pool_(&pool),
      options_(std::move(options)),
      commands_(),
      io_metrics_(io_metrics),
      registration_(scope, this, &redis_batch::expire_capability) {}

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_batch&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      options_(std::move(other.options_)),
      commands_(std::move(other.commands_)),
      io_metrics_(other.io_metrics_),
      registration_(std::move(other.registration_), this) {}

template <redis_batch_mode mode>
void redis_batch<mode>::require_ready() const {
    registration_.require_active();
    if (pool_ == nullptr) {
        throw std::logic_error("redis command batch has already been consumed or expired");
    }
}

template <redis_batch_mode mode>
redis_command_payload redis_batch<mode>::consume() {
    require_ready();
    auto& pool = *std::exchange(pool_, nullptr);
    return {pool, std::move(options_), std::move(commands_)};
}

template <redis_batch_mode mode>
void redis_batch<mode>::expire() noexcept {
    pool_ = nullptr;
    options_ = {};
    io_metrics_ = nullptr;
    std::vector<redis_owned_command> empty;
    commands_.swap(empty);
}

template <redis_batch_mode mode>
void redis_batch<mode>::expire_capability(void* target) noexcept {
    static_cast<redis_batch*>(target)->expire();
}

template <redis_batch_mode mode>
redis_batch<mode>& redis_batch<mode>::command(std::span<const std::string_view> args) {
    require_ready();
    const bool barrier = validate_redis_pooled_command(args, false, mode == redis_batch_mode::pipeline);
    if (barrier) {
        require_bounded_redis_wait(options_, "redis barrier pipeline requires a stop_token or finite operation timeout");
    }
    return append_owned(make_owned_redis_command(args));
}

template <redis_batch_mode mode>
redis_batch<mode>& redis_batch<mode>::append_owned(redis_owned_command command) {
    commands_.push_back(std::move(command));
    return *this;
}

template <redis_batch_mode mode>
task<std::vector<redis_value>> redis_batch<mode>::execute_owned(
    request_io_metrics* io_metrics, redis_command_payload payload_value) {
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    if constexpr (mode == redis_batch_mode::pipeline) {
        co_return co_await payload_value.pool_.get().execute_pipeline(
            std::move(payload_value.commands_), std::move(payload_value.options_));
    } else {
        auto replies = co_await payload_value.pool_.get().execute_pipeline(
            std::move(payload_value.commands_), std::move(payload_value.options_), true);
        if (replies.empty()) {
            throw redis_error(redis_error::code_type::protocol_error, "redis transaction returned no replies");
        }
        for (std::size_t i = 0; i < replies.size(); ++i) {
            throw_if_redis_transaction_reply_error(replies[i], i);
        }
        auto exec_reply = std::move(replies.back());
        if (exec_reply.kind() != redis_value::kind_type::array) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis transaction reply");
        }

        // Every reply node lives in `resource`, so the EXEC elements move without copying.
        co_return std::move(redis_types_access::elements(exec_reply));
    }
}

template <redis_batch_mode mode>
task<std::vector<redis_value>> redis_batch<mode>::exec() && {
    auto& scope = registration_.scope();
    auto payload_value = consume();
    return make_scoped_operation(scope, execute_owned(io_metrics_, std::move(payload_value)));
}

template class redis_batch<redis_batch_mode::pipeline>;
template class redis_batch<redis_batch_mode::transaction>;

}  // namespace detail

redis_session redis_session::bind_caller_scope(::ruvia::operation_scope& scope,
    detail::request_io_metrics* io_metrics, const stop_token& stop_token_value) && {
    registration_.require_active();
    redis_session bound(*runtime_, scope, io_metrics);
    bound.operation_options_ = override_operation_options(
        operation_options_, operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_token_value});
    return bound;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// ARGV: mode, ttl policy, milliseconds, required count, required names,
// followed by mutation triples (set/delete, field, value). All validation
// precedes writes: Redis scripts are atomic but do not roll back runtime errors.
constexpr std::string_view write_script = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
local exists = kind ~= 'none'
if exists and kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if exists and redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
local required = tonumber(ARGV[4])
local start = 5 + required
local changes = {}
for i = start, #ARGV, 3 do changes[ARGV[i + 1]] = ARGV[i] end
for i = 5, 4 + required do
    local field = ARGV[i]
    if changes[field] == 'd' or
       (changes[field] ~= 's' and (ARGV[1] == 'insert' or (ARGV[1] ~= 'update' and not exists) or (exists and redis.call('HEXISTS', KEYS[1], field) == 0))) then
        return redis.error_reply('required entity field is missing: ' .. field)
    end
end
if ARGV[1] == 'insert' and exists then return 0 end
if ARGV[1] == 'update' and not exists then return 0 end
for i = start, #ARGV, 3 do
    if ARGV[i] == 's' then redis.call('HSET', KEYS[1], ARGV[i + 1], ARGV[i + 2])
    else redis.call('HDEL', KEYS[1], ARGV[i + 1]) end
end
redis.call('HSET', KEYS[1], '__ruvia_entity', '1')
if ARGV[2] == 'expire' then redis.call('PEXPIRE', KEYS[1], ARGV[3])
elseif ARGV[2] == 'persist' then redis.call('PERSIST', KEYS[1]) end
if exists then return 2 else return 1 end
)lua";

constexpr std::string_view delete_script = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then return 0 end
if kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
return redis.call('DEL', KEYS[1])
)lua";

// Owns the digest text the script view borrows; built once per script.
class redis_orm_script_digest final {
public:
    explicit redis_orm_script_digest(std::string_view body) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        std::size_t digest_size = 0;
        if (EVP_Q_digest(nullptr, "SHA1", nullptr, body.data(), body.size(), digest.data(), &digest_size) != 1 ||
            digest_size * 2 != sha1_.size()) {
            throw std::runtime_error("redis entity script digest could not be computed");
        }
        http_encode_lower_hex(sha1_.data(), std::span<const unsigned char>(digest.data(), digest_size));
        script_ = {body, std::string_view(sha1_.data(), sha1_.size())};
    }

    [[nodiscard]] const redis_orm_script& script() const noexcept {
        return script_;
    }

private:
    std::array<char, 40> sha1_{};
    redis_orm_script script_;
};

void append_redis_orm_script_call(redis_orm_arguments_type& args, const redis_orm_script& script) {
    args.emplace_back("EVALSHA");
    args.emplace_back(script.sha1_);
    args.emplace_back("1");
}

}  // namespace

const redis_orm_script& redis_orm_write_script() {
    static const redis_orm_script_digest digest(write_script);
    return digest.script();
}

const redis_orm_script& redis_orm_delete_script() {
    static const redis_orm_script_digest digest(delete_script);
    return digest.script();
}

redis_orm_arguments_type redis_orm_write_arguments(std::string_view storage_prefix, std::string_view id,
    std::string_view mode, redis_write_options options, std::size_t extra_arguments) {
    if (options.ttl_ && (options.persist_ || options.ttl_->count() <= 0 || options.ttl_->count() > 9007199254740991LL)) {
        throw std::invalid_argument("entity TTL must be positive, at most 2^53-1 milliseconds, and cannot combine with persist");
    }
    redis_orm_arguments_type args;
    args.reserve(7 + extra_arguments);
    append_redis_orm_script_call(args, redis_orm_write_script());
    args.push_back(redis_entity_key(storage_prefix, id));
    args.emplace_back(mode);
    args.emplace_back(options.ttl_ ? "expire" : options.persist_ ? "persist"
                                                                 : "keep");
    if (options.ttl_) {
        http_append_signed(args.emplace_back(), static_cast<std::int64_t>(options.ttl_->count()));
    } else {
        args.emplace_back("0");
    }
    return args;
}

redis_orm_arguments_type redis_orm_delete_arguments(
    std::string_view storage_prefix, std::string_view id) {
    redis_orm_arguments_type args;
    args.reserve(4);
    append_redis_orm_script_call(args, redis_orm_delete_script());
    args.push_back(redis_entity_key(storage_prefix, id));
    return args;
}

task<redis_value> execute_redis_orm_script(redis_pool& pool, const redis_orm_script& script,
    redis_orm_arguments_type args, request_io_metrics* io_metrics, operation_options options) {
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline(options.timeout_);
    {
        auto reply = co_await pool.execute_borrowed(redis_command_args_view{args}, options, deadline);
        if (reply.kind() != redis_value::kind_type::error || !reply.error().starts_with("NOSCRIPT")) {
            co_return reply;
        }
        // The error is consumed by this decision; release its storage before
        // the fallback waits for its connection and response.
    }
    // The server's script cache was flushed or never loaded this script:
    // EVAL runs the body and caches it under the same digest for later calls.
    args[0].assign("EVAL");
    args[1].assign(script.body_);
    co_return co_await pool.execute_owned(std::move(args), std::move(options), std::nullopt, nullptr, deadline);
}

redis_write_result redis_orm_exec_result(redis_value&& reply) {
    const auto value = redis_value_integer(reply);
    switch (value) {
        case 0:
            return redis_write_result(0);
        case 1:
        case 2:
            return redis_write_result(1);
        default:
            throw redis_error(redis_error::code_type::protocol_error,
                "invalid entity write affected-row reply");
    }
}

redis_write_result redis_orm_delete_result(redis_value&& reply) {
    const auto value = redis_value_integer(reply);
    if (value == 0 || value == 1) {
        return redis_write_result(static_cast<std::uint64_t>(value));
    }
    throw redis_error(redis_error::code_type::protocol_error,
        "invalid entity delete affected-row reply");
}

void redis_orm_status_result(redis_value&& reply) {
    if (redis_value_string(reply) != "OK") {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity index status reply");
    }
}

std::uint64_t redis_orm_count(const redis_value& reply) {
    const auto count = redis_value_integer(reply);
    if (count < 0) {
        throw redis_error(redis_error::code_type::protocol_error, "negative entity result count");
    }
    return static_cast<std::uint64_t>(count);
}

std::uint64_t redis_orm_count_result(redis_value&& reply) {
    const auto values = redis_value_array(reply);
    if (values.size() != 1) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity count reply");
    }
    return redis_orm_count(values.front());
}

bool redis_orm_exists_result(redis_value&& reply) {
    return redis_orm_count_result(std::move(reply)) != 0;
}

}  // namespace ruvia::detail
