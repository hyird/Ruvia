#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

namespace ruvia {
// Already-expanded proxy URI path, borrowed until open_udp_tunnel() returns.
struct http_client_udp_tunnel_request_view final {
    std::string_view target_{};
    std::span<const http_header_view> headers_{};
};

struct http_connect_udp_target_view final {
    std::string_view host_{};
    std::uint16_t port_{0};
};
struct http_connect_udp_target final {
    // resource passed to the parser must outlive this result.
    std::pmr::string host_;
    std::uint16_t port_{0};
};
enum class http_connect_udp_error : std::uint8_t {
    invalid_target,
    invalid_path,
    invalid_request,
    invalid_response,
    invalid_capsule_protocol,
};
// RFC 9298's default URI template. IPv6 hosts have no brackets or zone ID;
// all reserved bytes are percent-encoded during template expansion.
[[nodiscard]] std::variant<std::pmr::string, http_connect_udp_error> encode_http_connect_udp_path(
    http_connect_udp_target_view target, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(
    std::string_view path, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

struct http_connect_udp_request_view final {
    // HTTP/1.1 Upgrade versus HTTP/2 or HTTP/3 Extended CONNECT.
    http_protocol_version version_{http_protocol_version::http3};
    std::string_view method_{"CONNECT"};
    std::string_view protocol_{"connect-udp"};
    std::string_view scheme_{"https"};
    std::string_view authority_{};
    std::string_view path_{};
    std::span<const http_header_view> headers_{};
};
[[nodiscard]] std::variant<std::monostate, http_connect_udp_error> validate_http_connect_udp_request(http_connect_udp_request_view request) noexcept;
[[nodiscard]] std::variant<std::monostate, http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header_view> headers) noexcept;
// Classifies the HTTP/1 Upgrade token; full validation remains mandatory.
[[nodiscard]] bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept;
[[nodiscard]] std::variant<std::monostate, http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept;
// Takes application response metadata, validates it, and supplies the required
// Capsule-Protocol and version-specific status/Upgrade fields. HTTP/1 status 101
// can only be produced by this dedicated driver boundary.
[[nodiscard]] std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version);
[[nodiscard]] std::variant<http1_response_head_plan, http_connect_udp_error> prepare_http1_connect_udp_response_head(const http_response& response) noexcept;
}  // namespace ruvia
