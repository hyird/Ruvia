#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseDecoding.h"
#include "ruvia/web/detail/client/HttpClientResponseMemory.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/client/HttpClientResultBudget.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {

class AllocationCounter final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t liveAllocations() const noexcept {
        return liveAllocations_;
    }
    [[nodiscard]] std::size_t liveBytes() const noexcept {
        return liveBytes_;
    }
    [[nodiscard]] std::size_t largeAllocations() const noexcept {
        return largeAllocations_;
    }
    [[nodiscard]] std::size_t largeBytes() const noexcept {
        return largeBytes_;
    }
    [[nodiscard]] std::size_t smallCachedAllocations() const noexcept {
        return smallCachedAllocations_;
    }
    [[nodiscard]] std::size_t smallCachedBytes() const noexcept {
        return smallCachedBytes_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* const result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++liveAllocations_;
        liveBytes_ += bytes;
        if (bytes >= kLargeAllocationBytes) {
            ++largeAllocations_;
            largeBytes_ += bytes;
        } else {
            ++smallCachedAllocations_;
            smallCachedBytes_ += bytes;
        }
        return result;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        --liveAllocations_;
        liveBytes_ -= bytes;
        if (bytes >= kLargeAllocationBytes) {
            --largeAllocations_;
            largeBytes_ -= bytes;
        } else {
            --smallCachedAllocations_;
            smallCachedBytes_ -= bytes;
        }
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    static constexpr std::size_t kLargeAllocationBytes = 256 * 1024;
    std::size_t liveAllocations_{};
    std::size_t liveBytes_{};
    std::size_t largeAllocations_{};
    std::size_t largeBytes_{};
    std::size_t smallCachedAllocations_{};
    std::size_t smallCachedBytes_{};
};

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
    auto run = [&]() -> ruvia::Task<void> {
        co_await operation();
        worker.attachment.stop();
    };
    auto root = worker.attachment.loop().start(run());
    worker.attachment.run();
    root.get();
    io.restart();
}

ruvia::Task<void> awaitCancelledResponse(ruvia::detail::HttpClientResponseState& state,
    bool& caughtExpectedError) {
    try {
        (void)co_await ruvia::detail::makeScopedOperation(
            state.bodyOperationScope, state.readAll(2 * 1024 * 1024));
    } catch (const ruvia::HttpClientError& error) {
        caughtExpectedError = error.code() == ruvia::HttpClientError::Code::kCancelled;
    }
}

ruvia::Task<void> awaitFailedResponse(ruvia::detail::HttpClientResponseState& state,
    bool& caughtExpectedError) {
    try {
        (void)co_await ruvia::detail::makeScopedOperation(
            state.bodyOperationScope, state.readAll(2 * 1024 * 1024));
    } catch (const std::runtime_error& error) {
        caughtExpectedError = std::string_view(error.what()) == "response fixture failed";
    }
}

}  // namespace

RUVIA_TEST(client_response_memory_domain_pins_state_and_releases_pooled_storage_last) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    AllocationCounter upstream;
    std::weak_ptr<ruvia::detail::HttpClientResultBudgetDomain> budgetLifetime;
    std::optional<ruvia::HttpClientResponseBytes> retained;

    runOperation(worker, io, [&]() -> ruvia::Task<void> {
        auto budget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 2 * 1024 * 1024 + 8});
        budgetLifetime = budget;
        ruvia::detail::HttpClientPool pool(io, worker.handle,
            ruvia::detail::HttpClientConfigStorage(ruvia::HttpClientConfig{.host = "unused.test"},
                std::pmr::get_default_resource()),
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 3 * 1024 * 1024},
            std::pmr::get_default_resource());
        auto domain = ruvia::detail::HttpClientResponseMemoryDomain::create(
            worker.handle, budget, upstream);
        budget.reset();
        auto* const state = domain->createState(pool);

        const std::string headerValue(512 * 1024, 'h');
        const std::string trailerValue(512 * 1024, 't');
        const std::string body(1024 * 1024, 'b');
        state->headers.push_back(ruvia::HttpHeader::copyOf("x-long", headerValue, state->resource));
        state->trailers.push_back(ruvia::HttpHeader::copyOf("x-trailer", trailerValue, state->resource));
        state->buffered.assign(body);
        state->pending.assign("tail");
        state->complete = true;

        domain->detachTransportBindings(pool);
        RUVIA_CHECK(state->pool == nullptr);
        RUVIA_CHECK_EQ(state->headers.front().value(), std::string_view(headerValue));
        RUVIA_CHECK_EQ(state->trailers.front().value(), std::string_view(trailerValue));
        RUVIA_CHECK_EQ(std::string_view(state->buffered), body);
        RUVIA_CHECK_EQ(std::string_view(state->pending), "tail");
        RUVIA_CHECK(upstream.largeBytes() >= headerValue.size() + trailerValue.size());

        std::size_t cachedLargeBytes{};
        std::size_t cachedSmallBytes{};
        auto operation = [&]() -> ruvia::Task<void> {
            std::optional<ruvia::HttpClientResponseBytes> budgetFiller;
            {
                auto cold = ruvia::detail::makeScopedOperation(
                    state->bodyOperationScope, state->readAll(body.size() + 4));
            }
            RUVIA_CHECK_EQ(std::string_view(state->buffered), body);
            RUVIA_CHECK_EQ(std::string_view(state->pending), "tail");

            bool sizeRejected = false;
            try {
                (void)co_await ruvia::detail::makeScopedOperation(
                    state->bodyOperationScope, state->readAll(1));
            } catch (const ruvia::HttpClientError& error) {
                sizeRejected = error.code() == ruvia::HttpClientError::Code::kResponseTooLarge;
            }
            RUVIA_CHECK(sizeRejected);
            RUVIA_CHECK_EQ(std::string_view(state->buffered), body);
            RUVIA_CHECK_EQ(std::string_view(state->pending), "tail");

            for (int iteration = 0; iteration < 16; ++iteration) {
                if (iteration != 0) {
                    state->buffered.assign(body);
                    state->pending.assign("tail");
                    state->offset = 0;
                }
                auto result = co_await ruvia::detail::makeScopedOperation(
                    state->bodyOperationScope, state->readAll(body.size() + 4));
                RUVIA_CHECK_EQ(result.size(), body.size() + 4);
                RUVIA_CHECK_EQ(result.bytes().front(), std::byte{'b'});
                RUVIA_CHECK_EQ(result.bytes().back(), std::byte{'l'});
                RUVIA_CHECK_EQ(state->headers.front().value(), std::string_view(headerValue));
                RUVIA_CHECK_EQ(state->trailers.front().value(), std::string_view(trailerValue));
                if (iteration == 0) {
                    retained.emplace(std::move(result));
                }
                RUVIA_CHECK_EQ(retained->bytes().front(), std::byte{'b'});
                RUVIA_CHECK_EQ(retained->bytes().back(), std::byte{'l'});
                if (iteration == 0) {
                    RUVIA_CHECK_EQ(budgetLifetime.lock()->retainedBytes(), body.size() + 4);
                    state->buffered.assign(body);
                    state->pending.assign("tail");
                    auto second = co_await ruvia::detail::makeScopedOperation(
                        state->bodyOperationScope, state->readAll(body.size() + 4));
                    budgetFiller.emplace(std::move(second));

                    state->buffered.assign(body);
                    state->pending.assign("tail");
                    bool budgetFull = false;
                    try {
                        (void)co_await ruvia::detail::makeScopedOperation(
                            state->bodyOperationScope, state->readAll(body.size() + 4));
                    } catch (const ruvia::HttpClientError& error) {
                        budgetFull = error.code() ==
                                     ruvia::HttpClientError::Code::kResultBudgetExceeded;
                    }
                    RUVIA_CHECK(budgetFull);
                    RUVIA_CHECK_EQ(std::string_view(state->buffered), body);
                    RUVIA_CHECK_EQ(std::string_view(state->pending), "tail");
                    budgetFiller.reset();
                    auto retry = co_await ruvia::detail::makeScopedOperation(
                        state->bodyOperationScope, state->readAll(body.size() + 4));
                    RUVIA_CHECK_EQ(retry.size(), body.size() + 4);
                } else {
                    RUVIA_CHECK_EQ(upstream.largeBytes(), cachedLargeBytes);
                    RUVIA_CHECK_EQ(upstream.smallCachedBytes(), cachedSmallBytes);
                }
                if (iteration == 0) {
                    cachedLargeBytes = upstream.largeBytes();
                    cachedSmallBytes = upstream.smallCachedBytes();
                }
            }
            state->bodyOperationScope.close();
            co_await state->bodyOperationScope.closeAndJoin();
            bool expiredScopeRejected = false;
            try {
                auto expired = ruvia::detail::makeScopedOperation(
                    state->bodyOperationScope, state->readAll(body.size()));
                (void)co_await std::move(expired);
            } catch (const std::logic_error&) {
                expiredScopeRejected = true;
            }
            RUVIA_CHECK(expiredScopeRejected);
        };
        co_await operation();

        domain.reset();
        RUVIA_CHECK(!budgetLifetime.expired());
        RUVIA_CHECK(worker.handle.isCurrent());
        RUVIA_CHECK(state->resource != nullptr);
        if (auto liveBudget = budgetLifetime.lock()) {
            RUVIA_CHECK_EQ(liveBudget->retainedBytes(), std::size_t{1024 * 1024 + 4});
        } else {
            RUVIA_CHECK(false);
        }
        RUVIA_CHECK(upstream.liveAllocations() > 0);
        state->releaseReference();
        RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.liveBytes(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.largeAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.largeBytes(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.smallCachedAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.smallCachedBytes(), std::size_t{0});
        RUVIA_CHECK(!budgetLifetime.expired());

        pool.closeNow();
        co_await pool.join();
    });
    RUVIA_CHECK(retained.has_value());
    RUVIA_CHECK_EQ(retained->bytes().size(), std::size_t{1024 * 1024 + 4});
    retained.reset();
    RUVIA_CHECK(budgetLifetime.expired());
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(client_response_memory_domain_joins_error_waiters_before_releasing_storage) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    AllocationCounter upstream;
    std::weak_ptr<ruvia::detail::HttpClientResultBudgetDomain> budgetLifetime;

    runOperation(worker, io, [&]() -> ruvia::Task<void> {
        auto budget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 2 * 1024 * 1024});
        budgetLifetime = budget;
        ruvia::detail::HttpClientPool pool(io, worker.handle,
            ruvia::detail::HttpClientConfigStorage(ruvia::HttpClientConfig{.host = "unused.test"},
                std::pmr::get_default_resource()),
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 2 * 1024 * 1024},
            std::pmr::get_default_resource());
        auto domain = ruvia::detail::HttpClientResponseMemoryDomain::create(
            worker.handle, budget, upstream);
        budget.reset();

        auto* const cancelledState = domain->createState(pool);
        auto* const failedState = domain->createState(pool);
        const std::string body(1024 * 1024, 'c');
        const std::string header(512 * 1024, 'h');
        for (auto* state : {cancelledState, failedState}) {
            state->headers.push_back(ruvia::HttpHeader::copyOf("x-retained", header, state->resource));
            state->buffered.assign(body);
        }
        domain.reset();
        RUVIA_CHECK(!budgetLifetime.expired());
        RUVIA_CHECK(upstream.largeBytes() >= 2 * (body.size() + header.size()));

        bool cancelledAsExpected = false;
        ruvia::TaskScope cancelledTasks(worker.handle);
        cancelledTasks.spawn(awaitCancelledResponse(*cancelledState, cancelledAsExpected));
        RUVIA_CHECK(cancelledState->collectAll);
        cancelledState->errorCode =
            static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kCancelled);
        cancelledState->complete = true;
        cancelledState->dataSignal.notify();
        co_await cancelledTasks.join();
        cancelledState->bodyOperationScope.close();
        co_await cancelledState->bodyOperationScope.closeAndJoin();
        RUVIA_CHECK(cancelledAsExpected);
        RUVIA_CHECK_EQ(std::string_view(cancelledState->buffered), body);
        RUVIA_CHECK_EQ(cancelledState->headers.front().value(), std::string_view(header));

        bool failureAsExpected = false;
        ruvia::TaskScope failedTasks(worker.handle);
        failedTasks.spawn(awaitFailedResponse(*failedState, failureAsExpected));
        RUVIA_CHECK(failedState->collectAll);
        failedState->failure = std::make_exception_ptr(std::runtime_error("response fixture failed"));
        failedState->complete = true;
        failedState->dataSignal.notify();
        co_await failedTasks.join();
        failedState->bodyOperationScope.close();
        co_await failedState->bodyOperationScope.closeAndJoin();
        RUVIA_CHECK(failureAsExpected);
        RUVIA_CHECK_EQ(std::string_view(failedState->buffered), body);
        RUVIA_CHECK_EQ(failedState->headers.front().value(), std::string_view(header));

        cancelledState->memoryDomain()->detachTransportBindings(pool);
        RUVIA_CHECK(cancelledState->pool == nullptr);
        RUVIA_CHECK(failedState->pool == nullptr);
        cancelledState->releaseReference();
        RUVIA_CHECK(upstream.largeAllocations() > 0);
        failedState->releaseReference();
        RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.liveBytes(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.largeAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.largeBytes(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.smallCachedAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.smallCachedBytes(), std::size_t{0});
        RUVIA_CHECK(budgetLifetime.expired());

        pool.closeNow();
        co_await pool.join();
    });
}

RUVIA_TEST(client_response_memory_budget_is_shared_and_charged_before_buffer_growth) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    AllocationCounter upstream;
    auto budget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
        ruvia::HttpClientResultBudgetConfig{.max_in_flight_bytes = 2048});
    {
        auto first = ruvia::detail::HttpClientResponseMemoryDomain::create(worker.handle, budget, upstream);
        auto second = ruvia::detail::HttpClientResponseMemoryDomain::create(worker.handle, budget, upstream);
        for (int iteration = 0; iteration < 16; ++iteration) {
            {
                std::pmr::string held(first->resource());
                held.assign(1024, 'a');
                const auto charge = budget->in_flight_bytes();
                RUVIA_CHECK(charge >= 1025U);
                std::pmr::string rejected(second->resource());
                bool limited = false;
                try {
                    rejected.assign(1024, 'b');
                } catch (const ruvia::HttpClientError& error) {
                    limited = error.code() == ruvia::HttpClientError::Code::kResultBudgetExceeded;
                }
                RUVIA_CHECK(limited);
                RUVIA_CHECK_EQ(budget->in_flight_bytes(), charge);
                RUVIA_CHECK_EQ(held.size(), 1024U);
            }
            RUVIA_CHECK_EQ(budget->in_flight_bytes(), 0U);
        }
    }
    RUVIA_CHECK_EQ(upstream.liveBytes(), 0U);
}

RUVIA_TEST(client_response_decoder_allocations_share_the_receive_budget) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    auto budget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
        ruvia::HttpClientResultBudgetConfig{.max_in_flight_bytes = 128 * 1024});
    auto domain = ruvia::detail::HttpClientResponseMemoryDomain::create(worker.handle, budget);
    const std::string plain(256 * 1024, 'a');
    auto encoded = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, plain, {.maxEncodedBytes = 4096});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    runOperation(worker, io, [&]() -> ruvia::Task<void> {
        ruvia::detail::HttpClientResponseState state(worker.handle, domain->resource());
        state.responseBodyPlan = ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kGet, ruvia::http_status::kOk);
        state.headers.push_back(ruvia::HttpHeader::copyOf("Content-Encoding", "gzip", state.resource));
        state.pending.assign(encoded.encoded()->bytes());
        ruvia::detail::configureHttpClientResponseDecoding(state);
        bool limited = false;
        try {
            ruvia::detail::decodeHttpClientResponseContentEncoding(state, true, plain.size());
        } catch (const ruvia::HttpClientError& error) {
            limited = error.code() == ruvia::HttpClientError::Code::kResultBudgetExceeded;
        }
        RUVIA_CHECK(limited);
        RUVIA_CHECK(state.pending.empty());
        RUVIA_CHECK(state.buffered.empty());
        co_return;
    });
    RUVIA_CHECK_EQ(budget->in_flight_bytes(), 0U);
}
