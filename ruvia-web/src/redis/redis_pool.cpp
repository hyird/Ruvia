#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/core/async.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/web/detail/redis/redis_utils.h"

#include "redis/redis_protocol.h"
#include "redis/redis_registry.h"

namespace ruvia {
namespace detail {
namespace {

[[nodiscard]] std::span<const std::pmr::string> redis_arg_span(
    const std::pmr::vector<std::pmr::string>& args) noexcept {
    return args;
}

}  // namespace

static_assert(worker_cancellation_post_is_inline<redis_cancellation_target>);

task<redis_value> redis_pool::execute_owned(std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource, operation_options options) {
    return execute_with_timeout_impl(std::move(args), std::move(options), resource);
}

template <typename arg_source_type>
task<redis_value> redis_pool::execute_with_timeout_impl(
    arg_source_type args, operation_options options, std::pmr::memory_resource* resource) {
    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    const auto index = co_await acquire(operation_timeout_value, options.stop_token_);
    connection_guard_type guard_value(*this, index, options.stop_token_);
    auto& connection = guard_value.connection();
    try {
        if (!connection.connected_) {
            co_await connect(connection, &operation_timeout_value);
        }
        throw_if_aborted(connection);

        connection.write_buffer_.clear();
        const auto arg_span = redis_arg_span(args);
        connection.write_buffer_.reserve(resp_command_serialized_size(arg_span));
        append_resp_command(connection.write_buffer_, arg_span);
        const auto deadline_value = operation_timeout_value.constrained_by(command_timeout_);
        co_await async_socket_write(connection, deadline_value);

        auto reply = co_await read_reply(connection, deadline_value, resource);
        throw_if_aborted(connection);
        co_return reply;
    } catch (...) {
        guard_value.discard();
        throw;
    }
}

template <typename command_source_type>
task<std::pmr::vector<redis_value>> redis_pool::execute_pipeline_impl(
    command_source_type commands, operation_options options, std::pmr::memory_resource* resource) {
    const auto resolved = detail::pmr_resource_or_default(resource);
    std::pmr::vector<redis_value> replies(resolved);
    replies.reserve(commands.size());
    if (commands.empty()) {
        co_return replies;
    }

    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    const auto index = co_await acquire(operation_timeout_value, options.stop_token_);
    connection_guard_type guard_value(*this, index, options.stop_token_);
    auto& connection = guard_value.connection();
    try {
        if (!connection.connected_) {
            co_await connect(connection, &operation_timeout_value);
        }
        throw_if_aborted(connection);

        connection.write_buffer_.clear();
        std::size_t serialized_bytes = 0;
        for (const auto& command : commands) {
            const std::span<const std::pmr::string> args = command.args_;
            const auto command_bytes = resp_command_serialized_size(args);
            if (command_bytes > std::numeric_limits<std::size_t>::max() - serialized_bytes) {
                throw std::length_error("redis RESP pipeline is too large");
            }
            serialized_bytes += command_bytes;
        }
        connection.write_buffer_.reserve(serialized_bytes);
        for (const auto& command : commands) {
            const std::span<const std::pmr::string> args = command.args_;
            append_resp_command(connection.write_buffer_, args);
        }

        const auto deadline_value = operation_timeout_value.constrained_by(command_timeout_);
        co_await async_socket_write(connection, deadline_value);

        while (replies.size() < commands.size()) {
            replies.emplace_back(co_await read_reply(connection, deadline_value, resolved));
            throw_if_aborted(connection);
        }

        co_return replies;
    } catch (...) {
        guard_value.discard();
        throw;
    }
}

void redis_pool::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0) {
        return;
    }
    for (auto& connection : connections_) {
        if (connection.cancellation_id_ != cancellation_id) {
            continue;
        }
        connection.cancellation_id_ = 0;
        connection.abort_reason_ = connection_type::abort_reason_type::cancelled;
        close(connection);
        return;
    }
}

void redis_pool::throw_if_aborted(const connection_type& connection) const {
    if (connection.abort_reason_ == connection_type::abort_reason_type::cancelled) {
        throw redis_error(redis_error::code_type::cancelled, "redis operation cancelled");
    }
    if (connection.abort_reason_ == connection_type::abort_reason_type::closing) {
        throw redis_error(redis_error::code_type::closing, "redis pool is closing");
    }
}

task<std::pmr::vector<redis_value>> redis_pool::execute_pipeline(
    std::span<const redis_owned_command> commands, operation_options options,
    std::pmr::memory_resource* resource) {
    return execute_pipeline_impl(commands, std::move(options), resource);
}

task<std::pmr::vector<redis_value>> redis_pool::execute_pipeline(
    std::span<const redis_command_args_view> commands, operation_options options,
    std::pmr::memory_resource* resource) {
    return execute_pipeline_impl(commands, std::move(options), resource);
}

}  // namespace detail

}  // namespace ruvia
