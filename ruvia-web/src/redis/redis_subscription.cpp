#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/web/redis/redis_handle.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_subscription_connection.h"

namespace ruvia {
namespace {

void require_subscription_worker(void* target) noexcept {
    if (!static_cast<const worker_handle*>(target)->is_current()) {
        std::terminate();
    }
}

}  // namespace

redis_subscription::redis_subscription(detail::redis_subscription_set& subscriptions, operation_scope& scope,
    operation_options options)
    : connection_(subscriptions.open(std::move(options))),
      registration_(scope, this, &redis_subscription::expire_capability) {}

redis_subscription::~redis_subscription() = default;

void redis_subscription::expire_capability(void* target) noexcept {
    static_cast<redis_subscription*>(target)->connection_->abort(detail::client_abort_reason::closing);
}

scoped_operation<void> redis_subscription::send(
    std::string_view command, std::span<const std::string_view> names, bool require_names) {
    registration_.require_active();
    if (require_names && names.empty()) {
        throw std::invalid_argument("redis subscription requires at least one channel or pattern");
    }
    return make_scoped_operation(registration_.scope(),
        connection_->send(detail::own_redis_args(command, names, connection_->resource())),
        &require_subscription_worker, const_cast<worker_handle*>(&connection_->worker()));
}

scoped_operation<void> redis_subscription::subscribe(std::span<const std::string_view> channels) & {
    return send("SUBSCRIBE", channels, true);
}

scoped_operation<void> redis_subscription::psubscribe(std::span<const std::string_view> patterns) & {
    return send("PSUBSCRIBE", patterns, true);
}

scoped_operation<void> redis_subscription::unsubscribe(std::span<const std::string_view> channels) & {
    return send("UNSUBSCRIBE", channels, false);
}

scoped_operation<void> redis_subscription::punsubscribe(std::span<const std::string_view> patterns) & {
    return send("PUNSUBSCRIBE", patterns, false);
}

scoped_operation<std::optional<redis_pubsub_event>> redis_subscription::read() & {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), connection_->read(), &require_subscription_worker,
        const_cast<worker_handle*>(&connection_->worker()));
}

void redis_subscription::abort() noexcept {
    require_subscription_worker(const_cast<worker_handle*>(&connection_->worker()));
    connection_->abort(detail::client_abort_reason::closing);
}

}  // namespace ruvia
