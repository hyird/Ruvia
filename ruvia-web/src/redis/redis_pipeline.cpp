#include "ruvia/web/redis/redis_pipeline.h"

#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/redis/redis_utils.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_registry.h"

namespace ruvia::detail {

redis_command_batch::redis_command_batch(redis_pool& pool, operation_options options,
    std::pmr::memory_resource* resource) noexcept
    : pool_(&pool),
      options_(std::move(options)),
      commands_(pmr_resource_or_default(resource)) {}

redis_command_batch::redis_command_batch(redis_command_batch&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      options_(std::move(other.options_)),
      commands_(std::move(other.commands_)) {}

void redis_command_batch::require_ready() const {
    if (pool_ == nullptr) {
        throw std::logic_error("redis command batch has already been consumed or expired");
    }
}

std::pmr::memory_resource* redis_command_batch::resource() const noexcept {
    return commands_.get_allocator().resource();
}

redis_command_payload redis_command_batch::consume() {
    require_ready();
    auto& pool = *std::exchange(pool_, nullptr);
    return {pool, std::move(options_), std::move(commands_)};
}

void redis_command_batch::expire() noexcept {
    pool_ = nullptr;
    options_ = {};
    std::pmr::vector<redis_owned_command> empty(resource());
    commands_.swap(empty);
}

redis_command_batch& redis_command_batch::command(std::span<const std::string_view> args) {
    require_ready();
    (void)validate_redis_pooled_command(args, false);
    commands_.emplace_back(make_owned_redis_command(resource(), args));
    return *this;
}

}  // namespace ruvia::detail

namespace ruvia {

redis_pipeline::redis_pipeline(detail::redis_pool& pool, operation_options options,
    std::pmr::memory_resource* resource, operation_scope& operation_scope) noexcept
    : batch_(pool, std::move(options), resource),
      registration_(operation_scope, this, &redis_pipeline::expire_capability) {}

redis_pipeline::redis_pipeline(redis_pipeline&& other) noexcept
    : batch_(std::move(other.batch_)),
      registration_(std::move(other.registration_), this) {}

void redis_pipeline::expire_capability(void* target) noexcept {
    static_cast<redis_pipeline*>(target)->batch_.expire();
}

redis_pipeline& redis_pipeline::command(std::span<const std::string_view> args) {
    registration_.require_active();
    batch_.command(args);
    return *this;
}

task<std::pmr::vector<redis_value>> redis_pipeline::execute_owned(
    detail::redis_command_payload payload_value) {
    co_return co_await payload_value.pool_.get().execute_pipeline(
        std::span<const detail::redis_owned_command>(payload_value.commands_),
        std::move(payload_value.options_), payload_value.commands_.get_allocator().resource());
}

scoped_operation<std::pmr::vector<redis_value>> redis_pipeline::exec() && {
    auto& scope = registration_.scope();
    auto payload_value = batch_.consume();
    return make_scoped_operation(scope, execute_owned(std::move(payload_value)));
}

}  // namespace ruvia
