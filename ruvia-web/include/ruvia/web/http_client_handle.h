#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include <asio/ip/udp.hpp>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_client_tunnel_request_view.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/quic_types.h"
#include "ruvia/web/http_client_advertisement.h"
#include "ruvia/web/http_client_exchange.h"
#include "ruvia/web/http_client_push.h"
#include "ruvia/web/http_client_response.h"
#include "ruvia/web/http_client_tunnel.h"
#include "ruvia/web/http_client_tunnel_config.h"
#include "ruvia/web/http_client_types.h"

namespace ruvia {

namespace detail {
class http_client_pool;
class http_client_registry;
}  // namespace detail

class context;

class http_client_handle final {
public:
    http_client_handle(const http_client_handle& other);
    http_client_handle& operator=(const http_client_handle&) = delete;

    [[nodiscard]] http_client_handle with_options(operation_options options) const;
    [[nodiscard]] scoped_operation<http_client_response> send(
        const http_client_request_view& request) const;
    [[nodiscard]] scoped_operation<http_client_exchange> open_request(
        const http_client_request_view& head, http_client_upload_config upload = {}) const;
    [[nodiscard]] scoped_operation<http_client_tunnel_result> open_tunnel(const http_client_tunnel_request_view& request, http_client_tunnel_config config = {}) const;
    // Negotiates RFC 9298 and returns an accepted tunnel or ordinary rejection.
    // The proxy authority comes from this registered origin. Capsule-Protocol
    // and HTTP/1 Upgrade fields are driver-owned. Use accepted.tunnel()->udp().
    [[nodiscard]] scoped_operation<http_client_tunnel_result> open_udp_tunnel(const http_client_udp_tunnel_request_view& request, http_client_tunnel_config config = {}) const;
    [[nodiscard]] http_client_stats stats() const;
    [[nodiscard]] quic_path_migration start_quic_path_migration(
        const asio::ip::udp::endpoint& local_endpoint) const;
    [[nodiscard]] std::optional<quic_path_migration> path_migration(
        std::uint64_t id) const;
    [[nodiscard]] quic_operation_status cancel_quic_path_migration(std::uint64_t id) const;
    [[nodiscard]] std::optional<http_client_advertisement> next_advertisement() const;
    [[nodiscard]] std::optional<http_client_push> next_push() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] http_scheme scheme() const;

private:
    friend class detail::http_client_registry;
    friend class context;
    friend class web_worker_context;
    http_client_handle(detail::http_client_pool& pool, std::pmr::memory_resource* resource,
        ::ruvia::operation_scope& scope) noexcept;
    http_client_handle(detail::http_client_pool& pool, std::pmr::memory_resource* resource,
        ::ruvia::operation_scope& scope, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;

    detail::http_client_pool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    operation_options options_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
