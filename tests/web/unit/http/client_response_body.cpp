#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/post.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
class TestWorker final {
public:
    explicit TestWorker(asio::io_context& io)
        : attachment(ruvia::attachEventLoop(io, {.mailboxCapacity = 8})),
          handle(attachment.loop().handle()) {}

    ruvia::EventLoopAttachment attachment;
    ruvia::WorkerHandle handle;
};

template <typename Operation>
void runOperation(TestWorker& worker, asio::io_context& io, Operation&& operation) {
    std::exception_ptr failure;
    asio::co_spawn(io, ruvia::asAwaitable(operation()),
        [&worker, &failure](std::exception_ptr error) {
            failure = error;
            worker.attachment.stop();
        });
    worker.attachment.run();
    io.restart();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

RUVIA_TEST(client_body_chunks_preserve_octets_and_pending_data_does_not_invalidate_views) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        state.buffered.assign("\0\xff\xc3", 3);
        state.pending.assign("\xa9", 1);
        state.complete = true;
        auto operation = [&]() -> ruvia::Task<void> {
            const auto first = co_await state.read<std::span<const std::byte>>();
            RUVIA_CHECK(first.has_value());
            RUVIA_CHECK_EQ(first->size(), std::size_t{3});
            RUVIA_CHECK((*first)[0] == std::byte{0});
            RUVIA_CHECK((*first)[1] == std::byte{0xff});
            state.pending.append("tail");
            RUVIA_CHECK((*first)[2] == std::byte{0xc3});
            const auto next = co_await state.read<std::span<const std::byte>>();
            RUVIA_CHECK_EQ(next->size(), std::size_t{5});
            RUVIA_CHECK((*next)[0] == std::byte{0xa9});
            RUVIA_CHECK(!(co_await state.read<std::string_view>()));
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(client_body_collection_reclaims_temporaries_and_retains_results_and_headers) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    std::optional<std::pmr::vector<std::byte>> retained;
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        const std::string payload(1024, '\xff');
        const std::string header(128, 'h');
        state.headers.push_back(ruvia::HttpHeader::copyOf("x-retained", header, &resource));
        state.buffered.assign(payload);
        state.pending.reserve(payload.size());
        state.complete = true;
        auto operation = [&]() -> ruvia::Task<void> {
            {
                const auto before = resource.liveAllocations();
                {
                    auto discarded = ruvia::detail::makeScopedOperation(state.bodyOperationScope, state.readAll(4096));
                }
                RUVIA_CHECK_EQ(resource.liveAllocations(), before);
                RUVIA_CHECK_EQ(state.offset, std::size_t{0});
                RUVIA_CHECK(!state.collectAll);
            }
            retained.emplace(co_await state.readAll(4096));
            const auto baseline = resource.liveAllocations();
            for (int i = 0; i < 64; ++i) {
                state.offset = 0;
                state.buffered.assign(payload);
                state.pending.assign("\0\x80", 2);
                {
                    auto bytes = co_await state.readAll(4096);
                    RUVIA_CHECK_EQ(bytes.size(), payload.size() + 2);
                    RUVIA_CHECK(bytes[payload.size()] == std::byte{0});
                    RUVIA_CHECK(bytes.back() == std::byte{0x80});
                }
                RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
                RUVIA_CHECK_EQ(state.headers.front().value(), std::string_view(header));
                RUVIA_CHECK_EQ(retained->size(), payload.size());
                RUVIA_CHECK(retained->front() == std::byte{0xff});
            }
            state.offset = 0;
            bool limited = false;
            try {
                (void)co_await state.readAll(1);
            } catch (const ruvia::HttpClientError&) {
                limited = true;
            }
            RUVIA_CHECK(limited);
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            state.failure = std::make_exception_ptr(std::runtime_error("transport failed"));
            bool failed = false;
            try {
                (void)co_await state.readAll(4096);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK(retained->back() == std::byte{0xff});
    RUVIA_CHECK(resource.liveAllocations() > 0);
    retained.reset();
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(client_body_collection_cancellation_joins_before_storage_is_released) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        state.buffered.assign(256, 'x');
        const auto baseline = resource.liveAllocations();
        bool cancelled = false;
        bool pendingBeforeCancel = false;
        std::exception_ptr failure;
        auto operation = [&]() -> ruvia::Task<void> {
            try {
                (void)co_await ruvia::detail::makeScopedOperation(state.bodyOperationScope, state.readAll(4096));
            } catch (const std::system_error& error) {
                cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
            }
        };
        asio::co_spawn(io, ruvia::asAwaitable(operation()),
            [&worker, &failure](std::exception_ptr error) {
                failure = error;
                worker.attachment.stop();
            });
        asio::post(io, [&] {
            pendingBeforeCancel = !cancelled;
            state.failure = std::make_exception_ptr(
                std::system_error(std::make_error_code(std::errc::operation_canceled)));
            state.complete = true;
            state.dataSignal.notify();
        });
        worker.attachment.run();
        io.restart();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        RUVIA_CHECK(pendingBeforeCancel);
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}
