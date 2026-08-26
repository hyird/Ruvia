#include "test_harness.h"

#include "ruvia/core/Task.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

struct SuspendedLoader final {
    explicit SuspendedLoader(asio::io_context& io)
        : dispatcher(std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8)),
          worker(ruvia::detail::WorkerHandleAccess::make(dispatcher)),
          signal(worker) {}

    ruvia::Task<std::string_view> readAll() {
        ++readCalls;
        co_await signal.wait();
        co_return "buffered";
    }

    ruvia::Task<void> discard() {
        ++discardCalls;
        co_await signal.wait();
    }

    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher;
    ruvia::WorkerHandle worker;
    ruvia::detail::WorkerSignal signal;
    int readCalls{0};
    int discardCalls{0};
};

ruvia::Task<void> completeLoad(ruvia::detail::RequestBodyLoader& loader, std::string_view& body) {
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

ruvia::Task<void> discardThenRejectLoad(ruvia::detail::RequestBodyLoader& loader, bool& rejected) {
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
    asio::io_context io(1);
    ruvia::detail::RequestBodyLoaderBinding<SuspendedLoader> binding(io);
    std::string_view body;
    bool rejected = false;

    auto first = asio::co_spawn(
        io, ruvia::detail::taskAsAwaitable(completeLoad(binding.facade(), body)), asio::use_future);
    io.poll();
    RUVIA_CHECK(body.empty());

    io.restart();
    auto second = asio::co_spawn(io,
        ruvia::detail::taskAsAwaitable(rejectConcurrentDiscard(binding.facade(), rejected)),
        asio::use_future);
    asio::post(io, [&binding] { binding.loader().signal.notify(); });
    io.run();
    first.get();
    second.get();

    RUVIA_CHECK_EQ(body, std::string_view("buffered"));
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(binding.loader().readCalls, 1);
    RUVIA_CHECK_EQ(binding.loader().discardCalls, 0);
}

RUVIA_TEST(request_body_loader_discard_is_terminal) {
    asio::io_context io(1);
    ruvia::detail::RequestBodyLoaderBinding<SuspendedLoader> binding(io);
    bool rejected = false;

    auto future = asio::co_spawn(io,
        ruvia::detail::taskAsAwaitable(discardThenRejectLoad(binding.facade(), rejected)),
        asio::use_future);
    io.poll();
    RUVIA_CHECK(!rejected);

    io.restart();
    asio::post(io, [&binding] { binding.loader().signal.notify(); });
    io.run();
    future.get();

    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(binding.loader().discardCalls, 1);
    RUVIA_CHECK_EQ(binding.loader().readCalls, 0);
}

RUVIA_TEST(request_body_loader_reuses_one_buffered_result) {
    asio::io_context io(1);
    ruvia::detail::RequestBodyLoaderBinding<SuspendedLoader> binding(io);
    std::string_view first;
    std::string_view second;

    auto future = asio::co_spawn(io,
        ruvia::detail::taskAsAwaitable(loadTwice(binding.facade(), first, second)),
        asio::use_future);
    io.poll();
    RUVIA_CHECK(first.empty());

    io.restart();
    asio::post(io, [&binding] { binding.loader().signal.notify(); });
    io.run();
    future.get();

    RUVIA_CHECK_EQ(first, std::string_view("buffered"));
    RUVIA_CHECK_EQ(second, first);
    RUVIA_CHECK_EQ(binding.loader().readCalls, 1);
}
