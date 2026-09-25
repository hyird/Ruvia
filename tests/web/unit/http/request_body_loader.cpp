#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {

struct SuspendedLoader final {
    explicit SuspendedLoader(const ruvia::WorkerHandle& worker)
        : signal(worker) {}

    ruvia::Task<std::string_view> readAll() {
        ++readCalls;
        co_await signal.wait();
        co_return "buffered";
    }

    ruvia::Task<void> discard() {
        ++discardCalls;
        co_await signal.wait();
    }

    ruvia::WorkerSignal signal;
    int readCalls{0};
    int discardCalls{0};
};

void runBounded(ruvia::EventLoopAttachment& attachment, asio::io_context& io) {
    asio::steady_timer timeout(io);
    auto timedOut = std::make_shared<bool>(false);
    timeout.expires_after(std::chrono::seconds(5));
    timeout.async_wait([timedOut, &io](const std::error_code& error) {
        if (!error) {
            *timedOut = true;
            io.stop();
        }
    });

    attachment.run();
    timeout.cancel();
    if (*timedOut) {
        throw std::runtime_error("request body loader test timed out");
    }
}

ruvia::Task<void> completeLoad(
    ruvia::detail::RequestBodyLoader& loader, std::string_view& body) {
    body = co_await loader.readAll();
}

ruvia::Task<void> rejectConcurrentDiscard(
    ruvia::detail::RequestBodyLoader& loader, bool& rejected) {
    try {
        co_await loader.discard();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::Task<void> discardThenRejectLoad(
    ruvia::detail::RequestBodyLoader& loader, bool& rejected) {
    co_await loader.discard();
    try {
        (void)co_await loader.readAll();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::Task<void> loadTwice(
    ruvia::detail::RequestBodyLoader& loader, std::string_view& first, std::string_view& second) {
    first = co_await loader.readAll();
    second = co_await loader.readAll();
}

}  // namespace

RUVIA_TEST(request_body_loader_rejects_read_discard_overlap) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    ruvia::detail::RequestBodyLoaderBinding<SuspendedLoader> binding(worker);
    std::string_view body;
    bool rejected = false;
    std::exception_ptr firstFailure;
    std::exception_ptr secondFailure;
    int completedOperations = 0;
    const auto complete = [&] {
        if (++completedOperations == 2) {
            io.stop();
        }
    };

    asio::co_spawn(io, ruvia::asAwaitable(completeLoad(binding.facade(), body)),
        [&](std::exception_ptr failure) {
            firstFailure = failure;
            complete();
        });
    // Let the first read suspend before restarting the context for the overlap check.
    asio::post(io, [&io] { io.stop(); });
    runBounded(attachment, io);
    RUVIA_CHECK(body.empty());
    RUVIA_CHECK_EQ(binding.loader().readCalls, 1);

    io.restart();
    asio::co_spawn(io, ruvia::asAwaitable(rejectConcurrentDiscard(binding.facade(), rejected)),
        [&](std::exception_ptr failure) {
            secondFailure = failure;
            complete();
        });
    asio::post(io, [&binding] { binding.loader().signal.notify(); });
    runBounded(attachment, io);
    if (firstFailure) {
        std::rethrow_exception(firstFailure);
    }
    if (secondFailure) {
        std::rethrow_exception(secondFailure);
    }

    RUVIA_CHECK_EQ(body, std::string_view("buffered"));
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(binding.loader().readCalls, 1);
    RUVIA_CHECK_EQ(binding.loader().discardCalls, 0);
}

RUVIA_TEST(request_body_loader_discard_is_terminal) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    ruvia::detail::RequestBodyLoaderBinding<SuspendedLoader> binding(worker);
    bool rejected = false;
    std::exception_ptr failure;

    asio::co_spawn(io,
        ruvia::asAwaitable(discardThenRejectLoad(binding.facade(), rejected)),
        [&](std::exception_ptr error) {
            failure = error;
            io.stop();
        });
    asio::post(io, [&io] { io.stop(); });
    runBounded(attachment, io);
    RUVIA_CHECK(!rejected);

    io.restart();
    asio::post(io, [&binding] { binding.loader().signal.notify(); });
    runBounded(attachment, io);
    if (failure) {
        std::rethrow_exception(failure);
    }

    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(binding.loader().discardCalls, 1);
    RUVIA_CHECK_EQ(binding.loader().readCalls, 0);
}

RUVIA_TEST(request_body_loader_reuses_one_buffered_result) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    ruvia::detail::RequestBodyLoaderBinding<SuspendedLoader> binding(worker);
    std::string_view first;
    std::string_view second;
    std::exception_ptr failure;

    asio::co_spawn(io, ruvia::asAwaitable(loadTwice(binding.facade(), first, second)),
        [&](std::exception_ptr error) {
            failure = error;
            io.stop();
        });
    asio::post(io, [&io] { io.stop(); });
    runBounded(attachment, io);
    RUVIA_CHECK(first.empty());

    io.restart();
    asio::post(io, [&binding] { binding.loader().signal.notify(); });
    runBounded(attachment, io);
    if (failure) {
        std::rethrow_exception(failure);
    }

    RUVIA_CHECK_EQ(first, std::string_view("buffered"));
    RUVIA_CHECK_EQ(second, first);
    RUVIA_CHECK_EQ(binding.loader().readCalls, 1);
}
