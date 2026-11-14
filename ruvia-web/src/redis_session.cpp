#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "http_client_pool.h"
#include "http_header.h"
#include "redis.h"
#include "redis_codec.h"
#include "redis_pool.h"
#include "stop_token.h"

namespace ruvia::detail {
// Classifies a raw pooled command before it is encoded. Connection-stateful
// commands are rejected; WAIT/WAITAOF are accepted only where a barrier is
// allowed. Returns whether the command needs the blocking pool; a blocking
// command throws when `allow_blocking` is false. An XREAD/XREADGROUP option
// this classifier does not know throws instead of being guessed non-blocking.
[[nodiscard]] bool validate_redis_pooled_command(std::span<const std::string_view> args, bool allow_blocking, bool allow_barrier = false);

// A wait the server may hold open indefinitely (a blocking command, a barrier,
// a subscription) must be bounded by its owner: a stop token or a finite
// operation timeout. Throws std::invalid_argument with `message` otherwise.
void require_bounded_redis_wait(const operation_options& options, const char* message);

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
[[nodiscard]] bool stream_read_options_block(std::span<const std::string_view> args, std::size_t first_option, redis_stream_read_kind kind) {
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
            throw std::invalid_argument("redis WAIT and WAITAOF require a non-transaction pipeline containing the writes");
        }
        return true;
    }
    const auto reject_stateful = [] {
        throw std::invalid_argument("stateful redis commands are not allowed through pooled command()");
    };
    bool blocking = false;
    switch (command.size()) {
        case 4:
            if (is_redis_token(command, "AUTH") || is_redis_token(command, "EXEC") || is_redis_token(command, "QUIT") || is_redis_token(command, "SYNC")) {
                reject_stateful();
            }
            break;
        case 5:
            if (is_redis_token(command, "HELLO") || is_redis_token(command, "MULTI") || is_redis_token(command, "PSYNC") || is_redis_token(command, "RESET") || is_redis_token(command, "WATCH")) {
                reject_stateful();
            }
            if (is_redis_token(command, "BLPOP") || is_redis_token(command, "BRPOP")) {
                blocking = true;
            } else if (is_redis_token(command, "XREAD")) {
                blocking = stream_read_options_block(args, 1, redis_stream_read_kind::read);
            }
            break;
        case 6:
            if (is_redis_token(command, "ASKING") || is_redis_token(command, "CLIENT") || is_redis_token(command, "SELECT")) {
                reject_stateful();
            }
            blocking = is_redis_token(command, "BLMOVE") || is_redis_token(command, "BLMPOP") || is_redis_token(command, "BZMPOP");
            break;
        case 7:
            if (is_redis_token(command, "DISCARD") || is_redis_token(command, "MONITOR") || is_redis_token(command, "UNWATCH")) {
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
        throw std::invalid_argument("blocking redis command is not allowed in a pipeline or transaction");
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

[[nodiscard]] detail::redis_owned_command make_redis_scan_command(std::string_view command, std::optional<std::string_view> key, const redis_scan_options& options) {
    std::array<char, http_max_unsigned_decimal_size> cursor_scratch{};
    std::array<char, http_max_unsigned_decimal_size> count_scratch{};
    std::array<std::string_view, 7> args{};
    std::size_t count = 0;
    args[count++] = command;
    if (key.has_value()) {
        args[count++] = *key;
    }
    args[count++] = http_format_unsigned(cursor_scratch, options.cursor_.has_value() ? detail::redis_types_access::cursor_value(*options.cursor_) : 0);
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
[[nodiscard]] std::optional<timeout_duration> redis_block_reply_timeout(const operation_options& options, redis_block_wait wait) {
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

redis_session::redis_session(detail::redis_client_runtime& runtime, ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics) noexcept :
    runtime_(&runtime),
    io_metrics_(io_metrics),
    registration_(operation_scope, this, &redis_session::expire_capability) {}

redis_session::redis_session(detail::redis_client_runtime& runtime, ::ruvia::operation_scope& operation_scope, operation_options options) noexcept :
    runtime_(&runtime),
    operation_options_(std::move(options)),
    registration_(operation_scope, this, &redis_session::expire_capability) {}

redis_session::redis_session(const redis_session& other) noexcept :
    runtime_(other.runtime_),
    io_metrics_(other.io_metrics_),
    operation_options_(other.operation_options_),
    registration_(other.registration_, this) {}

redis_session redis_session::with_options(operation_options options) const {
    registration_.require_active();
    redis_session configured(*this);
    configured.operation_options_ = override_operation_options(operation_options_, std::move(options));
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

task<redis_value> redis_session::execute(detail::redis_pool& pool_value, detail::redis_owned_command&& command, std::optional<timeout_duration> reply_timeout) const {
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
        detail::require_bounded_redis_wait(operation_options_, "raw blocking redis command requires a stop_token or finite operation timeout");
    }
    return scoped(execute(blocking ? blocking_pool() : pool(), detail::make_owned_redis_command(args)));
}

task<redis_value> redis_session::command_owned(std::vector<std::string>&& args) const {
    registration_.require_active();
    return pool().execute_owned(std::move(args), operation_options_, std::nullopt, io_metrics_);
}

task<redis_value> redis_session::script_owned(const detail::redis_orm_script& script, std::vector<std::string>&& args) const {
    registration_.require_active();
    return detail::execute_redis_orm_script(pool(), script, std::move(args), io_metrics_, operation_options_);
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

task<std::vector<std::optional<std::string>>> redis_session::mget(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::optional<std::string>>>(pool(), detail::redis_command_with_keys("MGET", keys), detail::parse_redis_optional_string_array);
}

task<redis_set_result> redis_session::set(std::string_view key, std::string_view value, redis_set_options options) const {
    registration_.require_active();
    auto command = detail::redis_set_args(key, value, options);
    return execute_mapped<redis_set_result>(pool(), std::move(command), [options = std::move(options)](redis_value&& reply) {
        return detail::parse_redis_set_reply(std::move(reply), options);
    });
}

task<void> redis_session::mset(std::span<const std::pair<std::string_view, std::string_view>> items) const {
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::redis_mset_args(items), detail::parse_redis_ok);
}

task<std::optional<std::string>> redis_session::get_del(std::string_view key) const {
    return mapped<std::optional<std::string>>({"GETDEL", key}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_session::append(std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"APPEND", key, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::strlen(std::string_view key) const {
    return mapped<std::int64_t>({"STRLEN", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::incr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_integer_command("INCRBY", key, value), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::decr(std::string_view key) const {
    return mapped<std::int64_t>({"DECR", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::decr_by(std::string_view key, std::int64_t value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_integer_command("DECRBY", key, value), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::del(std::string_view key) const {
    return mapped<std::int64_t>({"DEL", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::del(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_command_with_keys("DEL", keys), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::unlink(std::string_view key) const {
    return mapped<std::int64_t>({"UNLINK", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::unlink(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_command_with_keys("UNLINK", keys), detail::parse_redis_integer);
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
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("EXPIRE", key, ttl.count()), detail::parse_redis_integer_bool);
}

task<bool> redis_session::pexpire(std::string_view key, std::chrono::milliseconds ttl) const {
    registration_.require_active();
    require_positive_redis_ttl(ttl, "redis pexpire TTL must be greater than zero");
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("PEXPIRE", key, ttl.count()), detail::parse_redis_integer_bool);
}

task<bool> redis_session::expire_at(std::string_view key, std::chrono::system_clock::time_point expires_at) const {
    registration_.require_active();
    const auto expires_ms = std::chrono::ceil<std::chrono::milliseconds>(expires_at.time_since_epoch()).count();
    return execute_mapped<bool>(pool(), detail::make_redis_integer_command("PEXPIREAT", key, expires_ms), detail::parse_redis_integer_bool);
}

task<bool> redis_session::persist(std::string_view key) const {
    return mapped<bool>({"PERSIST", key}, detail::parse_redis_integer_bool);
}

task<redis_ttl> redis_session::ttl(std::string_view key) const {
    return mapped<redis_ttl>({"TTL", key}, [](redis_value&& reply) {
        return detail::parse_redis_ttl(std::move(reply), true);
    });
}

task<redis_ttl> redis_session::pttl(std::string_view key) const {
    return mapped<redis_ttl>({"PTTL", key}, [](redis_value&& reply) {
        return detail::parse_redis_ttl(std::move(reply), false);
    });
}

task<std::int64_t> redis_session::incr(std::string_view key) const {
    return mapped<std::int64_t>({"INCR", key}, detail::parse_redis_integer);
}

task<std::optional<std::string>> redis_session::hget(std::string_view key, std::string_view field) const {
    return mapped<std::optional<std::string>>({"HGET", key, field}, detail::parse_redis_optional_string);
}

task<std::int64_t> redis_session::hset(std::string_view key, std::string_view field, std::string_view value) const {
    return mapped<std::int64_t>({"HSET", key, field, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::redis_hset_fields_args(key, fields_value), detail::parse_redis_integer);
}

task<std::vector<std::optional<std::string>>> redis_session::hmget(std::string_view key, std::span<const std::string_view> fields_value) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::optional<std::string>>>(pool(), detail::redis_command_with_key_fields("HMGET", key, fields_value), detail::parse_redis_optional_string_array);
}

task<std::vector<redis_key_value>> redis_session::hget_all(std::string_view key) const {
    return mapped<std::vector<redis_key_value>>({"HGETALL", key}, [](redis_value&& reply) {
        return detail::parse_redis_key_value_array(std::move(reply), "unexpected redis hgetall reply");
    });
}

task<std::int64_t> redis_session::hdel(std::string_view key, std::string_view field) const {
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

task<std::int64_t> redis_session::hincr_by(std::string_view key, std::string_view field, std::int64_t value) const {
    std::array<char, http_max_signed_decimal_size> value_scratch{};
    return mapped<std::int64_t>({"HINCRBY", key, field, http_format_signed(value_scratch, value)}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::lpush(std::string_view key, std::string_view value) const {
    return mapped<std::int64_t>({"LPUSH", key, value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::rpush(std::string_view key, std::string_view value) const {
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

task<std::vector<std::string>> redis_session::lrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::make_redis_range_command("LRANGE", key, start, stop), detail::parse_redis_string_array);
}

task<std::optional<std::string>> redis_session::lindex(std::string_view key, std::int64_t index) const {
    registration_.require_active();
    return execute_mapped<std::optional<std::string>>(pool(), detail::make_redis_integer_command("LINDEX", key, index), detail::parse_redis_optional_string);
}

task<void> redis_session::lset(std::string_view key, std::int64_t index, std::string_view value) const {
    std::array<char, http_max_signed_decimal_size> index_scratch{};
    return mapped<void>({"LSET", key, http_format_signed(index_scratch, index), value}, detail::parse_redis_ok);
}

task<void> redis_session::ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<void>(pool(), detail::make_redis_range_command("LTRIM", key, start, stop), detail::parse_redis_ok);
}

task<std::int64_t> redis_session::lrem(std::string_view key, std::int64_t count, std::string_view value) const {
    std::array<char, http_max_signed_decimal_size> count_scratch{};
    return mapped<std::int64_t>({"LREM", key, http_format_signed(count_scratch, count), value}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::sadd(std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"SADD", key, member}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::srem(std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"SREM", key, member}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::smembers(std::string_view key) const {
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

task<std::optional<std::string>> redis_session::srand_member(std::string_view key) const {
    return mapped<std::optional<std::string>>({"SRANDMEMBER", key}, detail::parse_redis_optional_string);
}

task<std::vector<std::string>> redis_session::sinter(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::redis_command_with_keys("SINTER", keys), detail::parse_redis_string_array);
}

task<std::vector<std::string>> redis_session::sunion(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::redis_command_with_keys("SUNION", keys), detail::parse_redis_string_array);
}

task<std::vector<std::string>> redis_session::sdiff(std::span<const std::string_view> keys) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::redis_command_with_keys("SDIFF", keys), detail::parse_redis_string_array);
}

task<std::int64_t> redis_session::zadd(std::string_view key, double score, std::string_view member) const {
    registration_.require_active();
    return execute_mapped<std::int64_t>(pool(), detail::make_redis_zadd_command(key, score, member), detail::parse_redis_integer);
}

task<std::int64_t> redis_session::zrem(std::string_view key, std::string_view member) const {
    return mapped<std::int64_t>({"ZREM", key, member}, detail::parse_redis_integer);
}

task<std::vector<std::string>> redis_session::zrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    return execute_mapped<std::vector<std::string>>(pool(), detail::make_redis_range_command("ZRANGE", key, start, stop), detail::parse_redis_string_array);
}

task<std::vector<redis_scored_value>> redis_session::zrange_with_scores(std::string_view key, std::int64_t start, std::int64_t stop) const {
    registration_.require_active();
    auto command = detail::make_redis_range_command("ZRANGE", key, start, stop, true);
    return execute_mapped<std::vector<redis_scored_value>>(pool(), std::move(command), detail::parse_redis_scored_array);
}

task<std::optional<double>> redis_session::zscore(std::string_view key, std::string_view member) const {
    return mapped<std::optional<double>>({"ZSCORE", key, member}, [](redis_value&& reply) {
        return detail::parse_redis_optional_double(std::move(reply), "invalid redis zscore reply");
    });
}

task<std::int64_t> redis_session::zcard(std::string_view key) const {
    return mapped<std::int64_t>({"ZCARD", key}, detail::parse_redis_integer);
}

task<std::int64_t> redis_session::zcount(std::string_view key, double min_value, double max_value) const {
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
    return execute_mapped<redis_scan_result>(pool(), make_redis_scan_command("SCAN", std::nullopt, options), detail::parse_redis_scan_result);
}

task<redis_hash_scan_result> redis_session::hscan(std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_hash_scan_result>(pool(), make_redis_scan_command("HSCAN", key, options), detail::parse_redis_hash_scan_result);
}

task<redis_scan_result> redis_session::sscan(std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_scan_result>(pool(), make_redis_scan_command("SSCAN", key, options), detail::parse_redis_scan_result);
}

task<redis_z_scan_result> redis_session::zscan(std::string_view key, redis_scan_options options) const {
    registration_.require_active();
    return execute_mapped<redis_z_scan_result>(pool(), make_redis_scan_command("ZSCAN", key, options), detail::parse_redis_z_scan_result);
}

task<redis_value> redis_session::eval(std::string_view script, std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(execute(pool(), detail::redis_eval_args("EVAL", script, keys, args)));
}

task<redis_value> redis_session::eval_sha(std::string_view sha1, std::span<const std::string_view> keys, std::span<const std::string_view> args) const {
    registration_.require_active();
    return scoped(execute(pool(), detail::redis_eval_args("EVALSHA", sha1, keys, args)));
}

task<std::string> redis_session::script_load(std::string_view script) const {
    return mapped<std::string>({"SCRIPT", "LOAD", script}, detail::take_redis_string);
}

task<std::vector<bool>> redis_session::script_exists(std::span<const std::string_view> sha1s) const {
    registration_.require_active();
    return execute_mapped<std::vector<bool>>(pool(), detail::redis_command_with_key_fields("SCRIPT", "EXISTS", sha1s), detail::parse_redis_bool_array);
}

task<std::optional<redis_key_value>> redis_session::blpop(std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    const auto reply_timeout = redis_block_reply_timeout(operation_options_, wait);
    return execute_mapped<std::optional<redis_key_value>>(blocking_pool(), detail::redis_blocking_pop_args("BLPOP", keys, wait), detail::parse_redis_blocking_pop_reply, reply_timeout);
}

task<std::optional<redis_key_value>> redis_session::brpop(std::span<const std::string_view> keys, redis_block_wait wait) const {
    registration_.require_active();
    const auto reply_timeout = redis_block_reply_timeout(operation_options_, wait);
    return execute_mapped<std::optional<redis_key_value>>(blocking_pool(), detail::redis_blocking_pop_args("BRPOP", keys, wait), detail::parse_redis_blocking_pop_reply, reply_timeout);
}

task<std::optional<redis_x_read_group_result>> redis_session::xread_group(std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams, redis_x_read_group_options options) const {
    registration_.require_active();
    auto args = detail::redis_x_read_group_args(group, consumer, streams, options);
    auto* selected_pool = &pool();
    std::optional<timeout_duration> reply_timeout;
    if (options.block_.has_value()) {
        selected_pool = &blocking_pool();
        reply_timeout = redis_block_reply_timeout(operation_options_, *options.block_);
    }
    return execute_mapped<std::optional<redis_x_read_group_result>>(*selected_pool, std::move(args), detail::parse_redis_x_read_group_reply, reply_timeout);
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
    detail::require_bounded_redis_wait(operation_options_, "redis subscription requires a stop_token or finite operation timeout");
    return redis_subscription(runtime_->subscriptions(), registration_.scope(), operation_options_);
}

namespace detail {

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_pool& pool, operation_options options, operation_scope& scope, request_io_metrics* io_metrics) noexcept :
    pool_(&pool),
    options_(std::move(options)),
    commands_(),
    io_metrics_(io_metrics),
    registration_(scope, this, &redis_batch::expire_capability) {}

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_batch&& other) noexcept :
    pool_(std::exchange(other.pool_, nullptr)),
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
task<std::vector<redis_value>> redis_batch<mode>::execute_owned(request_io_metrics* io_metrics, redis_command_payload payload_value) {
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    if constexpr (mode == redis_batch_mode::pipeline) {
        co_return co_await payload_value.pool_.get().execute_pipeline(std::move(payload_value.commands_), std::move(payload_value.options_));
    } else {
        auto replies = co_await payload_value.pool_.get().execute_pipeline(std::move(payload_value.commands_), std::move(payload_value.options_), true);
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

redis_session redis_session::bind_caller_scope(::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics, const stop_token& stop_token_value) && {
    registration_.require_active();
    redis_session bound(*runtime_, scope, io_metrics);
    bound.operation_options_ = override_operation_options(operation_options_, operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_token_value});
    return bound;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] bool redis_x_read_group_uses_no_ack(redis_x_read_group_acknowledgement_policy policy) {
    switch (policy) {
        case redis_x_read_group_acknowledgement_policy::track_pending:
            return false;
        case redis_x_read_group_acknowledgement_policy::no_ack:
            return true;
        default:
            throw std::invalid_argument("redis xreadgroup acknowledgement policy is invalid");
    }
}

[[nodiscard]] redis_owned_command encode_redis_pair_command(std::string_view command, std::optional<std::string_view> key, std::span<const std::pair<std::string_view, std::string_view>> pairs, const char* empty_message) {
    if (pairs.empty()) {
        throw std::invalid_argument(empty_message);
    }
    return encode_redis_command([&](const auto& emit) {
        emit(command);
        if (key.has_value()) {
            emit(*key);
        }
        for (const auto& [first, second] : pairs) {
            emit(first);
            emit(second);
        }
    });
}
}  // namespace

redis_owned_command make_owned_redis_command(std::span<const std::string_view> args) {
    redis_owned_command command{std::string()};
    append_resp_command(command.wire_, args);
    return command;
}

redis_owned_command make_owned_redis_command(std::string_view first, std::span<const std::string_view> rest) {
    redis_owned_command command{std::string()};
    append_resp_command(command.wire_, first, rest);
    return command;
}

redis_owned_command make_owned_redis_command(std::initializer_list<std::string_view> args) {
    return make_owned_redis_command(std::span<const std::string_view>(args.begin(), args.size()));
}

redis_owned_command make_redis_integer_command(std::string_view command, std::string_view key, std::int64_t value) {
    std::array<char, http_max_signed_decimal_size> value_scratch{};
    return make_owned_redis_command({command, key, http_format_signed(value_scratch, value)});
}

redis_owned_command make_redis_range_command(std::string_view command, std::string_view key, std::int64_t start, std::int64_t stop, bool with_scores) {
    std::array<char, http_max_signed_decimal_size> start_scratch{};
    std::array<char, http_max_signed_decimal_size> stop_scratch{};
    const std::string_view args[]{
        command,
        key,
        http_format_signed(start_scratch, start),
        http_format_signed(stop_scratch, stop),
        "WITHSCORES",
    };
    return make_owned_redis_command(std::span<const std::string_view>(args, with_scores ? 5U : 4U));
}

redis_owned_command make_redis_zadd_command(std::string_view key, double score, std::string_view member) {
    std::array<char, redis_score_scratch_size> score_scratch{};
    return make_owned_redis_command({"ZADD", key, format_redis_score(score_scratch, score), member});
}

bool redis_set_returns_previous(redis_set_previous_value_policy policy) {
    switch (policy) {
        case redis_set_previous_value_policy::discard:
            return false;
        case redis_set_previous_value_policy::return_value:
            return true;
        default:
            throw std::invalid_argument("redis set previous value policy is invalid");
    }
}

redis_owned_command redis_command_with_keys(std::string_view command, std::span<const std::string_view> keys) {
    if (keys.empty()) {
        throw std::invalid_argument("redis command requires at least one key");
    }
    return make_owned_redis_command(command, keys);
}

redis_owned_command redis_mset_args(std::span<const std::pair<std::string_view, std::string_view>> items) {
    return encode_redis_pair_command("MSET", std::nullopt, items, "redis mset requires at least one item");
}

redis_owned_command redis_set_args(std::string_view key, std::string_view value, const redis_set_options& options) {
    std::string_view condition_token;
    if (options.condition_) {
        switch (*options.condition_) {
            case redis_set_condition::if_absent:
                condition_token = "NX";
                break;
            case redis_set_condition::if_present:
                condition_token = "XX";
                break;
            default:
                throw std::invalid_argument("redis set condition is invalid");
        }
    }
    const bool returns_previous = redis_set_returns_previous(options.previous_value_);
    const auto* duration = options.expiration_ ? options.expiration_->duration() : nullptr;
    const bool keeps_existing = options.expiration_ && options.expiration_->keeps_existing();
    std::array<char, http_max_signed_decimal_size> duration_scratch{};
    const std::string_view duration_text = duration != nullptr ? http_format_signed(duration_scratch, static_cast<std::int64_t>(duration->count())) : std::string_view();
    return encode_redis_command([&](const auto& emit) {
        emit("SET");
        emit(key);
        emit(value);
        if (duration != nullptr) {
            emit("PX");
            emit(duration_text);
        }
        if (!condition_token.empty()) {
            emit(condition_token);
        }
        if (returns_previous) {
            emit("GET");
        }
        if (keeps_existing) {
            emit("KEEPTTL");
        }
    });
}

redis_owned_command redis_hset_fields_args(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields_value) {
    return encode_redis_pair_command("HSET", key, fields_value, "redis hset requires at least one field");
}

redis_owned_command redis_command_with_key_fields(std::string_view command, std::string_view key, std::span<const std::string_view> fields_value) {
    if (fields_value.empty()) {
        throw std::invalid_argument("redis command requires at least one field");
    }
    return encode_redis_command([&](const auto& emit) {
        emit(command);
        emit(key);
        for (const auto field : fields_value) {
            emit(field);
        }
    });
}

redis_owned_command redis_eval_args(std::string_view command, std::string_view script, std::span<const std::string_view> keys, std::span<const std::string_view> argv) {
    std::array<char, http_max_unsigned_decimal_size> key_count_scratch{};
    const auto key_count = http_format_unsigned(key_count_scratch, keys.size());
    return encode_redis_command([&](const auto& emit) {
        emit(command);
        emit(script);
        emit(key_count);
        for (const auto key : keys) {
            emit(key);
        }
        for (const auto arg : argv) {
            emit(arg);
        }
    });
}

redis_owned_command redis_blocking_pop_args(std::string_view command, std::span<const std::string_view> keys, redis_block_wait wait) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    // Seconds with millisecond precision: digits, '.', three fraction digits.
    std::array<char, http_max_signed_decimal_size + 4> timeout_scratch{};
    std::string_view timeout_text = "0";
    if (const auto duration = wait.duration(); duration.has_value()) {
        const auto milliseconds = duration->count();
        const auto seconds_text = http_format_signed(std::span<char, http_max_signed_decimal_size>(timeout_scratch.data(), http_max_signed_decimal_size), milliseconds / 1000);
        auto len = seconds_text.size();
        const auto remainder = milliseconds % 1000;
        if (remainder != 0) {
            timeout_scratch[len++] = '.';
            timeout_scratch[len++] = static_cast<char>('0' + remainder / 100);
            timeout_scratch[len++] = static_cast<char>('0' + (remainder / 10) % 10);
            timeout_scratch[len++] = static_cast<char>('0' + remainder % 10);
        }
        timeout_text = std::string_view(timeout_scratch.data(), len);
    }
    return encode_redis_command([&](const auto& emit) {
        emit(command);
        for (const auto key : keys) {
            emit(key);
        }
        emit(timeout_text);
    });
}

redis_owned_command redis_x_read_group_args(std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams, const redis_x_read_group_options& options) {
    if (group.empty() || consumer.empty()) {
        throw std::invalid_argument("redis xreadgroup requires a group and consumer");
    }
    if (streams.empty()) {
        throw std::invalid_argument("redis xreadgroup requires at least one stream");
    }
    for (const auto& stream : streams) {
        if (stream.stream_.empty() || stream.id_.empty()) {
            throw std::invalid_argument("redis xreadgroup stream and id must not be empty");
        }
    }
    const auto use_no_ack = redis_x_read_group_uses_no_ack(options.acknowledgement_);
    std::array<char, http_max_unsigned_decimal_size> count_scratch{};
    std::string_view count_text;
    if (options.count_.has_value()) {
        count_text = http_format_unsigned(count_scratch, options.count_->value());
    }
    std::array<char, http_max_signed_decimal_size> block_scratch{};
    std::string_view block_text = "0";
    if (options.block_.has_value()) {
        if (const auto duration = options.block_->duration(); duration.has_value()) {
            block_text = http_format_signed(block_scratch, static_cast<std::int64_t>(duration->count()));
        }
    }
    return encode_redis_command([&](const auto& emit) {
        emit("XREADGROUP");
        emit("GROUP");
        emit(group);
        emit(consumer);
        if (options.count_.has_value()) {
            emit("COUNT");
            emit(count_text);
        }
        if (options.block_.has_value()) {
            emit("BLOCK");
            emit(block_text);
        }
        if (use_no_ack) {
            emit("NOACK");
        }
        emit("STREAMS");
        for (const auto& stream : streams) {
            emit(stream.stream_.view());
        }
        for (const auto& stream : streams) {
            emit(stream.id_.view());
        }
    });
}

}  // namespace ruvia::detail
