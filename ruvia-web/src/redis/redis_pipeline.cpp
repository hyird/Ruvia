#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/integration/request_io_metrics.h"
#include "ruvia/web/redis/redis_handle.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_registry.h"
#include "redis/redis_types_access.h"
#include "redis/redis_utils.h"

namespace ruvia::detail {

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_pool& pool, operation_options options,
    std::pmr::memory_resource* resource, operation_scope& scope) noexcept
    : pool_(&pool),
      options_(std::move(options)),
      commands_(pmr_resource_or_default(resource)),
      registration_(scope, this, &redis_batch::expire_capability) {}

template <redis_batch_mode mode>
redis_batch<mode>::redis_batch(redis_batch&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      options_(std::move(other.options_)),
      commands_(std::move(other.commands_)),
      registration_(std::move(other.registration_), this) {}

template <redis_batch_mode mode>
void redis_batch<mode>::require_ready() const {
    registration_.require_active();
    if (pool_ == nullptr) {
        throw std::logic_error("redis command batch has already been consumed or expired");
    }
}

template <redis_batch_mode mode>
std::pmr::memory_resource* redis_batch<mode>::resource() const noexcept {
    return commands_.get_allocator().resource();
}

template <redis_batch_mode mode>
redis_command_payload redis_batch<mode>::consume() {
    require_ready();
    auto& pool = *std::exchange(pool_, nullptr);
    return {pool, std::move(options_), std::move(commands_)};
}

template <redis_batch_mode mode>
void redis_batch<mode>::expire() noexcept {
    pool_ = nullptr;
    options_ = {};
    std::pmr::vector<redis_owned_command> empty(resource());
    commands_.swap(empty);
}

template <redis_batch_mode mode>
void redis_batch<mode>::expire_capability(void* target) noexcept {
    static_cast<redis_batch*>(target)->expire();
}

template <redis_batch_mode mode>
redis_batch<mode>& redis_batch<mode>::command(std::span<const std::string_view> args) {
    require_ready();
    (void)validate_redis_pooled_command(args, false);
    return append_owned(make_owned_redis_command(resource(), args));
}

template <redis_batch_mode mode>
redis_batch<mode>& redis_batch<mode>::append_owned(redis_owned_command command) {
    commands_.push_back(std::move(command));
    return *this;
}

template <redis_batch_mode mode>
task<std::pmr::vector<redis_value>> redis_batch<mode>::execute_owned(
    operation_scope& scope, redis_command_payload payload_value) {
    scoped_request_io_timer timer(scope, request_io_kind::redis);
    auto* resource = payload_value.commands_.get_allocator().resource();
    if constexpr (mode == redis_batch_mode::pipeline) {
        co_return co_await payload_value.pool_.get().execute_pipeline(
            std::move(payload_value.commands_), std::move(payload_value.options_), resource);
    } else {
        auto replies = co_await payload_value.pool_.get().execute_pipeline(
            std::move(payload_value.commands_), std::move(payload_value.options_), resource, true);
        if (replies.empty()) {
            throw redis_error(redis_error::code_type::protocol_error, "redis transaction returned no replies");
        }
        for (std::size_t i = 0; i < replies.size(); ++i) {
            throw_if_redis_transaction_reply_error(replies[i], i);
        }
        auto exec_reply = std::move(replies.back());
        if (exec_reply.kind() != redis_value::kind_type::array) {
            throw redis_error(redis_error::code_type::command_error, "unexpected redis transaction reply");
        }

        // Every reply node lives in `resource`, so the EXEC elements move without copying.
        co_return std::move(redis_types_access::elements(exec_reply));
    }
}

template <redis_batch_mode mode>
task<std::pmr::vector<redis_value>> redis_batch<mode>::exec() && {
    auto& scope = registration_.scope();
    auto payload_value = consume();
    return make_scoped_operation(scope, execute_owned(scope, std::move(payload_value)));
}

template class redis_batch<redis_batch_mode::pipeline>;
template class redis_batch<redis_batch_mode::transaction>;

}  // namespace ruvia::detail
