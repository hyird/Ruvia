#include "http2_connection_fixture.h"

#include "ruvia/http/detail/response/HttpResponseHeadersAccess.h"

// Http2Connection: the connection preface, SETTINGS, PING and the frame loop.

// The sans-I/O core produces a SETTINGS frame (stream 0) into its outbound buffer,
// and consumeOutput drains it. Exercises the core with zero asio / zero I/O.

namespace {

void addUncheckedHeader(ruvia::HttpResponse& response, std::string_view name, std::string_view value) {
    auto& headers = const_cast<ruvia::HttpResponseHeaders&>(response.headers());
    (void)ruvia::detail::HttpResponseHeadersAccess::add(headers, name, value, 0);
}

}  // namespace

RUVIA_TEST(http2_connection_begin_server_connection_emits_settings_once) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);

    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.wantsWrite());

    conn.beginConnection();

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), Http2LocalSettings::kFrameBytes + ruvia::detail::kHttp2WindowUpdateFrameBytes);
    RUVIA_CHECK(conn.wantsWrite());

    const auto header = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(header.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK_EQ(header.streamId, static_cast<std::uint32_t>(0));
    RUVIA_CHECK_EQ(header.length, Http2LocalSettings::kPayloadBytes);

    const auto window = ruvia::detail::http2ParseFrameHeader(out.substr(Http2LocalSettings::kFrameBytes, 9));
    RUVIA_CHECK_EQ(window.type, static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(window.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(ruvia::detail::http2WindowUpdateIncrement(out.substr(Http2LocalSettings::kFrameBytes + 9, 4)), Http2LocalSettings::kInitialWindowSize - static_cast<std::uint32_t>(ruvia::detail::kHttp2DefaultInitialWindowSize));

    conn.beginConnection();
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), out.size());

    conn.consumeOutput(out.size());
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.wantsWrite());
}

RUVIA_TEST(http2_connection_output_consumption_is_transactional) {
    Http2Connection connection(std::pmr::get_default_resource(), ruvia::detail::Http2Role::kServer);
    connection.beginConnection();
    const std::string original(connection.pendingOutput());
    RUVIA_CHECK(!original.empty());

    RUVIA_CHECK(connection.consumeOutput(original.size() + 1) == ruvia::detail::Http2OutputConsumeStatus::kOutOfRange);
    RUVIA_CHECK_EQ(connection.pendingOutput(), std::string_view(original));

    RUVIA_CHECK(connection.consumeOutput(1) == ruvia::detail::Http2OutputConsumeStatus::kPending);
    RUVIA_CHECK_EQ(connection.pendingOutput(), std::string_view(original).substr(1));
    RUVIA_CHECK(connection.consumeOutput(original.size() - 1) == ruvia::detail::Http2OutputConsumeStatus::kDrained);
    RUVIA_CHECK(connection.pendingOutput().empty());
    RUVIA_CHECK(!connection.wantsWrite());
}

RUVIA_TEST(http2_connection_begin_client_connection_prefixes_same_settings_once) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);

    client.beginConnection();
    const auto out = client.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), ruvia::detail::kHttp2ClientPreface.size() + Http2LocalSettings::kFrameBytes + ruvia::detail::kHttp2WindowUpdateFrameBytes);
    RUVIA_CHECK_EQ(out.substr(0, ruvia::detail::kHttp2ClientPreface.size()), ruvia::detail::kHttp2ClientPreface);

    const auto settingsOffset = ruvia::detail::kHttp2ClientPreface.size();
    const auto settings = ruvia::detail::http2ParseFrameHeader(out.substr(settingsOffset, 9));
    RUVIA_CHECK_EQ(settings.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK_EQ(settings.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(settings.length, Http2LocalSettings::kPayloadBytes);

    const auto firstSize = out.size();
    client.beginConnection();
    RUVIA_CHECK_EQ(client.pendingOutput().size(), firstSize);
}

RUVIA_TEST(http2_connection_outbound_extension_method_is_valid_wire_token) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);

    const auto extension = client.submitRegularRequestHead("PROPFIND", "https", "example.test", "/dav", {}, Http2RequestContent::none());
    RUVIA_CHECK(extension.submitted() != nullptr);
    const auto extensionStreamId = submittedRequestStreamId(extension);
    RUVIA_CHECK_EQ(extensionStreamId, std::uint32_t{1});
    const auto* stream = client.stream(extensionStreamId);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->requestMethod(), std::string_view("PROPFIND"));
        RUVIA_CHECK(stream->requestKnownMethod() == ruvia::HttpKnownMethod::kUnknown);
    }

    client.consumeOutput(client.pendingOutput().size());
    const auto malformed = client.submitRegularRequestHead("BAD METHOD", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(malformed.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(malformed) == Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(3) == nullptr);
}

RUVIA_TEST(http2_connection_feed_before_begin_retains_input_and_is_retryable) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);

    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
    const auto beforeBegin = server.feed(std::string_view(settings, sizeof(settings)));
    RUVIA_CHECK(beforeBegin == ruvia::detail::Http2FeedResult::kConnectionNotStarted);
    RUVIA_CHECK(server.pendingOutput().empty());
    RUVIA_CHECK(!server.receivedPeerSettings());
    RUVIA_CHECK(!server.nextEvent().has_value());

    beginPeerInput(server);
    const auto retried = server.feed(std::string_view(settings, sizeof(settings)));
    RUVIA_CHECK(retried == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(server.receivedPeerSettings());
}

// feed() drives the SETTINGS handshake with zero I/O: feed the peer's empty
// SETTINGS frame and the core must emit a SETTINGS ACK.

RUVIA_TEST(http2_connection_feed_settings_emits_ack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    beginPeerInput(conn);

    char frame[9];
    ruvia::detail::http2EncodeFrameHeader(frame, 0, Http2FrameType::kSettings, 0, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);

    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() >= 9);
    const auto ack = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
    RUVIA_CHECK_EQ(ack.length, static_cast<std::uint32_t>(0));
}

RUVIA_TEST(http2_connection_enable_push_validation_uses_peer_direction) {
    char frame[15];
    auto* out = ruvia::detail::http2WriteFrameHeader(frame, 6, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(out, ruvia::detail::Http2SettingId::kEnablePush, 1);
    RUVIA_CHECK_EQ(out, frame + sizeof(frame));

    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginPeerInput(client);
    const auto clientResult = client.feed(std::string_view(frame, sizeof(frame)));
    RUVIA_CHECK(clientResult == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(client.connectionError().has_value());
    const auto goaway = client.pendingOutput();
    const auto goawayHeader = ruvia::detail::http2ParseFrameHeader(goaway.substr(0, 9));
    RUVIA_CHECK_EQ(goawayHeader.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(goaway.data() + 13)), static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));

    Http2Connection server(&resource);
    beginPeerInput(server);
    const auto serverResult = server.feed(std::string_view(frame, sizeof(frame)));
    RUVIA_CHECK(serverResult == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.connectionError().has_value());
    const auto ack = ruvia::detail::http2ParseFrameHeader(server.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
}

// A non-SETTINGS first frame is a protocol error (GOAWAY emitted, feed reports error).

RUVIA_TEST(http2_connection_feed_rejects_non_settings_first_frame) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    beginPeerInput(conn);

    char frame[9];
    ruvia::detail::http2EncodeFrameHeader(frame, 0, Http2FrameType::kPing, 0, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError().has_value());
    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() >= 9);
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}

RUVIA_TEST(http2_connection_server_requires_client_magic_before_frames) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    server.beginConnection();
    server.consumeOutput(server.pendingOutput().size());

    char bytes[ruvia::detail::kHttp2ClientPreface.size()]{};
    ruvia::detail::http2EncodeFrameHeader(bytes, 0, Http2FrameType::kSettings, 0, 0);
    const auto result = server.feed(std::string_view(bytes, sizeof(bytes)));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(server.connectionError().has_value());
    RUVIA_CHECK(!server.receivedPeerSettings());
}

RUVIA_TEST(http2_connection_first_peer_settings_must_not_be_ack_for_either_role) {
    char ack[9];
    ruvia::detail::http2EncodeFrameHeader(ack, 0, Http2FrameType::kSettings, ruvia::detail::kHttp2FlagAck, 0);

    std::pmr::monotonic_buffer_resource resource;
    {
        Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
        beginPeerInput(client);
        const auto result = client.feed(std::string_view(ack, sizeof(ack)));
        RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(client.connectionError().has_value());
        RUVIA_CHECK(!client.receivedPeerSettings());
    }
    {
        Http2Connection server(&resource);
        beginPeerInput(server);
        const auto result = server.feed(std::string_view(ack, sizeof(ack)));
        RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(server.connectionError().has_value());
        RUVIA_CHECK(!server.receivedPeerSettings());
    }
}

RUVIA_TEST(http2_connection_settings_validation_is_atomic_across_entries) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    handshake(client);

    char frame[9 + 12];
    auto* out = ruvia::detail::http2WriteFrameHeader(frame, 12, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(out, ruvia::detail::Http2SettingId::kEnableConnectProtocol, 1);
    out = ruvia::detail::http2WriteSettingsEntry(out, ruvia::detail::Http2SettingId::kMaxFrameSize, 0);
    RUVIA_CHECK_EQ(out, frame + sizeof(frame));

    RUVIA_CHECK(client.feed(std::string_view(frame, sizeof(frame))) == Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(!client.peerExtendedConnectEnabled());
}

// After the handshake, a PING is echoed back with the ACK flag and the same payload.

RUVIA_TEST(http2_connection_feed_ping_echoes_ack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    const char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(ping + 9, data, 8);
    (void)conn.feed(std::string_view(ping, sizeof(ping)));

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(9 + 8));
    const auto ack = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kPing));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
    RUVIA_CHECK(out.substr(9, 8) == std::string_view(data, 8));
}

RUVIA_TEST(http2_connection_partial_frame_reports_need_more_until_complete) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    std::memcpy(ping + 9, "12345678", 8);

    constexpr std::size_t kFirstBytes = 12;  // full header + partial payload
    const auto partial = conn.feed(std::string_view(ping, kFirstBytes));
    RUVIA_CHECK(partial == ruvia::detail::Http2FeedResult::kNeedInput);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const auto complete = conn.feed(std::string_view(ping + kFirstBytes, sizeof(ping) - kFirstBytes));
    RUVIA_CHECK(complete == ruvia::detail::Http2FeedResult::kAccepted);
    const auto ack = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kPing));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
}

// A complete HEADERS frame (END_HEADERS + END_STREAM) decodes the request head and the
// sans-I/O core emits kMessageHead then kMessageEnd; the head is exposed via stream().

RUVIA_TEST(http2_connection_event_queue_is_optional_and_discriminated) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeGetRequest(block);
    const auto frame = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
    const auto result = conn.feed(std::string_view(frame.data(), frame.size()));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());

    const auto e1 = conn.nextEvent().value();
    RUVIA_CHECK(e1.kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(e1.messageBodyChunk() == nullptr);
    RUVIA_CHECK(e1.goaway() == nullptr);
    RUVIA_CHECK_EQ(e1.messageHead()->streamId(), static_cast<std::uint32_t>(1));
    const auto e2 = conn.nextEvent().value();
    RUVIA_CHECK(e2.kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(e2.messageHead() == nullptr);
    RUVIA_CHECK_EQ(e2.messageEnd()->streamId(), static_cast<std::uint32_t>(1));
    RUVIA_CHECK(!conn.nextEvent().has_value());

    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr);
    RUVIA_CHECK_EQ(s->requestMethod(), std::string_view("GET"));
    RUVIA_CHECK(s->requestKnownMethod() == ruvia::HttpKnownMethod::kGet);
}

RUVIA_TEST(http2_connection_feed_preserves_empty_non_http_path) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeRequest(block, "GET", "git+ssh", "", std::nullopt);
    const auto frame = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));

    RUVIA_CHECK(conn.feed(std::string_view(frame.data(), frame.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->hasPath());
        RUVIA_CHECK(stream->requestPath().empty());
    }
}

RUVIA_TEST(http2_connection_feed_rejects_empty_http_path) {
    std::pmr::monotonic_buffer_resource resource;
    constexpr std::string_view schemes[] = {"http", "HTTPS"};
    for (const auto scheme : schemes) {
        Http2Connection conn(&resource);
        handshake(conn);
        std::pmr::string block(&resource);
        encodeRequest(block, "GET", scheme, "", "example.test");
        const auto frame = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));

        RUVIA_CHECK(conn.feed(std::string_view(frame.data(), frame.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(!conn.connectionError().has_value());
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kStreamClosed);
        RUVIA_CHECK(!conn.nextEvent().has_value());
        const auto out = conn.pendingOutput();
        const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 9)), static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    }
}

RUVIA_TEST(http2_connection_accepts_non_http_userinfo_authority) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    std::pmr::string block(&resource);
    encodeRequest(block, "GET", "git+ssh", "/repository", "deploy:secret@example.test:9418");
    const auto request = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));

    RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.connectionError().has_value());
    RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    const auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->requestAuthority(), std::string_view("deploy:secret@example.test:9418"));
    }
}

RUVIA_TEST(http2_connection_rejects_http_userinfo_authority) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    std::pmr::string block(&resource);
    encodeRequest(block, "GET", "https", "/", "user@example.test");
    const auto request = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));

    RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.connectionError().has_value());
    RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kStreamClosed);
    RUVIA_CHECK(!server.nextEvent().has_value());
    const auto out = server.pendingOutput();
    const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 9)), static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_connection_asterisk_path_requires_options_and_accepts_authority) {
    std::pmr::monotonic_buffer_resource resource;
    {
        Http2Connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encodeRequest(block, "GET", "https", "*");
        const auto request = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));

        RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(!server.connectionError().has_value());
        RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kStreamClosed);
        RUVIA_CHECK(!server.nextEvent().has_value());
        const auto out = server.pendingOutput();
        const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 9)), static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    }
    {
        Http2Connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encodeRequest(block, "OPTIONS", "https", "*");
        const auto request = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));

        RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(!server.connectionError().has_value());
        RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kMessageHead);
        RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
        const auto* stream = server.stream(1);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            RUVIA_CHECK_EQ(stream->requestAuthority(), std::string_view("example.com"));
            RUVIA_CHECK_EQ(stream->requestPath(), std::string_view("*"));
        }
    }
    {
        Http2Connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encodeRequest(block, "OPTIONS", "https", "*", std::nullopt);
        const auto request = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));

        RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(!server.connectionError().has_value());
        RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kMessageHead);
        RUVIA_CHECK(server.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
        const auto* stream = server.stream(1);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            RUVIA_CHECK_EQ(stream->requestPath(), std::string_view("*"));
        }
    }
}

RUVIA_TEST(http2_connection_accepts_huffman_block_larger_than_decoded_field_section) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // obs-text is legal field-value data. HPACK byte 0xdc has a 28-bit Huffman
    // code, so this 20 KiB decoded value expands to 70 KiB on the wire while
    // remaining well below the advertised 64 KiB decoded field-section limit.
    constexpr unsigned char kExpandedByte = 0xdc;
    constexpr std::size_t kDecodedValueBytes = 20 * 1024;
    std::pmr::string block(&resource);
    encodeGetRequest(block);
    encodeRepeatedHuffmanHeader(block, "x-huffman-expanded", kExpandedByte, kDecodedValueBytes);
    RUVIA_CHECK(block.size() > ruvia::kMaxHttpHeaderBytes);

    std::size_t offset = 0;
    const auto firstBytes = std::min(block.size(), static_cast<std::size_t>(Http2LocalSettings::kMaxFrameSize));
    const auto first = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), firstBytes));
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    offset += firstBytes;

    while (offset < block.size()) {
        const auto fragmentBytes = std::min(block.size() - offset, static_cast<std::size_t>(Http2LocalSettings::kMaxFrameSize));
        const auto flags = offset + fragmentBytes == block.size() ? ruvia::detail::kHttp2FlagEndHeaders : std::uint8_t{0};
        const auto continuation = continuationFrame(&resource, 1, flags, std::string_view(block.data() + offset, fragmentBytes));
        RUVIA_CHECK(conn.feed(std::string_view(continuation.data(), continuation.size())) == Http2FeedResult::kAccepted);
        offset += fragmentBytes;
    }

    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->requestHeaderCount(), std::size_t{1});
    const auto expanded = stream->requestHeaderAt(0);
    RUVIA_CHECK_EQ(expanded.name, std::string_view("x-huffman-expanded"));
    RUVIA_CHECK_EQ(expanded.value.size(), kDecodedValueBytes);
    RUVIA_CHECK(std::all_of(expanded.value.begin(), expanded.value.end(), [](char byte) { return static_cast<unsigned char>(byte) == kExpandedByte; }));
}

// If a discarded field block exceeds the buffering budget, the core cannot satisfy
// RFC 9113's mandatory decompression step. The connection error is COMPRESSION_ERROR,
// not an application/load-shedding code that would imply HPACK remains usable.

RUVIA_TEST(http2_connection_undecodable_discarded_block_is_compression_error) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);
    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) == Http2SubmitStatus::kAccepted);
    conn.consumeOutput(conn.pendingOutput().size());

    const std::string fullFrame(ruvia::detail::kHttp2DefaultMaxFrameSize, '\0');
    const auto first = headersFrame(&resource, 1, 0, fullFrame);
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) == ruvia::detail::Http2FeedResult::kAccepted);
    constexpr auto kFullFrameCount = ruvia::detail::kMaxHttp2EncodedHeaderBlockBytes / ruvia::detail::kHttp2DefaultMaxFrameSize;
    static_assert(ruvia::detail::kMaxHttp2EncodedHeaderBlockBytes % ruvia::detail::kHttp2DefaultMaxFrameSize == 0);
    for (std::size_t i = 1; i < kFullFrameCount; ++i) {
        const auto continuation = continuationFrame(&resource, 1, 0, fullFrame);
        RUVIA_CHECK(conn.feed(std::string_view(continuation.data(), continuation.size())) == ruvia::detail::Http2FeedResult::kAccepted);
    }
    const auto overflow = continuationFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders, "x");
    RUVIA_CHECK(conn.feed(std::string_view(overflow.data(), overflow.size())) == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError().has_value());
    const auto out = conn.pendingOutput();
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 13)), static_cast<std::uint32_t>(Http2ErrorCode::kCompressionError));
    conn.unpinStream(1);
}

RUVIA_TEST(http2_connection_trace_rejects_declared_or_transferred_content) {
    {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encodeRequest(block, "TRACE", "https", "/diagnostic");
        HpackEncoder::encodeHeader(block, "content-length", "0");
        const auto head = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);

        bool sawProtocolError = false;
        while (const auto event = conn.nextEvent()) {
            RUVIA_CHECK(event->messageHead() == nullptr);
            RUVIA_CHECK(event->messageEnd() == nullptr);
            if (const auto* closed = event->streamClosed()) {
                sawProtocolError = closed->source() == Http2StreamCloseSource::kLocal && closed->error() == Http2ErrorCode::kProtocolError;
            }
        }
        RUVIA_CHECK(sawProtocolError);
        RUVIA_CHECK(conn.stream(1) == nullptr);
    }

    {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encodeRequest(block, "TRACE", "https", "/diagnostic");
        const auto head = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders, std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
        RUVIA_CHECK(!conn.nextEvent().has_value());

        const auto content = dataFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream, "body");
        RUVIA_CHECK(conn.feed(std::string_view(content.data(), content.size())) == Http2FeedResult::kAccepted);

        bool sawBody = false;
        bool sawProtocolError = false;
        while (const auto event = conn.nextEvent()) {
            sawBody = sawBody || event->messageBodyChunk() != nullptr;
            if (const auto* closed = event->streamClosed()) {
                sawProtocolError = closed->source() == Http2StreamCloseSource::kLocal && closed->error() == Http2ErrorCode::kProtocolError;
            }
        }
        RUVIA_CHECK(!sawBody);
        RUVIA_CHECK(sawProtocolError);
        RUVIA_CHECK(conn.stream(1) == nullptr);
    }
}

RUVIA_TEST(http2_connection_trace_allows_empty_terminal_framing) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeRequest(block, "TRACE", "https", "/diagnostic");
    HpackEncoder::encodeHeader(block, "expect", "100-continue");
    const auto head = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders, std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        const auto expectation = stream->expectationPlan(ruvia::detail::HttpUnsupportedExpectationPolicy::kReject);
        RUVIA_CHECK(expectation.noAction() != nullptr);
        RUVIA_CHECK(expectation.sendContinue() == nullptr);
    }

    const auto end = dataFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream, {});
    RUVIA_CHECK(conn.feed(std::string_view(end.data(), end.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(!conn.connectionError().has_value());
}

RUVIA_TEST(http2_connection_options_content_requires_valid_content_type) {
    const auto checkRejected = [&ruvia_ctx](std::string_view method, std::optional<std::string_view> contentType, std::optional<std::string_view> contentLength, bool endStream) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encodeRequest(block, method, "https", "/diagnostics");
        if (contentType.has_value()) {
            HpackEncoder::encodeHeader(block, "content-type", *contentType);
        }
        if (contentLength.has_value()) {
            HpackEncoder::encodeHeader(block, "content-length", *contentLength);
        }
        auto flags = ruvia::detail::kHttp2FlagEndHeaders;
        if (endStream) {
            flags |= ruvia::detail::kHttp2FlagEndStream;
        }
        const auto head = headersFrame(&resource, 1, flags, std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);

        bool sawProtocolError = false;
        while (const auto event = conn.nextEvent()) {
            RUVIA_CHECK(event->messageHead() == nullptr);
            RUVIA_CHECK(event->messageEnd() == nullptr);
            if (const auto* closed = event->streamClosed()) {
                sawProtocolError = closed->source() == Http2StreamCloseSource::kLocal && closed->error() == Http2ErrorCode::kProtocolError;
            }
        }
        RUVIA_CHECK(sawProtocolError);
        RUVIA_CHECK(conn.stream(1) == nullptr);
    };

    checkRejected("OPTIONS", std::nullopt, "0", true);
    checkRejected("OPTIONS", std::nullopt, std::nullopt, false);
    checkRejected("OPTIONS", "not a media type", "0", true);

    {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encodeRequest(block, "OPTIONS", "https", "/diagnostics");
        HpackEncoder::encodeHeader(block, "content-type", "application/json; charset=utf-8");
        HpackEncoder::encodeHeader(block, "content-length", "0");
        const auto head = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
        RUVIA_CHECK(!conn.nextEvent().has_value());
    }

    {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encodeRequest(block, "OPTIONS", "https", "/diagnostics");
        HpackEncoder::encodeHeader(block, "content-type", "application/octet-stream");
        const auto head = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders, std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);

        const auto body = dataFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream, "x");
        RUVIA_CHECK(conn.feed(std::string_view(body.data(), body.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageBodyChunk);
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
        RUVIA_CHECK(!conn.nextEvent().has_value());
        conn.releaseReceivedData(1);
    }

    {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encodeRequest(block, "options", "https", "/diagnostics");
        HpackEncoder::encodeHeader(block, "content-length", "0");
        const auto head = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
        RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
        RUVIA_CHECK(!conn.nextEvent().has_value());
    }
}

RUVIA_TEST(http2_connection_rejects_invalid_content_type_syntax) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeRequest(block, "POST", "https", "/items");
    HpackEncoder::encodeHeader(block, "content-type", "not a media type");
    HpackEncoder::encodeHeader(block, "content-length", "0");
    const auto head = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kStreamClosed);
    RUVIA_CHECK(!conn.nextEvent().has_value());
}

RUVIA_TEST(http2_connection_rejects_invalid_content_encoding_syntax) {
    const auto check = [&ruvia_ctx](std::string_view value, bool rejected) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encodeRequest(block, "POST", "https", "/items");
        HpackEncoder::encodeHeader(block, "content-encoding", value);
        HpackEncoder::encodeHeader(block, "content-length", "0");
        const auto head = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);

        bool sawHead = false;
        bool sawProtocolError = false;
        while (const auto event = conn.nextEvent()) {
            sawHead = sawHead || event->messageHead() != nullptr;
            if (const auto* closed = event->streamClosed()) {
                sawProtocolError = closed->source() == Http2StreamCloseSource::kLocal && closed->error() == Http2ErrorCode::kProtocolError;
            }
        }
        RUVIA_CHECK(sawProtocolError == rejected);
        RUVIA_CHECK(sawHead != rejected);
    };

    check("gzip;level=9", true);
    check("bad coding", true);
    check("gzip/deflate", true);
    check(", gzip,,", false);
    check("deflate", false);
    check("gzip, br", false);
}

RUVIA_TEST(http2_connection_feed_preserves_pending_events_and_retries_input) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const auto data = dataFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream, "body");
    RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) == ruvia::detail::Http2FeedResult::kAccepted);
    const auto outputBeforeRetry = conn.pendingOutput().size();

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    std::memcpy(ping + 9, "retry-me", 8);
    const auto blocked = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(blocked == ruvia::detail::Http2FeedResult::kEventsPending);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), outputBeforeRetry);

    const auto chunk = conn.nextEvent().value();
    RUVIA_CHECK(chunk.kind() == Http2EventKind::kMessageBodyChunk);
    RUVIA_CHECK_EQ(chunk.messageBodyChunk()->bytes(), std::string_view("body"));
    const auto stillBlocked = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(stillBlocked == ruvia::detail::Http2FeedResult::kEventsPending);
    RUVIA_CHECK_EQ(chunk.messageBodyChunk()->bytes(), std::string_view("body"));

    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    const auto retried = conn.feed(std::string_view(ping, sizeof(ping)));
    RUVIA_CHECK(retried == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), outputBeforeRetry + sizeof(ping));
    const auto ack = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(outputBeforeRetry, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kPing));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
    RUVIA_CHECK(!conn.nextEvent().has_value());
}

RUVIA_TEST(http2_connection_rejects_invalid_outbound_content_encoding_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto check = [&resource, &ruvia_ctx](std::string_view value, bool rejected) {
        Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const ruvia::HttpHeaderView contentEncoding[] = {{"content-encoding", value}};
        const auto result = client.submitRegularRequestHead("POST", "https", "example.test", "/upload", contentEncoding, Http2RequestContent::knownLength(1));
        const bool failed = result.submitted() == nullptr;
        RUVIA_CHECK(failed == rejected);
        if (rejected && failed) {
            RUVIA_CHECK(requestHeadSubmitError(result) == Http2RequestHeadSubmitError::kInvalidMessage);
            RUVIA_CHECK(client.pendingOutput().empty());
            RUVIA_CHECK(client.stream(1) == nullptr);
        }
    };

    check("gzip;level=9", true);
    check("bad coding", true);
    check("", true);
    check(",gzip", true);
    check("gzip,", true);
    check("deflate", false);
    check("gzip, br", false);
}

RUVIA_TEST(http2_connection_validates_outbound_cors_fields_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto checkRejected = [&resource, &ruvia_ctx](std::string_view name, std::string_view value) {
        Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const ruvia::HttpHeaderView header[] = {{name, value}};
        const auto rejected = client.submitRegularRequestHead("OPTIONS", "https", "example.test", "/resource", header, Http2RequestContent::none());
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(rejected) == Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    checkRejected("origin", "https://example.test/path");
    checkRejected("access-control-request-method", "GET, POST");
    checkRejected("access-control-request-headers", "x-good, bad header");

    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const ruvia::HttpHeaderView validHeaders[] = {
        {"origin", "https://first.test https://second.test"},
        {"access-control-request-method", "GET"},
        {"access-control-request-headers", "x-first"},
        {"access-control-request-headers", "x-second, x-third"},
    };
    const auto accepted = client.submitRegularRequestHead("OPTIONS", "https", "example.test", "/resource", validHeaders, Http2RequestContent::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
}

RUVIA_TEST(http2_connection_rejects_100_continue_without_following_content_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    const ruvia::HttpHeaderView expectContinue[] = {{"expect", "100-Continue"}};
    const ruvia::HttpHeaderView combinedExpectation[] = {{"expect", "extension, 100-Continue"}};

    const auto checkRegularRejected = [&resource, &ruvia_ctx](std::span<const ruvia::HttpHeaderView> headers, Http2RequestContent content) {
        Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const auto rejected = client.submitRegularRequestHead("POST", "https", "example.test", "/upload", headers, content);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(rejected) == Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);

        const auto accepted = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(accepted.submitted() != nullptr);
        RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
    };

    checkRegularRejected(expectContinue, Http2RequestContent::none());
    checkRegularRejected(expectContinue, Http2RequestContent::knownLength(0));
    checkRegularRejected(combinedExpectation, Http2RequestContent::knownLength(0));

    Http2Connection connectClient(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(connectClient);
    const auto rejectedConnect = connectClient.submitConnectRequestHead("example.test:443", expectContinue);
    RUVIA_CHECK(rejectedConnect.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(rejectedConnect) == Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(connectClient.pendingOutput().empty());
    RUVIA_CHECK(connectClient.stream(1) == nullptr);

    const auto checkAccepted = [&resource, &ruvia_ctx](std::span<const ruvia::HttpHeaderView> headers, Http2RequestContent content) {
        Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const auto accepted = client.submitRegularRequestHead("POST", "https", "example.test", "/upload", headers, content);
        RUVIA_CHECK(accepted.submitted() != nullptr);
        RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
        RUVIA_CHECK(!client.pendingOutput().empty());
    };

    checkAccepted(expectContinue, Http2RequestContent::knownLength(1));
    checkAccepted(expectContinue, Http2RequestContent::streaming());
    const ruvia::HttpHeaderView extensionExpectation[] = {{"expect", "extension"}};
    checkAccepted(extensionExpectation, Http2RequestContent::none());
}

RUVIA_TEST(http2_connection_rejects_upgrade_required_final_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse buffered(&resource);
    buffered.status(ruvia::http_status::kUpgradeRequired);
    buffered.header("Upgrade", "websocket");
    const auto bufferedResult = submitBufferedResponseHead(conn, 1, buffered);
    RUVIA_CHECK(responseHeadSubmitFailureMessage(bufferedResult) == "invalid HTTP/2 response head message");
    RUVIA_CHECK(conn.pendingOutput().empty());

    ruvia::HttpResponse streaming(&resource);
    streaming.status(ruvia::http_status::kUpgradeRequired);
    streaming.header("Upgrade", "websocket");
    const auto streamingResult = conn.submitStreamingResponseHead(1, std::move(streaming), ruvia::detail::ResponseStreamKind::kGeneric, ruvia::detail::ResponseTrailerIntent::kNone);
    RUVIA_CHECK(responseHeadSubmitFailureMessage(streamingResult) == "invalid HTTP/2 response head message");
    RUVIA_CHECK(conn.pendingOutput().empty());

    // Both failures occur before HPACK/stream mutation, so a conformant final
    // response can still be submitted on the same stream.
    ruvia::HttpResponse fallback(&resource);
    fallback.status(ruvia::http_status::kBadRequest);
    RUVIA_CHECK(responseHeadSubmitted(submitBufferedResponseHead(conn, 1, fallback)));
    RUVIA_CHECK(!conn.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_rejects_connection_specific_final_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    constexpr std::pair<std::string_view, std::string_view> fields[] = {
        {"Connection", "close"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Transfer-Encoding", "chunked"},
        {"Upgrade", "websocket"},
    };
    for (const auto& [name, value] : fields) {
        ruvia::HttpResponse buffered(&resource);
        if (name == "TE") {
            addUncheckedHeader(buffered, name, value);
        } else {
            buffered.header(name, value);
        }
        const auto bufferedResult = submitBufferedResponseHead(conn, 1, buffered);
        RUVIA_CHECK(responseHeadSubmitFailureMessage(bufferedResult) == "invalid HTTP/2 response head message");
        RUVIA_CHECK(conn.pendingOutput().empty());

        ruvia::HttpResponse streaming(&resource);
        if (name == "TE") {
            addUncheckedHeader(streaming, name, value);
        } else {
            streaming.header(name, value);
        }
        const auto streamingResult = conn.submitStreamingResponseHead(1, std::move(streaming), ruvia::detail::ResponseStreamKind::kGeneric, ruvia::detail::ResponseTrailerIntent::kNone);
        RUVIA_CHECK(responseHeadSubmitFailureMessage(streamingResult) == "invalid HTTP/2 response head message");
        RUVIA_CHECK(conn.pendingOutput().empty());
    }

    // Every rejection happened before HPACK and stream mutation, so the same
    // stream can still accept one conformant final response.
    ruvia::HttpResponse fallback(&resource);
    fallback.status(ruvia::http_status::kInternalServerError);
    RUVIA_CHECK(responseHeadSubmitted(submitBufferedResponseHead(conn, 1, fallback)));
    RUVIA_CHECK(!conn.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_streaming_zero_content_length_stays_open_for_finish) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(ruvia::http_status::kOk);
    response.header("Content-Length", "0");
    RUVIA_CHECK(responseHeadSubmitted(conn.submitStreamingResponseHead(1, std::move(response), ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone)));
    const auto head = conn.pendingOutput();
    const auto headFrame = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK((headFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(head.size());

    RUVIA_CHECK(conn.submitData(1, "x", Http2EndStream::kKeepOpen) == Http2DataSubmitStatus::kContentLengthExceeded);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.finishResponse(1, validatedTrailers({})) == Http2FinishSubmitStatus::kAccepted);
    const auto terminal = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(terminal.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(terminal.length, static_cast<std::uint32_t>(0));
    RUVIA_CHECK((terminal.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

// Client role end-to-end against the server core with ZERO I/O: the client core opens
// stream 1, sends a GET, the server core dispatches a 200 "pong", and the client core
// surfaces the response head (status via the stream state), body chunk, and end.

RUVIA_TEST(http2_connection_client_role_get_round_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    server.beginConnection();
    Http2Connection client(&resource, Http2Role::kClient);
    client.beginConnection();

    std::string clientBody;
    std::uint16_t status = 0;
    bool clientSawHead = false;
    bool clientSawEnd = false;
    const auto onServerEvent = [&](const Http2Event& event) {
        if (const auto* messageEnd = event.messageEnd()) {
            const auto streamId = messageEnd->streamId();
            ruvia::HttpResponse response(&resource);
            response.status(ruvia::http_status::kOk);
            response.body("pong");
            RUVIA_CHECK(responseHeadSubmitted(submitBufferedResponseHead(server, streamId, response)));
            RUVIA_CHECK(server.submitData(streamId, "pong", Http2EndStream::kEndStream) == Http2DataSubmitStatus::kAccepted);
        }
    };
    const auto onClientEvent = [&](const Http2Event& event) {
        if (const auto* messageHead = event.messageHead()) {
            clientSawHead = true;
            if (auto* stream = client.stream(messageHead->streamId())) {
                const auto* responseStatus = stream->responseStatus();
                RUVIA_CHECK(responseStatus != nullptr);
                if (responseStatus != nullptr) {
                    status = responseStatus->value();
                }
            }
        } else if (const auto* bodyChunk = event.messageBodyChunk()) {
            clientBody.append(bodyChunk->bytes().data(), bodyChunk->bytes().size());
        } else if (event.messageEnd() != nullptr) {
            clientSawEnd = true;
        }
    };

    const auto request = client.submitRegularRequestHead("GET", "http", "example.com", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    RUVIA_CHECK_EQ(streamId, static_cast<std::uint32_t>(1));
    client.pinStream(streamId);
    const auto requestBytes = client.pendingOutput().size();
    RUVIA_CHECK(client.submitData(streamId, "forbidden", Http2EndStream::kEndStream) == Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(client.pendingOutput().size(), requestBytes);

    for (int round = 0; round < 4; ++round) {
        shuttleOnce(client, server, onServerEvent);
        shuttleOnce(server, client, onClientEvent);
    }

    RUVIA_CHECK(clientSawHead);
    RUVIA_CHECK_EQ(status, static_cast<std::uint16_t>(200));
    RUVIA_CHECK(clientBody == "pong");
    RUVIA_CHECK(clientSawEnd);
    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    // content-length was decoded into the stream (auto CL from the server head).
    const auto* remoteKnownLength = stream->remoteContent().allowedKnownLength();
    RUVIA_CHECK(remoteKnownLength != nullptr);
    RUVIA_CHECK_EQ(remoteKnownLength->declaredLength(), std::size_t{4});
    client.unpinStream(streamId);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
}

// Client role POST: the request body flows through submitData with END_STREAM, the
// server core buffers it (owner-side append) and answers; both directions complete.

RUVIA_TEST(http2_connection_client_role_post_round_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    server.beginConnection();
    Http2Connection client(&resource, Http2Role::kClient);
    client.beginConnection();

    std::string serverBody;
    std::string clientBody;
    bool clientSawEnd = false;
    const auto onServerEvent = [&](const Http2Event& event) {
        if (const auto* bodyChunk = event.messageBodyChunk()) {
            serverBody.append(bodyChunk->bytes().data(), bodyChunk->bytes().size());
        } else if (const auto* messageEnd = event.messageEnd()) {
            const auto streamId = messageEnd->streamId();
            ruvia::HttpResponse response(&resource);
            response.status(ruvia::http_status::kOk);
            response.body(serverBody);
            RUVIA_CHECK(responseHeadSubmitted(submitBufferedResponseHead(server, streamId, response)));
            RUVIA_CHECK(server.submitData(streamId, std::string_view(serverBody.data(), serverBody.size()), Http2EndStream::kEndStream) == Http2DataSubmitStatus::kAccepted);
        }
    };
    const auto onClientEvent = [&](const Http2Event& event) {
        if (const auto* bodyChunk = event.messageBodyChunk()) {
            clientBody.append(bodyChunk->bytes().data(), bodyChunk->bytes().size());
        } else if (event.messageEnd() != nullptr) {
            clientSawEnd = true;
        }
    };

    const auto request = client.submitRegularRequestHead("POST", "http", "example.com", "/echo", {}, Http2RequestContent::knownLength(5));
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    RUVIA_CHECK(client.submitData(streamId, "hello", Http2EndStream::kEndStream) == Http2DataSubmitStatus::kAccepted);

    for (int round = 0; round < 4; ++round) {
        shuttleOnce(client, server, onServerEvent);
        shuttleOnce(server, client, onClientEvent);
    }

    RUVIA_CHECK(serverBody == "hello");
    RUVIA_CHECK(clientBody == "hello");
    RUVIA_CHECK(clientSawEnd);
}

RUVIA_TEST(http2_connection_client_rejects_interim_field_flood) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string interim(&resource);
    HpackEncoder::encodeHeader(interim, ":status", "103");
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        HpackEncoder::encodeHeader(interim, "x-hint", "warm");
    }
    const auto frame = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders, std::string_view(interim.data(), interim.size()));
    RUVIA_CHECK(client.feed(std::string_view(frame.data(), frame.size())) == Http2FeedResult::kAccepted);

    bool sawClosed = false;
    while (const auto event = client.nextEvent()) {
        RUVIA_CHECK(event->messageHead() == nullptr);
        RUVIA_CHECK(event->messageEnd() == nullptr);
        if (const auto* closed = event->streamClosed()) {
            sawClosed = true;
            RUVIA_CHECK(closed->source() == Http2StreamCloseSource::kLocal);
            RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
        }
    }
    RUVIA_CHECK(sawClosed);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
    RUVIA_CHECK(!client.connectionError().has_value());

    const auto resetBytes = client.pendingOutput();
    RUVIA_CHECK_EQ(resetBytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(resetBytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(reset.streamId, streamId);
}

RUVIA_TEST(http2_connection_client_rejects_forbidden_interim_fields) {
    constexpr std::array forbiddenFields{
        std::pair{std::string_view("content-length"), std::string_view("0")},
        std::pair{std::string_view("trailer"), std::string_view("x-check")},
    };

    for (const auto& [name, value] : forbiddenFields) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", "103");
        HpackEncoder::encodeHeader(response, name, value);
        const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders, std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) == Http2FeedResult::kAccepted);

        bool sawClosed = false;
        while (const auto event = client.nextEvent()) {
            RUVIA_CHECK(event->messageHead() == nullptr);
            RUVIA_CHECK(event->messageEnd() == nullptr);
            if (const auto* closed = event->streamClosed()) {
                sawClosed = true;
                RUVIA_CHECK(closed->source() == Http2StreamCloseSource::kLocal);
                RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
            }
        }
        RUVIA_CHECK(sawClosed);
        RUVIA_CHECK(client.stream(streamId) == nullptr);
        RUVIA_CHECK(!client.connectionError().has_value());

        const auto resetBytes = client.pendingOutput();
        RUVIA_CHECK_EQ(resetBytes.size(), static_cast<std::size_t>(13));
        const auto reset = ruvia::detail::http2ParseFrameHeader(resetBytes.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(reset.streamId, streamId);
        RUVIA_CHECK_EQ(ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(resetBytes.data() + 9)), static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    }
}

RUVIA_TEST(http2_connection_client_validates_interim_representation_field_syntax) {
    const auto check = [&ruvia_ctx](std::string_view name, std::string_view value, bool rejected) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string interim(&resource);
        HpackEncoder::encodeHeader(interim, ":status", "103");
        HpackEncoder::encodeHeader(interim, name, value);
        const auto frame = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders, std::string_view(interim.data(), interim.size()));
        RUVIA_CHECK(client.feed(std::string_view(frame.data(), frame.size())) == Http2FeedResult::kAccepted);

        bool sawProtocolError = false;
        while (const auto event = client.nextEvent()) {
            if (const auto* closed = event->streamClosed()) {
                sawProtocolError = closed->source() == Http2StreamCloseSource::kLocal && closed->error() == Http2ErrorCode::kProtocolError;
            } else {
                RUVIA_CHECK(false);
            }
        }
        RUVIA_CHECK(sawProtocolError == rejected);
        RUVIA_CHECK((client.stream(streamId) == nullptr) == rejected);
    };

    check("content-encoding", "gzip;level=9", true);
    check("content-encoding", ", gzip,,", false);
    check("content-type", "not a media type", true);
    check("content-type", "text/html; charset=utf-8", false);
}

RUVIA_TEST(http2_connection_client_rejects_status_outside_http_range) {
    {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.pinStream(streamId);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", "599");
        const auto responseHead = headersFrame(&resource, streamId, static_cast<std::uint8_t>(ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream), std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) == Http2FeedResult::kAccepted);

        int messageHeads = 0;
        int messageEnds = 0;
        while (const auto event = client.nextEvent()) {
            messageHeads += event->messageHead() != nullptr ? 1 : 0;
            messageEnds += event->messageEnd() != nullptr ? 1 : 0;
            RUVIA_CHECK(event->streamClosed() == nullptr);
        }
        RUVIA_CHECK_EQ(messageHeads, 1);
        RUVIA_CHECK_EQ(messageEnds, 1);
        const auto* stream = client.stream(streamId);
        RUVIA_CHECK(stream != nullptr);
        const auto* responseStatus = stream->responseStatus();
        RUVIA_CHECK(responseStatus != nullptr);
        if (responseStatus != nullptr) {
            RUVIA_CHECK_EQ(*responseStatus, ruvia::HttpStatusCode::fromValue(599));
        }
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(!client.connectionError().has_value());
        client.unpinStream(streamId);
    }

    for (const std::string_view status : {"600", "999"}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", status);
        const auto responseHead = headersFrame(&resource, streamId, static_cast<std::uint8_t>(ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream), std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) == Http2FeedResult::kAccepted);

        bool sawClosed = false;
        while (const auto event = client.nextEvent()) {
            RUVIA_CHECK(event->messageHead() == nullptr);
            RUVIA_CHECK(event->messageEnd() == nullptr);
            if (const auto* closed = event->streamClosed()) {
                sawClosed = true;
                RUVIA_CHECK(closed->source() == Http2StreamCloseSource::kLocal);
                RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
            }
        }
        RUVIA_CHECK(sawClosed);
        RUVIA_CHECK(client.stream(streamId) == nullptr);
        RUVIA_CHECK(!client.connectionError().has_value());

        const auto resetBytes = client.pendingOutput();
        RUVIA_CHECK_EQ(resetBytes.size(), static_cast<std::size_t>(13));
        const auto reset = ruvia::detail::http2ParseFrameHeader(resetBytes.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(reset.streamId, streamId);
        RUVIA_CHECK_EQ(ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(resetBytes.data() + 9)), static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    }
}

RUVIA_TEST(http2_connection_client_rejects_invalid_content_type_syntax) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    HpackEncoder::encodeHeader(response, "content-type", "not a media type");
    HpackEncoder::encodeHeader(response, "content-length", "0");
    const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) == Http2FeedResult::kAccepted);

    bool sawProtocolError = false;
    while (const auto event = client.nextEvent()) {
        RUVIA_CHECK(event->messageHead() == nullptr);
        RUVIA_CHECK(event->messageEnd() == nullptr);
        if (const auto* closed = event->streamClosed()) {
            sawProtocolError = closed->source() == Http2StreamCloseSource::kLocal && closed->error() == Http2ErrorCode::kProtocolError;
        }
    }
    RUVIA_CHECK(sawProtocolError);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
    RUVIA_CHECK(!client.connectionError().has_value());

    const auto resetBytes = client.pendingOutput();
    RUVIA_CHECK_EQ(resetBytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(resetBytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(reset.streamId, streamId);
    RUVIA_CHECK_EQ(ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(resetBytes.data() + 9)), static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_connection_client_rejects_invalid_content_encoding_syntax) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    HpackEncoder::encodeHeader(response, "content-encoding", "gzip;level=9");
    HpackEncoder::encodeHeader(response, "content-length", "0");
    const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) == Http2FeedResult::kAccepted);

    bool sawProtocolError = false;
    while (const auto event = client.nextEvent()) {
        RUVIA_CHECK(event->messageHead() == nullptr);
        RUVIA_CHECK(event->messageEnd() == nullptr);
        if (const auto* closed = event->streamClosed()) {
            sawProtocolError = closed->source() == Http2StreamCloseSource::kLocal && closed->error() == Http2ErrorCode::kProtocolError;
        }
    }
    RUVIA_CHECK(sawProtocolError);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
    RUVIA_CHECK(!client.connectionError().has_value());
}

RUVIA_TEST(http2_connection_client_rejects_invalid_trailer_field_names_in_response_head) {
    for (const std::string_view value : {"Content-Length", "X-Checksum, bad field"}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead("GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", "200");
        HpackEncoder::encodeHeader(response, "trailer", value);
        HpackEncoder::encodeHeader(response, "content-length", "0");
        const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) == Http2FeedResult::kAccepted);

        bool sawProtocolError = false;
        while (const auto event = client.nextEvent()) {
            RUVIA_CHECK(event->messageHead() == nullptr);
            RUVIA_CHECK(event->messageEnd() == nullptr);
            if (const auto* closed = event->streamClosed()) {
                sawProtocolError = closed->source() == Http2StreamCloseSource::kLocal && closed->error() == Http2ErrorCode::kProtocolError;
            }
        }
        RUVIA_CHECK(sawProtocolError);
        RUVIA_CHECK(client.stream(streamId) == nullptr);
        RUVIA_CHECK(!client.connectionError().has_value());
    }
}

// --- flood defense-in-depth budgets (GOAWAY ENHANCE_YOUR_CALM) -----------------

// A peer that floods PINGs without ever letting us flush the echoed ACKs is cut off with
// GOAWAY(ENHANCE_YOUR_CALM) instead of accumulating unbounded ACK bytes.

RUVIA_TEST(http2_connection_ping_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    std::memset(ping + 9, 0, 8);

    bool tripped = false;
    for (int i = 0; i < 1200 && !tripped; ++i) {
        // Deliberately do NOT drain output between pings, so the un-drained PING budget
        // accumulates (consumeOutput would reset it -- see the keepalive test below).
        tripped = conn.feed(std::string_view(ping, sizeof(ping))) == ruvia::detail::Http2FeedResult::kProtocolFailure;
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connectionError().has_value());
    RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()), kEnhanceYourCalm);
}

// A peer that floods non-ACK SETTINGS without ever letting us flush the echoed ACKs is
// cut off with GOAWAY(ENHANCE_YOUR_CALM) instead of accumulating unbounded ACK bytes
// (CVE-2019-9515 SETTINGS flood -- the sibling of the PING flood above).

RUVIA_TEST(http2_connection_settings_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);

    bool tripped = false;
    for (int i = 0; i < 1200 && !tripped; ++i) {
        // Deliberately do NOT drain output between frames, so the un-drained SETTINGS
        // budget accumulates (consumeOutput would reset it -- see the drained test below).
        tripped = conn.feed(std::string_view(settings, sizeof(settings))) == ruvia::detail::Http2FeedResult::kProtocolFailure;
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connectionError().has_value());
    RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()), kEnhanceYourCalm);
}
