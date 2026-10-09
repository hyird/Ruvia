#include "redis/redis_handle_helpers.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/web/detail/redis/redis_utils.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_redis_token(std::string_view value, std::string_view token) {
    return http_ascii_equals_ignore_case(value, token);
}

[[nodiscard]] bool xread_options_contain_blocking(
    std::span<const std::string_view> args, std::size_t first_option, bool allow_no_ack) {
    for (std::size_t i = first_option; i < args.size();) {
        const auto token = args[i];
        if (is_redis_token(token, "STREAMS")) {
            return false;
        }
        if (is_redis_token(token, "BLOCK")) {
            return true;
        }
        if (is_redis_token(token, "COUNT")) {
            i += 2;
            continue;
        }
        if (allow_no_ack && is_redis_token(token, "NOACK")) {
            ++i;
            continue;
        }
        return false;
    }
    return false;
}

[[nodiscard]] bool xread_command_is_blocking(std::span<const std::string_view> args) {
    return xread_options_contain_blocking(args, 1, false);
}

[[nodiscard]] bool xread_group_command_is_blocking(std::span<const std::string_view> args) {
    if (args.size() < 4 || !is_redis_token(args[1], "GROUP")) {
        return false;
    }
    return xread_options_contain_blocking(args, 4, true);
}

}  // namespace

std::string_view redis_value_string(const redis_value& value) {
    if (value.kind() == redis_value::kind_type::error) {
        throw redis_error(redis_error::code_type::command_error, value.error());
    }
    if (value.kind() != redis_value::kind_type::string) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not a string");
    }
    return value.string();
}

std::int64_t redis_value_integer(const redis_value& value) {
    if (value.kind() == redis_value::kind_type::error) {
        throw redis_error(redis_error::code_type::command_error, value.error());
    }
    if (value.kind() != redis_value::kind_type::integer) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not an integer");
    }
    return value.integer();
}

bool redis_value_integer_bool(const redis_value& value) {
    const auto integer = redis_value_integer(value);
    if (integer == 0) {
        return false;
    }
    if (integer == 1) {
        return true;
    }
    throw redis_error(redis_error::code_type::protocol_error, "redis boolean reply is not 0 or 1");
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

bool redis_x_read_group_uses_no_ack(redis_x_read_group_acknowledgement_policy policy) {
    switch (policy) {
        case redis_x_read_group_acknowledgement_policy::track_pending:
            return false;
        case redis_x_read_group_acknowledgement_policy::no_ack:
            return true;
        default:
            throw std::invalid_argument("redis xreadgroup acknowledgement policy is invalid");
    }
}

std::span<const redis_value> redis_value_array(const redis_value& value) {
    if (value.kind() == redis_value::kind_type::error) {
        throw redis_error(redis_error::code_type::command_error, value.error());
    }
    if (value.kind() != redis_value::kind_type::array) {
        throw redis_error(redis_error::code_type::protocol_error, "redis reply is not an array");
    }
    return value.array();
}

void throw_if_redis_error(const redis_value& value) {
    if (value.kind() == redis_value::kind_type::error) {
        throw redis_error(redis_error::code_type::command_error, value.error());
    }
}

void throw_if_redis_transaction_reply_error(const redis_value& value, std::size_t index) {
    if (value.kind() != redis_value::kind_type::error) {
        return;
    }
    std::string message("redis transaction reply ");
    message.append(std::to_string(index));
    message.append(": ");
    message.append(value.error());
    throw redis_error(redis_error::code_type::command_error, message);
}

bool validate_redis_pooled_command(std::span<const std::string_view> args, bool allow_blocking) {
    if (args.empty() || args.front().empty()) {
        throw std::invalid_argument("redis command requires a command name");
    }
    const auto command = args.front();
    constexpr std::string_view stateful[]{
        "ASKING",
        "AUTH",
        "CLIENT",
        "DISCARD",
        "EXEC",
        "HELLO",
        "MONITOR",
        "MULTI",
        "PSUBSCRIBE",
        "PUNSUBSCRIBE",
        "QUIT",
        "READONLY",
        "READWRITE",
        "RESET",
        "SELECT",
        "SSUBSCRIBE",
        "SUBSCRIBE",
        "SUNSUBSCRIBE",
        "UNWATCH",
        "WATCH",
    };
    for (const auto name : stateful) {
        if (http_ascii_equals_ignore_case(command, name)) {
            throw std::invalid_argument(
                "stateful redis commands are not allowed through pooled command()");
        }
    }

    bool blocking = false;
    constexpr std::string_view always_blocking[]{"BLPOP", "BRPOP", "BRPOPLPUSH", "BLMOVE", "BLMPOP",
        "BZPOPMIN", "BZPOPMAX", "BZMPOP", "WAIT", "WAITAOF"};
    for (const auto name : always_blocking) {
        blocking = blocking || http_ascii_equals_ignore_case(command, name);
    }
    if (http_ascii_equals_ignore_case(command, "XREAD")) {
        blocking = blocking || xread_command_is_blocking(args);
    }
    if (http_ascii_equals_ignore_case(command, "XREADGROUP")) {
        blocking = blocking || xread_group_command_is_blocking(args);
    }
    if (blocking && !allow_blocking) {
        throw std::invalid_argument(
            "blocking redis command is not allowed in a pipeline or transaction");
    }
    return blocking;
}

task<redis_value> execute_owned_redis_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    return executor.pool_->execute_owned(std::move(args), resource, std::move(executor.options_));
}

task<redis_value> execute_owned_redis_command(redis_pool& pool, std::pmr::vector<std::pmr::string> args,
    operation_options options, std::pmr::memory_resource* resource) {
    return pool.execute_owned(std::move(args), resource, std::move(options));
}

task<std::optional<std::pmr::string>> redis_string_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    throw_if_redis_error(value);
    if (value.null()) {
        co_return std::nullopt;
    }
    const auto text = redis_value_string(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

task<std::int64_t> redis_integer_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    throw_if_redis_error(value);
    co_return redis_value_integer(value);
}

task<std::pmr::vector<std::pmr::string>> redis_string_array_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    throw_if_redis_error(value);
    const auto items = redis_value_array(value);
    std::pmr::vector<std::pmr::string> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throw_if_redis_error(item);
        if (!item.null()) {
            const auto text = redis_value_string(item);
            emplace_redis_string(output, text);
        }
    }
    co_return output;
}

task<std::pmr::vector<bool>> redis_bool_array_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    throw_if_redis_error(value);
    const auto items = redis_value_array(value);
    std::pmr::vector<bool> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throw_if_redis_error(item);
        output.emplace_back(redis_value_integer_bool(item));
    }
    co_return output;
}

task<std::pmr::vector<std::optional<std::pmr::string>>> redis_optional_string_array_command(
    redis_command_executor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    throw_if_redis_error(value);
    const auto items = redis_value_array(value);
    std::pmr::vector<std::optional<std::pmr::string>> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throw_if_redis_error(item);
        if (item.null()) {
            output.emplace_back(std::nullopt);
        } else {
            const auto text = redis_value_string(item);
            auto& stored = output.emplace_back(std::in_place, resource);
            stored->assign(text.data(), text.size());
        }
    }
    co_return output;
}

task<void> redis_ok_command(redis_command_executor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    throw_if_redis_error(value);
    if (!http_ascii_equals_ignore_case(redis_value_string(value), "OK")) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis status reply");
    }
    co_return;
}

task<std::pmr::string> redis_status_command(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    throw_if_redis_error(value);
    const auto text = redis_value_string(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

}  // namespace ruvia::detail
