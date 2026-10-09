#include "ruvia/http/Http1WebSocketClientHandshake.h"

#include <array>
#include <stdexcept>
#include <variant>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"

#include "client/Http1ClientRequestHeaders.h"
#include "util/HttpBase64.h"
#include "websocket/HttpWebSocketAcceptKey.h"
#include "websocket/WebSocketSubprotocolSet.h"

namespace ruvia {
namespace {
bool isReservedHandshakeHeader(std::string_view name) noexcept {
    constexpr std::string_view names[] = {"host", "connection", "upgrade",
        "sec-websocket-key", "sec-websocket-version", "sec-websocket-protocol",
        "sec-websocket-extensions", "content-length"};
    for (auto candidate : names) {
        if (detail::httpAsciiEqualsIgnoreCase(name, candidate)) {
            return true;
        }
    }
    return false;
}
}  // namespace

void Http1WebSocketClientHandshake::validateConfiguration(
    std::span<const HttpHeaderView> headers, std::span<const std::string_view> subprotocols,
    std::string_view userAgent) {
    for (const auto& header : headers) {
        if (isReservedHandshakeHeader(header.name())) {
            throw std::invalid_argument("invalid or reserved WebSocket handshake header");
        }
    }
    RequestHeaderFacts facts;
    Http1ClientRequestPrepareError error{};
    if (!analyzeHeaders(headers, facts, error)) {
        throw std::invalid_argument(std::string(http1ClientRequestPrepareErrorMessage(error)));
    }
    detail::WebSocketSubprotocolSet seen;
    for (auto protocol : subprotocols) {
        if (!seen.append(protocol)) {
            throw std::invalid_argument("invalid WebSocket subprotocol");
        }
    }
    if (!userAgent.empty()) {
        const std::array generated{HttpHeaderView{"User-Agent", userAgent}};
        if (!analyzeHeaders(generated, facts, error)) {
            throw std::invalid_argument(std::string(http1ClientRequestPrepareErrorMessage(error)));
        }
    }
}

Http1WebSocketClientHandshake::Http1WebSocketClientHandshake(
    Http1WebSocketClientHandshakeConfigView options, std::pmr::memory_resource* resource)
    : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      key_(resource_),
      negotiation_({.subprotocols = options.subprotocols, .deflate = options.deflate}, resource_),

      headers_(resource_),
      userAgent_(options.userAgent, resource_) {
    validateConfiguration(options.headers, options.subprotocols, options.userAgent);
    std::array<char, 24> encoded{};
    detail::encodeHttpBase64(encoded.data(), options.nonce);
    key_.assign(encoded.data(), 24);
    headers_.reserve(options.headers.size());
    for (auto header : options.headers) {
        headers_.emplace_back(header.name(), header.value(), resource_);
    }
}

Http1ClientRequestPrepareResult Http1WebSocketClientHandshake::prepareRequest(
    const HttpOriginView& origin, std::string_view target, std::span<char> headBuffer) const {
    std::pmr::vector<HttpHeaderView> headers(resource_);
    headers.reserve(headers_.size() + 6);
    for (const auto& header : headers_) {
        headers.emplace_back(header.name, header.value);
    }
    headers.emplace_back("Upgrade", "websocket");
    headers.emplace_back("Connection", "Upgrade");
    headers.emplace_back("Sec-WebSocket-Key", key_);
    for (const auto& field : negotiation_.requestFields()) {
        headers.emplace_back(field.name(), field.value());
    }
    if (!userAgent_.empty()) {
        headers.emplace_back("User-Agent", userAgent_);
    }
    return Http1ClientRequestWriter({.resource = resource_})
        .prepare(origin, {.method = "GET", .target = target, .headers = headers}, headBuffer);
}

std::variant<Http1WebSocketClientHandshakeResultView, Http1WebSocketClientHandshakeError>
Http1WebSocketClientHandshake::validateResponse(const Http1ParsedClientResponseHead& response) const {
    if (response.plan().protocolUpgrade() == nullptr ||
        response.head().status() != http_status::kSwitchingProtocols) {
        return Http1WebSocketClientHandshakeError::kResponseStatus;
    }
    detail::WebSocketAcceptKey expected{};
    detail::encodeWebSocketAccept(expected, key_);
    std::size_t accepts = 0;
    bool acceptMatches = false, hasUpgrade = false, hasConnection = false;

    for (const auto& header : response.head().headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Accept")) {
            ++accepts;
            acceptMatches = detail::httpTrimOws(header.value()) ==
                            std::string_view(expected.data(), expected.size());
        } else if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            hasUpgrade = hasUpgrade || detail::httpHasToken(header.value(), "websocket");
        } else if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Connection")) {
            hasConnection = hasConnection || detail::httpHasToken(header.value(), "upgrade");
        }
    }
    if (accepts != 1 || !acceptMatches) {
        return Http1WebSocketClientHandshakeError::kAccept;
    }
    if (!hasUpgrade) {
        return Http1WebSocketClientHandshakeError::kUpgrade;
    }
    if (!hasConnection) {
        return Http1WebSocketClientHandshakeError::kConnection;
    }

    const auto negotiation = negotiation_.validateFields(response.head().headers());
    if ((negotiation.index() != 0)) {
        return std::get<1>(negotiation) == WebSocketClientNegotiationError::kSubprotocol ? Http1WebSocketClientHandshakeError::kSubprotocol : Http1WebSocketClientHandshakeError::kExtensions;
    }
    return Http1WebSocketClientHandshakeResultView{std::get<0>(negotiation).selectedSubprotocol, std::get<0>(negotiation).compression};
}
}  // namespace ruvia
