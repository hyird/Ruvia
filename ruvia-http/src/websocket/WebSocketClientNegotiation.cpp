#include "ruvia/http/WebSocketClientNegotiation.h"

#include <array>
#include <charconv>
#include <stdexcept>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"

#include "HttpHeaderAccess.h"
#include "websocket/HttpWebSocketPermessageDeflate.h"
#include "websocket/WebSocketSubprotocolSet.h"

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
std::variant<WebSocketClientNegotiationResultView, Error> validate(const Fields& fields, Name name, Value value,
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
                return Error::kSubprotocol;
            }
        } else if (equal(n, "sec-websocket-extensions")) {
            ++extensions;
            if (extensions > 1) {
                return Error::kExtensions;
            }
            auto negotiation = parseResponseExtension(detail::httpTrimOws(v), offer);
            if (!negotiation) {
                return Error::kExtensions;
            }
            result.compression = *negotiation;
        } else if (extended && (equal(n, "sec-websocket-accept") || equal(n, "sec-websocket-key") || equal(n, "upgrade") || equal(n, "connection"))) {
            return Error::kForbiddenField;
        }
    }
    return result;
}
}  // namespace
namespace {
template <class Protocol, class Field>
void validate_and_visit_config(const WebSocketClientNegotiationConfigView& config,
    Protocol&& protocol, Field&& field) {
    auto valid_window = [](std::optional<int> window_bits) {
        return !window_bits || (*window_bits >= 8 && *window_bits <= 15);
    };
    if (!valid_window(config.deflate.serverMaxWindowBits) || !valid_window(config.deflate.clientMaxWindowBits) ||
        (config.deflate.clientMaxWindowBits && !config.deflate.offerClientMaxWindowBits)) {
        throw std::invalid_argument("invalid WebSocket client deflate offer");
    }
    detail::WebSocketSubprotocolSet seen;
    std::size_t joined_bytes = 0;
    for (const auto protocol_view : config.subprotocols) {
        if (!seen.append(protocol_view)) {
            throw std::invalid_argument("invalid WebSocket subprotocol");
        }
        if (joined_bytes != 0) {
            if (joined_bytes > kMaxHttpHeaderBytes - 2) {
                throw std::invalid_argument("WebSocket subprotocol field too large");
            }
            joined_bytes += 2;
        }
        if (protocol_view.size() > kMaxHttpHeaderBytes - joined_bytes) {
            throw std::invalid_argument("WebSocket subprotocol field too large");
        }
        joined_bytes += protocol_view.size();
        protocol(protocol_view);
    }
    std::size_t header_bytes = 0;
    auto append = [&](std::string_view name, std::string_view value, std::size_t value_bytes) {
        if (header_bytes > kMaxHttpHeaderBytes || kMaxHttpHeaderBytes - header_bytes < 4 ||
            name.size() > kMaxHttpHeaderBytes - header_bytes - 4 ||
            value_bytes > kMaxHttpHeaderBytes - header_bytes - 4 - name.size()) {
            throw std::invalid_argument("WebSocket client fields too large");
        }
        header_bytes += name.size() + value_bytes + 4;
        field(name, value, value_bytes);
    };
    if (config.headers.size() > kMaxHttpHeaderFields - 3) {
        throw std::invalid_argument("too many WebSocket client headers");
    }
    for (const auto& header : config.headers) {
        if (reserved(header.name()) || !isValidHttpHeaderName(header.name()) || !isValidHttpHeaderValue(header.value())) {
            throw std::invalid_argument("invalid WebSocket client header");
        }
        append(header.name(), header.value(), header.value().size());
    }
    append("Sec-WebSocket-Version", "13", 2);
    if (!config.subprotocols.empty()) {
        append("Sec-WebSocket-Protocol", {}, joined_bytes);
    }
    if (config.deflate.enabled) {
        WebSocketCompression parameters{.enabled = true,
            .serverNoContextTakeover = config.deflate.serverNoContextTakeover,
            .clientNoContextTakeover = config.deflate.clientNoContextTakeover,
            .serverMaxWindowBits = config.deflate.serverMaxWindowBits,
            .clientMaxWindowBits = config.deflate.clientMaxWindowBits};
        const auto encoded = detail::webSocketCompressionExtension(parameters);
        constexpr std::size_t k_client_window_bits_suffix_bytes = sizeof("; client_max_window_bits") - 1;
        const std::size_t offer_bytes = encoded.view().size() +
                                        ((config.deflate.offerClientMaxWindowBits && !config.deflate.clientMaxWindowBits)
                                                ? k_client_window_bits_suffix_bytes
                                                : 0);
        append("Sec-WebSocket-Extensions", encoded.view(), offer_bytes);
    }
}
}  // namespace
void WebSocketClientNegotiation::validate_configuration(WebSocketClientNegotiationConfigView config) {
    validate_and_visit_config(config, [](std::string_view) {}, [](std::string_view, std::string_view, std::size_t) {});
}
WebSocketClientNegotiation::WebSocketClientNegotiation(WebSocketClientNegotiationConfigView config, std::pmr::memory_resource* resource)
    : resource_(resource ? resource : std::pmr::get_default_resource()),
      deflate_(config.deflate),
      protocols_(resource_),
      fields_(resource_) {
    std::pmr::string joined(resource_);
    validate_and_visit_config(config, [&](std::string_view protocol_view) {
            protocols_.emplace_back(protocol_view);
            if (!joined.empty()) {
                joined.append(", ");
            }
            joined.append(protocol_view); }, [&](std::string_view name, std::string_view value, std::size_t value_bytes) {
            std::pmr::string materialized(value, resource_);
            if (name == "Sec-WebSocket-Protocol") {
                materialized = std::move(joined);
            } else if (name == "Sec-WebSocket-Extensions" && value_bytes > value.size()) {
                materialized.append("; client_max_window_bits");
            }
            std::pmr::string normalized_name(name, resource_);
            for (char& character : normalized_name) {
                character = static_cast<char>(httpAsciiToLower(static_cast<unsigned char>(character)));
            }
            fields_.push_back(detail::HttpHeaderAccess::make(
                std::move(normalized_name), std::move(materialized))); });
}
std::variant<WebSocketClientNegotiationResultView, Error> WebSocketClientNegotiation::validateFields(std::span<const HttpHeader> fields) const {
    return validate(fields, [](const auto& field) { return field.name(); }, [](const auto& field) { return field.value(); }, protocols_, deflate_, false);
}
Http2RequestHeadSubmitResult WebSocketClientNegotiation::submitHttp2Request(Http2Connection& connection, std::string_view scheme, std::string_view authority, std::string_view target) const {
    std::pmr::vector<HttpHeaderView> headers(resource_);
    for (const auto& f : fields_) {
        headers.emplace_back(f.name(), f.value());
    }
    return connection.submitRequestHead(Http2ExtendedConnectRequestHeadView{.protocol = "websocket", .scheme = scheme, .authority = authority, .target = target, .headers = headers});
}
std::variant<Http3ClientRequestHead, Http3ClientRequestHeadFailure> WebSocketClientNegotiation::encodeHttp3Request(std::string_view scheme, std::string_view authority, std::string_view target, bool peerEnableConnectProtocol, Http3FieldSectionLimits limits) const {
    if (scheme != "https" && scheme != "http") {
        return Http3ClientRequestHeadFailure{Http3ClientRequestHeadError::kInvalidTarget};
    }
    std::pmr::vector<Http3FieldSectionFieldView> fields(resource_);
    for (const auto& field : fields_) {
        fields.push_back({field.name(), field.value()});
    }
    return encodeHttp3ClientRequestHead({.method = "CONNECT", .scheme = scheme, .authority = authority, .path = target, .fields = fields, .protocol = "websocket", .peerEnableConnectProtocol = peerEnableConnectProtocol}, limits, resource_);
}
std::variant<WebSocketClientNegotiationResultView, Error> WebSocketClientNegotiation::validateResponse(const HttpClientResponseHead& head, bool streamOpen) const {
    if (head.status().value() < 200 || head.status().value() >= 300) {
        return Error::kResponseStatus;
    }
    if (!streamOpen) {
        return Error::kStreamClosed;
    }
    return validate(head.headers(), [](const auto& field) { return field.name(); }, [](const auto& field) { return field.value(); }, protocols_, deflate_, true);
}
std::variant<WebSocketClientNegotiationResultView, Error> WebSocketClientNegotiation::validateResponse(const Http3MessageHead& head, bool streamOpen) const {
    if (head.status < 200 || head.status >= 300) {
        return Error::kResponseStatus;
    }
    if (!streamOpen) {
        return Error::kStreamClosed;
    }
    return validate(head.headers, [](const auto& field) -> std::string_view { return field.name; }, [](const auto& field) -> std::string_view { return field.value; }, protocols_, deflate_, true);
}
}  // namespace ruvia
