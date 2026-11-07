#include "redis/redis_command_routing.h"

#include <cstddef>
#include <stdexcept>

#include "ruvia/http/http_ascii.h"

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
