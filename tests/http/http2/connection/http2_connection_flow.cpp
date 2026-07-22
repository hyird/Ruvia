#include "http2_connection_fixture.h"

// Http2Connection: flow control windows and GOAWAY draining.

// A valid connection-level WINDOW_UPDATE just opens the send window: no error, no
// output frame.

RUVIA_TEST(http2_connection_feed_connection_window_update_ok) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 0, 1000);
    const auto result = conn.feed(std::string_view(wu, sizeof(wu)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// A zero-increment connection WINDOW_UPDATE is a protocol error (GOAWAY).

RUVIA_TEST(http2_connection_feed_zero_window_update_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 0, 0);
    const auto result = conn.feed(std::string_view(wu, sizeof(wu)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError().has_value());
    const auto goaway = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}

// Opening stream 3 transitions skipped stream 1 from idle to closed (RFC 9113
// §5.1.1). A WINDOW_UPDATE is permitted there, but §6.9 still requires a stream
// PROTOCOL_ERROR when its increment is zero. Since RST_STREAM is forbidden on
// the already-closed stream, the implementation promotes that error to GOAWAY.

RUVIA_TEST(http2_connection_zero_window_update_on_skipped_stream_is_connection_error) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeGetRequest(block);
    const auto request = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(request) == Http2FeedResult::kAccepted);
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(conn.pendingOutput().size());

    char update[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(update, 1, 0);
    RUVIA_CHECK(conn.feed(std::string_view(update, sizeof(update))) ==
        Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(
        conn.connectionError() == Http2ErrorCode::kProtocolError);

    const auto out = conn.pendingOutput();
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(
        goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(goaway.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

// RFC 9113 §6.8 distinguishes the two GOAWAY malformations: a nonzero stream id is a
// PROTOCOL_ERROR, a payload shorter than the 8 fixed octets is a FRAME_SIZE_ERROR.

RUVIA_TEST(http2_connection_malformed_goaway_error_codes) {
    const auto goawayErrorFor = [&](std::uint32_t streamId, std::size_t payloadBytes) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);
        conn.consumeOutput(conn.pendingOutput().size());

        std::pmr::string frame(&resource);
        char hdr[9];
        ruvia::detail::http2EncodeFrameHeader(
            hdr,
            static_cast<std::uint32_t>(payloadBytes),
            Http2FrameType::kGoaway,
            0,
            streamId);
        frame.append(hdr, 9);
        frame.append(payloadBytes, '\0');
        (void)conn.feed(std::string_view(frame.data(), frame.size()));

        const auto out = conn.pendingOutput();
        const auto header = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(header.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
        return ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(out.data() + 13));
    };

    RUVIA_CHECK_EQ(
        goawayErrorFor(1, 8),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    RUVIA_CHECK_EQ(
        goawayErrorFor(0, 7),
        static_cast<std::uint32_t>(Http2ErrorCode::kFrameSizeError));
}

// RST_STREAM referencing an idle (never-opened) stream is a protocol error (GOAWAY).

RUVIA_TEST(http2_connection_feed_rst_on_idle_stream_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char frame[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(frame, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(frame + 9, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError().has_value());
    const auto g = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(g.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}

RUVIA_TEST(http2_connection_consumed_data_batches_window_updates_at_half_window) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
        Http2FeedResult::kAccepted);
    while (conn.nextEvent().has_value()) {
    }

    constexpr std::uint32_t chunkBytes = 16 * 1024;
    constexpr std::uint32_t threshold =
        ruvia::detail::kHttp2ReceiveWindowUpdateThreshold;
    static_assert(threshold % chunkBytes == 0);
    std::pmr::string body(chunkBytes, 'x', &resource);
    const auto data = dataFrame(
        &resource,
        1,
        0,
        std::string_view(body.data(), body.size()));

    for (std::uint32_t consumed = chunkBytes;
         consumed <= threshold;
         consumed += chunkBytes) {
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
            Http2FeedResult::kAccepted);
        RUVIA_CHECK(conn.nextEvent().value().kind() ==
            Http2EventKind::kMessageBodyChunk);
        RUVIA_CHECK(!conn.nextEvent().has_value());
        conn.releaseReceivedData(1);
        if (consumed < threshold) {
            RUVIA_CHECK(conn.pendingOutput().empty());
        }
    }

    const auto updates = conn.pendingOutput();
    RUVIA_CHECK_EQ(
        updates.size(),
        std::size_t{2 * ruvia::detail::kHttp2WindowUpdateFrameBytes});
    const auto connectionUpdate =
        ruvia::detail::http2ParseFrameHeader(updates.substr(0, 9));
    const auto streamUpdate = ruvia::detail::http2ParseFrameHeader(
        updates.substr(ruvia::detail::kHttp2WindowUpdateFrameBytes, 9));
    RUVIA_CHECK_EQ(
        connectionUpdate.type,
        static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(connectionUpdate.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        streamUpdate.type,
        static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(streamUpdate.streamId, std::uint32_t{1});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2WindowUpdateIncrement(updates.substr(9, 4)),
        threshold);
    RUVIA_CHECK_EQ(
        ruvia::detail::http2WindowUpdateIncrement(updates.substr(
            ruvia::detail::kHttp2WindowUpdateFrameBytes + 9,
            4)),
        threshold);
}

// A body larger than the send window is partially sent and the remainder queued. The
// core owns that remainder; a second submission is rejected without growing output.
// WINDOW_UPDATE drains it with END_STREAM and reports the stream ready for new input.

RUVIA_TEST(http2_connection_submit_data_blocks_then_drains_on_window) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 3);  // stream 1 starts with a 3-byte send window
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(ruvia::http_status::kOk);
    response.header("Content-Length", "10");
    const auto headResult = conn.submitStreamingResponseHead(
        1,
        std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);
    RUVIA_CHECK(responseHeadSubmitted(headResult));
    conn.consumeOutput(conn.pendingOutput().size());

    const char body[5] = {'a', 'b', 'c', 'd', 'e'};
    const auto r1 = conn.submitData(
        1, std::string_view(body, 5), Http2EndStream::kKeepOpen);
    RUVIA_CHECK(r1 == Http2DataSubmitStatus::kQueued);
    RUVIA_CHECK(conn.hasQueuedData(1));
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(
        requireLocalKnownLength(*stream).declaredLength(), std::uint64_t{10});
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{3});

    const auto out1 = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(out1.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(3));            // only 3 fit
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);   // not terminal
    const auto queuedBytes = out1.size();
    RUVIA_CHECK(conn.submitData(1, "later", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kBackpressured);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), queuedBytes);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{3});
    conn.consumeOutput(out1.size());

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 1, 10);  // reopen stream 1's window
    (void)conn.feed(std::string_view(wu, sizeof(wu)));

    const auto out2 = conn.pendingOutput();
    const auto d2 = ruvia::detail::http2ParseFrameHeader(out2.substr(0, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d2.length, static_cast<std::uint32_t>(2));            // remaining 2
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) == 0);

    const auto drained = conn.takeDrainedDataStreams();
    RUVIA_CHECK_EQ(drained.size(), static_cast<std::size_t>(1));
    RUVIA_CHECK_EQ(drained[0], static_cast<std::uint32_t>(1));
    RUVIA_CHECK(!conn.hasQueuedData(1));
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});
    RUVIA_CHECK(conn.submitData(1, "later", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{10});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{10});
}

RUVIA_TEST(http2_connection_goaway_rejects_unprocessed_requests_in_core) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshakeWithWindow(client, 0);

    const auto first = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/first", {},
        Http2RequestContent::none());
    const auto second = client.submitRegularRequestHead(
        "POST", "https", "example.test", "/upload", {},
        Http2RequestContent::streaming());
    RUVIA_CHECK(first.submitted() != nullptr);
    RUVIA_CHECK(second.submitted() != nullptr);
    const auto firstStreamId = submittedRequestStreamId(first);
    const auto secondStreamId = submittedRequestStreamId(second);
    RUVIA_CHECK_EQ(firstStreamId, std::uint32_t{1});
    RUVIA_CHECK_EQ(secondStreamId, std::uint32_t{3});
    client.pinStream(secondStreamId);
    client.consumeOutput(client.pendingOutput().size());
    RUVIA_CHECK(client.submitData(
        secondStreamId, "queued", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kQueued);
    RUVIA_CHECK(client.hasQueuedData(secondStreamId));

    const auto goaway = goawayFrame(
        &resource, firstStreamId, Http2ErrorCode::kNoError);
    const auto result = client.feed(
        std::string_view(goaway.data(), goaway.size()));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!client.connectionError().has_value());
    RUVIA_CHECK(client.draining());
    const auto reciprocal = client.pendingOutput();
    const auto reciprocalHead = ruvia::detail::http2ParseFrameHeader(
        reciprocal.substr(0, 9));
    RUVIA_CHECK_EQ(
        reciprocalHead.type,
        static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read31(
            reinterpret_cast<const unsigned char*>(reciprocal.data() + 9)),
        std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(reciprocal.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kNoError));
    client.consumeOutput(reciprocal.size());

    const auto info = client.peerGoaway();
    RUVIA_CHECK(info.has_value());
    RUVIA_CHECK_EQ(info->lastStreamId(), firstStreamId);
    RUVIA_CHECK(info->error() == Http2ErrorCode::kNoError);
    auto event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kGoaway);
    RUVIA_CHECK_EQ(event.goaway()->lastStreamId(), firstStreamId);
    RUVIA_CHECK(event.goaway()->error() == Http2ErrorCode::kNoError);
    event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kRequestUnprocessed);
    RUVIA_CHECK_EQ(
        event.requestUnprocessed()->streamId(), secondStreamId);
    RUVIA_CHECK(!client.nextEvent().has_value());

    auto* firstStream = client.stream(firstStreamId);
    auto* secondStream = client.stream(secondStreamId);
    RUVIA_CHECK(firstStream != nullptr && !firstStream->isAborted());
    RUVIA_CHECK(secondStream != nullptr && secondStream->isAborted());
    RUVIA_CHECK(secondStream->localSend().aborted() != nullptr);
    RUVIA_CHECK(secondStream->localSend().aborted()->source() ==
        ruvia::detail::Http2StreamCloseSource::kPeerGoaway);
    RUVIA_CHECK(!secondStream->releasePeerConcurrencySlot());
    RUVIA_CHECK(!client.hasQueuedData(secondStreamId));
    RUVIA_CHECK(requestHeadSubmitError(client.submitRegularRequestHead(
        "GET", "https", "example.test", "/new", {},
        Http2RequestContent::none())) ==
        Http2RequestHeadSubmitError::kConnectionUnavailable);

    client.unpinStream(secondStreamId);
    RUVIA_CHECK(client.stream(secondStreamId) == nullptr);

    // GOAWAY does not abort a request at or below the peer boundary. Its response can
    // still complete after the reciprocal local drain has started.
    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    const auto responseHead = headersFrame(
        &resource,
        firstStreamId,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(
        std::string_view(responseHead.data(), responseHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(!client.connectionError().has_value());
}

RUVIA_TEST(http2_connection_goaway_last_stream_id_is_monotonic) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);
    for (const std::string_view path : {"/one", "/two", "/three"}) {
        const auto submitted = client.submitRegularRequestHead(
            "GET", "https", "example.test", path, {},
            Http2RequestContent::none());
        RUVIA_CHECK(submitted.submitted() != nullptr);
    }
    client.consumeOutput(client.pendingOutput().size());

    const auto notice = goawayFrame(
        &resource, 0x7fffffffU, Http2ErrorCode::kNoError);
    RUVIA_CHECK(client.feed(
        std::string_view(notice.data(), notice.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    auto event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kGoaway);
    RUVIA_CHECK_EQ(
        event.goaway()->lastStreamId(), std::uint32_t{0x7fffffffU});
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(client.stream(5) != nullptr);
    RUVIA_CHECK(client.draining());
    RUVIA_CHECK(!client.pendingOutput().empty());  // reciprocal GOAWAY(NO_ERROR)
    client.consumeOutput(client.pendingOutput().size());

    const auto narrowed = goawayFrame(
        &resource, 3, Http2ErrorCode::kInternalError);
    RUVIA_CHECK(client.feed(
        std::string_view(narrowed.data(), narrowed.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kGoaway);
    RUVIA_CHECK_EQ(event.goaway()->lastStreamId(), std::uint32_t{3});
    RUVIA_CHECK(event.goaway()->error() == Http2ErrorCode::kInternalError);
    event = client.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kRequestUnprocessed);
    RUVIA_CHECK_EQ(
        event.requestUnprocessed()->streamId(), std::uint32_t{5});
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(client.stream(5) == nullptr);
    const auto narrowedInfo = client.peerGoaway();
    RUVIA_CHECK(narrowedInfo.has_value());
    RUVIA_CHECK_EQ(narrowedInfo->lastStreamId(), std::uint32_t{3});
    RUVIA_CHECK(narrowedInfo->error() == Http2ErrorCode::kInternalError);
    RUVIA_CHECK(client.pendingOutput().empty());  // local drain is idempotent

    const auto invalidIncrease = goawayFrame(
        &resource, 5, Http2ErrorCode::kNoError);
    RUVIA_CHECK(client.feed(
        std::string_view(invalidIncrease.data(), invalidIncrease.size())) ==
        ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(client.connectionError() == Http2ErrorCode::kProtocolError);
    RUVIA_CHECK_EQ(client.peerGoaway()->lastStreamId(), std::uint32_t{3});
    const auto localGoaway = client.pendingOutput();
    const auto head = ruvia::detail::http2ParseFrameHeader(
        localGoaway.substr(0, 9));
    RUVIA_CHECK_EQ(head.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(localGoaway.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_connection_goaway_cannot_exclude_a_started_response) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);
    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {},
        Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    const auto responseHead = headersFrame(
        &resource,
        streamId,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(
        std::string_view(responseHead.data(), responseHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!client.nextEvent().has_value());

    const auto contradictory = goawayFrame(
        &resource, 0, Http2ErrorCode::kNoError);
    RUVIA_CHECK(client.feed(
        std::string_view(contradictory.data(), contradictory.size())) ==
        ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(client.connectionError() == Http2ErrorCode::kProtocolError);
    RUVIA_CHECK(!client.peerGoaway().has_value());
    RUVIA_CHECK(client.stream(streamId) != nullptr);
    RUVIA_CHECK(!client.stream(streamId)->isAborted());
    RUVIA_CHECK(!client.nextEvent().has_value());
}

RUVIA_TEST(http2_connection_peer_goaway_drains_without_truncating_server_request) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource, Http2Role::kServer);
    handshake(server);

    const auto requestHead = postHeadFrame(&resource, "4");
    RUVIA_CHECK(server.feed(
        std::string_view(requestHead.data(), requestHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    auto event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK_EQ(event.messageHead()->streamId(), std::uint32_t{1});
    RUVIA_CHECK(!server.nextEvent().has_value());

    // The client GOAWAY and the rest of an already established request can share one
    // transport read. The core must consume both frames and preserve event order.
    const auto peerGoaway = goawayFrame(
        &resource, 0, Http2ErrorCode::kNoError);
    const auto requestBody = dataFrame(
        &resource,
        1,
        ruvia::detail::kHttp2FlagEndStream,
        "body");
    std::pmr::string batch(&resource);
    batch.append(peerGoaway.data(), peerGoaway.size());
    batch.append(requestBody.data(), requestBody.size());
    const auto result = server.feed(
        std::string_view(batch.data(), batch.size()));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.connectionError().has_value());
    RUVIA_CHECK(server.draining());
    RUVIA_CHECK(server.peerGoaway().has_value());

    event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kGoaway);
    RUVIA_CHECK_EQ(event.goaway()->lastStreamId(), std::uint32_t{0});
    event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kMessageBodyChunk);
    RUVIA_CHECK(event.messageBodyChunk()->bytes() == "body");
    event = server.nextEvent().value();
    RUVIA_CHECK(event.kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK_EQ(event.messageEnd()->streamId(), std::uint32_t{1});
    RUVIA_CHECK(!server.nextEvent().has_value());

    // Reciprocal GOAWAY uses the highest accepted client stream (1), not the peer's
    // directional last-stream-id (which refers to server-initiated streams).
    const auto reciprocal = server.pendingOutput();
    const auto reciprocalHead = ruvia::detail::http2ParseFrameHeader(
        reciprocal.substr(0, 9));
    RUVIA_CHECK_EQ(
        reciprocalHead.type,
        static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read31(
            reinterpret_cast<const unsigned char*>(reciprocal.data() + 9)),
        std::uint32_t{1});
    server.consumeOutput(reciprocal.size());

    // A stream opened after our advertised boundary is safely refused, while stream 1
    // remains response-capable.
    std::pmr::string lateBlock(&resource);
    encodeGetRequest(lateBlock);
    const auto late = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(lateBlock.data(), lateBlock.size()));
    RUVIA_CHECK(server.feed(
        std::string_view(late.data(), late.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.nextEvent().has_value());
    const auto reset = server.pendingOutput();
    const auto resetHead = ruvia::detail::http2ParseFrameHeader(reset.substr(0, 9));
    RUVIA_CHECK_EQ(
        resetHead.type,
        static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(resetHead.streamId, std::uint32_t{3});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(reset.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kRefusedStream));
    server.consumeOutput(reset.size());

    ruvia::HttpResponse response(&resource);
    response.status(ruvia::http_status::kOk);
    response.body("ok");
    RUVIA_CHECK(responseHeadSubmitted(
        submitBufferedResponseHead(server, 1, response)));
    RUVIA_CHECK(server.submitData(
        1, "ok", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK(!server.connectionError().has_value());
}

// Graceful drain (RFC 9113 §6.8): beginDrain emits GOAWAY(NO_ERROR) at the last
// accepted stream id; streams already open keep working, HEADERS for a higher id are
// refused with RST_STREAM(REFUSED_STREAM), and beginDrain is idempotent.

RUVIA_TEST(http2_connection_begin_drain_refuses_new_streams) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);  // stream 1 open (half-closed remote)
    conn.consumeOutput(conn.pendingOutput().size());

    conn.beginDrain();
    // GOAWAY(NO_ERROR, lastStreamId=1) emitted without a connection error.
    const auto goaway = conn.pendingOutput();
    RUVIA_CHECK(goaway.size() >= 9);
    const auto gh = ruvia::detail::http2ParseFrameHeader(goaway.substr(0, 9));
    RUVIA_CHECK_EQ(gh.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.draining());
    conn.consumeOutput(goaway.size());

    // A new stream ABOVE the advertised id (3) is refused only after its complete
    // multi-frame field block is decoded. The inserted dynamic entry must survive.
    std::pmr::string block(&resource);
    encodeGetRequest(block);
    encodeShortDynamicHeader(block, "x-refused", "indexed");
    const auto split = block.size() / 2;
    const auto h = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), split));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.headerBlockInProgress());
    RUVIA_CHECK(conn.pendingOutput().empty());
    const auto continuation = continuationFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data() + split, block.size() - split));
    (void)conn.feed(std::string_view(continuation.data(), continuation.size()));
    while (conn.nextEvent().has_value()) {
        // stream 3 must NOT surface as a request (it was refused)
    }
    const auto rst = conn.pendingOutput();
    RUVIA_CHECK(rst.size() >= 9);
    const auto rh = ruvia::detail::http2ParseFrameHeader(rst.substr(0, 9));
    RUVIA_CHECK_EQ(rh.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(rh.streamId, static_cast<std::uint32_t>(3));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(rst.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kRefusedStream));
    RUVIA_CHECK(!conn.connectionError().has_value());  // refusal is not a connection error
    conn.consumeOutput(rst.size());

    // A later refused stream can reference the dynamic entry created by stream 3;
    // successful decode yields another REFUSED_STREAM, never COMPRESSION_ERROR.
    std::pmr::string dependent(&resource);
    encodeGetRequest(dependent);
    HpackEncoder::encodeIndexed(dependent, 62);
    const auto h5 = headersFrame(
        &resource,
        5,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(dependent.data(), dependent.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h5.data(), h5.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    const auto rst5 = conn.pendingOutput();
    const auto rh5 = ruvia::detail::http2ParseFrameHeader(rst5.substr(0, 9));
    RUVIA_CHECK_EQ(rh5.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(rh5.streamId, static_cast<std::uint32_t>(5));
    RUVIA_CHECK(!conn.connectionError().has_value());
    conn.consumeOutput(rst5.size());

    // Stream 1 (opened before the drain) can still be answered.
    ruvia::HttpResponse response(&resource);
    response.status(ruvia::http_status::kOk);
    response.body("ok");
    RUVIA_CHECK(responseHeadSubmitted(
        submitBufferedResponseHead(conn, 1, response)));
    RUVIA_CHECK(conn.submitData(1, "ok", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.pendingOutput().size() > 9);  // response frames produced
    conn.consumeOutput(conn.pendingOutput().size());

    conn.beginDrain();  // idempotent: no further GOAWAY
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// RFC 9113 §6.8: "Endpoints MUST NOT increase the value they send in the last stream
// identifier." A drain advertises id 1; a later HEADERS(3) is refused but still raises
// the internal idle-stream high-water mark. A fatal GOAWAY after that must still
// advertise 1 -- widening to 3 would tell the peer that a request it already retried
// elsewhere (on REFUSED_STREAM) might have been processed after all.

RUVIA_TEST(http2_connection_fatal_goaway_never_widens_drained_last_stream_id) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);  // stream 1 open
    conn.consumeOutput(conn.pendingOutput().size());

    conn.beginDrain();  // GOAWAY(NO_ERROR, lastStreamId=1)
    const auto drainGoaway = conn.pendingOutput();
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read31(
            reinterpret_cast<const unsigned char*>(drainGoaway.data() + 9)),
        static_cast<std::uint32_t>(1));
    conn.consumeOutput(drainGoaway.size());

    // Stream 3 arrives after the drain: refused, but it bumps lastStreamId_ to 3.
    std::pmr::string block(&resource);
    encodeGetRequest(block);
    const auto h3 = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h3.data(), h3.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    conn.consumeOutput(conn.pendingOutput().size());  // RST_STREAM(3, REFUSED_STREAM)

    // Now trip a connection error: the fatal GOAWAY must not advertise past 1.
    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 0, 0);
    RUVIA_CHECK(conn.feed(std::string_view(wu, sizeof(wu))) ==
        ruvia::detail::Http2FeedResult::kProtocolFailure);

    const auto fatal = conn.pendingOutput();
    const auto fh = ruvia::detail::http2ParseFrameHeader(fatal.substr(0, 9));
    RUVIA_CHECK_EQ(fh.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read31(
            reinterpret_cast<const unsigned char*>(fatal.data() + 9)),
        static_cast<std::uint32_t>(1));
}

RUVIA_TEST(http2_connection_fatal_failure_atomically_supersedes_local_drain) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    client.consumeOutput(client.pendingOutput().size());

    client.beginDrain();
    RUVIA_CHECK(client.draining());
    RUVIA_CHECK(!client.connectionError().has_value());
    client.consumeOutput(client.pendingOutput().size());

    // SETTINGS is connection-scoped. A non-zero stream id is fatal and must
    // replace, rather than coexist with, the earlier graceful drain state.
    char invalidSettings[9];
    ruvia::detail::http2EncodeFrameHeader(
        invalidSettings, 0, Http2FrameType::kSettings, 0, 1);
    RUVIA_CHECK(client.feed(std::string_view(
        invalidSettings, sizeof(invalidSettings))) ==
        Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(!client.draining());
    RUVIA_CHECK(client.connectionError() ==
        Http2ErrorCode::kProtocolError);

    const auto failedOutputSize = client.pendingOutput().size();
    client.beginDrain();
    RUVIA_CHECK(!client.draining());
    RUVIA_CHECK_EQ(client.pendingOutput().size(), failedOutputSize);

    const auto rejected = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {},
        Http2RequestContent::none());
    RUVIA_CHECK(rejected.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(rejected) ==
        Http2RequestHeadSubmitError::kConnectionUnavailable);
}

// Every non-empty DATA event retains receive-window debt. Removing the stream must
// transfer that debt into the connection's batched credit, even if the owner never
// calls releaseReceivedData(), or the shared window would shrink permanently.

RUVIA_TEST(http2_connection_window_debt_batches_on_removal) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto head = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
        Http2FeedResult::kAccepted);
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(conn.pendingOutput().size());

    constexpr std::uint32_t chunkBytes = Http2LocalSettings::kMaxFrameSize;
    constexpr std::uint32_t threshold =
        ruvia::detail::kHttp2ReceiveWindowUpdateThreshold;
    static_assert(threshold % chunkBytes == 0);
    std::pmr::string body(chunkBytes, 'x', &resource);
    const auto data = dataFrame(
        &resource,
        1,
        0,
        std::string_view(body.data(), body.size()));
    for (std::uint32_t received = chunkBytes;
         received <= threshold;
         received += chunkBytes) {
        RUVIA_CHECK(conn.feed(std::string_view(data.data(), data.size())) ==
            Http2FeedResult::kAccepted);
        RUVIA_CHECK(conn.nextEvent().value().kind() ==
            Http2EventKind::kMessageBodyChunk);
        RUVIA_CHECK(!conn.nextEvent().has_value());
        RUVIA_CHECK(conn.pendingOutput().empty());
    }

    // Peer RST_STREAM removes the unpinned stream. Exactly one threshold of banked
    // debt must now restore the connection scope, never the dead stream scope.
    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, 8 /* CANCEL */);
    RUVIA_CHECK(conn.feed(std::string_view(rst, sizeof(rst))) ==
        Http2FeedResult::kAccepted);
    while (conn.nextEvent().has_value()) {
    }

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(
        out.size(),
        std::size_t{ruvia::detail::kHttp2WindowUpdateFrameBytes});
    const auto update = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(
        update.type,
        static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(update.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2WindowUpdateIncrement(out.substr(9, 4)),
        threshold);
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_discarded_data_still_enforces_connection_window) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // Stream 1 banks valid DATA until owner acknowledgement, allowing the test to
    // exhaust the shared connection receive window without exceeding its stream
    // window.
    const auto streamingHead = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(
        std::string_view(streamingHead.data(), streamingHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    // Open then locally reset stream 3 so later DATA targets a known closed stream.
    std::pmr::string closedBlock(&resource);
    encodeGetRequest(closedBlock);
    const auto closedHead = headersFrame(
        &resource,
        3,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(closedBlock.data(), closedBlock.size()));
    RUVIA_CHECK(conn.feed(
        std::string_view(closedHead.data(), closedHead.size())) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    RUVIA_CHECK(conn.submitReset(3, Http2ErrorCode::kCancel) ==
        Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(conn.stream(3) == nullptr);
    conn.consumeOutput(conn.pendingOutput().size());

    std::string chunk(Http2LocalSettings::kMaxFrameSize, 'x');
    std::uint32_t remaining = Http2LocalSettings::kInitialWindowSize;
    while (remaining != 0) {
        const auto chunkBytes = static_cast<std::size_t>(
            remaining < chunk.size() ? remaining : chunk.size());
        const auto data = dataFrame(
            &resource,
            1,
            0,
            std::string_view(chunk.data(), chunkBytes));
        RUVIA_CHECK(conn.feed(
            std::string_view(data.data(), data.size())) ==
            ruvia::detail::Http2FeedResult::kAccepted);
        const auto event = conn.nextEvent().value();
        RUVIA_CHECK(event.kind() == Http2EventKind::kMessageBodyChunk);
        RUVIA_CHECK_EQ(event.messageBodyChunk()->bytes().size(), chunkBytes);
        RUVIA_CHECK(!conn.nextEvent().has_value());
        RUVIA_CHECK(conn.pendingOutput().empty());
        remaining -= static_cast<std::uint32_t>(chunkBytes);
    }

    // Even though stream 3 is closed and its DATA will be discarded, the DATA must
    // first fit the connection window. At zero remaining credit this is a connection
    // FLOW_CONTROL_ERROR, not RST_STREAM plus an unearned WINDOW_UPDATE.
    const auto overflow = dataFrame(&resource, 3, 0, "x");
    const auto result = conn.feed(
        std::string_view(overflow.data(), overflow.size()));
    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kFlowControlError);
    const auto goaway = conn.pendingOutput();
    const auto goawayHead = ruvia::detail::http2ParseFrameHeader(
        goaway.substr(0, 9));
    RUVIA_CHECK_EQ(
        goawayHead.type,
        static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(
            reinterpret_cast<const unsigned char*>(goaway.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kFlowControlError));
}

// Healthy keepalive PINGs (ACKs drained each round) never trip the budget, however many.

RUVIA_TEST(http2_connection_drained_pings_never_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    std::memset(ping + 9, 0, 8);

    for (int i = 0; i < 5000; ++i) {
        const auto r = conn.feed(std::string_view(ping, sizeof(ping)));
        RUVIA_CHECK(r == ruvia::detail::Http2FeedResult::kAccepted);
        conn.consumeOutput(conn.pendingOutput().size());  // flush ACK -> resets budget
        RUVIA_CHECK(!conn.connectionError().has_value());
    }
}

// Healthy SETTINGS re-tuning (ACKs drained each round) never trips, however many.

RUVIA_TEST(http2_connection_drained_settings_never_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);

    for (int i = 0; i < 5000; ++i) {
        const auto r = conn.feed(std::string_view(settings, sizeof(settings)));
        RUVIA_CHECK(r == ruvia::detail::Http2FeedResult::kAccepted);
        conn.consumeOutput(conn.pendingOutput().size());  // flush ACK -> resets budget
        RUVIA_CHECK(!conn.connectionError().has_value());
    }
}

RUVIA_TEST(http2_connection_window_update_after_peer_reset_never_reopens_stream) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);
        openThenPeerReset(conn, &resource, pinned);

        // RFC 9113 section 6.9 permits WINDOW_UPDATE on a closed stream. A
        // valid increment is ignored, including while pinned storage remains.
        char update[ruvia::detail::kHttp2WindowUpdateFrameBytes];
        ruvia::detail::http2WriteWindowUpdate(update, 1, 1);
        RUVIA_CHECK(
            conn.feed(std::string_view(update, sizeof(update))) ==
            ruvia::detail::Http2FeedResult::kAccepted);
        RUVIA_CHECK(!conn.connectionError().has_value());
        RUVIA_CHECK(conn.pendingOutput().empty());

        // Increment zero is still a PROTOCOL_ERROR. Because the peer reset
        // already closed this ordered stream, the core cannot legally send a
        // second RST_STREAM and must promote the stream error to the connection.
        ruvia::detail::http2WriteWindowUpdate(update, 1, 0);
        RUVIA_CHECK(
            conn.feed(std::string_view(update, sizeof(update))) ==
            ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(
            conn.connectionError() == Http2ErrorCode::kProtocolError);
        RUVIA_CHECK_EQ(
            firstGoawayError(conn.pendingOutput()),
            static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));

        if (pinned) {
            conn.unpinStream(1);
        }
    }
}

RUVIA_TEST(http2_connection_zero_window_update_on_closed_stream_is_connection_error) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);
        openThenLocalReset(conn, &resource, pinned);

        char update[ruvia::detail::kHttp2WindowUpdateFrameBytes];
        ruvia::detail::http2WriteWindowUpdate(update, 1, 1);
        RUVIA_CHECK(
            conn.feed(std::string_view(update, sizeof(update))) ==
            ruvia::detail::Http2FeedResult::kAccepted);
        RUVIA_CHECK(!conn.connectionError().has_value());
        RUVIA_CHECK(conn.pendingOutput().empty());

        ruvia::detail::http2WriteWindowUpdate(update, 1, 0);
        RUVIA_CHECK(
            conn.feed(std::string_view(update, sizeof(update))) ==
            ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(
            conn.connectionError() == Http2ErrorCode::kProtocolError);
        RUVIA_CHECK_EQ(
            firstGoawayError(conn.pendingOutput()),
            static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));

        if (pinned) {
            conn.unpinStream(1);
        }
    }
}
