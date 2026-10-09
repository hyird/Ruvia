#pragma once

#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/web/detail/redis/redis_batch_forwarding.h"
#include "ruvia/web/detail/redis/redis_command_batch.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia {

class redis_handle;

class redis_pipeline final {
public:
    redis_pipeline(const redis_pipeline&) = delete;
    redis_pipeline& operator=(const redis_pipeline&) = delete;
    redis_pipeline(redis_pipeline&& other) noexcept;
    redis_pipeline& operator=(redis_pipeline&&) = delete;

    RUVIA_REDIS_BATCH_FORWARDING(redis_pipeline)

    redis_pipeline& command(std::span<const std::string_view> args);
    redis_pipeline& command(std::initializer_list<std::string_view> args) = delete;

    // Every command owns its arguments before this call returns.
    template <typename... args_type>
        requires detail::redis_argument_pack<args_type...>
    redis_pipeline& command(args_type&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    // Consumes the batch before returning the lazy task, so the coroutine frame
    // owns every command and never borrows this builder through `this`.
    scoped_operation<std::pmr::vector<redis_value>> exec() &&;

private:
    friend class redis_handle;
    detail::redis_command_batch batch_;
    scoped_capability_registration registration_;

    redis_pipeline(detail::redis_pool& pool, operation_options options,
        std::pmr::memory_resource* resource, operation_scope& operation_scope) noexcept;
    [[nodiscard]] static task<std::pmr::vector<redis_value>> execute_owned(
        detail::redis_command_payload payload);

    static void expire_capability(void* target) noexcept;
};

}  // namespace ruvia
