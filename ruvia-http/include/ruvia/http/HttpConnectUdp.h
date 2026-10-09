#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/Http1ResponseHeadPlan.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {
// Already-expanded proxy URI path, borrowed until openUdpTunnel() returns.
struct HttpClientUdpTunnelRequestView final {
    std::string_view target{};
    std::span<const HttpHeaderView> headers{};
};

struct HttpConnectUdpTargetView final {
    std::string_view host{};
    std::uint16_t port{0};
};
struct HttpConnectUdpTarget final {
    // resource passed to the parser must outlive this result.
    std::pmr::string host;
    std::uint16_t port{0};
};
enum class HttpConnectUdpError : std::uint8_t {
    kInvalidTarget,
    kInvalidPath,
    kInvalidRequest,
    kInvalidResponse,
    kInvalidCapsuleProtocol,
};
// RFC 9298's default URI template. IPv6 hosts have no brackets or zone ID;
// all reserved bytes are percent-encoded during template expansion.
[[nodiscard]] std::variant<std::pmr::string, HttpConnectUdpError> encodeHttpConnectUdpPath(
    HttpConnectUdpTargetView target, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<HttpConnectUdpTarget, HttpConnectUdpError> parseHttpConnectUdpPath(
    std::string_view path, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

struct HttpConnectUdpRequestView final {
    // HTTP/1.1 Upgrade versus HTTP/2 or HTTP/3 Extended CONNECT.
    HttpProtocolVersion version{HttpProtocolVersion::kHttp3};
    std::string_view method{"CONNECT"};
    std::string_view protocol{"connect-udp"};
    std::string_view scheme{"https"};
    std::string_view authority{};
    std::string_view path{};
    std::span<const HttpHeaderView> headers{};
};
[[nodiscard]] std::variant<std::monostate, HttpConnectUdpError> validateHttpConnectUdpRequest(HttpConnectUdpRequestView request) noexcept;
[[nodiscard]] std::variant<std::monostate, HttpConnectUdpError> validateHttpConnectUdpResponse(
    HttpProtocolVersion version, std::uint16_t status, std::span<const HttpHeaderView> headers) noexcept;
// Classifies the HTTP/1 Upgrade token; full validation remains mandatory.
[[nodiscard]] bool isHttpConnectUdpUpgradeRequest(const HttpRequest& request) noexcept;
[[nodiscard]] std::variant<std::monostate, HttpConnectUdpError> validateHttpConnectUdpRequest(const HttpRequest& request) noexcept;
// Takes application response metadata, validates it, and supplies the required
// Capsule-Protocol and version-specific status/Upgrade fields. HTTP/1 status 101
// can only be produced by this dedicated driver boundary.
[[nodiscard]] std::variant<HttpResponse, HttpConnectUdpError> prepareHttpConnectUdpResponse(HttpResponse response, HttpProtocolVersion version);
[[nodiscard]] std::variant<Http1ResponseHeadPlan, HttpConnectUdpError> prepareHttp1ConnectUdpResponseHead(const HttpResponse& response) noexcept;
}  // namespace ruvia
