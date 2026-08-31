#include "http2_connection_fixture.h"

#include <array>

// Http2Connection: inbound and outbound DATA.

// A DATA frame after the head yields a kMessageBodyChunk carrying the bytes, then
// (on END_STREAM) kMessageEnd. WINDOW_UPDATE is withheld until the event owner
// explicitly confirms that the borrowed bytes were consumed.

RUVIA_TEST(http2_connection_feed_data_emits_body_chunk_and_end) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto h = postHeadFrame(&resource, "");
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const char body[5] = {'h', 'e', 'l', 'l', 'o'};
    const auto d =
        dataFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream, std::string_view(body, 5));
    (void)conn.feed(std::string_view(d.data(), d.size()));

    const auto chunk = conn.nextEvent().value();
    RUVIA_CHECK(chunk.kind() == Http2EventKind::kMessageBodyChunk);
    RUVIA_CHECK_EQ(chunk.messageBodyChunk()->streamId(), static_cast<std::uint32_t>(1));
    RUVIA_CHECK(chunk.messageBodyChunk()->bytes() == std::string_view(body, 5));
    const auto end = conn.nextEvent().value();
    RUVIA_CHECK(end.kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK_EQ(end.messageEnd()->streamId(), static_cast<std::uint32_t>(1));

    RUVIA_CHECK(conn.pendingOutput().empty());
    conn.releaseAllReceivedData(1);
    // END_STREAM leaves no live stream scope. The consumed connection credit is
    // retained below the batching threshold without manufacturing output.
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// A DATA frame is protocol progress even when its Data field is empty, but it is not
// an application body chunk. Padding still consumes both flow-control windows and
// queues batched credit, while END_STREAM still emits the terminal message event.

RUVIA_TEST(http2_connection_empty_data_never_emits_body_chunk) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const auto empty = dataFrame(&resource, 1, 0, {});
    RUVIA_CHECK(
        conn.feed(std::string_view(empty.data(), empty.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());

    const std::string_view paddingOnly("\x02\0\0", 3);
    const auto padded = dataFrame(&resource, 1, ruvia::detail::kHttp2FlagPadded, paddingOnly);
    RUVIA_CHECK(
        conn.feed(std::string_view(padded.data(), padded.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    RUVIA_CHECK(conn.pendingOutput().empty());

    const auto terminal = dataFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream, {});
    RUVIA_CHECK(conn.feed(std::string_view(terminal.data(), terminal.size())) ==
                Http2FeedResult::kAccepted);
    const auto end = conn.nextEvent();
    RUVIA_CHECK(end.has_value());
    if (end.has_value()) {
        RUVIA_CHECK(end->kind() == Http2EventKind::kMessageEnd);
    }
    RUVIA_CHECK(!conn.nextEvent().has_value());
}

RUVIA_TEST(http2_connection_padding_only_data_does_not_amplify_output) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    RUVIA_CHECK(
        conn.feed(std::string_view(head.data(), head.size())) == Http2FeedResult::kAccepted);
    while (conn.nextEvent().has_value()) {
    }

    const std::string_view paddingOnly("\x02\0\0", 3);
    const auto padded = dataFrame(&resource, 1, ruvia::detail::kHttp2FlagPadded, paddingOnly);
    for (int i = 0; i < 1200; ++i) {
        RUVIA_CHECK(conn.feed(std::string_view(padded.data(), padded.size())) ==
                    Http2FeedResult::kAccepted);
        RUVIA_CHECK(!conn.nextEvent().has_value());
    }
    // 3,600 consumed octets are far below half the advertised 1 MiB window.
    // Per-frame updates would turn 15 KiB of input framing into 31.2 KiB of
    // unread output; credit must remain pending instead.
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.connectionError().has_value());
}

RUVIA_TEST(http2_connection_same_feed_data_credit_queues_owner_batch) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    const auto first = dataFrame(&resource, 1, 0, "one");
    const auto second = dataFrame(&resource, 1, 0, "two");
    std::pmr::string batch(&resource);
    batch.append(head.data(), head.size());
    batch.append(first.data(), first.size());
    batch.append(second.data(), second.size());

    RUVIA_CHECK(conn.feed(std::string_view(batch.data(), batch.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    const auto firstChunk = conn.nextEvent().value();
    RUVIA_CHECK_EQ(firstChunk.messageBodyChunk()->bytes(), std::string_view("one"));
    const auto secondChunk = conn.nextEvent().value();
    RUVIA_CHECK_EQ(secondChunk.messageBodyChunk()->bytes(), std::string_view("two"));
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());

    // One owner acknowledgement releases the complete copied event batch and
    // queues its six flow-controlled octets once in each live receive scope.
    conn.releaseAllReceivedData(1);
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// A DATA/END_STREAM that falls short of a declared content-length is a protocol error:
// the core RST_STREAMs the stream and does NOT emit kMessageEnd.

RUVIA_TEST(http2_connection_feed_data_short_of_content_length_resets) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto h = postHeadFrame(&resource, "10");  // promises 10 bytes
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);

    const char body[5] = {'s', 'h', 'o', 'r', 't'};  // only 5, with END_STREAM
    const auto d =
        dataFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream, std::string_view(body, 5));
    (void)conn.feed(std::string_view(d.data(), d.size()));

    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageBodyChunk);
    // The length mismatch aborts the stream: kStreamClosed (never kMessageEnd), and
    // the (unpinned) stream is removed from the table.
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kStreamClosed);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

// submitResponseHead emits a HEADERS block (END_HEADERS, no END_STREAM when a body
// follows); submitData then sends the buffered body as a terminal DATA frame.

RUVIA_TEST(http2_connection_submit_response_head_and_body) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse resp({.resource = &resource});
    resp.status(ruvia::http_status::kOk);
    resp.body("hello");
    const auto headResult = submitBufferedResponseHead(conn, 1, resp);
    RUVIA_CHECK(responseHeadSubmitted(headResult));

    const auto head = conn.pendingOutput();
    const auto hd = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(hd.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(head.size());

    const auto r = conn.submitData(1, "hello", Http2EndStream::kEndStream);
    RUVIA_CHECK(r == Http2DataSubmitStatus::kAccepted);
    const auto body = conn.pendingOutput();
    const auto dd = ruvia::detail::http2ParseFrameHeader(body.substr(0, 9));
    RUVIA_CHECK_EQ(dd.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(dd.length, static_cast<std::uint32_t>(5));
    RUVIA_CHECK((dd.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

RUVIA_TEST(http2_connection_rejects_every_data_submission_after_local_end_stream) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    RUVIA_CHECK(responseHeadSubmitted(conn.submitStreamingResponseHead(1, std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());

    std::uint64_t state = 0x4ae7'196d'25f0'83bcULL;
    const auto next = [&state] {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };

    for (std::size_t sample = 0; sample < 64; ++sample) {
        const auto bodyBytes = static_cast<std::size_t>(next() % 9U);
        std::string body;
        for (std::size_t i = 0; i < bodyBytes; ++i) {
            body.push_back(static_cast<char>('a' + (next() % 26U)));
        }
        const auto endStream = (next() & 1U) != 0 || sample == 63
                                   ? Http2EndStream::kEndStream
                                   : Http2EndStream::kKeepOpen;
        const auto status = conn.submitData(1, body, endStream);
        RUVIA_CHECK(status == Http2DataSubmitStatus::kAccepted);
        if (endStream == Http2EndStream::kEndStream) {
            break;
        }
        conn.consumeOutput(conn.pendingOutput().size());
    }

    const auto afterEndOutput = conn.pendingOutput();
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
    }
    for (const std::string_view body : {std::string_view{}, std::string_view("x"),
             std::string_view("second body")}) {
        RUVIA_CHECK(conn.submitData(1, body, Http2EndStream::kKeepOpen) ==
                    Http2DataSubmitStatus::kInvalidState);
        RUVIA_CHECK_EQ(conn.pendingOutput(), afterEndOutput);
        RUVIA_CHECK(conn.submitData(1, body, Http2EndStream::kEndStream) ==
                    Http2DataSubmitStatus::kInvalidState);
        RUVIA_CHECK_EQ(conn.pendingOutput(), afterEndOutput);
    }
}

RUVIA_TEST(http2_connection_rejects_data_before_head_without_output) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    const auto before = conn.pendingOutput().size();
    RUVIA_CHECK(conn.submitData(1, "body", Http2EndStream::kEndStream) ==
                Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), before);
}

// HEAD carries the representation metadata (including Content-Length) but the
// protocol core terminates the stream on HEADERS and tells the runtime not to
// submit DATA, even when the application response contains bytes.

RUVIA_TEST(http2_connection_head_buffered_response_suppresses_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveRequest(conn, &resource, "HEAD");

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    response.body("hello");
    const auto headResult = submitBufferedResponseHead(conn, 1, response);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(submittedResponsePlan(headResult).bodySuppressed());
    RUVIA_CHECK(!submittedResponsePlan(headResult).sendBody());
    RUVIA_CHECK_EQ(
        submittedResponsePlan(headResult).contentLength(), static_cast<std::uint64_t>(5));
    const auto head = conn.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    const auto beforeRejectedData = conn.pendingOutput().size();
    RUVIA_CHECK(conn.submitData(1, "forbidden", Http2EndStream::kEndStream) ==
                Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), beforeRejectedData);
}

// 205 is not a normal zero-byte response whose body phase may remain open: RFC
// 9110 §15.3.6 forbids content. The shared response plan therefore terminates the
// HTTP/2 stream on HEADERS even when the application attached buffered bytes.

RUVIA_TEST(http2_connection_reset_content_suppresses_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kResetContent);
    response.body("must-not-be-sent");
    response.header("Content-Length", "16");
    const auto headResult = submitBufferedResponseHead(conn, 1, response);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(!submittedResponsePlan(headResult).statusAllowsBody());
    RUVIA_CHECK(submittedResponsePlan(headResult).bodySuppressed());
    RUVIA_CHECK(!submittedResponsePlan(headResult).sendBody());
    RUVIA_CHECK_EQ(
        submittedResponsePlan(headResult).contentLength(), static_cast<std::uint64_t>(0));
    const auto head = conn.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    const auto beforeRejectedData = conn.pendingOutput().size();
    RUVIA_CHECK(conn.submitData(1, "forbidden", Http2EndStream::kEndStream) ==
                Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), beforeRejectedData);
}

RUVIA_TEST(http2_connection_short_finish_does_not_mutate_queued_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 3);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    response.header("Content-Length", "8");
    RUVIA_CHECK(responseHeadSubmitted(conn.submitStreamingResponseHead(1, std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(
        conn.submitData(1, "12345", Http2EndStream::kKeepOpen) == Http2DataSubmitStatus::kQueued);
    conn.consumeOutput(conn.pendingOutput().size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{3});
    RUVIA_CHECK(conn.finishResponse(1, validatedTrailers({})) ==
                Http2FinishSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(stream->localSend().responseContentOpen() != nullptr);
    RUVIA_CHECK(conn.hasQueuedData(1));
    RUVIA_CHECK(conn.pendingOutput().empty());

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 1, 10);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    const auto drainedOutput = conn.pendingOutput();
    const auto drainedFrame = ruvia::detail::http2ParseFrameHeader(drainedOutput.substr(0, 9));
    RUVIA_CHECK_EQ(drainedFrame.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK((drainedFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(drainedOutput.size());
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});

    RUVIA_CHECK(
        conn.submitData(1, "678", Http2EndStream::kEndStream) == Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{8});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{8});
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
}

// RFC 8441 Extended CONNECT: a CONNECT + :protocol=websocket head emits kMessageHead
// with NO kMessageEnd (the tunnel stays open), and the stream carries the
// extendedConnectWebSocket mark for the owner's route policy. The handshake atomically
// opens the tunnel, tunnel DATA flows as kTunnelData events (no content-length
// required), and submitData carries frames
// back on the still-open stream.

RUVIA_TEST(http2_connection_websocket_tunnel_handshake_and_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":protocol", "websocket");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/ws");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    HpackEncoder::encodeHeader(block, "sec-websocket-version", "13");
    const auto h = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));

    bool sawHeaders = false;
    bool sawEnd = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* head = event->messageHead(); head != nullptr && head->streamId() == 1) {
            sawHeaders = true;
        }
        if (event->messageEnd() != nullptr) {
            sawEnd = true;
        }
    }
    RUVIA_CHECK(sawHeaders);
    RUVIA_CHECK(!sawEnd);  // the tunnel must stay open

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == Http2ConnectForm::kExtended);
    RUVIA_CHECK(stream->protocolIsWebSocket());

    // Owner route policy admitted a WebSocket route: answer 200 and open the tunnel.
    conn.consumeOutput(conn.pendingOutput().size());
    ruvia::detail::Http1ServerRequestParser negotiationParser;
    const auto negotiationRequest = negotiationParser.parseMessage(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "\r\n");
    constexpr std::array<std::string_view, 1> supportedProtocols{"chat"};
    auto negotiation = ruvia::detail::makeWebSocketServerNegotiation(
        negotiationRequest.request, {.supportedSubprotocols = supportedProtocols});
    const auto handshakeResult = conn.submitWebSocketHandshake(1, std::move(negotiation));
    RUVIA_CHECK(handshakeResult.submitted() != nullptr);
    RUVIA_CHECK(handshakeResult.failure() == nullptr);
    RUVIA_CHECK(handshakeResult.submitted()->subprotocol() == "chat");

    auto duplicateNegotiation = ruvia::detail::makeWebSocketServerNegotiation(
        negotiationRequest.request, {.supportedSubprotocols = supportedProtocols});
    const auto duplicateHandshakeResult =
        conn.submitWebSocketHandshake(1, std::move(duplicateNegotiation));
    RUVIA_CHECK(duplicateHandshakeResult.submitted() == nullptr);
    RUVIA_CHECK(duplicateHandshakeResult.failure() != nullptr);
    RUVIA_CHECK(duplicateHandshakeResult.failure()->error() ==
                ruvia::detail::Http2WebSocketHandshakeSubmitError::kInvalidState);
    RUVIA_CHECK_EQ(duplicateNegotiation.subprotocol(), std::string_view("chat"));

    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() > 9);
    const auto head = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(head.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK_EQ(head.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK((head.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((head.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // stream open
    conn.consumeOutput(out.size());

    // Inbound tunnel bytes (a would-be masked frame) surface as tunnel DATA even with
    // no content-length: the tunnel is exempt from body accounting.
    char data[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(data, 4, Http2FrameType::kData, 0, 1);
    std::memcpy(data + 9, "\x81\x80\x01\x02", 4);
    (void)conn.feed(std::string_view(data, sizeof(data)));
    bool sawChunk = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* tunnelData = event->tunnelData(); tunnelData != nullptr &&
                                                          tunnelData->streamId() == 1 &&
                                                          tunnelData->bytes().size() == 4) {
            sawChunk = true;
        }
    }
    RUVIA_CHECK(sawChunk);

    // Outbound tunnel frames ride submitData on the still-open stream.
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "\x81\x02hi", Http2EndStream::kKeepOpen) ==
                Http2DataSubmitStatus::kAccepted);
    const auto frameOut = conn.pendingOutput();
    const auto dataHead = ruvia::detail::http2ParseFrameHeader(frameOut.substr(0, 9));
    RUVIA_CHECK_EQ(dataHead.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK((dataHead.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    RUVIA_CHECK(frameOut.substr(9) == std::string_view("\x81\x02hi"));
}

RUVIA_TEST(http2_connection_rejects_half_closed_websocket_opening_handshake) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":protocol", "websocket");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/ws");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    HpackEncoder::encodeHeader(block, "sec-websocket-version", "13");
    const auto h = headersFrame(&resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h.data(), h.size())) == Http2FeedResult::kAccepted);

    bool sawHeaders = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* head = event->messageHead(); head != nullptr && head->streamId() == 1) {
            sawHeaders = true;
        }
        RUVIA_CHECK(event->tunnelEnd() == nullptr);
    }
    RUVIA_CHECK(sawHeaders);

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->remoteReceive().connectPendingEndStream() != nullptr);

    ruvia::detail::Http1ServerRequestParser negotiationParser;
    const auto negotiationRequest = negotiationParser.parseMessage(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "\r\n");
    auto negotiation =
        ruvia::detail::makeWebSocketServerNegotiation(negotiationRequest.request, {});
    const auto handshakeResult = conn.submitWebSocketHandshake(1, std::move(negotiation));
    RUVIA_CHECK(handshakeResult.submitted() == nullptr);
    RUVIA_CHECK(handshakeResult.failure() != nullptr);
    if (const auto* failure = handshakeResult.failure()) {
        RUVIA_CHECK(
            failure->error() == ruvia::detail::Http2WebSocketHandshakeSubmitError::kInvalidState);
    }
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// RFC 9110 Section 6.4.1 defines HEAD/204/304 responses as having no message
// content. RFC 9113 Section 8.1.1 therefore makes a non-empty DATA payload a
// malformed response and requires PROTOCOL_ERROR on that stream. This is distinct
// from Content-Length representation metadata, which remains legal for HEAD/304.

RUVIA_TEST(http2_connection_client_rejects_data_for_responses_without_content) {
    struct Case final {
        std::string_view method;
        std::string_view status;
    };
    constexpr std::array cases{Case{"HEAD", "200"}, Case{"GET", "204"}, Case{"GET", "304"}};

    for (const auto& testCase : cases) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead(
            testCase.method, "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.pinStream(streamId);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", testCase.status);
        const auto responseHead =
            headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                    Http2FeedResult::kAccepted);
        RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
        RUVIA_CHECK(!client.nextEvent().has_value());
        const auto* live = client.stream(streamId);
        RUVIA_CHECK(live != nullptr);
        RUVIA_CHECK(live->remoteContent().metadataOnlyWithoutLength() != nullptr);

        const auto forbidden =
            dataFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndStream, "x");
        RUVIA_CHECK(client.feed(std::string_view(forbidden.data(), forbidden.size())) ==
                    Http2FeedResult::kAccepted);

        bool sawClosed = false;
        while (const auto event = client.nextEvent()) {
            RUVIA_CHECK(event->messageBodyChunk() == nullptr);
            RUVIA_CHECK(event->messageEnd() == nullptr);
            if (const auto* closed = event->streamClosed()) {
                sawClosed = true;
                RUVIA_CHECK(closed->source() == ruvia::detail::Http2StreamCloseSource::kLocal);
                RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
            }
        }
        RUVIA_CHECK(sawClosed);
        RUVIA_CHECK(!client.connectionError().has_value());
        const auto reset = client.pendingOutput();
        const auto resetHead = ruvia::detail::http2ParseFrameHeader(reset.substr(0, 9));
        RUVIA_CHECK_EQ(resetHead.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(reset.data() + 9)),
            static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
        client.unpinStream(streamId);
    }
}

RUVIA_TEST(http2_connection_client_allows_empty_terminal_data_without_content_event) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead(
        "HEAD", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    HpackEncoder::encodeHeader(response, "content-length", "12");
    const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    const auto* metadata = client.stream(streamId)->remoteContent().metadataOnlyKnownLength();
    RUVIA_CHECK(metadata != nullptr);
    RUVIA_CHECK_EQ(metadata->declaredLength(), std::size_t{12});

    const auto terminal = dataFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndStream, {});
    RUVIA_CHECK(client.feed(std::string_view(terminal.data(), terminal.size())) ==
                Http2FeedResult::kAccepted);
    const auto end = client.nextEvent();
    RUVIA_CHECK(end.has_value());
    RUVIA_CHECK(end->kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(streamId)->remoteReceive().endStream() != nullptr);

    client.unpinStream(streamId);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
}

RUVIA_TEST(http2_connection_discarded_data_batches_full_payload_credit_once) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) == Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(conn.stream(1) == nullptr);
    conn.consumeOutput(conn.pendingOutput().size());

    // The flow-controlled length is 6: one Pad Length byte, two data bytes, and
    // three padding bytes. A closed stream has no stream window to restore, but all
    // six octets must join the connection's batch exactly once.
    constexpr char paddedPayload[] = {3, 'o', 'k', 0, 0, 0};
    const auto discarded = dataFrame(&resource, 1, ruvia::detail::kHttp2FlagPadded,
        std::string_view(paddedPayload, sizeof(paddedPayload)));
    RUVIA_CHECK(conn.feed(std::string_view(discarded.data(), discarded.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());

    constexpr std::uint32_t threshold = ruvia::detail::kHttp2ReceiveWindowUpdateThreshold;
    std::string chunk(Http2LocalSettings::kMaxFrameSize, 'x');
    std::uint32_t remaining = threshold - static_cast<std::uint32_t>(sizeof(paddedPayload));
    while (remaining != 0) {
        const auto chunkBytes =
            static_cast<std::size_t>(remaining < chunk.size() ? remaining : chunk.size());
        const auto data = dataFrame(&resource, 1, 0, std::string_view(chunk.data(), chunkBytes));
        RUVIA_CHECK(
            conn.feed(std::string_view(data.data(), data.size())) == Http2FeedResult::kAccepted);
        remaining -= static_cast<std::uint32_t>(chunkBytes);
        if (remaining != 0) {
            RUVIA_CHECK(conn.pendingOutput().empty());
        }
    }

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), std::size_t{ruvia::detail::kHttp2WindowUpdateFrameBytes});
    const auto update = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(update.type, static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(update.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(ruvia::detail::http2WindowUpdateIncrement(out.substr(9, 4)), threshold);
}

RUVIA_TEST(http2_connection_data_after_peer_reset_is_connection_error) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);
        openThenPeerReset(conn, &resource, pinned);

        // The peer's RST_STREAM precedes this DATA on the same ordered byte
        // stream. It cannot be an in-flight race with a reset sent by us, and
        // replying with another stream frame would itself violate RFC 9113 6.4.
        const auto data = dataFrame(&resource, 1, 0, {});
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
                    ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kStreamClosed);
        RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()),
            static_cast<std::uint32_t>(Http2ErrorCode::kStreamClosed));

        if (pinned) {
            conn.unpinStream(1);
        }
    }
}

RUVIA_TEST(http2_connection_data_after_local_reset_is_discarded_without_stream_output) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);
        openThenLocalReset(conn, &resource, pinned);

        // DATA might have been in flight when this endpoint sent RST_STREAM.
        // Minimal processing consumes connection flow control but cannot emit a
        // second stream frame after the stream entered the closed state.
        const auto data = dataFrame(&resource, 1, 0, {});
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
                    ruvia::detail::Http2FeedResult::kAccepted);
        RUVIA_CHECK(!conn.connectionError().has_value());
        RUVIA_CHECK(conn.pendingOutput().empty());

        if (pinned) {
            conn.unpinStream(1);
        }
    }
}

RUVIA_TEST(http2_connection_closed_stream_data_flood_never_amplifies_output) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    openThenLocalReset(conn, &resource);

    // Empty DATA isolates the closed-state decision from flow-control credit.
    // Even without draining output, discarded frames must not manufacture an
    // unbounded queue of illegal second RST_STREAM frames.
    const auto data = dataFrame(&resource, 1, 0, {});
    bool acceptedAll = true;
    for (int i = 0; i < 1200; ++i) {
        if (conn.feed(std::string_view(data.data(), data.size())) !=
            ruvia::detail::Http2FeedResult::kAccepted) {
            acceptedAll = false;
            break;
        }
    }
    RUVIA_CHECK(acceptedAll);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_closed_data_credit_does_not_amplify_output) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    openThenLocalReset(conn, &resource);

    const auto data = dataFrame(&resource, 1, 0, "x");
    for (int i = 0; i < 1200; ++i) {
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
                    ruvia::detail::Http2FeedResult::kAccepted);
    }
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());
}
