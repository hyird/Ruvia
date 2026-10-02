#include <array>
#include <string>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpDatagram.h"
#include "ruvia/http/HttpResponseServer.h"

#include "test_harness.h"

RUVIA_TEST(http_connect_udp_default_template_roundtrip_and_target_rejection) {
    for (const auto host : {"example.test", "192.0.2.6", "2001:db8::42"}) {
        const auto path = ruvia::encodeHttpConnectUdpPath({.host = host, .port = 443});
        RUVIA_CHECK(path.has_value());
        const auto target = ruvia::parseHttpConnectUdpPath(*path);
        RUVIA_CHECK(target && target->host == host && target->port == 443);
    }
    RUVIA_CHECK(!ruvia::encodeHttpConnectUdpPath({.host = "fe80::1%eth0", .port = 80}));
    RUVIA_CHECK(!ruvia::encodeHttpConnectUdpPath({.host = "[::1]", .port = 80}));
    RUVIA_CHECK(!ruvia::encodeHttpConnectUdpPath({.host = "example.test", .port = 0}));
    RUVIA_CHECK(!ruvia::parseHttpConnectUdpPath("/.well-known/masque/udp/2001:db8::42/443/"));
    RUVIA_CHECK(!ruvia::parseHttpConnectUdpPath("/.well-known/masque/udp/host/65536/"));
}
RUVIA_TEST(http_connect_udp_both_upgrade_and_extended_connect_handshakes) {
    const std::array extended{ruvia::HttpHeaderView{"capsule-protocol", "?1;version=1"}};
    RUVIA_CHECK(ruvia::validateHttpConnectUdpRequest({.authority = "example.test", .path = "/proxy", .headers = extended}));
    RUVIA_CHECK(ruvia::validateHttpConnectUdpResponse(ruvia::HttpProtocolVersion::kHttp3, 200, extended));
    const std::array upgrade{ruvia::HttpHeaderView{"Host", "example.test"}, ruvia::HttpHeaderView{"Connection", "Upgrade"},
        ruvia::HttpHeaderView{"Upgrade", "connect-udp"}, ruvia::HttpHeaderView{"Capsule-Protocol", "?1"}};
    RUVIA_CHECK(ruvia::validateHttpConnectUdpRequest({.version = ruvia::HttpProtocolVersion::kHttp11, .method = "GET", .authority = "example.test", .path = "/proxy", .headers = upgrade}));
    RUVIA_CHECK(ruvia::validateHttpConnectUdpResponse(ruvia::HttpProtocolVersion::kHttp11, 101, upgrade));
    RUVIA_CHECK(!ruvia::validateHttpConnectUdpRequest({.authority = "example.test", .path = "/proxy", .headers = upgrade}));
    RUVIA_CHECK(!ruvia::validateHttpConnectUdpResponse(ruvia::HttpProtocolVersion::kHttp3, 400, extended));
    RUVIA_CHECK(!ruvia::parseHttpCapsuleProtocol("?1, ?0"));
    RUVIA_CHECK(!ruvia::parseHttpCapsuleProtocol("true"));
    const auto disabled = ruvia::parseHttpCapsuleProtocol("?0");
    RUVIA_CHECK(disabled && !*disabled);
}
RUVIA_TEST(http_datagram_session_checks_negotiation_context_size_and_half_close) {
    ruvia::HttpDatagramSession session({.http3StreamId = 4, .localH3Datagram = true, .peerH3Datagram = true, .quicDatagram = true, .maxQuicPayloadBytes = 1200});
    const std::string payload = "udp";
    const auto plan = session.prepareUdpDatagram(payload, ruvia::HttpDatagramTransport::kQuic);
    RUVIA_CHECK(plan && plan->prefixSize == 2 && plan->payload.data() == payload.data());
    std::string wire(plan->prefix.data(), plan->prefixSize);
    wire.append(payload);
    const auto received = session.receiveUdpDatagram(wire, ruvia::HttpDatagramTransport::kQuic);
    RUVIA_CHECK(received && received->has_value() && (*received)->payload.size() == 3);
    wire[1] = 2;
    RUVIA_CHECK(session.receiveUdpDatagram(wire, ruvia::HttpDatagramTransport::kQuic)->has_value() == false);
    const std::string large(1199, 'x');
    RUVIA_CHECK(!session.prepareUdpDatagram(large, ruvia::HttpDatagramTransport::kQuic));
    RUVIA_CHECK(session.prepareUdpDatagram(large, ruvia::HttpDatagramTransport::kCapsule));
    session.closeSend();
    session.closeReceive();
    RUVIA_CHECK(!session.prepareUdpDatagram(payload, ruvia::HttpDatagramTransport::kCapsule));
    RUVIA_CHECK(!session.receiveUdpDatagram(wire, ruvia::HttpDatagramTransport::kQuic)->has_value());
    ruvia::HttpDatagramSession capsule;
    RUVIA_CHECK(!capsule.quicDatagramsEnabled());
    RUVIA_CHECK(!capsule.prepareUdpDatagram(payload, ruvia::HttpDatagramTransport::kQuic));
    const std::string tooLarge(65528, 'x');
    RUVIA_CHECK(!capsule.prepareUdpDatagram(tooLarge, ruvia::HttpDatagramTransport::kCapsule));
}

RUVIA_TEST(http_connect_udp_response_preparation_owns_required_fields_and_http1_upgrade_framing) {
    for (const auto version : {ruvia::HttpProtocolVersion::kHttp11, ruvia::HttpProtocolVersion::kHttp2, ruvia::HttpProtocolVersion::kHttp3}) {
        ruvia::HttpResponse response;
        response.status(ruvia::http_status::kCreated);
        response.header("X-Proxy", "test");
        auto prepared = ruvia::prepareHttpConnectUdpResponse(std::move(response), version);
        RUVIA_CHECK(prepared.has_value());
        if (!prepared) {
            continue;
        }
        RUVIA_CHECK(prepared->header("capsule-protocol") == "?1");
        RUVIA_CHECK(prepared->header("x-proxy") == "test");
        if (version == ruvia::HttpProtocolVersion::kHttp11) {
            RUVIA_CHECK(prepared->status() == ruvia::http_status::kSwitchingProtocols);
            auto plan = ruvia::prepareHttp1ConnectUdpResponseHead(*prepared);
            RUVIA_CHECK(plan.has_value());
            if (plan) {
                ruvia::HttpResponseHeadBuffer head{std::pmr::polymorphic_allocator<char>{}};
                ruvia::appendHttp1ResponseHead(*prepared, head, *plan);
                RUVIA_CHECK(head.view().starts_with("HTTP/1.1 101 "));
                RUVIA_CHECK(head.view().find("Connection: Upgrade\r\n") != std::string_view::npos);
                RUVIA_CHECK(head.view().find("Content-Length:") == std::string_view::npos);
                RUVIA_CHECK(head.view().find("Transfer-Encoding:") == std::string_view::npos);
            }
        } else {
            RUVIA_CHECK(prepared->status() == ruvia::http_status::kCreated);
            RUVIA_CHECK(!prepared->header("connection") && !prepared->header("upgrade"));
        }
    }
    for (const auto field : {"Content-Length", "Transfer-Encoding", "Content-Type", "Content-Encoding", "Connection", "Upgrade", "Trailer"}) {
        ruvia::HttpResponse invalid;
        invalid.header(field, field == std::string_view("Content-Length") ? "0" : field == std::string_view("Content-Type") ? "application/octet-stream"
                                                                                                                            : "test");
        RUVIA_CHECK(!ruvia::prepareHttpConnectUdpResponse(std::move(invalid), ruvia::HttpProtocolVersion::kHttp3));
    }
    ruvia::HttpResponse disabled;
    disabled.header("Capsule-Protocol", "?0");
    RUVIA_CHECK(!ruvia::prepareHttpConnectUdpResponse(std::move(disabled), ruvia::HttpProtocolVersion::kHttp11));
    ruvia::HttpResponse content;
    content.body("forbidden");
    RUVIA_CHECK(!ruvia::prepareHttpConnectUdpResponse(std::move(content), ruvia::HttpProtocolVersion::kHttp2));
}

RUVIA_TEST(http_connect_udp_http1_request_writer_generates_upgrade_without_message_content) {
    ruvia::Http1ClientRequestWriter writer;
    std::array<char, 4096> buffer;
    const std::array fields{ruvia::HttpHeaderView{"Capsule-Protocol", "?1"}, ruvia::HttpHeaderView{"X-Proxy", "test"}};
    const auto origin = ruvia::HttpOriginView::https({.host = "[::1]", .port = 8443});
    auto result = writer.prepareConnectUdp(origin, "/udp", fields, buffer);
    RUVIA_CHECK(result.prepared());
    if (result.prepared()) {
        const auto head = result.prepared()->head();
        RUVIA_CHECK(head.starts_with("GET /udp HTTP/1.1\r\n"));
        RUVIA_CHECK(head.find("Host: [::1]:8443\r\n") != std::string_view::npos);
        RUVIA_CHECK(head.find("Upgrade: connect-udp\r\n") != std::string_view::npos);
        RUVIA_CHECK(head.find("Content-Length:") == std::string_view::npos);
        RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string_view::npos);
    }
    auto missing = writer.prepareConnectUdp(origin, "/udp", {}, buffer);
    RUVIA_CHECK(missing.failure());
    const std::array invalid{ruvia::HttpHeaderView{"Capsule-Protocol", "?0"}};
    auto disabled = writer.prepareConnectUdp(origin, "/udp", invalid, buffer);
    RUVIA_CHECK(disabled.failure());
}
