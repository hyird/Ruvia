#include "http2_connection_fixture.h"

#include <new>

// Http2Connection: the stream table: admission, PRIORITY, RST_STREAM and close.

namespace {

#if !defined(_MSC_VER)
class ToggleRejectingMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};
#endif  // !_MSC_VER

}  // namespace

RUVIA_TEST(http2_connection_peer_stream_limit_waits_for_both_half_closes) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    applyPeerMaxConcurrentStreams(client, 1);

    const auto first = client.submitRegularRequestHead(
        "POST", "https", "example.test", "/upload", {}, Http2RequestContent::streaming());
    RUVIA_CHECK(first.submitted() != nullptr);
    const auto firstStreamId = submittedRequestStreamId(first);
    RUVIA_CHECK_EQ(firstStreamId, std::uint32_t{1});
    client.consumeOutput(client.pendingOutput().size());

    const auto whileOpen = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {}, Http2RequestContent::none());
    RUVIA_CHECK(whileOpen.submitted() == nullptr);
    RUVIA_CHECK(
        requestHeadSubmitError(whileOpen) == Http2RequestHeadSubmitError::kPeerStreamLimitReached);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(3) == nullptr);

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "200");
    const auto responseHead = headersFrame(&resource, firstStreamId,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    while (client.nextEvent().has_value()) {
    }

    const auto peerHalfOnly = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {}, Http2RequestContent::none());
    RUVIA_CHECK(peerHalfOnly.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(peerHalfOnly) ==
                Http2RequestHeadSubmitError::kPeerStreamLimitReached);
    RUVIA_CHECK(client.submitData(firstStreamId, {}, Http2EndStream::kEndStream) ==
                Http2DataSubmitStatus::kAccepted);
    client.consumeOutput(client.pendingOutput().size());

    const auto afterBothHalves = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {}, Http2RequestContent::none());
    RUVIA_CHECK(afterBothHalves.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(afterBothHalves), std::uint32_t{3});
}

RUVIA_TEST(http2_connection_peer_reset_releases_peer_stream_limit_slot) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    applyPeerMaxConcurrentStreams(client, 1);

    const auto first = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/first", {}, Http2RequestContent::none());
    RUVIA_CHECK(first.submitted() != nullptr);
    const auto firstStreamId = submittedRequestStreamId(first);
    client.consumeOutput(client.pendingOutput().size());
    RUVIA_CHECK(requestHeadSubmitError(client.submitRegularRequestHead(
                    "GET", "https", "example.test", "/second", {}, Http2RequestContent::none())) ==
                Http2RequestHeadSubmitError::kPeerStreamLimitReached);

    char reset[13];
    ruvia::detail::http2EncodeFrameHeader(reset, 4, Http2FrameType::kRstStream, 0, firstStreamId);
    ruvia::detail::http2Write32(reset + 9, static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
    RUVIA_CHECK(client.feed(std::string_view(reset, sizeof(reset))) ==
                ruvia::detail::Http2FeedResult::kAccepted);

    const auto afterReset = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/second", {}, Http2RequestContent::none());
    RUVIA_CHECK(afterReset.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(afterReset), std::uint32_t{3});
}

// RFC 9113 deprecated the RFC 7540 priority tree. Dependency and weight are ignored
// after validating frame shape, including the old self-dependency case.

RUVIA_TEST(http2_connection_feed_priority_payload_is_ignored) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // A self-dependent advisory payload on a live stream has no stream-state effect.
    driveGetRequest(conn, &resource);  // stream 1 open
    conn.consumeOutput(conn.pendingOutput().size());
    char live[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(live, 5, Http2FrameType::kPriority, 0, 1);
    ruvia::detail::http2Write32(live + 9, 1);  // depends on stream 1 (itself)
    live[13] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(live, sizeof(live))) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(conn.stream(1) != nullptr && !conn.stream(1)->isAborted());

    // The same is true on an idle stream; PRIORITY never opens it.
    char idle[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(idle, 5, Http2FrameType::kPriority, 0, 7);
    ruvia::detail::http2Write32(idle + 9, 7);  // idle stream depends on itself
    idle[13] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(idle, sizeof(idle))) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.pendingOutput().empty());  // ignored: no RST, no GOAWAY
}

RUVIA_TEST(http2_connection_malformed_priority_is_stream_frame_size_error) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);  // stream 1 open

    char malformed[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(malformed, 4, Http2FrameType::kPriority, 0, 1);
    ruvia::detail::http2Write32(malformed + 9, 0);
    RUVIA_CHECK(
        conn.feed(std::string_view(malformed, sizeof(malformed))) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());

    const auto out = conn.pendingOutput();
    const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(reset.streamId, std::uint32_t{1});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kFrameSizeError));
    RUVIA_CHECK(conn.stream(1) == nullptr);

    const auto event = conn.nextEvent();
    RUVIA_CHECK(event.has_value());
    if (event.has_value()) {
        const auto* closed = event->streamClosed();
        RUVIA_CHECK(closed != nullptr);
        if (closed != nullptr) {
            RUVIA_CHECK_EQ(closed->streamId(), std::uint32_t{1});
            RUVIA_CHECK(closed->source() == Http2StreamCloseSource::kLocal);
            RUVIA_CHECK(closed->error() == Http2ErrorCode::kFrameSizeError);
        }
    }
}

RUVIA_TEST(http2_connection_malformed_idle_priority_is_connection_error) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char malformed[9 + 1];
    ruvia::detail::http2EncodeFrameHeader(malformed, 1, Http2FrameType::kPriority, 0, 7);
    malformed[9] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(malformed, sizeof(malformed))) ==
                Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kFrameSizeError);
    const auto out = conn.pendingOutput();
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(goaway.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kFrameSizeError));
}

RUVIA_TEST(http2_connection_priority_stream_zero_is_connection_protocol_error) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char malformed[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(malformed, 4, Http2FrameType::kPriority, 0, 0);
    ruvia::detail::http2Write32(malformed + 9, 0);
    RUVIA_CHECK(conn.feed(std::string_view(malformed, sizeof(malformed))) ==
                Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kProtocolError);
    const auto out = conn.pendingOutput();
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_connection_rejects_non_increasing_new_peer_stream_id) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string firstBlock(&resource);
    encodeGetRequest(firstBlock);
    const auto first = headersFrame(&resource, 5,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(firstBlock.data(), firstBlock.size()));
    RUVIA_CHECK(
        conn.feed(std::string_view(first.data(), first.size())) == Http2FeedResult::kAccepted);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    std::pmr::string lowerBlock(&resource);
    encodeGetRequest(lowerBlock);
    const auto lower = headersFrame(&resource, 3,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(lowerBlock.data(), lowerBlock.size()));
    RUVIA_CHECK(conn.feed(std::string_view(lower.data(), lower.size())) ==
                Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kProtocolError);
}

// A client can cancel while a multi-frame response head is incomplete. The partial
// compressed block must move out of the stream before removal, then CONTINUATION
// completes silently and updates HPACK for later responses.

RUVIA_TEST(http2_connection_client_reset_detaches_partial_response_head) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    client.beginConnection();
    client.consumeOutput(client.pendingOutput().size());
    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
    RUVIA_CHECK(client.feed(std::string_view(settings, sizeof(settings))) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    client.consumeOutput(client.pendingOutput().size());

    const auto firstHead = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(firstHead.submitted() != nullptr);
    const auto firstStream = submittedRequestStreamId(firstHead);
    RUVIA_CHECK_EQ(firstStream, static_cast<std::uint32_t>(1));
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string responseBlock(&resource);
    HpackEncoder::encodeHeader(responseBlock, ":status", "200");
    const auto statusBytes = responseBlock.size();
    encodeShortDynamicHeader(responseBlock, "x-response", "indexed");
    const auto first = headersFrame(
        &resource, firstStream, 0, std::string_view(responseBlock.data(), statusBytes));
    RUVIA_CHECK(client.feed(std::string_view(first.data(), first.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.headerBlockInProgress());

    RUVIA_CHECK(
        client.submitReset(firstStream, Http2ErrorCode::kCancel) == Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(client.stream(firstStream) == nullptr);
    client.consumeOutput(client.pendingOutput().size());

    const auto continuation = continuationFrame(&resource, firstStream,
        ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(responseBlock.data() + statusBytes, responseBlock.size() - statusBytes));
    RUVIA_CHECK(client.feed(std::string_view(continuation.data(), continuation.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!client.connectionError().has_value());
    RUVIA_CHECK(client.pendingOutput().empty());  // no second RST
    RUVIA_CHECK(!client.nextEvent().has_value());

    const auto nextHead = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/next", {}, Http2RequestContent::none());
    RUVIA_CHECK(nextHead.submitted() != nullptr);
    const auto nextStream = submittedRequestStreamId(nextHead);
    RUVIA_CHECK_EQ(nextStream, static_cast<std::uint32_t>(3));
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string nextResponse(&resource);
    HpackEncoder::encodeHeader(nextResponse, ":status", "200");
    HpackEncoder::encodeIndexed(nextResponse, 62);  // x-response: indexed
    const auto final = headersFrame(&resource, nextStream,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(nextResponse.data(), nextResponse.size()));
    RUVIA_CHECK(client.feed(std::string_view(final.data(), final.size())) ==
                ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!client.connectionError().has_value());
}

// submitReset emits a RST_STREAM and marks the stream reset so no further response
// bytes are produced for it.

RUVIA_TEST(http2_connection_submit_reset_emits_rst) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) == Http2SubmitStatus::kAccepted);
    const auto out = conn.pendingOutput();
    const auto r = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(r.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(r.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_local_reset_unknown_and_repeat_emit_no_illegal_frame) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    RUVIA_CHECK(conn.submitReset(99, Http2ErrorCode::kCancel) == Http2SubmitStatus::kInvalidState);
    RUVIA_CHECK(conn.pendingOutput().empty());

    driveGetRequest(conn, &resource);
    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) == Http2SubmitStatus::kAccepted);
    const auto firstResetBytes = conn.pendingOutput().size();
    RUVIA_CHECK_EQ(firstResetBytes, static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));

    RUVIA_CHECK(conn.submitReset(1, Http2ErrorCode::kCancel) == Http2SubmitStatus::kClosed);
    RUVIA_CHECK_EQ(conn.pendingOutput().size(), firstResetBytes);

    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    RUVIA_CHECK(client.submitReset(1, Http2ErrorCode::kCancel) == Http2SubmitStatus::kInvalidState);
    RUVIA_CHECK(client.pendingOutput().empty());
}

// A pinned stream (handler in flight) is NOT freed by a peer RST_STREAM: it stays in
// the table (so the handler's request views survive) but is marked reset, and
// kStreamClosed is emitted so the owner can drop the response. unpin then frees it.

RUVIA_TEST(http2_connection_pinned_stream_survives_peer_reset) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);  // stream 1 created + decoded
    RUVIA_CHECK(conn.stream(1) != nullptr);

    conn.pinStream(1);

    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, 8 /* CANCEL */);
    (void)conn.feed(std::string_view(rst, sizeof(rst)));

    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr);    // kept alive because pinned
    RUVIA_CHECK(s->isAborted());  // retained storage, but protocol ownership ended
    bool sawClosed = false;
    while (const auto event = conn.nextEvent()) {
        if (const auto* closed = event->streamClosed();
            closed != nullptr && closed->streamId() == 1) {
            sawClosed = true;
            RUVIA_CHECK(closed->source() == ruvia::detail::Http2StreamCloseSource::kPeer);
            RUVIA_CHECK(closed->error() == Http2ErrorCode::kCancel);
        }
    }
    RUVIA_CHECK(sawClosed);

    conn.unpinStream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);  // freed once the handler finished
}

// Unpinning a stream that completed normally on both halves frees it without a reset.

RUVIA_TEST(http2_connection_unpin_frees_completed_stream) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);
    RUVIA_CHECK(conn.stream(1) != nullptr);
    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kNoContent);
    const auto headResult = submitBufferedResponseHead(conn, 1, response);
    RUVIA_CHECK(responseHeadSubmitted(headResult));
    conn.consumeOutput(conn.pendingOutput().size());
    conn.unpinStream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// Dropping the last owner while the local response is still open must produce an
// explicit terminal transition, rather than silently erasing a peer-visible stream.

RUVIA_TEST(http2_connection_unpin_incomplete_stream_emits_cancel_reset) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);

    conn.unpinStream(1);

    RUVIA_CHECK(conn.stream(1) == nullptr);
    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(13));
    const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(reset.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
}

#if !defined(_MSC_VER)
RUVIA_TEST(http2_connection_unpin_keeps_pin_when_owner_reset_allocation_fails) {
    ToggleRejectingMemoryResource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);

    const std::string largeHint(4096, 'x');
    const std::array<ruvia::HttpHeaderView, 1> headers{ruvia::HttpHeaderView{"Link", largeHint}};
    const ruvia::HttpInterimResponseHead earlyHints(ruvia::http_status::kEarlyHints, headers);
    RUVIA_CHECK(conn.submitInterimResponseHead(1, earlyHints) == Http2SubmitStatus::kAccepted);

    resource.rejectAllocations();
    bool allocationFailed = false;
    try {
        conn.unpinStream(1);
    } catch (const std::bad_alloc&) {
        allocationFailed = true;
    }
    RUVIA_CHECK(allocationFailed);
    RUVIA_CHECK(conn.stream(1) != nullptr);
    RUVIA_CHECK(!conn.stream(1)->isAborted());

    resource.rejectAllocations(false);
    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
    RUVIA_CHECK(
        conn.feed(std::string_view(rst, sizeof(rst))) == ruvia::detail::Http2FeedResult::kAccepted);

    auto* retained = conn.stream(1);
    RUVIA_CHECK(retained != nullptr);
    if (retained != nullptr) {
        RUVIA_CHECK(retained->isAborted());
    }
    conn.unpinStream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);
}
#endif  // !_MSC_VER

// RFC 9110 Section 15.3.6 requires every 205 response to have zero-length
// content. Unlike HEAD/204/304, 205 still has an ordinary content phase, but a
// peer cannot use that phase to transfer any non-empty DATA. HTTP/2 can reject
// this without losing connection synchronization because the failure is scoped
// to the response stream.

RUVIA_TEST(http2_connection_client_rejects_reset_content_payload) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "205");
    const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!client.nextEvent().has_value());

    const auto forbidden = dataFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndStream, "x");
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
    RUVIA_CHECK_EQ(reset.size(), static_cast<std::size_t>(13));
    const auto resetHead = ruvia::detail::http2ParseFrameHeader(reset.substr(0, 9));
    RUVIA_CHECK_EQ(resetHead.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(reset.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    client.unpinStream(streamId);
}

RUVIA_TEST(http2_connection_client_rejects_nonzero_reset_content_length) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "205");
    HpackEncoder::encodeHeader(response, "content-length", "1");
    const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                Http2FeedResult::kAccepted);

    bool sawClosed = false;
    while (const auto event = client.nextEvent()) {
        RUVIA_CHECK(event->messageHead() == nullptr);
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
    RUVIA_CHECK_EQ(reset.size(), static_cast<std::size_t>(13));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2Read32(reinterpret_cast<const unsigned char*>(reset.data() + 9)),
        static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    client.unpinStream(streamId);
}

RUVIA_TEST(http2_connection_client_accepts_empty_reset_content_terminal) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "205");
    HpackEncoder::encodeHeader(response, "content-length", "0");
    const auto responseHead = headersFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(std::string_view(responseHead.data(), responseHead.size())) ==
                Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(!client.nextEvent().has_value());
    const auto* known = client.stream(streamId)->remoteContent().allowedKnownLength();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declaredLength(), std::size_t{0});

    const auto terminal = dataFrame(&resource, streamId, ruvia::detail::kHttp2FlagEndStream, {});
    RUVIA_CHECK(client.feed(std::string_view(terminal.data(), terminal.size())) ==
                Http2FeedResult::kAccepted);
    const auto end = client.nextEvent();
    RUVIA_CHECK(end.has_value());
    RUVIA_CHECK(end->kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!client.nextEvent().has_value());
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(!client.connectionError().has_value());
    client.unpinStream(streamId);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
}

// Client role protocol errors: HEADERS on an odd stream never opened is a connection
// error, and HEADERS on an even (server-initiated) stream is one too (push disabled).

RUVIA_TEST(http2_connection_client_role_rejects_unexpected_streams) {
    std::pmr::monotonic_buffer_resource resource;
    {
        Http2Connection client(&resource, Http2Role::kClient);
        client.beginConnection();
        client.consumeOutput(client.pendingOutput().size());
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        (void)client.feed(std::string_view(settings, sizeof(settings)));
        std::pmr::string head(&resource);
        HpackEncoder::encodeHeader(head, ":status", "200");
        const auto idle = headersFrame(&resource, 5, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(head.data(), head.size()));
        (void)client.feed(std::string_view(idle.data(), idle.size()));
        RUVIA_CHECK(client.connectionError().has_value());  // HEADERS on idle stream -> GOAWAY
    }
    {
        Http2Connection client(&resource, Http2Role::kClient);
        client.beginConnection();
        client.consumeOutput(client.pendingOutput().size());
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        (void)client.feed(std::string_view(settings, sizeof(settings)));
        std::pmr::string head(&resource);
        HpackEncoder::encodeHeader(head, ":status", "200");
        const auto even = headersFrame(&resource, 2, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(head.data(), head.size()));
        (void)client.feed(std::string_view(even.data(), even.size()));
        RUVIA_CHECK(client.connectionError().has_value());  // no push: even ids are never valid
    }
}

RUVIA_TEST(http2_connection_repeated_peer_reset_is_connection_error) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);
        openThenPeerReset(conn, &resource, pinned);

        // The second reset is ordered after the reset that this peer already
        // sent. Unlike a reset racing with one sent by us, it cannot predate
        // the peer's knowledge that the stream is closed.
        char rst[9 + 4];
        ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
        ruvia::detail::http2Write32(rst + 9, static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
        RUVIA_CHECK(conn.feed(std::string_view(rst, sizeof(rst))) ==
                    ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kStreamClosed);
        RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()),
            static_cast<std::uint32_t>(Http2ErrorCode::kStreamClosed));

        if (pinned) {
            conn.unpinStream(1);
        }
    }
}

RUVIA_TEST(http2_connection_racing_reset_does_not_spend_rapid_reset_budget) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    openThenLocalReset(conn, &resource);

    // A reset that raced with the reset sent by this endpoint closes no live
    // stream and spawned no new handler. It is required minimal processing,
    // not one unit of the rapid-reset lifecycle budget.
    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, static_cast<std::uint32_t>(Http2ErrorCode::kCancel));
    RUVIA_CHECK(
        conn.feed(std::string_view(rst, sizeof(rst))) == ruvia::detail::Http2FeedResult::kAccepted);

    std::pmr::string block(&resource);
    encodeGetRequest(block);
    for (std::uint32_t sid = 3; sid < 3U + 2U * 1000U; sid += 2) {
        const auto head = headersFrame(&resource, sid,
            ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
            std::string_view(block.data(), block.size()));
        RUVIA_CHECK(conn.feed(std::string_view(head.data(), head.size())) ==
                    ruvia::detail::Http2FeedResult::kAccepted);
        while (conn.nextEvent().has_value()) {
        }
        conn.consumeOutput(conn.pendingOutput().size());

        ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, sid);
        RUVIA_CHECK(conn.feed(std::string_view(rst, sizeof(rst))) ==
                    ruvia::detail::Http2FeedResult::kAccepted);
        while (conn.nextEvent().has_value()) {
        }
        conn.consumeOutput(conn.pendingOutput().size());
    }
    RUVIA_CHECK(!conn.connectionError().has_value());
}

RUVIA_TEST(http2_connection_malformed_priority_after_peer_reset_is_connection_error) {
    for (const bool pinned : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection conn(&resource);
        handshake(conn);
        openThenPeerReset(conn, &resource, pinned);

        // PRIORITY itself is legal in every stream state, but its payload must
        // contain exactly five bytes. The resulting FRAME_SIZE_ERROR cannot be
        // reported with RST_STREAM after this peer-originated reset.
        char priority[9 + 4]{};
        ruvia::detail::http2EncodeFrameHeader(priority, 4, Http2FrameType::kPriority, 0, 1);
        RUVIA_CHECK(conn.feed(std::string_view(priority, sizeof(priority))) ==
                    ruvia::detail::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kFrameSizeError);
        RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()),
            static_cast<std::uint32_t>(Http2ErrorCode::kFrameSizeError));

        if (pinned) {
            conn.unpinStream(1);
        }
    }
}

RUVIA_TEST(http2_connection_malformed_priority_without_active_stream_is_connection_error) {
    for (const bool closed : {false, true}) {
        for (const bool pinned : {false, true}) {
            if (!closed && pinned) {
                continue;
            }
            std::pmr::monotonic_buffer_resource resource;
            Http2Connection conn(&resource);
            handshake(conn);
            if (closed) {
                openThenLocalReset(conn, &resource, pinned);
            }

            // A malformed PRIORITY requires a stream FRAME_SIZE_ERROR, but
            // emitting RST_STREAM is itself forbidden on idle and closed streams.
            // Promoting the error to the connection is the only legal report.
            char priority[9 + 4]{};
            ruvia::detail::http2EncodeFrameHeader(priority, 4, Http2FrameType::kPriority, 0, 1);
            RUVIA_CHECK(conn.feed(std::string_view(priority, sizeof(priority))) ==
                        ruvia::detail::Http2FeedResult::kProtocolFailure);
            RUVIA_CHECK(conn.connectionError() == Http2ErrorCode::kFrameSizeError);
            RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()),
                static_cast<std::uint32_t>(Http2ErrorCode::kFrameSizeError));

            if (pinned) {
                conn.unpinStream(1);
            }
        }
    }
}

// A rapid-reset flood (open a stream, RST it, repeat -- never letting a response finish)
// is cut off with GOAWAY(ENHANCE_YOUR_CALM); the 128-stream cap alone never trips because
// each RST immediately frees the slot (CVE-2023-44487).

RUVIA_TEST(http2_connection_rapid_reset_flood_trips_enhance_your_calm) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeGetRequest(block);

    bool tripped = false;
    for (std::uint32_t sid = 1; sid < 1U + 2U * 1200U; sid += 2) {
        const auto h = headersFrame(&resource, sid,
            ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
            std::string_view(block.data(), block.size()));
        if (conn.feed(std::string_view(h.data(), h.size())) ==
            ruvia::detail::Http2FeedResult::kProtocolFailure) {
            tripped = true;
            break;
        }
        while (conn.nextEvent().has_value()) {
        }
        conn.consumeOutput(conn.pendingOutput().size());

        char rst[9 + 4];
        ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, sid);
        ruvia::detail::http2Write32(rst + 9, 0);
        if (conn.feed(std::string_view(rst, sizeof(rst))) ==
            ruvia::detail::Http2FeedResult::kProtocolFailure) {
            tripped = true;
            break;  // leave the GOAWAY in the outbound buffer for inspection
        }
        while (conn.nextEvent().has_value()) {
        }
        conn.consumeOutput(conn.pendingOutput().size());
    }
    RUVIA_CHECK(tripped);
    RUVIA_CHECK(conn.connectionError().has_value());
    RUVIA_CHECK_EQ(firstGoawayError(conn.pendingOutput()), kEnhanceYourCalm);
}
