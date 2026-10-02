#include "ruvia/http/WebSocketClientNegotiation.h"

#include <array>
#include <charconv>
#include <stdexcept>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/HttpHeaderAccess.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/websocket/handshake/WebSocketSubprotocolSet.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"

namespace ruvia {
namespace {
using Error = WebSocketClientNegotiationError;
bool equal(std::string_view a, std::string_view b) noexcept {
    return detail::httpAsciiEqualsIgnoreCase(a, b);
}
bool reserved(std::string_view name) noexcept {
    return name.empty() || name.front() == ':' || equal(name, "host") || equal(name, "connection") || equal(name, "upgrade") || equal(name, "keep-alive") || equal(name, "proxy-connection") || equal(name, "transfer-encoding") || equal(name, "content-length") || equal(name, "trailer") || equal(name, "expect") || equal(name, "sec-websocket-key") || equal(name, "sec-websocket-accept") || equal(name, "sec-websocket-version") || equal(name, "sec-websocket-protocol") || equal(name, "sec-websocket-extensions");
}
std::optional<WebSocketCompression> parseResponseExtension(std::string_view text, WebSocketClientDeflateOffer offer) {
    if (!offer.enabled) {
        return {};
    }
    auto first = detail::httpFindUnquotedDelimiter(text, 0, ';');
    if (!equal(detail::httpTrimOws(text.substr(0, first)), "permessage-deflate")) {
        return {};
    }
    WebSocketCompression result{.enabled = true, .serverNoContextTakeover = false, .clientNoContextTakeover = false};
    std::uint8_t seen = 0;
    for (auto start = first; start < text.size();) {
        ++start;
        auto end = detail::httpFindUnquotedDelimiter(text, start, ';');
        auto part = detail::httpTrimOws(text.substr(start, end - start));
        auto equals = part.find('=');
        auto name = detail::httpTrimOws(part.substr(0, equals));
        auto value = equals == std::string_view::npos ? std::string_view{} : detail::httpTrimOws(part.substr(equals + 1));
        std::uint8_t bit = 0;
        if (equal(name, "server_no_context_takeover")) {
            bit = 1;
            if (equals != std::string_view::npos) {
                return {};
            }
            result.serverNoContextTakeover = true;
        } else if (equal(name, "client_no_context_takeover")) {
            bit = 2;
            if (equals != std::string_view::npos) {
                return {};
            }
            result.clientNoContextTakeover = true;
        } else if (equal(name, "server_max_window_bits")) {
            bit = 4;
            result.serverMaxWindowBits = detail::webSocketDeflateWindowBits(value);
            if (!result.serverMaxWindowBits) {
                return {};
            }
        } else if (equal(name, "client_max_window_bits")) {
            bit = 8;
            result.clientMaxWindowBits = detail::webSocketDeflateWindowBits(value);
            if (!result.clientMaxWindowBits || (!offer.offerClientMaxWindowBits && !offer.clientMaxWindowBits)) {
                return {};
            }
        } else {
            return {};
        }
        if (seen & bit) {
            return {};
        }
        seen |= bit;
        start = end;
    }
    if (offer.serverNoContextTakeover && !result.serverNoContextTakeover) {
        return {};
    }
    if (offer.serverMaxWindowBits && (!result.serverMaxWindowBits || *result.serverMaxWindowBits > *offer.serverMaxWindowBits)) {
        return {};
    }

    return result;
}
template <class Fields, class Name, class Value>
std::expected<WebSocketClientNegotiationResultView, Error> validate(const Fields& fields, Name name, Value value,
    const std::pmr::vector<std::pmr::string>& protocols, WebSocketClientDeflateOffer offer, bool extended) {
    WebSocketClientNegotiationResultView result;
    std::size_t selected = 0, extensions = 0;
    for (const auto& field : fields) {
        auto n = name(field);
        auto v = value(field);
        if (equal(n, "sec-websocket-protocol")) {
            ++selected;
            result.selectedSubprotocol = detail::httpTrimOws(v);
            bool found = false;
            for (const auto& protocol : protocols) {
                found = found || protocol == result.selectedSubprotocol;
            }
            if (selected > 1 || !found) {
                return std::unexpected(Error::kSubprotocol);
            }
        } else if (equal(n, "sec-websocket-extensions")) {
            ++extensions;
            if (extensions > 1) {
                return std::unexpected(Error::kExtensions);
            }
            auto negotiation = parseResponseExtension(detail::httpTrimOws(v), offer);
            if (!negotiation) {
                return std::unexpected(Error::kExtensions);
            }
            result.compression = *negotiation;
        } else if (extended && (equal(n, "sec-websocket-accept") || equal(n, "sec-websocket-key") || equal(n, "upgrade") || equal(n, "connection"))) {
            return std::unexpected(Error::kForbiddenField);
        }
    }
    return result;
}
}  // namespace
WebSocketClientNegotiation::WebSocketClientNegotiation(WebSocketClientNegotiationConfigView config, std::pmr::memory_resource* resource)
    : resource_(resource ? resource : std::pmr::get_default_resource()),
      deflate_(config.deflate),
      protocols_(resource_),
      fields_(resource_) {
    auto validWindow = [](std::optional<int> v) { return !v || (*v >= 8 && *v <= 15); };
    if (!validWindow(deflate_.serverMaxWindowBits) || !validWindow(deflate_.clientMaxWindowBits) || (deflate_.clientMaxWindowBits && !deflate_.offerClientMaxWindowBits)) {
        throw std::invalid_argument("invalid WebSocket client deflate offer");
    }
    detail::WebSocketSubprotocolSet seen;
    std::pmr::string joined(resource_);
    std::size_t headerBytes = 0;
    for (auto p : config.subprotocols) {
        if (!seen.append(p)) {
            throw std::invalid_argument("invalid WebSocket subprotocol");
        }
        protocols_.emplace_back(p);
        if (!joined.empty()) {
            joined.append(", ");
        }
        if (joined.size() > kMaxHttpHeaderBytes || p.size() > kMaxHttpHeaderBytes - joined.size()) {
            throw std::invalid_argument("WebSocket subprotocol field too large");
        }
        joined.append(p);
    }
    auto append = [&](std::string_view name, std::string_view value) {
        if (headerBytes > kMaxHttpHeaderBytes || kMaxHttpHeaderBytes - headerBytes < 4 ||
            name.size() > kMaxHttpHeaderBytes - headerBytes - 4 || value.size() > kMaxHttpHeaderBytes - headerBytes - 4 - name.size()) {
            throw std::invalid_argument("WebSocket client fields too large");
        }
        headerBytes += name.size() + value.size() + 4;
        std::pmr::string normalizedName(name, resource_);
        for (char& character : normalizedName) {
            character = static_cast<char>(httpAsciiToLower(static_cast<unsigned char>(character)));
        }
        fields_.push_back(detail::HttpHeaderAccess::make(std::move(normalizedName), std::pmr::string(value, resource_)));
    };
    if (config.headers.size() > kMaxHttpHeaderFields - 3) {
        throw std::invalid_argument("too many WebSocket client headers");
    }
    for (const auto& header : config.headers) {
        if (reserved(header.name()) || !isValidHttpHeaderName(header.name()) || !isValidHttpHeaderValue(header.value())) {
            throw std::invalid_argument("invalid WebSocket client header");
        }
        append(header.name(), header.value());
    }
    append("Sec-WebSocket-Version", "13");
    if (!joined.empty()) {
        append("Sec-WebSocket-Protocol", joined);
    }
    if (deflate_.enabled) {
        WebSocketCompression parameters{.enabled = true, .serverNoContextTakeover = deflate_.serverNoContextTakeover, .clientNoContextTakeover = deflate_.clientNoContextTakeover, .serverMaxWindowBits = deflate_.serverMaxWindowBits, .clientMaxWindowBits = deflate_.clientMaxWindowBits};
        auto encoded = detail::webSocketCompressionExtension(parameters);
        std::pmr::string offer(encoded.view(), resource_);
        if (deflate_.offerClientMaxWindowBits && !deflate_.clientMaxWindowBits) {
            offer.append("; client_max_window_bits");
        }
        append("Sec-WebSocket-Extensions", offer);
    }
}
std::expected<WebSocketClientNegotiationResultView, Error> WebSocketClientNegotiation::validateFields(std::span<const HttpHeader> fields) const {
    return validate(fields, [](const auto& field) { return field.name(); }, [](const auto& field) { return field.value(); }, protocols_, deflate_, false);
}
Http2RequestHeadSubmitResult WebSocketClientNegotiation::submitHttp2Request(Http2Connection& connection, std::string_view scheme, std::string_view authority, std::string_view target) const {
    std::pmr::vector<HttpHeaderView> headers(resource_);
    for (const auto& f : fields_) {
        headers.emplace_back(f.name(), f.value());
    }
    return connection.submitRequestHead(Http2ExtendedConnectRequestHeadView{.protocol = "websocket", .scheme = scheme, .authority = authority, .target = target, .headers = headers});
}
std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> WebSocketClientNegotiation::encodeHttp3Request(std::string_view scheme, std::string_view authority, std::string_view target, bool peerEnableConnectProtocol, Http3FieldSectionLimits limits) const {
    if (scheme != "https" && scheme != "http") {
        return std::unexpected(Http3ClientRequestHeadFailure{Http3ClientRequestHeadError::kInvalidTarget});
    }
    std::pmr::vector<Http3FieldSectionFieldView> fields(resource_);
    for (const auto& field : fields_) {
        fields.push_back({field.name(), field.value()});
    }
    return encodeHttp3ClientRequestHead({.method = "CONNECT", .scheme = scheme, .authority = authority, .path = target, .fields = fields, .protocol = "websocket", .peerEnableConnectProtocol = peerEnableConnectProtocol}, limits, resource_);
}
std::expected<WebSocketClientNegotiationResultView, Error> WebSocketClientNegotiation::validateResponse(const HttpClientResponseHead& head, bool streamOpen) const {
    if (head.status().value() < 200 || head.status().value() >= 300) {
        return std::unexpected(Error::kResponseStatus);
    }
    if (!streamOpen) {
        return std::unexpected(Error::kStreamClosed);
    }
    return validate(head.headers(), [](const auto& field) { return field.name(); }, [](const auto& field) { return field.value(); }, protocols_, deflate_, true);
}
std::expected<WebSocketClientNegotiationResultView, Error> WebSocketClientNegotiation::validateResponse(const Http3MessageHead& head, bool streamOpen) const {
    if (head.status < 200 || head.status >= 300) {
        return std::unexpected(Error::kResponseStatus);
    }
    if (!streamOpen) {
        return std::unexpected(Error::kStreamClosed);
    }
    return validate(head.headers, [](const auto& field) -> std::string_view { return field.name; }, [](const auto& field) -> std::string_view { return field.value; }, protocols_, deflate_, true);
}
}  // namespace ruvia
