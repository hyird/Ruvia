#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/stop_token.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/websocket_client_negotiation.h"
#include "ruvia/web/http_client_types.h"

namespace ruvia::detail {

class http_client_pool;
class http_client_tunnel_state;
class http_client_upload_state;
class http_client_output_queue;
struct http_client_config_storage;

// Syntax of http_client_request_options' route overrides, independently of
// any alias; throws std::invalid_argument.
void validate_http_client_route_options(const http_client_request_options& options);

// The effective origin and transport of one request: the alias defaults with
// the request's route overrides applied. Views borrow the alias configuration
// or the owning request storage.
struct http_client_route final {
    http_origin_view origin_;
    // RFC 3986 authority with the scheme's default port omitted.
    std::string_view authority_;
    std::string_view dial_host_;
    std::uint16_t dial_port_{};
    // TLS SNI and verified peer name; empty for http.
    std::string_view server_name_;
    // The authority is the alias authority: its cookie jar applies.
    bool alias_origin_{true};
    // Dial target and server name are the alias defaults: a connection of
    // this transport is authenticated for the alias origin (server push).
    bool alias_transport_{true};
    // Both of the above: Alt-Svc applies.
    bool alias_route_{true};
};

struct http_client_websocket_handshake final {
    http_client_websocket_handshake(websocket_client_negotiation_config_view config,
        std::pmr::memory_resource* resource);
    websocket_client_negotiation negotiation_;
    std::array<char, 24> key_;
    std::pmr::string selected_subprotocol_;
    websocket_compression compression_{};
};

class http_client_request_storage final {
public:
    // Complete owning boundary: reserve field metadata once and normalize/copy
    // every field before asynchronous work can outlive caller storage.
    http_client_request_storage(std::string_view method, std::string_view target,
        std::span<const http_header_view> fields, std::pmr::memory_resource* resource);

    http_client_request_storage(const http_client_request_storage&) = delete;
    http_client_request_storage& operator=(const http_client_request_storage&) = delete;
    http_client_request_storage(http_client_request_storage&& other) noexcept;
    http_client_request_storage& operator=(http_client_request_storage&&) noexcept = delete;

    http_client_request_storage& set_body(std::string_view body);

    void set_host_stop(stop_token token) noexcept {
        host_stop_ = std::move(token);
    }
    [[nodiscard]] const stop_token& host_stop() const noexcept {
        return host_stop_;
    }

    void set_tunnel(std::string_view authority, std::string_view protocol);
    void set_websocket(std::shared_ptr<http_client_websocket_handshake> handshake) noexcept {
        websocket_ = std::move(handshake);
    }
    [[nodiscard]] http_client_websocket_handshake* websocket() const noexcept {
        return websocket_.get();
    }
    void bind_tunnel(http_client_tunnel_state& tunnel) noexcept {
        tunnel_ = &tunnel;
    }
    [[nodiscard]] http_client_tunnel_state* tunnel() const noexcept {
        return tunnel_;
    }
    [[nodiscard]] http_client_output_queue* output() const noexcept;
    [[nodiscard]] bool is_tunnel() const noexcept {
        return is_tunnel_;
    }
    void set_upgrade() noexcept {
        is_upgrade_ = true;
        is_tunnel_ = true;
    }
    [[nodiscard]] bool is_upgrade() const noexcept {
        return is_upgrade_;
    }
    [[nodiscard]] std::string_view tunnel_authority() const& noexcept {
        return tunnel_authority_;
    }
    std::string_view tunnel_authority() const&& = delete;
    [[nodiscard]] std::string_view tunnel_protocol() const& noexcept {
        return tunnel_protocol_;
    }
    std::string_view tunnel_protocol() const&& = delete;
    void bind_upload(http_client_upload_state& upload) noexcept {
        upload_ = &upload;
    }
    [[nodiscard]] http_client_upload_state* upload() const noexcept {
        return upload_;
    }
    [[nodiscard]] std::string_view method() const& noexcept {
        return method_;
    }
    [[nodiscard]] std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view target() const& noexcept {
        return target_;
    }
    [[nodiscard]] std::string_view target() const&& = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return method_.get_allocator().resource();
    }

    [[nodiscard]] std::string_view body() const& noexcept {
        return body_;
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] bool has_body() const noexcept {
        return has_body_;
    }
    [[nodiscard]] const std::pmr::vector<http_header>& headers() const noexcept {
        return headers_;
    }
    [[nodiscard]] bool udp_tunnel() const noexcept {
        return tunnel_protocol_ == "connect-udp";
    }
    // Validates the route overrides against `config` and fixes route(). Only
    // once the storage reached the address it keeps while its request runs:
    // a move drops the resolved views.
    void resolve_route(const http_client_config_storage& config);
    [[nodiscard]] const http_client_route& route() const& noexcept {
        return *route_;
    }
    const http_client_route& route() const&& = delete;
    http_client_request_options options_;
    std::shared_ptr<http_client_pool> pool_owner_;

private:
    http_client_request_storage(
        std::string_view method, std::string_view target, std::pmr::memory_resource* resource);

    std::pmr::string method_;
    std::pmr::string target_;
    std::pmr::vector<http_header> headers_;
    std::pmr::string body_;
    std::pmr::string tunnel_authority_;
    std::pmr::string tunnel_protocol_;
    std::shared_ptr<http_client_websocket_handshake> websocket_;
    stop_token host_stop_;
    bool is_tunnel_{};
    bool is_upgrade_{false};
    http_client_tunnel_state* tunnel_{};
    bool has_body_{false};
    http_client_upload_state* upload_{};
    // The normalized authority override that route() views.
    std::pmr::string route_authority_;
    std::optional<http_client_route> route_;
};

}  // namespace ruvia::detail
