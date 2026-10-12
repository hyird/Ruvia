#include "ruvia/web/redis/redis_transaction.h"

#include <stdexcept>
#include <utility>

#include "redis/redis_handle_helpers.h"
#include "redis/redis_registry.h"
#include "redis/redis_types_access.h"

namespace ruvia {

redis_transaction::redis_transaction(detail::redis_pool& pool, operation_options options,
    std::pmr::memory_resource* resource, operation_scope& scope) noexcept
    : batch_(pool, std::move(options), resource),
      registration_(scope, this, &redis_transaction::expire_capability) {}

redis_transaction::redis_transaction(redis_transaction&& other) noexcept
    : batch_(std::move(other.batch_)),
      registration_(std::move(other.registration_), this) {}

void redis_transaction::expire_capability(void* target) noexcept {
    static_cast<redis_transaction*>(target)->batch_.expire();
}

redis_transaction& redis_transaction::command(std::span<const std::string_view> args) {
    registration_.require_active();
    batch_.command(args);
    return *this;
}

task<std::pmr::vector<redis_value>> redis_transaction::execute_owned(detail::redis_command_payload payload_value) {
    auto* resource = payload_value.commands_.get_allocator().resource();
    std::pmr::vector<detail::redis_command_args_view> framed(resource);
    framed.reserve(payload_value.commands_.size() + 2);
    auto append_command_view = [&framed](const detail::redis_owned_command& command) {
        framed.emplace_back(command.args_);
    };
    auto multi = detail::make_owned_redis_command(resource, "MULTI");
    auto exec = detail::make_owned_redis_command(resource, "EXEC");
    append_command_view(multi);
    for (const auto& command : payload_value.commands_) {
        append_command_view(command);
    }
    append_command_view(exec);

    auto replies = co_await payload_value.pool_.get().execute_pipeline(
        std::span<const detail::redis_command_args_view>(framed), std::move(payload_value.options_), resource);
    if (replies.empty()) {
        throw redis_error(redis_error::code_type::protocol_error, "redis transaction returned no replies");
    }
    for (std::size_t i = 0; i < replies.size(); ++i) {
        detail::throw_if_redis_transaction_reply_error(replies[i], i);
    }
    auto exec_reply = std::move(replies.back());
    if (exec_reply.kind() != redis_value::kind_type::array) {
        throw redis_error(redis_error::code_type::command_error, "unexpected redis transaction reply");
    }

    // Every reply node lives in `resource`, so the EXEC elements move without copying.
    co_return std::move(detail::redis_types_access::elements(exec_reply));
}

scoped_operation<std::pmr::vector<redis_value>> redis_transaction::exec() && {
    auto& scope = registration_.scope();
    auto payload_value = batch_.consume();
    return make_scoped_operation(scope, execute_owned(std::move(payload_value)));
}

}  // namespace ruvia
