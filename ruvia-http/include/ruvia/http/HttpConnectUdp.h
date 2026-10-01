#pragma once

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolVersion.h"

namespace ruvia {
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
[[nodiscard]] std::expected<std::pmr::string, HttpConnectUdpError> encodeHttpConnectUdpPath(
    HttpConnectUdpTargetView target, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::expected<HttpConnectUdpTarget, HttpConnectUdpError> parseHttpConnectUdpPath(
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
[[nodiscard]] std::expected<void, HttpConnectUdpError> validateHttpConnectUdpRequest(HttpConnectUdpRequestView request) noexcept;
[[nodiscard]] std::expected<void, HttpConnectUdpError> validateHttpConnectUdpResponse(
    HttpProtocolVersion version, std::uint16_t status, std::span<const HttpHeaderView> headers) noexcept;
}  // namespace ruvia
