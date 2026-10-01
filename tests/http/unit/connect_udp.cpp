#include <array>
#include <string>

#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpDatagram.h"

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
