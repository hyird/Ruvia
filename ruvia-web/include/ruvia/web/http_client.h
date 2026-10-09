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
    [[nodiscard]] scoped_operation<http_client_response> send(
        const http_client_request_view& request) const&;
    scoped_operation<http_client_response> send(const http_client_request_view&) const&& = delete;

    [[nodiscard]] scoped_operation<http_client_exchange> open_request(
        const http_client_request_view& head, http_client_upload_config upload = {}) const&;
    scoped_operation<http_client_exchange> open_request(const http_client_request_view&, http_client_upload_config = {}) const&& = delete;

    // Idempotent and callable from any thread. It only requests immediate
    // shutdown; use shutdown() when the worker teardown must be awaited.
    void close() noexcept;
    // Cancels and joins client producers/send operations, then retires their
    // transport borrows on the bound loop. Response body consumers are owned by
    // the response, not this shutdown: finish/join them and destroy responses
    // on the owning worker before the event_loop retires.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] scoped_operation<http_client_tunnel_result> open_tunnel(const http_client_tunnel_request_view& request, http_client_tunnel_config config = {}) const&;
    scoped_operation<http_client_tunnel_result> open_tunnel(const http_client_tunnel_request_view&, http_client_tunnel_config = {}) const&& = delete;
    [[nodiscard]] scoped_operation<http_client_tunnel_result> open_udp_tunnel(const http_client_udp_tunnel_request_view& request, http_client_tunnel_config config = {}) const&;
    scoped_operation<http_client_tunnel_result> open_udp_tunnel(const http_client_udp_tunnel_request_view&, http_client_tunnel_config = {}) const&& = delete;
    [[nodiscard]] http_client_stats stats() const;
    // QUIC migration is owner-loop-only. The candidate socket is retained by
    // its connection until validation terminates; abort closes that connection.
    [[nodiscard]] quic_path_migration start_quic_path_migration(
        const asio::ip::udp::endpoint& local_endpoint) const;
    [[nodiscard]] std::optional<quic_path_migration> path_migration(
        std::uint64_t id) const;
    [[nodiscard]] quic_operation_status cancel_quic_path_migration(std::uint64_t id) const;
    [[nodiscard]] std::optional<http_client_advertisement> next_advertisement() const&;
    [[nodiscard]] std::optional<http_client_push> next_push() const&;
    std::optional<http_client_push> next_push() const&& = delete;
    std::optional<http_client_advertisement> next_advertisement() const&& = delete;
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
