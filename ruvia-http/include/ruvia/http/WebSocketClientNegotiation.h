#pragma once

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/HttpClientResponseHead.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace ruvia {
struct WebSocketClientDeflateOffer final {
    bool enabled{false};
    bool serverNoContextTakeover{true};
    bool clientNoContextTakeover{true};
    std::optional<int> serverMaxWindowBits{};
    bool offerClientMaxWindowBits{true};
    std::optional<int> clientMaxWindowBits{};
};
struct WebSocketClientNegotiationConfigView final {
    std::span<const HttpHeaderView> headers{};
    std::span<const std::string_view> subprotocols{};
    WebSocketClientDeflateOffer deflate{};
};
enum class WebSocketClientNegotiationError : std::uint8_t {
    kResponseStatus,
    kStreamClosed,
    kSubprotocol,
    kExtensions,
    kForbiddenField,
};
struct WebSocketClientNegotiationResultView final {
    // Borrows the supplied response headers.
    std::string_view selectedSubprotocol{};
    WebSocketCompression compression{};
};

// HTTP-version-independent RFC 6455/7692 client negotiation owner. Configuration
// is copied into resource once. It supplies request fields to HTTP/1 Upgrade or
// RFC 8441/9220 Extended CONNECT, and validates the server's exact selection.
// The resource must outlive this object and prepared field sections. All header
// views produced below expire with this owner; response views expire with the head.
class WebSocketClientNegotiation final {
public:
    WebSocketClientNegotiation(WebSocketClientNegotiationConfigView config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    WebSocketClientNegotiation(WebSocketClientNegotiation&&) noexcept = default;
    WebSocketClientNegotiation& operator=(WebSocketClientNegotiation&&) = delete;
    WebSocketClientNegotiation(const WebSocketClientNegotiation&) = delete;
    WebSocketClientNegotiation& operator=(const WebSocketClientNegotiation&) = delete;
    [[nodiscard]] std::span<const HttpHeader> requestFields() const& noexcept {
        return fields_;
    }
    std::span<const HttpHeader> requestFields() const&& = delete;
    [[nodiscard]] std::expected<WebSocketClientNegotiationResultView, WebSocketClientNegotiationError>
    validateFields(std::span<const HttpHeader> fields) const;
    [[nodiscard]] Http2RequestHeadSubmitResult submitHttp2Request(Http2Connection& connection,
        std::string_view scheme, std::string_view authority, std::string_view target) const;
    [[nodiscard]] std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encodeHttp3Request(
        std::string_view scheme, std::string_view authority, std::string_view target,
        bool peerEnableConnectProtocol, Http3FieldSectionLimits limits = {}) const;
    [[nodiscard]] std::expected<WebSocketClientNegotiationResultView, WebSocketClientNegotiationError>
    validateResponse(const HttpClientResponseHead& head, bool streamOpen) const;
    [[nodiscard]] std::expected<WebSocketClientNegotiationResultView, WebSocketClientNegotiationError>
    validateResponse(const Http3MessageHead& head, bool streamOpen) const;

private:
    std::pmr::memory_resource* resource_;
    WebSocketClientDeflateOffer deflate_;
    std::pmr::vector<std::pmr::string> protocols_;
    std::pmr::vector<HttpHeader> fields_;
};
}  // namespace ruvia
