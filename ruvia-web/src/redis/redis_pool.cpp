#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/async.h"
#include "ruvia/web/detail/integration/request_io_metrics.h"

#include "redis/redis_protocol.h"
#include "redis/redis_registry.h"
#include "redis/redis_utils.h"

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

void redis_pool::slot_policy::throw_acquire_failure(const redis_pool&, worker_wait_status status) {
    switch (status) {
        case worker_wait_status::timed_out:
            throw redis_error(redis_error::code_type::timeout, "redis connection pool acquire timed out");
        case worker_wait_status::cancelled:
            throw redis_error(redis_error::code_type::cancelled, "redis operation cancelled");
        case worker_wait_status::closed:
            throw redis_error(redis_error::code_type::closing, "redis pool is closing");
        case worker_wait_status::value:
        case worker_wait_status::worker_stopping:
            break;
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
    pool.connections_[slot]->abort(client_abort_reason::cancelled);
}

task<redis_value> redis_pool::execute_owned(std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource, operation_options options,
    std::optional<std::chrono::milliseconds> reply_timeout, operation_scope* scope) {
    return execute_commands<redis_value>(
        std::move(args), std::move(options), reply_timeout, resource, std::nullopt, scope);
}

task<redis_value> redis_pool::execute_borrowed(redis_command_args_view args,
    std::pmr::memory_resource* resource, operation_options options, operation_timeout deadline) {
    return execute_commands<redis_value>(args, std::move(options), std::nullopt, resource, deadline);
}

// Own one lease, cancellation registration and failure cleanup for both a
// single command and a pipeline. The source remains in this coroutine frame;
// borrowed sources are retained by their enclosing caller until completion.
template <typename result_type, typename command_source_type>
task<result_type> redis_pool::execute_commands(command_source_type source, operation_options options,
    std::optional<std::chrono::milliseconds> reply_timeout, std::pmr::memory_resource* resource,
    std::optional<operation_timeout> deadline, operation_scope* scope) {
    scoped_request_io_timer timer(scope, request_io_kind::redis);
    constexpr bool single_reply = std::is_same_v<result_type, redis_value>;
    const auto commands = [&] {
        if constexpr (single_reply) {
            return std::array{redis_command_args_view{redis_arg_span(source)}};
        } else {
            return source;
        }
    }();
    auto* const resolved = pmr_resource_or_default(resource);
    result_type result(resolved);
    if constexpr (!single_reply) {
        result.reserve(commands.size());
    }
    if (commands.empty()) {
        co_return result;
    }

    // Ordinary commands start their budget here; an ORM retry retains the
    // deadline established when its enclosing operation started.
    const operation_timeout operation_timeout_value = deadline ? *deadline : operation_timeout(options.timeout_);
    const auto slot = co_await slots_.acquire(operation_timeout_value, config_.acquire_timeout_, options.stop_token_);
    const slot_pool_type::lease_guard lease(slots_, slot);
    slot_pool_type::cancellation_guard cancellation(slots_, slot, options.stop_token_);
    auto& connection = *connections_[slot];
    try {
        co_await connection.connect(&operation_timeout_value, command_timeout_);
        slots_.throw_if_aborted(slot);

        auto& write_buffer = connection.write_buffer();
        write_buffer.clear();
        std::size_t serialized_bytes = 0;
        for (const auto& command : commands) {
            const std::span<const std::pmr::string> args = command.args_;
            const auto command_bytes = resp_command_serialized_size(args);
            if (command_bytes > std::numeric_limits<std::size_t>::max() - serialized_bytes) {
                throw std::length_error("redis RESP request is too large");
            }
            serialized_bytes += command_bytes;
        }
        write_buffer.reserve(serialized_bytes);
        for (const auto& command : commands) {
            const std::span<const std::pmr::string> args = command.args_;
            append_resp_command(write_buffer, args);
        }

        const auto deadline_value = operation_timeout_value.constrained_by(command_timeout_).constrained_by(reply_timeout);
        co_await connection.write_request(deadline_value);
        if constexpr (single_reply) {
            result = co_await connection.read_reply(deadline_value, resolved);
            slots_.throw_if_aborted(slot);
        } else {
            while (result.size() < commands.size()) {
                result.emplace_back(co_await connection.read_reply(deadline_value, resolved));
                slots_.throw_if_aborted(slot);
            }
        }
        co_return result;
    } catch (...) {
        cancellation.finish();
        connection.close();
        throw;
    }
}

task<std::pmr::vector<redis_value>> redis_pool::execute_pipeline(
    std::span<const redis_owned_command> commands, operation_options options,
    std::pmr::memory_resource* resource) {
    return execute_commands<std::pmr::vector<redis_value>>(commands, std::move(options), std::nullopt, resource, std::nullopt);
}

task<std::pmr::vector<redis_value>> redis_pool::execute_pipeline(
    std::span<const redis_command_args_view> commands, operation_options options,
    std::pmr::memory_resource* resource) {
    return execute_commands<std::pmr::vector<redis_value>>(commands, std::move(options), std::nullopt, resource, std::nullopt);
}

}  // namespace ruvia::detail
