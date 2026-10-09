#include "redis/redis_handle_command_ops.h"

#include "ruvia/http/http_ascii.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_types_access.h"

namespace ruvia::detail {

task<void> execute_redis_ping(redis_command_executor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto reply = co_await redis_status_command(std::move(executor), std::move(args), resource);
    if (!http_ascii_equals_ignore_case(reply, "PONG")) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis ping reply");
    }
}

task<redis_set_result> execute_redis_set(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, redis_set_options options,
    std::pmr::memory_resource* resource) {
    auto reply = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    throw_if_redis_error(reply);
    const auto returns_previous = redis_set_returns_previous(options.previous_value_);
    if (reply.null()) {
        const bool applied =
            returns_previous &&
            (!options.condition_.has_value() || *options.condition_ == redis_set_condition::if_absent);
        co_return redis_types_access::set_result(applied);
    }
    const auto text = redis_value_string(reply);
    if (!returns_previous) {
        if (!http_ascii_equals_ignore_case(text, "OK")) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis set reply");
        }
        co_return redis_types_access::set_result(true);
    }
    const bool applied =
        !options.condition_.has_value() || *options.condition_ != redis_set_condition::if_absent;
    co_return redis_types_access::set_result(
        applied, std::pmr::string(text.data(), text.size(), resource));
}

task<bool> execute_redis_integer_bool(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto reply = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    co_return redis_value_integer_bool(reply);
}

task<std::pmr::vector<redis_key_value>> execute_redis_key_value_array(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::string_view context_value,
    std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    co_return parse_redis_key_value_array(value, resource, context_value);
}

task<std::pmr::vector<redis_scored_value>> execute_redis_scored_array(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await execute_owned_redis_command(std::move(executor), std::move(args), resource);
    co_return parse_redis_scored_array(value, resource);
}

task<std::optional<double>> execute_redis_optional_double(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::string_view context_value,
    std::pmr::memory_resource* resource) {
    auto reply = co_await redis_string_command(std::move(executor), std::move(args), resource);
    if (!reply) {
        co_return std::nullopt;
    }
    co_return parse_redis_double(*reply, context_value);
}

}  // namespace ruvia::detail
