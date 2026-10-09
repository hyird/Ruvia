#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/web/detail/redis/redis_argument_pack.h"
#include "ruvia/web/detail/redis/redis_batch_forwarding.h"
#include "ruvia/web/detail/redis/redis_command_batch.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia {

class redis_transaction final {
public:
    redis_transaction(const redis_transaction&) = delete;
    redis_transaction& operator=(const redis_transaction&) = delete;
    redis_transaction(redis_transaction&& other) noexcept;
    redis_transaction& operator=(redis_transaction&&) = delete;

    RUVIA_REDIS_BATCH_FORWARDING(redis_transaction)
    redis_transaction& command(std::span<const std::string_view> args);
    redis_transaction& command(std::initializer_list<std::string_view> args) = delete;
    redis_transaction& watch(std::string_view key);
    redis_transaction& watch(std::span<const std::string_view> keys);

    // As on redis_pipeline: command words and watched keys as ordinary arguments,
    // copied into the batch before the call returns. Two or more keys, so a
    // single-key watch(key) keeps its own overload.
    template <typename... args_type>
        requires detail::redis_argument_pack<args_type...>
    redis_transaction& command(args_type&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires(detail::redis_argument_pack<keys_type...> && sizeof...(keys_type) >= 2)
    redis_transaction& watch(keys_type&&... keys) {
        const std::string_view views[]{std::string_view(keys)...};
        return watch(std::span<const std::string_view>(views));
    }

    redis_transaction& unwatch();

    // A transaction is a single-use command batch. Its commands are transferred
    // into the returned coroutine frame before this builder may be destroyed.
    scoped_operation<std::pmr::vector<redis_value>> exec() &&;

private:
    friend class redis_handle;

    redis_transaction(detail::redis_pool& pool, operation_options options,
        std::pmr::memory_resource* resource, operation_scope& scope) noexcept;
    [[nodiscard]] static task<std::pmr::vector<redis_value>> execute_owned(
        detail::redis_command_payload payload, std::pmr::vector<detail::redis_owned_command> watches);

    detail::redis_command_batch batch_;
    std::pmr::vector<detail::redis_owned_command> watches_;
    scoped_capability_registration registration_;
    static void expire_capability(void* target) noexcept;
};

}  // namespace ruvia
