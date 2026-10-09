#pragma once

#include <memory>
#include <string_view>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/web/http_client.h"

#include "client/client_lifecycle.h"
#include "client/http_client_registry.h"

namespace ruvia::detail {

class http_client_state final : public std::enable_shared_from_this<http_client_state> {
public:
    http_client_state(event_loop loop, const http_client_config& config,
        http_client_result_budget_config result_budget);

    http_client_state(const http_client_state&) = delete;
    http_client_state& operator=(const http_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }

    [[nodiscard]] http_client_handle handle(operation_options options);
    [[nodiscard]] http_client_stats stats();
    [[nodiscard]] std::string_view host();
    [[nodiscard]] std::uint16_t port();
    [[nodiscard]] http_scheme scheme();

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<http_client_state>;
    static constexpr std::string_view client_name{"HTTP"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] http_client_registry& backend() noexcept {
        return clients_;
    }

    event_loop loop_;
    worker_handle worker_;
    worker_memory memory_;
    http_client_registry clients_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<http_client_state> lifecycle_;
};

}  // namespace ruvia::detail
