#pragma once

#include <memory>

#include <asio/fwd.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/web/redis/redis_session.h"

#include "redis/redis_config_storage.h"
#include "redis/redis_pool.h"
#include "redis/redis_subscription_connection.h"

namespace ruvia::detail {

// The single backend owner used by standalone clients and application registrations.
// Both pools and the Pub/Sub connections borrow this stable owner's normalized
// configuration, TLS context and worker.
class redis_client_runtime final {
public:
    redis_client_runtime(asio::io_context& io_context, const worker_ref& worker_value,
        redis_config_storage config);
    redis_client_runtime(asio::io_context&, worker_ref&&, redis_config_storage) = delete;
    ~redis_client_runtime();

    redis_client_runtime(const redis_client_runtime&) = delete;
    redis_client_runtime& operator=(const redis_client_runtime&) = delete;

    [[nodiscard]] task<void> connect();
    void close_now() noexcept;
    // An application-registry handle; `io_metrics` is the request's counter
    // block, or null outside a request.
    [[nodiscard]] redis_session session(::ruvia::operation_scope& scope, request_io_metrics* io_metrics);
    // A standalone client handle, which never belongs to a request.
    [[nodiscard]] redis_session session(::ruvia::operation_scope& scope, operation_options options);

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] const redis_config_storage& config() const noexcept {
        return config_;
    }
    // The configuration of the runtime behind an active handle, for a
    // consumer that sizes its own use of that store (the DB query cache).
    [[nodiscard]] static const redis_config_storage& handle_config(const redis_session& handle) noexcept {
        return handle.runtime_->config();
    }
    [[nodiscard]] redis_pool& general() noexcept {
        return general_;
    }
    [[nodiscard]] redis_pool& blocking() noexcept {
        return blocking_;
    }
    [[nodiscard]] redis_subscription_set& subscriptions() noexcept {
        return subscriptions_;
    }

private:
    redis_config_storage config_;
    worker_ref worker_;
    // One client TLS context per alias, borrowed by every connection of both
    // pools and every subscription; configured only when TLS is enabled.
    asio::ssl::context tls_context_;
    redis_pool general_;
    redis_pool blocking_;
    redis_subscription_set subscriptions_;
};

}  // namespace ruvia::detail
