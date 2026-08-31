#include "http2_connection_fixture.h"

// Http2Connection: HEADERS, CONTINUATION, HPACK and trailers.

RUVIA_TEST(http2_connection_header_table_reduction_prefixes_next_field_block) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginPeerInput(client);

    // RFC 9113 §4.3.1: after acknowledging a reduction of the peer's HPACK
    // dynamic-table maximum, the next field block must begin with a conformant
    // Dynamic Table Size Update. The stateless Ruvia encoder uses no dynamic
    // entries, so it can truthfully select zero (encoded as the single byte 0x20).
    char settings[15];
    auto* out = ruvia::detail::http2WriteFrameHeader(settings, 6, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(
        out, ruvia::detail::Http2SettingId::kHeaderTableSize, 0);
    RUVIA_CHECK_EQ(out, settings + sizeof(settings));
    RUVIA_CHECK(
        client.feed(std::string_view(settings, sizeof(settings))) == Http2FeedResult::kAccepted);

    const auto ack = client.pendingOutput();
    RUVIA_CHECK_EQ(ack.size(), std::size_t{9});
    RUVIA_CHECK(
        (ruvia::detail::http2ParseFrameHeader(ack).flags & ruvia::detail::kHttp2FlagAck) != 0);
    client.consumeOutput(ack.size());

    const auto submitted = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(submitted.submitted() != nullptr);
    const auto bytes = client.pendingOutput();
    const auto headers = ruvia::detail::http2ParseFrameHeader(bytes.substr(0, 9));
    RUVIA_CHECK_EQ(headers.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK(headers.length > 0);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(bytes[9]), static_cast<unsigned char>(0x20));
}

RUVIA_TEST(http2_connection_rejects_invalid_hpack_size_update_sequences) {
    // RFC 7541 section 4.2 permits at most two table-size updates at the start
    // of a field block and requires the smallest value before the final value.
    // Either violation makes the field block undecodable, which RFC 9113
    // section 4.3 maps to a connection-level COMPRESSION_ERROR.
    constexpr std::string_view invalidPrefixes[] = {
        std::string_view("\x20\x21\x22", 3),  // three updates
        std::string_view("\x2a\x25", 2),      // 10 then 5: smallest is last
    };

    for (const auto prefix : invalidPrefixes) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(prefix, &resource);
        encodeGetRequest(block);
        const auto request = headersFrame(&resource, 1,
            ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
                    Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kCompressionError);

        const auto output = conn.pendingOutput();
        RUVIA_CHECK(output.size() >= 17);
        if (output.size() >= 17) {
            const auto goaway = ruvia::detail::http2ParseFrameHeader(output.substr(0, 9));
            RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
            RUVIA_CHECK_EQ(ruvia::detail::http2Read32(
                               reinterpret_cast<const unsigned char*>(output.data() + 13)),
                static_cast<std::uint32_t>(Http2ErrorCode::kCompressionError));
        }
    }
}

// RFC 9113 §4.2/§6.2 gives malformed HEADERS payloads two distinct connection
// errors: missing mandatory PRIORITY fields are FRAME_SIZE_ERROR, while an invalid
// Pad Length remains PROTOCOL_ERROR.

RUVIA_TEST(http2_connection_malformed_headers_payload_error_codes) {
    const auto goawayErrorFor = [&](std::uint8_t flags, std::string_view payload) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        const auto frame = headersFrame(&resource, 1, flags, payload);
        RUVIA_CHECK(conn.feed(frame) == Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(conn.connectionError().has_value());

        const auto out = conn.pendingOutput();
        const auto header = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(header.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
        return ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 13));
    };

    RUVIA_CHECK_EQ(
        goawayErrorFor(ruvia::detail::kHttp2FlagPriority, std::string_view("\0\0\0\0", 4)),
        static_cast<std::uint32_t>(Http2ErrorCode::kFrameSizeError));
    RUVIA_CHECK_EQ(goawayErrorFor(ruvia::detail::kHttp2FlagPadded, std::string_view()),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_connection_rejects_regular_header_values_with_edge_whitespace) {
    for (const auto value : {" value", "value ", "\tvalue", "value\t"}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);

        std::pmr::string block(&resource);
        encodeGetRequest(block);
        HpackEncoder::encodeHeader(block, "x-test", value);
        const auto request = headersFrame(&resource, 1,
            ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
            std::string_view(block.data(), block.size()));

        RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
                    Http2FeedResult::kAccepted);
        RUVIA_CHECK(!conn.connectionError().has_value());
        const auto event = conn.nextEvent();
        RUVIA_CHECK(event.has_value());
        if (event.has_value()) {
            RUVIA_CHECK(event->kind() == Http2EventKind::kStreamClosed);
        }
        RUVIA_CHECK(!conn.nextEvent().has_value());

        const auto out = conn.pendingOutput();
        RUVIA_CHECK(out.size() >= ruvia::detail::kHttp2FrameHeaderBytes + 4);
        if (out.size() >= ruvia::detail::kHttp2FrameHeaderBytes + 4) {
            const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
            RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
            RUVIA_CHECK_EQ(reset.streamId, std::uint32_t{1});
            RUVIA_CHECK_EQ(
                ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
                static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
        }
        RUVIA_CHECK(conn.stream(1) == nullptr);
    }
}

// A HEADERS frame WITHOUT END_HEADERS leaves the block open (awaiting CONTINUATION); a
// CONTINUATION carrying the rest with END_HEADERS completes the head and emits the event.

RUVIA_TEST(http2_connection_feed_headers_continuation_completes_head) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string first(&resource);
    HpackEncoder::encodeHeader(first, ":method", "GET");
    HpackEncoder::encodeHeader(first, ":scheme", "https");
    std::pmr::string second(&resource);
    HpackEncoder::encodeHeader(second, ":path", "/");
    HpackEncoder::encodeHeader(second, ":authority", "example.com");

    // HEADERS with END_STREAM but no END_HEADERS -> no event yet.
    const auto h = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndStream,
        std::string_view(first.data(), first.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h.data(), h.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    // CONTINUATION with END_HEADERS -> head completes.
    char chdr[9];
    ruvia::detail::http2EncodeFrameHeader(chdr, static_cast<std::uint32_t>(second.size()),
        Http2FrameType::kContinuation, ruvia::detail::kHttp2FlagEndHeaders, 1);
    std::pmr::string cont(&resource);
    cont.append(chdr, 9);
    cont.append(second.data(), second.size());
    RUVIA_CHECK(conn.feed(std::string_view(cont.data(), cont.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);

    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr && s->requestMethod() == "GET");
    RUVIA_CHECK(s != nullptr && s->requestKnownMethod() == ruvia::HttpKnownMethod::kGet);
}

// RFC 9113 requires field blocks received after our RST_STREAM to be minimally
// processed. A pinned reset stream must not capture the fragments, and the dynamic
// entry created by the discarded block must remain usable by the next stream.

RUVIA_TEST(http2_connection_local_reset_discards_multiframe_headers_and_keeps_hpack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);
    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) == Http2SubmitStatus::kAccepted);
    conn.consumeOutput(conn.pendingOutput().size());

    std::pmr::string dynamic(&resource);
    encodeShortDynamicHeader(dynamic, "x-discarded", "indexed");
    const auto split = dynamic.size() / 2;
    const auto first = headersFrame(&resource, 1, 0, std::string_view(dynamic.data(), split));
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.headerBlockInProgress());
    RUVIA_CHECK(conn.pendingOutput().empty());  // no second RST

    const auto last = continuationFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(dynamic.data() + split, dynamic.size() - split));
    RUVIA_CHECK(conn.feed(std::string_view(last.data(), last.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(!conn.headerBlockInProgress());
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.stream(1) != nullptr && conn.stream(1)->isAborted());

    std::pmr::string nextBlock(&resource);
    encodeGetRequest(nextBlock);
    HpackEncoder::encodeIndexed(nextBlock, 62);  // x-discarded: indexed
    const auto next = headersFrame(&resource, 3,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(nextBlock.data(), nextBlock.size()));
    RUVIA_CHECK(conn.feed(std::string_view(next.data(), next.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.connectionError().has_value());
    conn.unpinStream(1);
}

// A trailer section without END_STREAM is a stream error, but its complete field
// block still has to update HPACK before the reset is emitted. Splitting it proves the
// core neither sends an early RST nor mistakes the required CONTINUATION for a new frame.

RUVIA_TEST(http2_connection_invalid_multiframe_trailer_resets_after_hpack_decode) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto request = postHeadFrame(&resource, "");
    RUVIA_CHECK(conn.feed(std::string_view(request.data(), request.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!conn.nextEvent().has_value());
    conn.consumeOutput(conn.pendingOutput().size());

    std::pmr::string trailer(&resource);
    encodeShortDynamicHeader(trailer, "x-invalid-trailer", "indexed");
    const auto split = trailer.size() / 2;
    const auto first = headersFrame(&resource, 1, 0, std::string_view(trailer.data(), split));
    RUVIA_CHECK(conn.feed(std::string_view(first.data(), first.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.headerBlockInProgress());
    RUVIA_CHECK(conn.pendingOutput().empty());

    const auto last = continuationFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(trailer.data() + split, trailer.size() - split));
    RUVIA_CHECK(conn.feed(std::string_view(last.data(), last.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    const auto resetBytes = conn.pendingOutput();
    RUVIA_CHECK_EQ(resetBytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(resetBytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(resetBytes.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(resetBytes.size());

    std::pmr::string nextBlock(&resource);
    encodeGetRequest(nextBlock);
    HpackEncoder::encodeIndexed(nextBlock, 62);  // x-invalid-trailer: indexed
    const auto next = headersFrame(&resource, 3,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(nextBlock.data(), nextBlock.size()));
    RUVIA_CHECK(conn.feed(std::string_view(next.data(), next.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.connectionError().has_value());
}

RUVIA_TEST(http2_connection_rejects_trailer_field_flood) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection connection(&resource);
    handshake(connection);

    const auto request = postHeadFrame(&resource, "");
    RUVIA_CHECK(connection.feed(std::string_view(request.data(), request.size())) ==
                Http2FeedResult::kAccepted);
    RUVIA_CHECK(connection.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!connection.nextEvent().has_value());

    std::pmr::string trailers(&resource);
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        HpackEncoder::encodeHeader(trailers, "x-trace", "value");
    }
    const auto trailerFrame = headersFrame(&resource, 1,
        static_cast<std::uint8_t>(
            ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream),
        std::string_view(trailers.data(), trailers.size()));
    RUVIA_CHECK(connection.feed(std::string_view(trailerFrame.data(), trailerFrame.size())) ==
                Http2FeedResult::kAccepted);

    bool sawClosed = false;
    while (const auto event = connection.nextEvent()) {
        RUVIA_CHECK(event->messageEnd() == nullptr);
        if (const auto* closed = event->streamClosed()) {
            sawClosed = true;
            RUVIA_CHECK(closed->source() == Http2StreamCloseSource::kLocal);
            RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
        }
    }
    RUVIA_CHECK(sawClosed);
    RUVIA_CHECK(connection.stream(1) == nullptr);
    RUVIA_CHECK(!connection.connectionError().has_value());

    const auto resetBytes = connection.pendingOutput();
    RUVIA_CHECK_EQ(resetBytes.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(resetBytes.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(reset.streamId, std::uint32_t{1});
}

RUVIA_TEST(http2_connection_rejects_invalid_request_head_before_hpack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const auto reject = [&](std::string_view method, std::string_view scheme,
                            std::optional<std::string_view> authority, std::string_view path,
                            std::span<const ruvia::HttpHeaderView> headers) {
        const auto result = client.submitRegularRequestHead(
            method, scheme, authority, path, headers, Http2RequestContent::none());
        RUVIA_CHECK(result.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(result) == Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    const ruvia::HttpHeaderView uppercase[] = {{"X-Test", "value"}};
    const ruvia::HttpHeaderView connection[] = {{"connection", "keep-alive"}};
    const ruvia::HttpHeaderView matchingHost[] = {{"host", "example.test"}};
    const ruvia::HttpHeaderView mismatchedHost[] = {{"host", "other.test"}};
    const ruvia::HttpHeaderView invalidExpect[] = {{"expect", "bad value"}};
    const ruvia::HttpHeaderView invalidTrailerList[] = {{"trailer", "x-checksum, bad field"}};
    const ruvia::HttpHeaderView emptyTrailerElement[] = {{"trailer", ","}};
    const ruvia::HttpHeaderView forbiddenTrailerName[] = {{"trailer", "content-length"}};
    reject("CONNECT", "https", "example.test:443", "/", {});
    reject("GET bad", "https", "example.test", "/", {});
    reject("GET", "1ftp", "example.test", "/", {});
    reject("GET", "https", "example.test", "*", {});
    reject("GET", "https", std::nullopt, "/", {});
    reject("GET", "HTTP", std::nullopt, "/", matchingHost);
    reject("GET", "https", "user@example.test", "/", {});
    reject("GET", "https", "example.test", "relative", {});
    reject("GET", "https", "example.test", "", {});
    reject("GET", "https", "example.test", "/", uppercase);
    reject("GET", "https", "example.test", "/", connection);
    reject("GET", "https", "example.test", "/", mismatchedHost);
    reject("GET", "https", "example.test", "/", invalidExpect);
    reject("GET", "https", "example.test", "/", invalidTrailerList);
    reject("GET", "https", "example.test", "/", emptyTrailerElement);
    reject("GET", "https", "example.test", "/", forbiddenTrailerName);

    const ruvia::HttpHeaderView emptyTrailerList[] = {{"trailer", ""}};
    const auto acceptedEmptyTrailerList = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", emptyTrailerList, Http2RequestContent::none());
    RUVIA_CHECK(acceptedEmptyTrailerList.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(acceptedEmptyTrailerList), std::uint32_t{1});
    client.consumeOutput(client.pendingOutput().size());

    const auto accepted = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{3});
}

RUVIA_TEST(http2_connection_submits_options_asterisk_with_authority) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);

    const auto accepted = client.submitRegularRequestHead(
        "OPTIONS", "https", "example.test", "*", {}, Http2RequestContent::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
    RUVIA_CHECK(!client.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_terminal_large_head_sets_end_stream_only_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kNoContent);
    const std::string largeValue(20 * 1024, 'a');
    response.header("X-Large", largeValue);
    const auto result = submitBufferedResponseHead(conn, 1, response);
    RUVIA_CHECK(responseHeadSubmitted(result));

    auto out = conn.pendingOutput();
    bool first = true;
    bool sawContinuation = false;
    bool sawEndHeaders = false;
    while (out.size() >= 9) {
        const auto frame = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK(out.size() >= 9 + frame.length);
        if (first) {
            RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
            RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
            first = false;
        } else {
            RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kContinuation));
            RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
            sawContinuation = true;
        }
        if ((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0) {
            sawEndHeaders = true;
        }
        out.remove_prefix(9 + frame.length);
    }
    RUVIA_CHECK(!first);
    RUVIA_CHECK(sawContinuation);
    RUVIA_CHECK(sawEndHeaders);
    RUVIA_CHECK(out.empty());
}

RUVIA_TEST(http2_connection_streaming_content_length_finish_and_trailers_are_exact) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    response.header("Content-Length", "5");
    RUVIA_CHECK(responseHeadSubmitted(conn.submitStreamingResponseHead(1, std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);

    RUVIA_CHECK(conn.submitData(1, "hey", Http2EndStream::kEndStream) ==
                Http2DataSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(conn.submitData(1, "toolong", Http2EndStream::kKeepOpen) ==
                Http2DataSubmitStatus::kContentLengthExceeded);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{0});

    RUVIA_CHECK(
        conn.submitData(1, "hel", Http2EndStream::kKeepOpen) == Http2DataSubmitStatus::kAccepted);
    conn.consumeOutput(conn.pendingOutput().size());
    const std::array<ruvia::HttpHeaderView, 1> trailers{ruvia::HttpHeaderView{"X-Checksum", "ok"}};
    RUVIA_CHECK(conn.finishResponse(1, validatedTrailers(trailers)) ==
                Http2FinishSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(stream->localSend().responseContentOpen() != nullptr);
    RUVIA_CHECK(conn.pendingOutput().empty());

    RUVIA_CHECK(
        conn.submitData(1, "lo", Http2EndStream::kKeepOpen) == Http2DataSubmitStatus::kAccepted);
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(
        conn.finishResponse(1, validatedTrailers(trailers)) == Http2FinishSubmitStatus::kAccepted);
    const auto trailer = conn.pendingOutput();
    const auto trailerBytes = trailer.size();
    RUVIA_CHECK(trailerBytes != 0);
    // The terminal call committed the complete section and closed the local half;
    // no separately staged metadata can be stranded by a later DATA submission.
    RUVIA_CHECK(
        conn.submitData(1, {}, Http2EndStream::kEndStream) == Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), trailerBytes);
    const auto frame = ruvia::detail::http2ParseFrameHeader(trailer.substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});
}

// An explicitly registered streaming HEAD route still cannot emit a payload.
// The method/status decision belongs to the HTTP/2 core, not the Web sink.

RUVIA_TEST(http2_connection_head_streaming_response_ends_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveRequest(conn, &resource, "HEAD");

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    response.header("Content-Length", "10");
    const auto headResult = conn.submitStreamingResponseHead(1, std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(submittedResponsePlan(headResult).bodyPlan().statusAllowsBody());
    RUVIA_CHECK(submittedResponsePlan(headResult).bodyPlan().bodySuppressed());
    RUVIA_CHECK(submittedResponsePlan(headResult).headDisposition() ==
                ResponseStreamHeadDisposition::kMessageEnded);
    RUVIA_CHECK(conn.stream(1)->localContent().forbidden() != nullptr);
    RUVIA_CHECK(conn.stream(1)->localContent().knownLength() == nullptr);
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

RUVIA_TEST(http2_connection_head_response_can_end_with_trailers_only) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveRequest(conn, &resource, "HEAD");

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    response.header("Content-Length", "10");
    const auto headResult = conn.submitStreamingResponseHead(1, std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kPresent);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(submittedResponsePlan(headResult).bodyPlan().bodySuppressed());
    RUVIA_CHECK(submittedResponsePlan(headResult).headDisposition() ==
                ResponseStreamHeadDisposition::kTrailersOnly);
    RUVIA_CHECK(submittedResponsePlan(headResult).trailerFraming() ==
                ResponseStreamTrailerFraming::kHttp2TrailingHeaders);
    const auto initialHead = conn.pendingOutput();
    const auto initialFrame = ruvia::detail::http2ParseFrameHeader(initialHead.substr(0, 9));
    RUVIA_CHECK_EQ(initialFrame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((initialFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(initialHead.size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->localSend().responseContentOpen() == nullptr);
    RUVIA_CHECK(stream->localSend().responseTrailersOnly() != nullptr);
    RUVIA_CHECK(stream->localContent().forbidden() != nullptr);
    RUVIA_CHECK(conn.submitData(1, "forbidden", Http2EndStream::kKeepOpen) ==
                Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK(conn.pendingOutput().empty());

    // Declaring trailer intent cannot fall back to an empty DATA terminator.
    RUVIA_CHECK(
        conn.finishResponse(1, validatedTrailers({})) == Http2FinishSubmitStatus::kInvalidState);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(stream->localSend().responseTrailersOnly() != nullptr);

    const std::array<ruvia::HttpHeaderView, 1> trailers{
        ruvia::HttpHeaderView{"Server-Timing", "db;dur=4"}};
    RUVIA_CHECK(
        conn.finishResponse(1, validatedTrailers(trailers)) == Http2FinishSubmitStatus::kAccepted);
    const auto terminal = conn.pendingOutput();
    const auto terminalFrame = ruvia::detail::http2ParseFrameHeader(terminal.substr(0, 9));
    RUVIA_CHECK_EQ(terminalFrame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((terminalFrame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
}

RUVIA_TEST(http2_response_finish_owns_trailer_section_atomically) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    const std::array<ruvia::HttpHeaderView, 1> validTrailers{
        ruvia::HttpHeaderView{"X-Checksum", "ok"}};
    RUVIA_CHECK(conn.finishResponse(1, validatedTrailers(validTrailers)) ==
                Http2FinishSubmitStatus::kInvalidState);

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    RUVIA_CHECK(responseHeadSubmitted(conn.submitStreamingResponseHead(1, std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());

    const std::array<ruvia::HttpHeaderView, 2> mixedTrailers{
        ruvia::HttpHeaderView{"X-Checksum", "ok"}, ruvia::HttpHeaderView{"Content-Length", "2"}};
    const auto mixedResult = ruvia::detail::httpResponseTrailerSection(mixedTrailers);
    RUVIA_CHECK(mixedResult.section() == nullptr);
    RUVIA_CHECK(mixedResult.failure() != nullptr);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.stream(1)->localSend().responseContentOpen() != nullptr);

    RUVIA_CHECK(conn.finishResponse(1, validatedTrailers(validTrailers)) ==
                Http2FinishSubmitStatus::kAccepted);
    const auto acceptedBytes = conn.pendingOutput().size();
    RUVIA_CHECK(acceptedBytes != 0);
    RUVIA_CHECK(conn.finishResponse(1, validatedTrailers(validTrailers)) ==
                Http2FinishSubmitStatus::kInvalidState);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), acceptedBytes);
}

RUVIA_TEST(http2_connection_reset_content_streaming_ends_on_headers) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kResetContent);
    response.header("Content-Length", "9");
    const auto headResult = conn.submitStreamingResponseHead(1, std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone);

    RUVIA_CHECK(responseHeadSubmitted(headResult));
    RUVIA_CHECK(!submittedResponsePlan(headResult).bodyPlan().statusAllowsBody());
    RUVIA_CHECK(submittedResponsePlan(headResult).bodyPlan().bodySuppressed());
    RUVIA_CHECK(submittedResponsePlan(headResult).headDisposition() ==
                ResponseStreamHeadDisposition::kMessageEnded);
    RUVIA_CHECK(conn.stream(1)->localContent().forbidden() != nullptr);
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

RUVIA_TEST(http2_connection_peer_reset_discards_queued_data_and_trailers) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 0);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    RUVIA_CHECK(responseHeadSubmitted(conn.submitStreamingResponseHead(1, std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "deferred", Http2EndStream::kKeepOpen) ==
                Http2DataSubmitStatus::kQueued);
    RUVIA_CHECK(conn.hasQueuedData(1));
    RUVIA_CHECK(conn.pendingOutput().empty());

    const std::array<ruvia::HttpHeaderView, 1> trailers{ruvia::HttpHeaderView{"X-Checksum", "ok"}};
    RUVIA_CHECK(
        conn.finishResponse(1, validatedTrailers(trailers)) == Http2FinishSubmitStatus::kQueued);

    char rst[13];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
    (void)conn.feed(std::string_view(rst, sizeof(rst)));
    while (conn.nextEvent().has_value()) {
    }
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK(!conn.hasQueuedData(1));
    RUVIA_CHECK(conn.takeDrainedDataStreams().empty());
    RUVIA_CHECK(conn.pendingOutput().empty());

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 1, 100);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.takeDrainedDataStreams().empty());
}

// A HEAD response's Content-Length is representation metadata, not a DATA
// contract. The same exemption must apply when trailing HEADERS, rather than the
// initial response HEADERS, carries END_STREAM.

RUVIA_TEST(http2_connection_client_head_representation_length_survives_trailer_terminal) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead(
        "HEAD", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    HpackEncoder::encodeHeader(response, "content-length", "10");
    const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!client.nextEvent().has_value());
    const auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    const auto* known = stream->remoteContent().metadataOnlyKnownLength();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declaredLength(), std::size_t{10});
    RUVIA_CHECK(stream->remoteContent().metadataOnlyKnownLength() != nullptr);
    RUVIA_CHECK_EQ(stream->remoteHeaderCount(), std::size_t{1});
    RUVIA_CHECK(stream->remoteInitialHeaderCount() == std::optional<std::size_t>{1});

    std::pmr::string trailers(&resource);
    HpackEncoder::encodeHeader(trailers, "server-timing", "db;dur=4");
    const auto trailerHead = headersFrame(&resource, streamId,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(trailers.data(), trailers.size()));
    RUVIA_CHECK(client.feed(std::string_view(trailerHead.data(), trailerHead.size())) ==
                Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(!client.connectionError().has_value());
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(streamId)->remoteReceive().endStream() != nullptr);
    RUVIA_CHECK_EQ(client.stream(streamId)->remoteHeaderCount(), std::size_t{2});
    RUVIA_CHECK(
        client.stream(streamId)->remoteInitialHeaderCount() == std::optional<std::size_t>{1});
    const auto storedTrailer = client.stream(streamId)->remoteHeaderAt(1);
    RUVIA_CHECK_EQ(storedTrailer.name, std::string_view("server-timing"));
    RUVIA_CHECK_EQ(storedTrailer.value, std::string_view("db;dur=4"));

    client.unpinStream(streamId);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
}

RUVIA_TEST(http2_connection_client_rejects_204_content_length) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "204");
    HpackEncoder::encodeHeader(response, "content-length", "0");
    const auto responseHead = headersFrame(&resource, streamId,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                Http2FeedResult::kAccepted);

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
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(resetBytes.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_connection_client_accepts_combined_equal_response_content_length) {
    {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead(
            "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", "200");
        HpackEncoder::encodeHeader(response, "content-length", "5, 5");
        const auto responseHead =
            headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                    Http2FeedResult::kAccepted);
        RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
        RUVIA_CHECK(!client.nextEvent().has_value());
        auto* stream = client.stream(streamId);
        RUVIA_CHECK(stream != nullptr);
        if (stream != nullptr) {
            const auto* known = stream->remoteContent().allowedKnownLength();
            RUVIA_CHECK(known != nullptr);
            if (known != nullptr) {
                RUVIA_CHECK_EQ(known->declaredLength(), std::size_t{5});
            }
        }
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(!client.connectionError().has_value());
    }

    {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead(
            "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", "200");
        HpackEncoder::encodeHeader(response, "content-length", "5, 6");
        const auto responseHead =
            headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                    Http2FeedResult::kAccepted);

        bool sawClosed = false;
        while (const auto event = client.nextEvent()) {
            RUVIA_CHECK(event->messageHead() == nullptr);
            if (const auto* closed = event->streamClosed()) {
                sawClosed = true;
                RUVIA_CHECK(closed->source() == Http2StreamCloseSource::kLocal);
                RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
            }
        }
        RUVIA_CHECK(sawClosed);
        RUVIA_CHECK(client.stream(streamId) == nullptr);
        RUVIA_CHECK(!client.pendingOutput().empty());
        RUVIA_CHECK(!client.connectionError().has_value());
    }
}

RUVIA_TEST(http2_connection_applies_response_specific_trailer_rules) {
    struct Case final {
        std::string_view name;
        std::string_view value;
        bool accepted;
    };
    constexpr Case cases[] = {
        // RFC 9110 Section 14.3 explicitly permits Accept-Ranges in a trailer.
        {"accept-ranges", "bytes", true},
        // Date is response control data that has to be known before content.
        {"date", "Sun, 06 Nov 1994 08:49:37 GMT", false},
    };

    for (const auto& test : cases) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead(
            "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", "200");
        const auto responseHead =
            headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
                std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                    Http2FeedResult::kAccepted);
        RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
        RUVIA_CHECK(!client.nextEvent().has_value());

        std::pmr::string trailers(&resource);
        HpackEncoder::encodeHeader(trailers, test.name, test.value);
        const auto trailerHead = headersFrame(&resource, streamId,
            static_cast<std::uint8_t>(
                ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream),
            std::string_view(trailers.data(), trailers.size()));
        RUVIA_CHECK(client.feed(std::string_view(trailerHead.data(), trailerHead.size())) ==
                    Http2FeedResult::kAccepted);

        bool sawMessageEnd = false;
        bool sawProtocolClose = false;
        while (const auto event = client.nextEvent()) {
            sawMessageEnd = sawMessageEnd || event->messageEnd() != nullptr;
            if (const auto* closed = event->streamClosed()) {
                sawProtocolClose = closed->source() == Http2StreamCloseSource::kLocal &&
                                   closed->error() == Http2ErrorCode::kProtocolError;
            }
        }
        RUVIA_CHECK_EQ(sawMessageEnd, test.accepted);
        RUVIA_CHECK_EQ(sawProtocolClose, !test.accepted);
        RUVIA_CHECK_EQ(client.pendingOutput().empty(), test.accepted);
        RUVIA_CHECK(!client.connectionError().has_value());
    }
}

// Server-role trailers: a trailing HEADERS block WITHOUT END_STREAM is a protocol
// error on that stream (RFC 9113 §8.1) -- the core RSTs and closes it, no kMessageEnd.

RUVIA_TEST(http2_connection_server_trailers_without_end_stream_rejected) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // Open stream 1 with a body (POST, no END_STREAM on HEADERS).
    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "POST");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    const auto h = headersFrame(&resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().has_value()) {
    }
    conn.consumeOutput(conn.pendingOutput().size());

    // A trailer HEADERS block with END_HEADERS but NO END_STREAM -> stream error.
    std::pmr::string trailer(&resource);
    HpackEncoder::encodeHeader(trailer, "x-checksum", "abc");
    const auto t = headersFrame(&resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders,  // deliberately no END_STREAM
        std::string_view(trailer.data(), trailer.size()));
    (void)conn.feed(std::string_view(t.data(), t.size()));

    bool sawClosed = false;
    bool sawEnd = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* closed = event->streamClosed()) {
            sawClosed = true;
            RUVIA_CHECK(closed->source() == ruvia::detail::Http2StreamCloseSource::kLocal);
            RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
        }
        if (event->messageEnd() != nullptr) {
            sawEnd = true;
        }
    }
    RUVIA_CHECK(sawClosed);
    RUVIA_CHECK(!sawEnd);                              // never completes the request
    RUVIA_CHECK(!conn.connectionError().has_value());  // stream error, not connection error
    const auto rst = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(rst.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK(conn.stream(1) == nullptr);  // removed, not leaked
}

// Semantic response trailers queued behind a window-blocked body: the HEADERS must be
// emitted AFTER the deferred DATA drains (RFC 9113 §8.1), carrying END_STREAM in place
// of it -- never ahead of the body bytes.

RUVIA_TEST(http2_connection_trailers_wait_for_blocked_body) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 4);  // tiny 4-byte stream send window
    driveGetRequest(conn, &resource);
    conn.consumeOutput(conn.pendingOutput().size());

    // Streaming response head declares an exact 8-byte content length. Only 4 DATA
    // bytes fit the window, so the other 4 are core-owned and deferred -> kQueued.
    ruvia::HttpResponse head({.resource = &resource});
    head.status(ruvia::http_status::kOk);
    head.header("Content-Length", "8");
    RUVIA_CHECK(responseHeadSubmitted(conn.submitStreamingResponseHead(1, std::move(head),
        ruvia::detail::ResponseStreamKind::kGeneric, ResponseTrailerIntent::kNone)));
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "AAAABBBB", Http2EndStream::kKeepOpen) ==
                Http2DataSubmitStatus::kQueued);
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{8});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{4});

    // First 4 bytes went out as DATA (no END_STREAM).
    auto out = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(4));
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(out.size());

    // Queue trailers while the remaining 4 bytes are still window-blocked.
    const std::array<ruvia::HttpHeaderView, 1> invalidTrailers{
        ruvia::HttpHeaderView{"Content-Length", "8"}};
    const auto invalidResult = ruvia::detail::httpResponseTrailerSection(invalidTrailers);
    RUVIA_CHECK(invalidResult.section() == nullptr);
    RUVIA_CHECK(invalidResult.failure() != nullptr);
    RUVIA_CHECK(conn.hasQueuedData(1));
    RUVIA_CHECK(stream->localSend().responseContentOpen() != nullptr);
    RUVIA_CHECK(conn.pendingOutput().empty());
    const std::array<ruvia::HttpHeaderView, 1> trailers{ruvia::HttpHeaderView{"X-Checksum", "ok"}};
    RUVIA_CHECK(
        conn.finishResponse(1, validatedTrailers(trailers)) == Http2FinishSubmitStatus::kQueued);
    RUVIA_CHECK(conn.pendingOutput().empty());  // nothing emitted yet (still blocked)

    // Peer WINDOW_UPDATE reopens the window: the deferred DATA drains, THEN the trailer
    // HEADERS(END_STREAM) follows -- in that order.
    char wu[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(wu, 4, Http2FrameType::kWindowUpdate, 0, 1);
    ruvia::detail::http2Write32(wu + 9, 100);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    while (conn.nextEvent().has_value()) {
    }

    out = conn.pendingOutput();
    const auto d2 = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d2.length, static_cast<std::uint32_t>(4));           // the remaining body
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // NOT on the DATA
    out = out.substr(9 + d2.length);
    const auto th = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(th.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));  // trailer
    RUVIA_CHECK((th.flags & ruvia::detail::kHttp2FlagEndStream) != 0);             // END_STREAM here
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{8});
    const auto trailerPayload = out.substr(9, th.length);
    RUVIA_CHECK(trailerPayload.find("x-checksum") != std::string_view::npos);
    RUVIA_CHECK(trailerPayload.find("X-Checksum") == std::string_view::npos);
}

// A HEADERS without END_HEADERS followed by an endless stream of EMPTY CONTINUATION
// frames keeps the field block "in progress" forever: empty frames add no bytes,
// so the accumulated-block size cap never trips. The CONTINUATION frame-count
// budget cuts the peer off with GOAWAY(ENHANCE_YOUR_CALM) (RFC 9113 §6.10,
// CVE-2024-27316).

RUVIA_TEST(http2_connection_continuation_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeGetRequest(block);
    // Open the block WITHOUT END_HEADERS so CONTINUATION frames are expected.
    const auto head = headersFrame(&resource, 1, 0, std::string_view(block.data(), block.size()));
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) !=
                ruvia::detail::Http2FeedResult::kProtocolFailure);

    const auto empty = continuationFrame(&resource, 1, 0, {});
    bool tripped = false;
    for (std::uint32_t i = 0; i < ruvia::detail::kHttp2MaxContinuationFrames + 2 && !tripped; ++i) {
        tripped = conn.feed(std::string_view(empty.data(), empty.size())) ==
                  ruvia::detail::Http2FeedResult::kProtocolFailure;
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connectionError().has_value());
    RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()), kEnhanceYourCalm);
}

// A well-formed head split across a few CONTINUATION frames completes normally: the
// budget is generous enough that legitimate fragmentation never trips it.

RUVIA_TEST(http2_connection_fragmented_headers_within_budget_complete) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeGetRequest(block);
    const std::string_view whole(block.data(), block.size());
    const auto q = whole.size() / 4;
    const auto head = headersFrame(&resource, 1, 0, whole.substr(0, q));
    RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) !=
                ruvia::detail::Http2FeedResult::kProtocolFailure);
    const auto c1 = continuationFrame(&resource, 1, 0, whole.substr(q, q));
    const auto c2 = continuationFrame(&resource, 1, 0, whole.substr(2 * q, q));
    const auto c3 = continuationFrame(&resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        whole.substr(3 * q));
    RUVIA_CHECK(conn.feed(std::string_view(c1.data(), c1.size())) !=
                ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.feed(std::string_view(c2.data(), c2.size())) !=
                ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.feed(std::string_view(c3.data(), c3.size())) !=
                ruvia::detail::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(!conn.connectionError().has_value());

    bool sawHead = false;
    while (const auto event = conn.nextEvent()) {
        if (event->messageHead() != nullptr) {
            sawHead = true;
        }
    }
    RUVIA_CHECK(sawHead);
}
