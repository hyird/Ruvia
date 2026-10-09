#include "ruvia/http/Http3WebSocketHandshake.h"

#include <algorithm>
#include <limits>
#include <memory_resource>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"

#include "server/HttpDateCache.h"
#include "websocket/HttpWebSocketHandshakeFields.h"
#include "websocket/WebSocketServerNegotiation.h"

namespace ruvia {
namespace {

std::pmr::memory_resource* normalizedResource(std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

bool headerNameEquals(std::string_view name, std::string_view expected) noexcept {
    return httpAsciiEqualsIgnoreCase(name, expected);
}

}  // namespace

HttpProtocolError Http3WebSocketHandshakeFailure::protocolError() const noexcept {
    switch (kind_) {
        case Kind::kInvalidRequest:
            return HttpProtocolError(http_status::kBadRequest, "invalid WebSocket handshake");
        case Kind::kUnsupportedVersion:
            return HttpProtocolError(http_status::kBadRequest, "unsupported WebSocket version");
    }
    return HttpProtocolError(http_status::kBadRequest, "invalid WebSocket handshake");
}

void Http3WebSocketHandshakeFailure::applyRequiredResponseHeaders(HttpResponse& response) const {
    if (kind_ == Kind::kUnsupportedVersion) {
        response.header_stable_view("Sec-WebSocket-Version", "13");
    }
}

std::variant<std::monostate, Http3WebSocketHandshakeFailure> validateHttp3WebSocketHandshake(
    const HttpRequest& request, std::string_view protocol, bool streamOpen) noexcept {
    if (!streamOpen || request.knownMethod() != HttpKnownMethod::kConnect ||
        request.protocolVersion() != HttpProtocolVersion::kHttp3 ||
        !httpAsciiEqualsIgnoreCase(protocol, "websocket")) {
        return Http3WebSocketHandshakeFailure(
            Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
    }

    std::size_t versionCount = 0;
    std::string_view version;
    for (const auto& header : request.headers()) {
        const auto name = header.name();
        if (headerNameEquals(name, "content-length") ||
            headerNameEquals(name, "sec-websocket-key") ||
            headerNameEquals(name, "sec-websocket-accept") ||
            headerNameEquals(name, "connection") || headerNameEquals(name, "upgrade")) {
            return Http3WebSocketHandshakeFailure(
                Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
        }
        if (headerNameEquals(name, "sec-websocket-version")) {
            ++versionCount;
            version = header.value();
        }
    }
    if (versionCount != 1 || !detail::webSocketSubprotocolOffersValid(request) ||
        !detail::webSocketExtensionOffersValid(request)) {
        return Http3WebSocketHandshakeFailure(
            Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
    }
    if (version != "13") {
        return Http3WebSocketHandshakeFailure(
            Http3WebSocketHandshakeFailure::Kind::kUnsupportedVersion);
    }
    return {};
}

std::variant<Http3WebSocketHandshake, Http3WebSocketHandshakeFailure>
makeHttp3WebSocketHandshake(const HttpRequest& request, std::string_view protocol,
    bool streamOpen, Http3WebSocketHandshakeOptions options) {
    const auto validation = validateHttp3WebSocketHandshake(request, protocol, streamOpen);
    if ((validation.index() != 0)) {
        return std::get<1>(validation);
    }

    auto* resource = normalizedResource(options.resource);
    auto negotiation = detail::makeWebSocketServerNegotiation(request, {
                                                                           .supportedSubprotocols = options.supportedSubprotocols,
                                                                           .responseHeaders = options.responseHeaders,
                                                                           .resource = resource,
                                                                           .deflate = options.deflate,
                                                                       });

    Http3WebSocketHandshake result(resource);
    result.subprotocol_.assign(negotiation.subprotocol());
    result.compression_ = negotiation.compression();

    const auto applicationHeaders = negotiation.responseHeaders();
    const bool applicationDate = std::ranges::any_of(applicationHeaders,
        [](const HttpHeader& header) { return headerNameEquals(header.name(), "date"); });
    const auto date = options.date.empty() ? detail::cachedDateValue() : options.date;
    if (!date.empty() && !isValidHttpHeaderValue(date)) {
        return Http3WebSocketHandshakeFailure(
            Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
    }

    std::pmr::vector<Http3FieldSectionFieldView> fields(resource);
    fields.reserve(3 + static_cast<std::size_t>(!negotiation.subprotocol().empty()) +
                   static_cast<std::size_t>(!negotiation.extensions().empty()) + applicationHeaders.size());
    fields.emplace_back(":status", "200");
    if (!date.empty() && !applicationDate) {
        fields.emplace_back("date", date);
    }
    if (!negotiation.subprotocol().empty()) {
        fields.emplace_back("sec-websocket-protocol", negotiation.subprotocol());
    }
    if (!negotiation.extensions().empty()) {
        fields.emplace_back("sec-websocket-extensions", negotiation.extensions());
    }
    for (const auto& header : applicationHeaders) {
        fields.emplace_back(header.name(), header.value());
    }

    auto section = encodeHttp3FieldSection(fields, resource);
    if ((section.index() != 0)) {
        return Http3WebSocketHandshakeFailure(
            Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
    }
    constexpr auto headerCapacity = 2 * kHttp3VarIntMaxBytes;
    if (std::get<0>(section).size() > std::numeric_limits<std::size_t>::max() - headerCapacity) {
        return Http3WebSocketHandshakeFailure(
            Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
    }
    result.headersFrame_.resize(headerCapacity + std::get<0>(section).size());
    const auto encoded = encodeHttp3Frame(result.headersFrame_,
        static_cast<std::uint64_t>(Http3FrameType::kHeaders), std::get<0>(section));
    if ((encoded.index() != 0)) {
        return Http3WebSocketHandshakeFailure(
            Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
    }
    result.headersFrame_.resize(std::get<0>(encoded));
    return result;
}

}  // namespace ruvia
