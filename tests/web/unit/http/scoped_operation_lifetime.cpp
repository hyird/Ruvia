#include "streaming_fixture.h"
#include "context_services_fixture.h"

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/MultipartReader.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"

#include <limits>

// What happens to a stored operation when the capability it borrowed goes away first.

namespace {

ruvia::ScopedOperation<ruvia::HttpResponse> makeExpiredNotFoundResponse() {
    ruvia::WorkerMemory worker;
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::RequestMemory memory(worker);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    auto context = ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    return context.notFound();
}

ruvia::Task<void> awaitExpiredNotFoundResponse(ruvia::ScopedOperation<ruvia::HttpResponse>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::SseWriter makeExpiredSseWriter(CaptureStreamSink& sink) {
    auto writer = makeWriter(sink);
    return ruvia::detail::StreamingAccess::makeSseWriter(writer);
}

ruvia::SseWriter makeSseWriterAfterClosedWriterScope(CaptureStreamSink& sink) {
    auto writer = makeWriter(sink);
    ruvia::detail::StreamingAccess::releaseContext(writer);
    return ruvia::detail::StreamingAccess::makeSseWriter(writer);
}

ruvia::Task<void> writeExpiredSse(ruvia::SseWriter& writer, bool& rejected) {
    try {
        co_await writer.write(ruvia::SseMessage{.data = "must-not-run"});
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::Task<void> writeExpiredInvalidSse(ruvia::SseWriter& writer, bool& lifetimeRejected, bool& validationRan) {
    try {
        co_await writer.write(ruvia::SseMessage{.data = "must-not-run", .event = "bad\nevent"});
    } catch (const std::invalid_argument&) {
        validationRan = true;
    } catch (const std::logic_error&) {
        lifetimeRejected = true;
    }
}

ruvia::MultipartReader makeExpiredMultipartReader() {
    ruvia::detail::BodyReaderBinding<ImmediateBodySource> binding;
    return ruvia::MultipartReader(binding.facade(), {.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = ruvia::detail::processResource()});
}

ruvia::Task<void> readExpiredMultipart(ruvia::MultipartReader& reader, bool& rejected) {
    try {
        (void)co_await reader.read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

}  // namespace

RUVIA_TEST(response_stream_cold_operation_rejects_after_capability_teardown) {
    CaptureStreamSink sink;
    auto operation = makeExpiredWrite(sink);
    bool rejected = false;

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(awaitExpiredWrite(operation, rejected)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK(rejected);
    RUVIA_CHECK(sink.writes.empty());
}

RUVIA_TEST(websocket_cold_operation_rejects_after_facade_teardown) {
    CaptureWebSocket capture;
    auto operation = makeExpiredWebSocketWrite(capture);
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(awaitExpiredWrite(operation, rejected)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(capture.writes.empty());
}

RUVIA_TEST(body_reader_cold_operation_rejects_after_facade_teardown) {
    auto operation = makeExpiredBodyRead();
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(awaitExpiredBodyRead(operation, rejected)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(context_not_found_cold_operation_rejects_after_context_teardown) {
    auto operation = makeExpiredNotFoundResponse();
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(awaitExpiredNotFoundResponse(operation, rejected)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(sse_writer_rejects_after_stream_writer_teardown) {
    CaptureStreamSink sink;
    auto writer = makeExpiredSseWriter(sink);
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(writeExpiredSse(writer, rejected)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(writer.aborted());
    RUVIA_CHECK(sink.writes.empty());
}

RUVIA_TEST(sse_writer_checks_lifetime_before_message_validation) {
    CaptureStreamSink sink;
    auto writer = makeExpiredSseWriter(sink);
    bool lifetimeRejected = false;
    bool validationRan = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(writeExpiredInvalidSse(writer, lifetimeRejected, validationRan)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(lifetimeRejected);
    RUVIA_CHECK(!validationRan);
    RUVIA_CHECK(sink.writes.empty());
}

RUVIA_TEST(response_stream_writer_checks_lifetime_before_copying_payload_or_trailers) {
    CaptureStreamSink sink;
    auto writer = makeWriter(sink);
    ruvia::detail::StreamingAccess::releaseContext(writer);

    const std::string_view oversized("x", (std::numeric_limits<std::size_t>::max)());
    bool writeRejectedByLifetime = false;
    bool writeCopiedPayload = false;
    try {
        (void)writer.write(oversized);
    } catch (const std::length_error&) {
        writeCopiedPayload = true;
    } catch (const std::logic_error&) {
        writeRejectedByLifetime = true;
    }

    const std::array<ruvia::HttpHeaderView, 1> trailers{ruvia::HttpHeaderView{oversized, "value"}};
    bool endRejectedByLifetime = false;
    bool endCopiedTrailer = false;
    try {
        (void)writer.end(trailers);
    } catch (const std::length_error&) {
        endCopiedTrailer = true;
    } catch (const std::logic_error&) {
        endRejectedByLifetime = true;
    }

    RUVIA_CHECK(writeRejectedByLifetime);
    RUVIA_CHECK(!writeCopiedPayload);
    RUVIA_CHECK(endRejectedByLifetime);
    RUVIA_CHECK(!endCopiedTrailer);
    RUVIA_CHECK(sink.writes.empty());
    RUVIA_CHECK(sink.trailers.empty());
}

RUVIA_TEST(sse_writer_aborted_is_safe_after_closed_writer_scope) {
    CaptureStreamSink sink;
    auto writer = makeSseWriterAfterClosedWriterScope(sink);
    RUVIA_CHECK(writer.aborted());
}

RUVIA_TEST(multipart_reader_rejects_after_body_reader_teardown) {
    auto reader = makeExpiredMultipartReader();
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(readExpiredMultipart(reader, rejected)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(scoped_capability_move_relinks_and_parent_close_expires_destination) {
    ruvia::detail::ScopedOperationScope scope;
    int expiredCount = 0;
    TestScopedCapability first(scope, expiredCount);
    TestScopedCapability moved(std::move(first));
    moved.use();
    scope.close();
    RUVIA_CHECK_EQ(expiredCount, 1);
    bool rejected = false;
    try {
        moved.use();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(scoped_capability_copy_relinks_and_early_destruction_unlinks) {
    int expiredCount = 0;
    ruvia::detail::ScopedOperationScope scope;
    TestScopedCapability source(scope, expiredCount);
    {
        TestScopedCapability destroyedEarly(source);
        destroyedEarly.use();
    }
    TestScopedCapability survivingCopy(source);
    scope.close();
    RUVIA_CHECK_EQ(expiredCount, 2);

    bool sourceRejected = false;
    try {
        source.use();
    } catch (const std::logic_error&) {
        sourceRejected = true;
    }
    RUVIA_CHECK(sourceRejected);
}

RUVIA_TEST(scoped_operation_parent_close_destroys_cold_frame_immediately) {
    ruvia::detail::ScopedOperationScope scope;
    bool destroyed = false;
    auto operation = ruvia::detail::makeScopedOperation(scope, coldFrameTask(ColdFrameProbe(destroyed)));
    RUVIA_CHECK(!destroyed);
    scope.close();
    RUVIA_CHECK(destroyed);
    (void)operation;
}
