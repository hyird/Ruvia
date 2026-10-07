#include "ruvia/web/redis/RedisTransaction.h"

#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"

namespace ruvia {

RedisTransaction::RedisTransaction(detail::RedisPool& pool, OperationOptions options,
    std::pmr::memory_resource* resource, operation_scope& scope) noexcept
    : batch_(pool, std::move(options), resource),
      watches_(batch_.resource()),
      registration_(scope, this, &RedisTransaction::expire_capability) {}

RedisTransaction::RedisTransaction(RedisTransaction&& other) noexcept
    : batch_(std::move(other.batch_)),
      watches_(std::move(other.watches_)),
      registration_(std::move(other.registration_), this) {}

void RedisTransaction::expire_capability(void* target) noexcept {
    auto& transaction = *static_cast<RedisTransaction*>(target);
    transaction.batch_.expire();
    std::pmr::vector<detail::redis_owned_command> empty(transaction.watches_.get_allocator().resource());
    transaction.watches_.swap(empty);
}

RedisTransaction& RedisTransaction::command(std::span<const std::string_view> args) {
    registration_.require_active();
    batch_.command(args);
    return *this;
}

RedisTransaction& RedisTransaction::watch(std::string_view key) {
    return watch(std::span<const std::string_view>(&key, 1));
}

RedisTransaction& RedisTransaction::watch(std::span<const std::string_view> keys) {
    registration_.require_active();
    batch_.require_ready();
    if (keys.empty()) {
        return *this;
    }
    watches_.emplace_back(detail::make_owned_redis_command(batch_.resource(), "WATCH", keys));
    return *this;
}

RedisTransaction& RedisTransaction::unwatch() {
    registration_.require_active();
    batch_.require_ready();
    watches_.emplace_back(detail::make_owned_redis_command(batch_.resource(), "UNWATCH"));
    return *this;
}

Task<std::pmr::vector<RedisValue>> RedisTransaction::execute_owned(
    detail::redis_command_payload payload, std::pmr::vector<detail::redis_owned_command> watches) {
    auto* resource = payload.commands.get_allocator().resource();
    std::pmr::vector<detail::RedisCommandArgsView> framed(resource);
    framed.reserve(watches.size() + payload.commands.size() + 2);
    auto append_command_view = [&framed](const detail::redis_owned_command& command) {
        framed.emplace_back(command.args);
    };
    auto multi = detail::make_owned_redis_command(resource, "MULTI");
    auto exec = detail::make_owned_redis_command(resource, "EXEC");
    for (const auto& command : watches) {
        append_command_view(command);
    }
    append_command_view(multi);
    for (const auto& command : payload.commands) {
        append_command_view(command);
    }
    append_command_view(exec);

    auto replies = co_await payload.pool.get().executePipeline(
        std::span<const detail::RedisCommandArgsView>(framed), std::move(payload.options), resource);
    if (replies.empty()) {
        throw RedisError(RedisError::Code::kProtocolError, "redis transaction returned no replies");
    }
    for (std::size_t i = 0; i < replies.size(); ++i) {
        detail::throwIfRedisTransactionReplyError(replies[i], i);
    }
    auto execReply = std::move(replies.back());
    if (execReply.null()) {
        throw RedisError(RedisError::Code::kTransactionAborted, "redis transaction aborted");
    }
    if (execReply.kind() != RedisValue::Kind::kArray) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis transaction reply");
    }

    std::pmr::vector<RedisValue> result(resource);
    result.reserve(execReply.array().size());
    for (const auto& value : execReply.array()) {
        result.emplace_back(value);
    }
    co_return result;
}

ScopedOperation<std::pmr::vector<RedisValue>> RedisTransaction::exec() && {
    auto& scope = registration_.scope();
    auto payload = batch_.consume();
    return make_scoped_operation(scope, execute_owned(std::move(payload), std::move(watches_)));
}

}  // namespace ruvia
