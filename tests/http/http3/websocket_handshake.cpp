#include <array>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/Http3ServerRequest.h"
#include "ruvia/http/Http3WebSocketHandshake.h"
#include "ruvia/http/HttpResponse.h"

#include "test_harness.h"

namespace {

struct CapturedFields final {
    std::vector<std::string> names;
    std::vector<std::string> values;
};

bool collectField(void* opaque, ruvia::Http3FieldSectionFieldView field) {
    auto& captured = *static_cast<CapturedFields*>(opaque);
    captured.names.emplace_back(field.name);
    captured.values.emplace_back(field.value);
    return true;
}

bool makeRequest(std::span<const ruvia::Http3FieldSectionFieldView> fields,
    std::pmr::memory_resource* resource,
    std::optional<ruvia::Http3ServerRequest>& request) {
    auto section = ruvia::encodeHttp3FieldSection(fields, resource);
    if (!section) {
        return false;
    }
    auto head = ruvia::decodeHttp3MessageHead(
        *section, ruvia::Http3MessageHeadKind::kRequest, resource);
    if (!head) {
        return false;
    }
    request.emplace(*head, resource, resource);
    return true;
}

std::size_t countHeader(const CapturedFields& fields, std::string_view name) {
    std::size_t count = 0;
    for (const auto& candidate : fields.names) {
        count += candidate == name;
    }
    return count;
}

std::string_view findHeader(const CapturedFields& fields, std::string_view name) {
    for (std::size_t i = 0; i < fields.names.size(); ++i) {
        if (fields.names[i] == name) {
            return fields.values[i];
        }
    }
    return {};
}

}  // namespace

RUVIA_TEST(http3_websocket_handshake_builds_canonical_extended_connect_response) {
    std::pmr::monotonic_buffer_resource resource;
    const std::array requestFields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"origin", "https://example.test"},
        ruvia::Http3FieldSectionFieldView{"cookie", "session=abc"},
        ruvia::Http3FieldSectionFieldView{"authorization", "Bearer token"},
        ruvia::Http3FieldSectionFieldView{"x-client", "preserved"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-protocol", "chat, superchat"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-extensions", "permessage-deflate"},
    };
    std::optional<ruvia::Http3ServerRequest> request;
    RUVIA_CHECK(makeRequest(requestFields, &resource, request));
    if (!request) {
        return;
    }

    constexpr std::array<std::string_view, 1> supported{"superchat"};
    const std::array responseHeaders{
        ruvia::HttpHeaderView{"content-type", "application/websocket"},
        ruvia::HttpHeaderView{"set-cookie", "first=1; Path=/"},
        ruvia::HttpHeaderView{"set-cookie", "second=2; Path=/"},
    };
    auto handshake = ruvia::makeHttp3WebSocketHandshake(request->request(),
        request->extendedConnectProtocol(), true, {
                                                      .supportedSubprotocols = supported,
                                                      .responseHeaders = responseHeaders,
                                                      .resource = &resource,
                                                      .deflate = {.enabled = true},
                                                      .date = "Tue, 15 Nov 1994 08:12:31 GMT",
                                                  });
    RUVIA_CHECK(handshake.has_value());
    if (!handshake) {
        return;
    }
    RUVIA_CHECK_EQ(handshake->subprotocol(), "superchat");
    RUVIA_CHECK(handshake->compression() != (ruvia::WebSocketCompression{}));

    const auto frame = ruvia::decodeHttp3Frame(handshake->headersFrame());
    RUVIA_CHECK(frame.has_value());
    if (!frame) {
        return;
    }
    RUVIA_CHECK(frame->type == static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders));
    RUVIA_CHECK_EQ(frame->encodedBytes, handshake->headersFrame().size());
    CapturedFields captured;
    const auto decoded = ruvia::decodeHttp3FieldSection(frame->payload, collectField, &captured);
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK_EQ(findHeader(captured, ":status"), "200");
    RUVIA_CHECK_EQ(findHeader(captured, "date"), "Tue, 15 Nov 1994 08:12:31 GMT");
    RUVIA_CHECK_EQ(findHeader(captured, "sec-websocket-protocol"), "superchat");
    RUVIA_CHECK(!findHeader(captured, "sec-websocket-extensions").empty());
    RUVIA_CHECK_EQ(countHeader(captured, "set-cookie"), 2U);
    RUVIA_CHECK_EQ(findHeader(captured, "content-type"), "application/websocket");
    for (const auto prohibited : {"content-length", "connection", "upgrade",
             "sec-websocket-accept", "sec-websocket-key"}) {
        RUVIA_CHECK_EQ(countHeader(captured, prohibited), 0U);
    }
}

RUVIA_TEST(http3_websocket_handshake_rejects_invalid_version_framing_and_half_closed_stream) {
    std::pmr::monotonic_buffer_resource resource;
    const std::array validFields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"},
    };
    std::optional<ruvia::Http3ServerRequest> valid;
    RUVIA_CHECK(makeRequest(validFields, &resource, valid));
    if (!valid) {
        return;
    }
    RUVIA_CHECK(ruvia::validateHttp3WebSocketHandshake(
        valid->request(), valid->extendedConnectProtocol(), true)
            .has_value());
    const auto halfClosed = ruvia::validateHttp3WebSocketHandshake(
        valid->request(), valid->extendedConnectProtocol(), false);
    RUVIA_CHECK(!halfClosed);
    RUVIA_CHECK(halfClosed.error().kind() ==
                ruvia::Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
    const auto invalidProtocolError = halfClosed.error().protocolError();
    RUVIA_CHECK_EQ(invalidProtocolError.status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(std::string_view(invalidProtocolError.what()), "invalid WebSocket handshake");
    ruvia::HttpResponse invalidResponse;
    halfClosed.error().applyRequiredResponseHeaders(invalidResponse);
    RUVIA_CHECK(!invalidResponse.header("Sec-WebSocket-Version").has_value());
    const auto wrongProtocol = ruvia::validateHttp3WebSocketHandshake(
        valid->request(), "not-websocket", true);
    RUVIA_CHECK(!wrongProtocol);
    if (!wrongProtocol) {
        RUVIA_CHECK(wrongProtocol.error().kind() ==
                    ruvia::Http3WebSocketHandshakeFailure::Kind::kInvalidRequest);
    }

    const std::array wrongVersionFields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "12"},
    };
    std::optional<ruvia::Http3ServerRequest> wrongVersion;
    RUVIA_CHECK(makeRequest(wrongVersionFields, &resource, wrongVersion));
    if (wrongVersion) {
        const auto result = ruvia::validateHttp3WebSocketHandshake(
            wrongVersion->request(), wrongVersion->extendedConnectProtocol(), true);
        RUVIA_CHECK(!result);
        RUVIA_CHECK(result.error().kind() ==
                    ruvia::Http3WebSocketHandshakeFailure::Kind::kUnsupportedVersion);
        const auto protocolError = result.error().protocolError();
        RUVIA_CHECK_EQ(protocolError.status(), ruvia::http_status::kBadRequest);
        RUVIA_CHECK_EQ(std::string_view(protocolError.what()), "unsupported WebSocket version");

        const auto handshake = ruvia::makeHttp3WebSocketHandshake(
            wrongVersion->request(), wrongVersion->extendedConnectProtocol(), true);
        RUVIA_CHECK(!handshake);
        if (!handshake) {
            RUVIA_CHECK(handshake.error().kind() ==
                        ruvia::Http3WebSocketHandshakeFailure::Kind::kUnsupportedVersion);
        }

        ruvia::HttpResponse errorResponse;
        errorResponse.status(protocolError.status());
        errorResponse.header("Sec-WebSocket-Version", "8");
        errorResponse.header("X-Application-Error", "retained");
        result.error().applyRequiredResponseHeaders(errorResponse);
        RUVIA_CHECK_EQ(errorResponse.header("Sec-WebSocket-Version").value_or(""), "13");
        RUVIA_CHECK_EQ(errorResponse.header("X-Application-Error").value_or(""), "retained");
    }

    const std::array framedFields{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"},
        ruvia::Http3FieldSectionFieldView{"content-length", "0"},
    };
    std::optional<ruvia::Http3ServerRequest> framed;
    RUVIA_CHECK(makeRequest(framedFields, &resource, framed));
    if (framed) {
        const auto result = ruvia::validateHttp3WebSocketHandshake(
            framed->request(), framed->extendedConnectProtocol(), true);
        RUVIA_CHECK(!result);
    }
}

RUVIA_TEST(http3_websocket_handshake_rejects_duplicate_version_and_h1_handshake_fields) {
    std::pmr::monotonic_buffer_resource resource;
    const std::array duplicateVersion{
        ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
        ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
        ruvia::Http3FieldSectionFieldView{":scheme", "https"},
        ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
        ruvia::Http3FieldSectionFieldView{":path", "/socket"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"},
        ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"},
    };
    std::optional<ruvia::Http3ServerRequest> duplicate;
    RUVIA_CHECK(makeRequest(duplicateVersion, &resource, duplicate));
    if (duplicate) {
        RUVIA_CHECK(!ruvia::validateHttp3WebSocketHandshake(
            duplicate->request(), duplicate->extendedConnectProtocol(), true));
    }

    for (const auto forbidden : {"sec-websocket-key", "sec-websocket-accept", "content-length"}) {
        const std::array base{
            ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
            ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
            ruvia::Http3FieldSectionFieldView{":scheme", "https"},
            ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
            ruvia::Http3FieldSectionFieldView{":path", "/socket"},
            ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"},
            ruvia::Http3FieldSectionFieldView{
                forbidden, forbidden == std::string_view("content-length") ? "0" : "x"},
        };
        std::optional<ruvia::Http3ServerRequest> request;
        RUVIA_CHECK(makeRequest(base, &resource, request));
        if (request) {
            RUVIA_CHECK(!ruvia::validateHttp3WebSocketHandshake(
                request->request(), request->extendedConnectProtocol(), true));
        }
    }

    for (const auto forbidden : {"connection", "upgrade"}) {
        const std::array fields{
            ruvia::Http3FieldSectionFieldView{":method", "CONNECT"},
            ruvia::Http3FieldSectionFieldView{":protocol", "websocket"},
            ruvia::Http3FieldSectionFieldView{":scheme", "https"},
            ruvia::Http3FieldSectionFieldView{":authority", "example.test"},
            ruvia::Http3FieldSectionFieldView{":path", "/socket"},
            ruvia::Http3FieldSectionFieldView{"sec-websocket-version", "13"},
            ruvia::Http3FieldSectionFieldView{forbidden, "websocket"},
        };
        auto section = ruvia::encodeHttp3FieldSection(fields, &resource);
        RUVIA_CHECK(section.has_value());
        if (section) {
            const auto head = ruvia::decodeHttp3MessageHead(
                *section, ruvia::Http3MessageHeadKind::kRequest, &resource);
            RUVIA_CHECK(!head);
        }
    }
}
