#include "streaming_fixture.h"

// Writing a streamed response: exclusive output, writeln, the terminal trailer section and the
// post-head phases.

namespace {

ruvia::Task<void> writeOwnedChunk(ruvia::ResponseStreamWriter& writer) {
    std::pmr::string chunk("owned-chunk", ruvia::detail::processResource());
    co_await writer.writeOwned(std::move(chunk));
}

ruvia::Task<void> writeOwnedTextFrame(ruvia::WebSocket& socket) {
    std::pmr::string payload("owned-frame", ruvia::detail::processResource());
    co_await socket.textOwned(std::move(payload));
}

}  // namespace

RUVIA_TEST(body_reader_rejects_concurrent_consumers_of_one_borrowed_buffer) {
    asio::io_context io(1);
    ruvia::detail::BodyReaderBinding<SuspendedBodySource> binding;
    bool firstCompleted = false;
    bool secondRejected = false;

    auto first = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(completeBodyRead(binding.facade(), firstCompleted)), asio::use_future);
    while (!binding.reader().readSuspended) {
        RUVIA_CHECK_EQ(io.run_one(), std::size_t{1});
    }
    RUVIA_CHECK(!firstCompleted);

    io.restart();
    auto second = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(rejectConcurrentBodyRead(binding.facade(), secondRejected)), asio::use_future);
    asio::post(io, [&binding] { binding.reader().resume(); });
    io.run();
    first.get();
    second.get();

    RUVIA_CHECK(firstCompleted);
    RUVIA_CHECK(secondRejected);
}

RUVIA_TEST(response_stream_rejects_overlapping_output_operations) {
    asio::io_context io(1);
    SuspendedStreamSink sink;
    auto writer = makeSuspendedWriter(sink);
    bool firstCompleted = false;
    bool writeRejected = false;
    bool endRejected = false;
    bool failureObserved = false;

    auto first = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(completeStreamWrite(writer, "first", firstCompleted)), asio::use_future);
    while (!sink.writeSuspended) {
        RUVIA_CHECK_EQ(io.run_one(), std::size_t{1});
    }
    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{1});
    RUVIA_CHECK(!firstCompleted);

    io.restart();
    auto overlappingWrite = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(rejectConcurrentStreamWrite(writer, writeRejected)), asio::use_future);
    auto overlappingEnd = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(rejectConcurrentStreamEnd(writer, endRejected)), asio::use_future);
    io.poll();
    overlappingWrite.get();
    overlappingEnd.get();

    asio::post(io, [&sink] { sink.resume(); });
    io.restart();
    io.run();
    first.get();

    RUVIA_CHECK(firstCompleted);
    RUVIA_CHECK(writeRejected);
    RUVIA_CHECK(endRejected);
    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{1});
    RUVIA_CHECK_EQ(sink.ends, std::size_t{0});

    sink.failNextWrite = true;
    io.restart();
    auto failing = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(observeStreamWriteFailure(writer, failureObserved)), asio::use_future);
    io.run();
    failing.get();
    RUVIA_CHECK(failureObserved);

    io.restart();
    auto following = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(completeStreamWrite(writer, "following", firstCompleted)), asio::use_future);
    io.run();
    following.get();
    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{3});
    if (sink.writes.size() >= 3) {
        RUVIA_CHECK_EQ(sink.writes[2], std::string("following"));
    }
}

RUVIA_TEST(response_stream_writeln_emits_independent_lines) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(writeLines(writer)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.writes[0], std::string("first\n"));
    RUVIA_CHECK_EQ(sink.writes[1], std::string("second\n"));
}

RUVIA_TEST(response_stream_stored_writeln_operations_own_independent_payloads) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(writeStoredLines(writer)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.writes[0], std::string("stored-first\n"));
    RUVIA_CHECK_EQ(sink.writes[1], std::string("stored-second\n"));
}

RUVIA_TEST(websocket_stored_operation_owns_temporary_payload) {
    CaptureWebSocket capture;
    auto socket = ruvia::detail::WebSocketAccess::make(&capture, &readSocket, &writeSocket, &closeSocket);
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(writeStoredTemporaryWebSocketPayload(socket)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK_EQ(capture.writes.size(), std::size_t{1});
    RUVIA_CHECK_EQ(capture.writes[0], std::string("owned-payload"));
}

RUVIA_TEST(response_stream_write_owned_transfers_prebuilt_chunk) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(writeOwnedChunk(writer)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes.size(), std::size_t{1});
    RUVIA_CHECK_EQ(sink.writes[0], std::string("owned-chunk"));
}

RUVIA_TEST(websocket_text_owned_transfers_prebuilt_payload) {
    CaptureWebSocket capture;
    auto socket = ruvia::detail::WebSocketAccess::make(&capture, &readSocket, &writeSocket, &closeSocket);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(writeOwnedTextFrame(socket)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(capture.writes.size(), std::size_t{1});
    RUVIA_CHECK_EQ(capture.writes[0], std::string("owned-frame"));
}

RUVIA_TEST(response_stream_end_submits_one_terminal_trailer_section) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(endWithTrailers(writer)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.trailers.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.trailers[0], std::string("Digest=sha-256=value"));
    RUVIA_CHECK_EQ(sink.trailers[1], std::string("Server-Timing=db;dur=7"));
}

RUVIA_TEST(response_stream_stored_end_owns_trailer_names_and_values) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(endWithExpiredTrailerSources(writer)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK_EQ(sink.trailers.size(), std::size_t{1});
    RUVIA_CHECK_EQ(sink.trailers[0], std::string("X-Owned-Trailer=temporary-value"));
}

RUVIA_TEST(response_stream_state_drives_typed_post_head_phases) {
    using ruvia::detail::ResponseStreamState;
    ResponseStreamState bound;
    CaptureStreamSink opaqueContextStorage;
    auto* opaqueContext = reinterpret_cast<ruvia::Context*>(&opaqueContextStorage);
    bound.bindContext(opaqueContext, &unusedStreamingHead);
    bool rebindRejected = false;
    try {
        bound.bindContext(opaqueContext, &unusedStreamingHead);
    } catch (const std::logic_error&) {
        rebindRejected = true;
    }
    RUVIA_CHECK(rebindRejected);
    ResponseStreamState detached;
    detached.bindContext(opaqueContext, &unusedStreamingHead);
    detached.releaseContext();
    bool detachedContextRejected = false;
    try {
        (void)detached.streamingHead();
    } catch (const std::logic_error&) {
        detachedContextRejected = true;
    }
    RUVIA_CHECK(detachedContextRejected);
    bound.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(ruvia::detail::ResponseStreamFraming::kHttp1Chunked, ruvia::HttpKnownMethod::kGet, ruvia::http_status::kOk, ruvia::detail::ResponseTrailerIntent::kNone));
    bool committedContextReleased = false;
    try {
        (void)bound.streamingHead();
    } catch (const std::logic_error& error) {
        committedContextReleased = std::string_view(error.what()) == "response stream is already committed";
    }
    RUVIA_CHECK(committedContextReleased);

    // A committed stream that allows a body accepts a chunk before end()...
    ResponseStreamState open;
    open.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(ruvia::detail::ResponseStreamFraming::kHttp1Chunked, ruvia::HttpKnownMethod::kGet, ruvia::http_status::kMultiStatus, ruvia::detail::ResponseTrailerIntent::kNone));
    RUVIA_CHECK(open.commitPlan() != nullptr);
    RUVIA_CHECK_EQ(open.commitPlan()->responseStatus(), ruvia::http_status::kMultiStatus);
    RUVIA_CHECK(open.commitPlan()->framing() == ruvia::detail::ResponseStreamFraming::kHttp1Chunked);
    bool recommitRejected = false;
    try {
        open.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(ruvia::detail::ResponseStreamFraming::kHttp2Frames, ruvia::HttpKnownMethod::kGet, ruvia::HttpStatusCode::fromValue(418), ruvia::detail::ResponseTrailerIntent::kNone));
    } catch (const std::logic_error&) {
        recommitRejected = true;
    }
    RUVIA_CHECK(recommitRejected);
    RUVIA_CHECK_EQ(open.commitPlan()->responseStatus(), ruvia::http_status::kMultiStatus);
    open.ensureBodyAllowed();  // no throw
    open.ensureTrailersAllowed(ruvia::detail::ResponseStreamTrailerFraming::kHttp1Chunked);

    // ...but after end() a further body chunk would land past the terminal
    // 0\r\n\r\n (HTTP/1.1) or END_STREAM (HTTP/2) and desync the connection, so
    // it must be rejected -- the same way a post-end trailer already is.
    open.markEnded();
    open.markEnded();  // terminal transition is idempotent
    bool bodyAfterEnd = false;
    try {
        open.ensureBodyAllowed();
    } catch (const std::logic_error&) {
        bodyAfterEnd = true;
    }
    RUVIA_CHECK(bodyAfterEnd);
    bool trailerAfterEnd = false;
    try {
        open.ensureTrailersAllowed(ruvia::detail::ResponseStreamTrailerFraming::kHttp1Chunked);
    } catch (const std::logic_error&) {
        trailerAfterEnd = true;
    }
    RUVIA_CHECK(trailerAfterEnd);

    // Transport failure is a terminal alternative, not a second flag that can
    // coexist with Ended. A post-commit abort retains the exact wire plan for
    // dispatch accounting, while an uncommitted abort cannot manufacture one.
    ResponseStreamState abortedOpen;
    abortedOpen.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(ruvia::detail::ResponseStreamFraming::kHttp1Chunked, ruvia::HttpKnownMethod::kGet, ruvia::http_status::kPartialContent, ruvia::detail::ResponseTrailerIntent::kNone));
    abortedOpen.markAborted();
    abortedOpen.markAborted();
    RUVIA_CHECK(abortedOpen.aborted());
    RUVIA_CHECK(!abortedOpen.ended());
    RUVIA_CHECK(abortedOpen.committed());
    RUVIA_CHECK_EQ(abortedOpen.commitPlan()->responseStatus(), ruvia::http_status::kPartialContent);
    bool endAfterAbort = false;
    try {
        abortedOpen.markEnded();
    } catch (const std::logic_error&) {
        endAfterAbort = true;
    }
    RUVIA_CHECK(endAfterAbort);

    ResponseStreamState abortedBeforeCommit;
    abortedBeforeCommit.markAborted();
    RUVIA_CHECK(abortedBeforeCommit.aborted());
    RUVIA_CHECK(!abortedBeforeCommit.committed());
    RUVIA_CHECK(!abortedBeforeCommit.ended());
    RUVIA_CHECK(abortedBeforeCommit.commitPlan() == nullptr);

    // A suppressed body (e.g. HEAD, 204 or 304) still refuses to accept a body
    // chunk, but with the head-only completion signal: writing the body a GET
    // would have produced is correct handler behavior there, so dispatch must
    // be able to tell it apart from a post-end() sequencing bug.
    ResponseStreamState suppressed;
    suppressed.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(ruvia::detail::ResponseStreamFraming::kHttp1Chunked, ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk, ruvia::detail::ResponseTrailerIntent::kNone));
    bool bodyRejected = false;
    try {
        suppressed.ensureBodyAllowed();
    } catch (const ruvia::detail::ResponseStreamHeadOnlyComplete&) {
        bodyRejected = true;
    }
    RUVIA_CHECK(bodyRejected);

    // HTTP/2 can keep the same content-forbidden response open solely for a
    // terminal trailing-HEADERS block, without accidentally enabling DATA.
    ResponseStreamState trailersOnly;
    trailersOnly.markCommitted(ruvia::detail::httpResponseStreamCommitPlan(ruvia::detail::ResponseStreamFraming::kHttp2Frames, ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk, ruvia::detail::ResponseTrailerIntent::kPresent));
    RUVIA_CHECK(trailersOnly.committed());
    RUVIA_CHECK(!trailersOnly.ended());
    bool trailersOnlyBodyRejected = false;
    try {
        trailersOnly.ensureBodyAllowed();
    } catch (const std::logic_error&) {
        trailersOnlyBodyRejected = true;
    }
    RUVIA_CHECK(trailersOnlyBodyRejected);
    trailersOnly.ensureTrailersAllowed(ruvia::detail::ResponseStreamTrailerFraming::kHttp2TrailingHeaders);
}

RUVIA_TEST(response_stream_head_rejects_a_mismatched_status_plan) {
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kCreated);
    auto plan = ruvia::detail::httpResponseStreamCommitPlan(ruvia::detail::ResponseStreamFraming::kHttp1Chunked, ruvia::HttpKnownMethod::kGet, ruvia::http_status::kAccepted, ruvia::detail::ResponseTrailerIntent::kNone);
    bool rejected = false;
    try {
        (void)ruvia::detail::prepareResponseStreamHead(std::move(response), ruvia::detail::ResponseStreamKind::kGeneric, std::move(plan));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
