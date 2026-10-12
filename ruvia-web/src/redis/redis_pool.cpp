#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ruvia/core/async.h"
#include "ruvia/web/detail/redis/redis_utils.h"

#include "redis/redis_protocol.h"
#include "redis/redis_registry.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::span<const std::pmr::string> redis_arg_span(
    const std::pmr::vector<std::pmr::string>& args) noexcept {
    return args;
}

[[nodiscard]] std::span<const std::pmr::string> redis_arg_span(redis_command_args_view args) noexcept {
    return args.args_;
}

}  // namespace

void redis_pool::slot_policy::throw_acquire_failure(const redis_pool&, const pool_waiter_result& result) {
    if (result.timed_out() != nullptr) {
        throw redis_error(redis_error::code_type::timeout, "redis connection pool acquire timed out");
    }
    if (result.cancelled() != nullptr) {
        throw redis_error(redis_error::code_type::cancelled, "redis operation cancelled");
    }
    if (result.closed() != nullptr) {
        throw redis_error(redis_error::code_type::closing, "redis pool is closing");
    }
    std::terminate();
}

void redis_pool::slot_policy::throw_aborted(const redis_pool&, pool_slot_abort_reason reason) {
    switch (reason) {
        case pool_slot_abort_reason::cancelled:
            throw redis_error(redis_error::code_type::cancelled, "redis operation cancelled");
        case pool_slot_abort_reason::closing:
            throw redis_error(redis_error::code_type::closing, "redis pool is closing");
        case pool_slot_abort_reason::none:
            break;
    }
    std::terminate();
}

void redis_pool::slot_policy::cancel_slot(redis_pool& pool, std::size_t slot) noexcept {
    pool.abort(slot, client_abort_reason::cancelled);
}

task<redis_value> redis_pool::execute_owned(std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource, operation_options options,
    std::optional<std::chrono::milliseconds> reply_timeout) {
    return execute_with_timeout_impl(std::move(args), std::move(options), reply_timeout, resource, std::nullopt);
}

task<redis_value> redis_pool::execute_borrowed(redis_command_args_view args,
    std::pmr::memory_resource* resource, operation_options options, operation_timeout deadline) {
    return execute_with_timeout_impl(args, std::move(options), std::nullopt, resource, deadline);
}

template <typename arg_source_type>
task<redis_value> redis_pool::execute_with_timeout_impl(arg_source_type args, operation_options options,
    std::optional<std::chrono::milliseconds> reply_timeout, std::pmr::memory_resource* resource,
    std::optional<operation_timeout> deadline) {
    // Ordinary commands start their budget here; an ORM retry retains the
    // deadline established when its enclosing operation started.
    const operation_timeout operation_timeout_value = deadline ? *deadline : operation_timeout(options.timeout_);
    const auto slot = co_await slots_.acquire(operation_timeout_value, config_.acquire_timeout_, options.stop_token_);
    const slot_pool_type::lease_guard lease(slots_, slot);
    slot_pool_type::cancellation_guard cancellation(slots_, slot, options.stop_token_);
    auto& connection = *connections_[slot];
    try {
        if (!connection.connected_) {
            co_await connect(slot, &operation_timeout_value);
        }
        slots_.throw_if_aborted(slot);

        connection.write_buffer_.clear();
        const auto arg_span = redis_arg_span(args);
        connection.write_buffer_.reserve(resp_command_serialized_size(arg_span));
        append_resp_command(connection.write_buffer_, arg_span);
        const auto deadline_value =
            operation_timeout_value.constrained_by(command_timeout_).constrained_by(reply_timeout);
        co_await write_request(slot, deadline_value);

        auto reply = co_await read_reply(slot, deadline_value, resource);
        slots_.throw_if_aborted(slot);
        co_return reply;
    } catch (...) {
        cancellation.finish();
        close(slot);
        throw;
    }
}

template <typename command_source_type>
task<std::pmr::vector<redis_value>> redis_pool::execute_pipeline_impl(
    command_source_type commands, operation_options options, std::pmr::memory_resource* resource) {
    const auto resolved = pmr_resource_or_default(resource);
    std::pmr::vector<redis_value> replies(resolved);
    replies.reserve(commands.size());
    if (commands.empty()) {
        co_return replies;
    }

    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    const auto slot = co_await slots_.acquire(operation_timeout_value, config_.acquire_timeout_, options.stop_token_);
    const slot_pool_type::lease_guard lease(slots_, slot);
    slot_pool_type::cancellation_guard cancellation(slots_, slot, options.stop_token_);
    auto& connection = *connections_[slot];
    try {
        if (!connection.connected_) {
            co_await connect(slot, &operation_timeout_value);
        }
        slots_.throw_if_aborted(slot);

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
        co_await write_request(slot, deadline_value);

        while (replies.size() < commands.size()) {
            replies.emplace_back(co_await read_reply(slot, deadline_value, resolved));
            slots_.throw_if_aborted(slot);
        }

        co_return replies;
    } catch (...) {
        cancellation.finish();
        close(slot);
        throw;
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

}  // namespace ruvia::detail
