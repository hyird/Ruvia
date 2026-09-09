#include "ruvia/http/Http1WebSocketClientHandshake.h"

#include <array>
#include <stdexcept>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/detail/client/Http1ClientRequestHeaders.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/util/HttpBase64.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h"
#include "ruvia/http/detail/websocket/handshake/WebSocketSubprotocolSet.h"

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
      subprotocolHeader_(resource_),
      subprotocolRanges_(resource_),
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
    subprotocolRanges_.reserve(options.subprotocols.size());
    for (auto protocol : options.subprotocols) {
        if (!subprotocolHeader_.empty()) {
            subprotocolHeader_.append(", ");
        }
        const auto offset = subprotocolHeader_.size();
        subprotocolHeader_.append(protocol);
        subprotocolRanges_.push_back({offset, protocol.size()});
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
    headers.emplace_back("Sec-WebSocket-Version", "13");
    if (!subprotocolHeader_.empty()) {
        headers.emplace_back("Sec-WebSocket-Protocol", subprotocolHeader_);
    }
    if (!userAgent_.empty()) {
        headers.emplace_back("User-Agent", userAgent_);
    }
    return Http1ClientRequestWriter({.resource = resource_})
        .prepare(origin, {.method = "GET", .target = target, .headers = headers}, headBuffer);
}

std::expected<Http1WebSocketClientHandshakeResultView, Http1WebSocketClientHandshakeError>
Http1WebSocketClientHandshake::validateResponse(const Http1ParsedClientResponseHead& response) const {
    if (response.plan().protocolUpgrade() == nullptr ||
        response.head().status() != http_status::kSwitchingProtocols) {
        return std::unexpected(Http1WebSocketClientHandshakeError::kResponseStatus);
    }
    detail::WebSocketAcceptKey expected{};
    detail::encodeWebSocketAccept(expected, key_);
    std::size_t accepts = 0, protocols = 0, extensions = 0;
    bool acceptMatches = false, hasUpgrade = false, hasConnection = false;
    std::string_view selected;
    for (const auto& header : response.head().headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Accept")) {
            ++accepts;
            acceptMatches = detail::httpTrimOws(header.value()) ==
                            std::string_view(expected.data(), expected.size());
        } else if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            hasUpgrade = hasUpgrade || detail::httpHasToken(header.value(), "websocket");
        } else if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Connection")) {
            hasConnection = hasConnection || detail::httpHasToken(header.value(), "upgrade");
        } else if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Protocol")) {
            ++protocols;
            selected = detail::httpTrimOws(header.value());
        } else if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Extensions")) {
            ++extensions;
        }
    }
    if (accepts != 1 || !acceptMatches) {
        return std::unexpected(Http1WebSocketClientHandshakeError::kAccept);
    }
    if (!hasUpgrade) {
        return std::unexpected(Http1WebSocketClientHandshakeError::kUpgrade);
    }
    if (!hasConnection) {
        return std::unexpected(Http1WebSocketClientHandshakeError::kConnection);
    }
    if (extensions != 0) {
        return std::unexpected(Http1WebSocketClientHandshakeError::kExtensions);
    }
    bool offered = protocols == 0;
    const std::string_view offeredHeader = subprotocolHeader_;
    for (auto range : subprotocolRanges_) {
        offered = offered || offeredHeader.substr(range.first, range.second) == selected;
    }
    if (protocols > 1 || !offered) {
        return std::unexpected(Http1WebSocketClientHandshakeError::kSubprotocol);
    }
    return Http1WebSocketClientHandshakeResultView{selected};
}
}  // namespace ruvia
