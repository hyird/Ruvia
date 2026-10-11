#pragma once

#include <memory>
#include <optional>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_client_tunnel_request_view.h"
#include "ruvia/web/http_client_handle.h"
#include "ruvia/web/http_client_tunnel.h"
#include "ruvia/web/http_client_tunnel_config.h"

namespace ruvia {

namespace detail {
class http_client_state;
}

// One outbound HTTP origin bound to one Ruvia event loop. Construction does
// not create a thread, and connections are established lazily by send().
class http_client final {
public:
    http_client(event_loop loop, const http_client_config& config,
        http_client_result_budget_config result_budget = {});
    ~http_client();

    http_client(const http_client&) = delete;
    http_client& operator=(const http_client&) = delete;
    http_client(http_client&&) = delete;
    http_client& operator=(http_client&&) = delete;

    // Handles and operations borrow this client's open lifecycle. Starting one
    // from a temporary would immediately run the client destructor and begin
    // shutdown, so only lvalue clients may create them.
    [[nodiscard]] http_client_handle with_options(operation_options options) const&;
    http_client_handle with_options(operation_options) const&& = delete;
    // Returns the default-options handle. Each call registers one allocation-free
    // capability with the client operation scope, so keep the returned handle for
    // a sequence of requests instead of calling handle() per request.
    [[nodiscard]] http_client_handle handle() const&;
    http_client_handle handle() const&& = delete;

    // Idempotent and callable from any thread. It only requests immediate
    // shutdown; use shutdown() when the worker teardown must be awaited.
    void close() noexcept;
    // Cancels and joins client producers/send operations, then retires their
    // transport borrows on the bound loop. Response body consumers are owned by
    // the response, not this shutdown: finish/join them and destroy responses
    // on the owning worker before the event_loop retires.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] http_client_stats stats() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] http_scheme scheme() const;
    [[nodiscard]] const worker_handle& worker() const& noexcept;
    const worker_handle& worker() const&& = delete;

private:
    std::shared_ptr<detail::http_client_state> state_;
};

}  // namespace ruvia
