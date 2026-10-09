#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/Http1WebSocketClientHandshake.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/WebSocketClientNegotiation.h"

#include "test_harness.h"
#include "websocket/HttpWebSocketPermessageDeflate.h"

namespace {
void add(ruvia::Http3MessageHead& head, std::string_view name, std::string_view value) {
    head.headers.emplace_back(name, value, head.headers.get_allocator().resource());
}
}  // namespace
RUVIA_TEST(websocket_client_http2_tunnel_peer_fin_keeps_local_send_open) {
    auto client = ruvia::Http2Connection::client();
    auto server = ruvia::Http2Connection::server();
    std::string wire(server.pendingOutput());
    (void)server.consumeOutput(wire.size());
    RUVIA_CHECK(client.feed(wire) != ruvia::Http2FeedResult::kProtocolFailure);
    ruvia::WebSocketClientNegotiation negotiation({});
    const auto submitted = negotiation.submitHttp2Request(client, "https", "example.com", "/chat");
    RUVIA_CHECK(submitted.submitted() != nullptr);
    wire.assign(client.pendingOutput());
    (void)client.consumeOutput(wire.size());
    RUVIA_CHECK(server.feed(wire) != ruvia::Http2FeedResult::kProtocolFailure);
    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead());
    if (!request || !request->requestHead()) {
        return;
    }
    const auto validation = ruvia::validateHttp2WebSocketHandshake(server, 1, request->requestHead()->request());
    const auto handshake = server.submitWebSocketHandshake(1, request->requestHead()->request(), validation);
    RUVIA_CHECK(handshake.submitted() != nullptr);
    wire.assign(server.pendingOutput());
    (void)server.consumeOutput(wire.size());
    RUVIA_CHECK(client.feed(wire) != ruvia::Http2FeedResult::kProtocolFailure);
    auto response = client.nextEvent();
    RUVIA_CHECK(response && response->responseHead());
    RUVIA_CHECK(server.submitData(1, "peer", ruvia::Http2EndStream::kEndStream) == ruvia::Http2DataSubmitStatus::kAccepted);
    wire.assign(server.pendingOutput());
    (void)server.consumeOutput(wire.size());
    RUVIA_CHECK(client.feed(wire) != ruvia::Http2FeedResult::kProtocolFailure);
    auto data = client.nextEvent();
    RUVIA_CHECK(data && data->tunnelData());
    auto end = client.nextEvent();
    RUVIA_CHECK(end && end->tunnelEnd());
    data.reset();
    RUVIA_CHECK(!client.streamAborted(1));
    RUVIA_CHECK(client.submitData(1, "local", ruvia::Http2EndStream::kKeepOpen) == ruvia::Http2DataSubmitStatus::kAccepted);
    wire.assign(client.pendingOutput());
    (void)client.consumeOutput(wire.size());
    RUVIA_CHECK(server.feed(wire) != ruvia::Http2FeedResult::kProtocolFailure);
    auto localData = server.nextEvent();
    RUVIA_CHECK(localData && localData->tunnelData());
    if (localData && localData->tunnelData()) {
        RUVIA_CHECK_EQ(localData->tunnelData()->bytes(), std::string_view("local"));
    }
    RUVIA_CHECK(client.submitData(1, {}, ruvia::Http2EndStream::kEndStream) == ruvia::Http2DataSubmitStatus::kAccepted);
    wire.assign(client.pendingOutput());
    (void)client.consumeOutput(wire.size());
    RUVIA_CHECK(server.feed(wire) != ruvia::Http2FeedResult::kProtocolFailure);
    auto localEnd = server.nextEvent();
    RUVIA_CHECK(localEnd && localEnd->tunnelEnd());
    RUVIA_CHECK(client.submitData(1, "late", ruvia::Http2EndStream::kKeepOpen) == ruvia::Http2DataSubmitStatus::kClosed);
}

RUVIA_TEST(websocket_client_extended_connect_requires_peer_setting_and_emits_protocol) {
    const std::array protocols{std::string_view("chat")};
    ruvia::WebSocketClientNegotiation client({.subprotocols = protocols, .deflate = {.enabled = true}});
    auto disabled = client.encodeHttp3Request("https", "example.com", "/chat", false);
    RUVIA_CHECK(!(disabled.index() == 0));
    RUVIA_CHECK(std::get<1>(disabled).kind == ruvia::Http3ClientRequestHeadError::kConnectProtocolDisabled);
    auto encoded = client.encodeHttp3Request("https", "example.com", "/chat", true);
    RUVIA_CHECK((encoded.index() == 0));
    auto head = ruvia::decodeHttp3MessageHead(std::get<0>(encoded).fieldSection, ruvia::Http3MessageHeadKind::kRequest);
    RUVIA_CHECK((head.index() == 0));
    RUVIA_CHECK_EQ(std::string_view(std::get<0>(head).protocol), std::string_view("websocket"));
    RUVIA_CHECK_EQ(std::string_view(std::get<0>(head).method), std::string_view("CONNECT"));
    RUVIA_CHECK(!std::get<0>(head).contentLength);
    auto connection = ruvia::Http2Connection::client();
    auto submitted = client.submitHttp2Request(connection, "https", "example.com", "/chat");
    RUVIA_CHECK(submitted.failure() != nullptr);
    auto server = ruvia::Http2Connection::server();
    const std::string peerSettings(server.pendingOutput());
    RUVIA_CHECK(connection.feed(peerSettings) != ruvia::Http2FeedResult::kProtocolFailure);
    submitted = client.submitHttp2Request(connection, "https", "example.com", "/chat");
    RUVIA_CHECK(submitted.submitted() != nullptr);
    const std::string requestWire(connection.pendingOutput());
    RUVIA_CHECK(server.feed(requestWire) != ruvia::Http2FeedResult::kProtocolFailure);
    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead());
    if (request && request->requestHead()) {
        const auto validation = ruvia::validateHttp2WebSocketHandshake(server,
            request->requestHead()->streamId(), request->requestHead()->request());
        RUVIA_CHECK(validation.accepted() != nullptr);
    }
}
RUVIA_TEST(websocket_client_negotiation_accepts_asymmetric_context_and_window_parameters) {
    const std::array protocols{std::string_view("chat")};
    ruvia::WebSocketClientNegotiation client({.subprotocols = protocols, .deflate = {.enabled = true, .serverNoContextTakeover = false, .clientNoContextTakeover = false, .serverMaxWindowBits = 10, .clientMaxWindowBits = 12}});
    ruvia::Http3MessageHead head(std::pmr::get_default_resource());
    head.status = 200;
    add(head, "sec-websocket-protocol", "chat");
    add(head, "sec-websocket-extensions", "permessage-deflate; client_no_context_takeover; server_max_window_bits=10; client_max_window_bits=12");
    auto accepted = client.validateResponse(head, true);
    RUVIA_CHECK((accepted.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(accepted).selectedSubprotocol, std::string_view("chat"));
    RUVIA_CHECK(!std::get<0>(accepted).compression.serverNoContextTakeover);
    RUVIA_CHECK(std::get<0>(accepted).compression.clientNoContextTakeover);
    RUVIA_CHECK_EQ(std::get<0>(accepted).compression.serverMaxWindowBits.value(), 10);
    RUVIA_CHECK_EQ(std::get<0>(accepted).compression.clientMaxWindowBits.value(), 12);
    RUVIA_CHECK(!(client.validateResponse(head, false).index() == 0));
}
RUVIA_TEST(websocket_client_negotiation_rejects_duplicate_unknown_or_unoffered_selection) {
    ruvia::WebSocketClientNegotiation client({.deflate = {.enabled = true, .serverMaxWindowBits = 10, .offerClientMaxWindowBits = false}});
    for (auto extension : std::array{"permessage-deflate", "permessage-deflate; server_no_context_takeover; server_max_window_bits=11", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; client_max_window_bits=9", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; unknown", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; server_max_window_bits=10"}) {
        ruvia::Http3MessageHead head(std::pmr::get_default_resource());
        head.status = 200;
        add(head, "sec-websocket-extensions", extension);
        RUVIA_CHECK(!(client.validateResponse(head, true).index() == 0));
    }
    // A server may add its own window bound without an offer.
    ruvia::WebSocketClientNegotiation noServerWindowClient({.deflate = {.enabled = true, .serverNoContextTakeover = false, .clientNoContextTakeover = false}});
    {
        ruvia::Http3MessageHead head(std::pmr::get_default_resource());
        head.status = 200;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        RUVIA_CHECK((noServerWindowClient.validateResponse(head, true).index() == 0));
    }
    // The client window in an offer is a hint; the server may choose a larger one.
    ruvia::WebSocketClientNegotiation limitedClientWindow({.deflate = {.enabled = true, .serverNoContextTakeover = false, .clientNoContextTakeover = false, .clientMaxWindowBits = 9}});
    {
        ruvia::Http3MessageHead head(std::pmr::get_default_resource());
        head.status = 200;
        add(head, "sec-websocket-extensions", "permessage-deflate; client_max_window_bits=10");
        RUVIA_CHECK((limitedClientWindow.validateResponse(head, true).index() == 0));
    }
    // A client takeover preference in an offer is a hint.
    ruvia::WebSocketClientNegotiation requireClientNoTakeover({.deflate = {.enabled = true, .serverNoContextTakeover = false, .clientNoContextTakeover = true}});
    {
        ruvia::Http3MessageHead head(std::pmr::get_default_resource());
        head.status = 200;
        add(head, "sec-websocket-extensions", "permessage-deflate");
        RUVIA_CHECK((requireClientNoTakeover.validateResponse(head, true).index() == 0));
    }
    ruvia::Http3MessageHead head(std::pmr::get_default_resource());
    head.status = 200;
    add(head, "sec-websocket-protocol", "unoffered");
    RUVIA_CHECK(!(client.validateResponse(head, true).index() == 0));
}
RUVIA_TEST(websocket_h1_client_handshake_negotiates_deflate) {
    constexpr std::array<std::uint8_t, 16> nonce{'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};
    ruvia::Http1WebSocketClientHandshake client({.nonce = nonce, .deflate = {.enabled = true}});
    std::array<char, 2048> buffer{};
    auto prepared = client.prepareRequest(ruvia::HttpOriginView::https({.host = "example.com"}), "/", buffer);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    RUVIA_CHECK((prepared.prepared()->head().find("sec-websocket-extensions: permessage-deflate") != std::string_view::npos));
    ruvia::Http1ClientResponseParser parser(prepared.prepared()->exchangeState());
    auto response = parser.parse("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n\r\n");
    RUVIA_CHECK(response.parsed() != nullptr);
    auto accepted = client.validateResponse(*response.parsed());
    RUVIA_CHECK((accepted.index() == 0));
    RUVIA_CHECK(std::get<0>(accepted).compression.enabled);
}
RUVIA_TEST(websocket_deflate_asymmetric_context_and_eight_bit_window_round_trip) {
    ruvia::detail::WebSocketDeflate sender(6, true, false, 8, 15);
    ruvia::detail::WebSocketDeflate receiver(6, false, true, 15, 8);
    const std::string payload(2048, 'a');
    for (int i = 0; i < 3; ++i) {
        std::pmr::string compressed, decoded;
        RUVIA_CHECK(sender.compress(payload, compressed));
        RUVIA_CHECK(receiver.decompress(compressed, decoded, ruvia::ProtocolByteLimit::limited(4096)) == ruvia::detail::WebSocketInflateResult::kOk);
        RUVIA_CHECK_EQ(std::string_view(decoded), std::string_view(payload));
    }
}

namespace {
struct NegotiationResource final : std::pmr::memory_resource {
    std::size_t live{0};
    bool fail{false};
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // Fail request field storage, not noexcept debug iterator bookkeeping.
        if (fail && bytes >= 32) {
            throw std::bad_alloc();
        }
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        live += bytes;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        live -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace
RUVIA_TEST(websocket_client_negotiation_preserves_owned_offers_and_retained_results_across_operations) {
    NegotiationResource resource;
    {
        const std::array<std::string_view, 1> protocols{"channel.protocol.v1"};
        ruvia::WebSocketClientNegotiation negotiation({.subprotocols = protocols, .deflate = {.enabled = true}}, &resource);
        const auto retained = negotiation.encodeHttp3Request("https", "example.test", "/socket", true);
        RUVIA_CHECK((retained.index() == 0));
        const std::vector<char> snapshot(std::get<0>(retained).fieldSection.begin(), std::get<0>(retained).fieldSection.end());
        const auto baseline = resource.live;
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto head = negotiation.encodeHttp3Request("https", "example.test", "/other", true);
                RUVIA_CHECK((head.index() == 0));
            }
            RUVIA_CHECK_EQ(resource.live, baseline);
            RUVIA_CHECK(std::equal(snapshot.begin(), snapshot.end(), std::get<0>(retained).fieldSection.begin(), std::get<0>(retained).fieldSection.end()));
        }
        resource.fail = true;
        bool threw = false;
        try {
            (void)negotiation.encodeHttp3Request("https", "example.test", "/allocation", true);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        resource.fail = false;
        RUVIA_CHECK_EQ(resource.live, baseline);
    }
    RUVIA_CHECK_EQ(resource.live, 0u);
    {
        // Preparing an offer without sending it owns no transport or hidden job.
        ruvia::WebSocketClientNegotiation discarded({.deflate = {.enabled = true}}, &resource);
        RUVIA_CHECK(resource.live > 0);
    }
    RUVIA_CHECK_EQ(resource.live, 0u);
}
