#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/web/detail/http3/Http3RequestBodyReader.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t returns{};
    std::size_t bodyAllocations{};
    std::size_t bodyReturns{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations;
        if (size >= 512) {
            ++bodyAllocations;
        }
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++returns;
        if (size >= 512) {
            ++bodyReturns;
        }
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

ruvia::Task<std::optional<std::span<const std::byte>>> readFacade(ruvia::BodyReader& reader) {
    co_return co_await reader.read();
}

ruvia::Task<std::optional<std::span<const std::byte>>> readBody(
    ruvia::detail::Http3RequestBodyReader& reader) {
    co_return co_await reader.read();
}

template <typename Operation>
void runWorkerOperation(asio::io_context& io, ruvia::EventLoopAttachment& attachment, Operation&& operation) {
    std::exception_ptr failure;
    asio::co_spawn(io, ruvia::asAwaitable(operation()), [&](std::exception_ptr error) {
        failure = error;
        attachment.stop();
    });
    attachment.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

RUVIA_TEST(http3_request_body_reader_resumes_one_producer_without_invalidating_borrowed_chunks) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    CountingResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::detail::Http3RequestBodyReader body(worker, 1024, &resource);
        ruvia::TaskScope tasks(worker, {.resource = &resource});
        ruvia::WorkerSignal started(worker), produced(worker), consumed(worker);
        const std::string firstBytes(1024, 'a'), secondBytes(1024, 'b');
        bool completed = false;
        bool expired = false;
        {
            auto cold = body.waitForSpace();  // Discarding a lazy wait reserves no producer lane.
        }
        auto producer = [&]() -> ruvia::Task<void> {
            for (unsigned step = 0; step < 32; ++step) {
                started.notify();
                if (!(co_await body.waitForSpace(1024))) {
                    co_return;
                }
                RUVIA_CHECK(body.availableCapacity() == 1024);
                RUVIA_CHECK(body.enqueue(secondBytes));
                produced.notify();
                co_await consumed.wait();
            }
        };
        auto watchdog = [&]() -> ruvia::Task<void> {
            (void)co_await ruvia::sleepFor(worker, std::chrono::milliseconds(500));
            if (!completed) {
                expired = true;
                (void)body.shutdown();
                started.notify();
                produced.notify();
                consumed.notify();
            }
        };
        RUVIA_CHECK(body.enqueue(firstBytes));
        tasks.spawn(producer());
        tasks.spawn(watchdog());
        std::optional<std::size_t> allocationBaseline;
        for (unsigned step = 0; step < 32; ++step) {
            co_await started.wait();
            if (expired) {
                break;
            }
            bool concurrentRejected = false;
            try {
                (void)co_await body.waitForSpace();
            } catch (const std::logic_error&) {
                concurrentRejected = true;
            }
            RUVIA_CHECK(concurrentRejected);
            const auto first = co_await body.read();
            co_await produced.wait();
            if (expired) {
                break;
            }
            RUVIA_CHECK(first && std::string_view(reinterpret_cast<const char*>(first->data()), first->size()) == firstBytes);
            const auto second = co_await body.read();
            RUVIA_CHECK(second && std::string_view(reinterpret_cast<const char*>(second->data()), second->size()) == secondBytes);
            const auto live = resource.allocations - resource.returns;
            if (allocationBaseline) {
                RUVIA_CHECK_EQ(live, *allocationBaseline);
            } else {
                allocationBaseline = live;
            }
            if (step + 1 < 32) {
                RUVIA_CHECK(body.enqueue(firstBytes));
            }
            consumed.notify();
        }
        completed = true;
        RUVIA_CHECK(!expired);
        if (!expired) {
            RUVIA_CHECK(body.finish());
            RUVIA_CHECK(!(co_await body.read()));
        }
        co_await tasks.join();
    };
    runWorkerOperation(io, attachment, operation);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3_request_body_reader_terminal_states_wake_blocked_producer) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    CountingResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        using Reader = ruvia::detail::Http3RequestBodyReader;
        for (const auto terminal : {Reader::Terminal::kFin, Reader::Terminal::kError,
                 Reader::Terminal::kCancelled, Reader::Terminal::kShutdown}) {
            Reader body(worker, 512, &resource);
            ruvia::TaskScope tasks(worker, {.resource = &resource});
            ruvia::WorkerSignal started(worker);
            RUVIA_CHECK(body.enqueue(std::string(512, 'a')));
            const auto borrowed = co_await body.read();
            RUVIA_CHECK(body.enqueue(std::string(512, 'b')));
            bool resumed = false;
            bool available = true;
            auto producer = [&]() -> ruvia::Task<void> {
                started.notify();
                available = co_await body.waitForSpace();
                resumed = true;
            };
            tasks.spawn(producer());
            co_await started.wait();
            RUVIA_CHECK(!resumed);
            switch (terminal) {
                case Reader::Terminal::kFin:
                    RUVIA_CHECK(body.finish());
                    break;
                case Reader::Terminal::kError:
                    RUVIA_CHECK(body.fail(std::make_error_code(std::errc::protocol_error)));
                    break;
                case Reader::Terminal::kCancelled:
                    RUVIA_CHECK(body.cancel());
                    break;
                case Reader::Terminal::kShutdown:
                    RUVIA_CHECK(body.shutdown());
                    break;
                case Reader::Terminal::kOpen:
                    break;
            }
            co_await tasks.join();
            RUVIA_CHECK(resumed && !available);
            RUVIA_CHECK(borrowed && borrowed->front() == std::byte{'a'});
            RUVIA_CHECK(!(co_await body.waitForSpace()));
            bool tooLarge = false;
            try {
                (void)co_await body.waitForSpace(513);
            } catch (const std::invalid_argument&) {
                tooLarge = true;
            }
            RUVIA_CHECK(tooLarge);
        }
    };
    runWorkerOperation(io, attachment, operation);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3_request_body_reader_owns_bounded_chunks_and_orders_fin) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 16});
    const auto worker = attachment.loop().handle();
    CountingResource resource;
    bool accepted = false;
    bool rejected = false;
    bool finished = false;
    bool lateRejected = false;
    bool viewOkay = false;
    bool eof = false;
    bool failed = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            ruvia::detail::BodyReaderBinding<ruvia::detail::Http3RequestBodyReader> binding(
                worker, 1024, &resource);
            auto& body = binding.reader();
            const std::string firstStorage(600, 'a');
            const std::string secondStorage(700, 'b');
            const std::string rejectedStorage(1100, 'x');
            const std::string_view firstBytes(firstStorage);
            const std::string_view secondBytes(secondStorage);
            accepted = body.enqueue(firstBytes);
            rejected = !body.enqueue(rejectedStorage);

            auto first = co_await ruvia::asAwaitable(readFacade(binding.facade()));
            const auto firstView = first.has_value() ? std::string_view(
                reinterpret_cast<const char*>(first->data()), first->size()) : std::string_view{};
            accepted = accepted && body.enqueue(secondBytes);
            viewOkay = firstView == firstBytes;
            finished = body.finish();
            auto second = co_await ruvia::asAwaitable(readBody(body));
            lateRejected = !body.enqueue("late");
            const bool secondOkay = second.has_value() && second->size() == secondBytes.size() &&
                std::string_view(reinterpret_cast<const char*>(second->data()), second->size()) ==
                    secondBytes;
            viewOkay = viewOkay && secondOkay;
            auto end = co_await ruvia::asAwaitable(readBody(body));
            eof = !end.has_value();
        } catch (...) {
            failed = true;
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(accepted);
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(finished);
    RUVIA_CHECK(lateRejected);
    RUVIA_CHECK(viewOkay);
    RUVIA_CHECK(eof);
    RUVIA_CHECK(!failed);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3_request_body_reader_repeated_reads_release_prior_chunk_without_losing_next) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 8});
    const auto worker = attachment.loop().handle();
    CountingResource resource;
    bool correct = true;
    bool failed = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            ruvia::detail::Http3RequestBodyReader reader(worker, 1024, &resource);
            for (char digit = '0'; digit != '8'; ++digit) {
                const std::string payload(600, digit);
                const bool enqueued = reader.enqueue(payload);
                correct = correct && enqueued;
                if (!enqueued) {
                    break;
                }
                auto view = co_await ruvia::asAwaitable(readBody(reader));
                correct = correct && view.has_value() && view->size() == payload.size() &&
                    std::string_view(reinterpret_cast<const char*>(view->data()), view->size()) == payload;
                correct = correct && resource.bodyAllocations == resource.bodyReturns + 1;
            }
            const bool finished = reader.finish();
            correct = correct && finished;
            auto end = co_await ruvia::asAwaitable(readBody(reader));
            correct = correct && !end.has_value();
            correct = correct && resource.bodyAllocations == resource.bodyReturns;
        } catch (...) {
            failed = true;
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(correct);
    RUVIA_CHECK(!failed);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3_request_body_reader_cancel_wakes_and_joins_a_suspended_read) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 8});
    const auto worker = attachment.loop().handle();
    CountingResource resource;
    bool cancelled = false;
    bool joined = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        ruvia::detail::Http3RequestBodyReader reader(worker, 64, &resource);
        asio::post(io, [&] { cancelled = reader.cancel(); });
        try {
            (void)co_await ruvia::asAwaitable(readBody(reader));
        } catch (const std::system_error& error) {
            joined = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(cancelled);
    RUVIA_CHECK(joined);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3_request_body_reader_error_discards_unread_bytes_without_invalidating_active_view) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 8});
    const auto worker = attachment.loop().handle();
    CountingResource resource;
    bool retained = false;
    bool rejected = false;
    bool released = false;
    bool failed = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            ruvia::detail::Http3RequestBodyReader reader(worker, 1024, &resource);
            const std::string first(600, 'a');
            const std::string pending(700, 'b');
            (void)reader.enqueue(first);
            auto view = co_await ruvia::asAwaitable(readBody(reader));
            (void)reader.enqueue(pending);
            RUVIA_CHECK(reader.fail(std::make_error_code(std::errc::bad_message)));
            retained = view.has_value() && view->size() == first.size() &&
                std::string_view(reinterpret_cast<const char*>(view->data()), view->size()) == first;
            RUVIA_CHECK_EQ(reader.queuedBytes(), 0U);
            try {
                (void)co_await ruvia::asAwaitable(readBody(reader));
            } catch (const std::system_error& error) {
                rejected = error.code() == std::make_error_code(std::errc::bad_message);
            }
            released = resource.bodyAllocations == resource.bodyReturns;
        } catch (...) {
            failed = true;
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(retained);
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(released);
    RUVIA_CHECK(!failed);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3_request_body_reader_shutdown_wakes_and_joins_a_suspended_read) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 8});
    const auto worker = attachment.loop().handle();
    CountingResource resource;
    bool shutdown = false;
    bool joined = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        ruvia::detail::Http3RequestBodyReader reader(worker, 64, &resource);
        asio::post(io, [&] { shutdown = reader.shutdown(); });
        try {
            (void)co_await ruvia::asAwaitable(readBody(reader));
        } catch (const std::system_error& error) {
            joined = error.code() == std::make_error_code(std::errc::owner_dead);
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(shutdown);
    RUVIA_CHECK(joined);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3_request_body_reader_cold_task_does_not_allocate_body_storage) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 8});
    const auto worker = attachment.loop().handle();
    CountingResource resource;
    {
        ruvia::detail::Http3RequestBodyReader reader(worker, 64, &resource);
        auto cold = reader.read();
        (void)cold;
    }
    RUVIA_CHECK_EQ(resource.bodyAllocations, std::size_t{0});
    RUVIA_CHECK_EQ(resource.bodyReturns, std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}
