#include "streaming_fixture.h"

// What happens to a stored operation when the capability it borrowed goes away first.

RUVIA_TEST(response_stream_cold_operation_rejects_after_capability_teardown) {
    CaptureStreamSink sink;
    auto operation = makeExpiredWrite(sink);
    bool rejected = false;

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(
            awaitExpiredWrite(operation, rejected)),
        asio::use_future);
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
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(
            awaitExpiredWrite(operation, rejected)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(capture.writes.empty());
}

RUVIA_TEST(body_reader_cold_operation_rejects_after_facade_teardown) {
    auto operation = makeExpiredBodyRead();
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx,
        ruvia::detail::taskAsAwaitable(
            awaitExpiredBodyRead(operation, rejected)),
        asio::use_future);
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
    auto operation = ruvia::detail::makeScopedOperation(
        scope, coldFrameTask(ColdFrameProbe(destroyed)));
    RUVIA_CHECK(!destroyed);
    scope.close();
    RUVIA_CHECK(destroyed);
    (void)operation;
}
