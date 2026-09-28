#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/HttpClientResponse.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/client/HttpClientResultBudget.h"
#include "ruvia/web/detail/http3/Http3ClientBodyBudget.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"

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

class LoopbackResponseServer final {
public:
    LoopbackResponseServer(asio::io_context& ioContext, const ruvia::WorkerHandle& worker,
        std::vector<std::string> bodies)
        : ioContext_(ioContext),
          acceptor_(ioContext, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          done_(worker) {
        responses_.reserve(bodies.size());
        for (const auto& body : bodies) {
            responses_.push_back("HTTP/1.1 200 OK\r\nContent-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + body);
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        acceptNext();
    }

    [[nodiscard]] ruvia::Task<void> wait() {
        co_await done_.wait();
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    void acceptNext() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(ioContext_);
        acceptor_.async_accept(*socket, [this, socket](const std::error_code& error) {
            if (error) {
                fail(error);
                return;
            }
            auto request = std::make_shared<asio::streambuf>();
            asio::async_read_until(*socket, *request, "\r\n\r\n",
                [this, socket, request](const std::error_code& readError, std::size_t) {
                    if (readError) {
                        fail(readError);
                        return;
                    }
                    const auto responseIndex = nextResponse_++;
                    asio::async_write(*socket, asio::buffer(responses_[responseIndex]),
                        [this, socket](const std::error_code& writeError, std::size_t) {
                            if (writeError) {
                                fail(writeError);
                                return;
                            }
                            if (nextResponse_ == responses_.size()) {
                                done_.notify();
                            } else {
                                acceptNext();
                            }
                        });
                });
        });
    }

    void fail(const std::error_code& error) {
        if (failure_ == nullptr) {
            failure_ = std::make_exception_ptr(std::system_error(error));
            done_.notify();
        }
    }

    asio::io_context& ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<std::string> responses_;
    ruvia::WorkerSignal done_;
    std::exception_ptr failure_;
    std::size_t nextResponse_{0};
};

[[nodiscard]] ruvia::HttpClientConfig localHttpClientConfig(std::uint16_t port) {
    return ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp,
        .host = "127.0.0.1",
        .port = port,
        .protocol = ruvia::HttpClientProtocol::kHttp1Only};
}

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
    ruvia::detail::Http3ClientBodyBudget receiveBudget(32);
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        state.buffered.assign("\0\xff\xc3", 3);
        state.pending.assign("\xa9", 1);
        RUVIA_CHECK(state.bindHttp3BodyBudget(receiveBudget));
        RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{4});
        state.complete = true;
        auto operation = [&]() -> ruvia::Task<void> {
            const auto first = co_await state.read<std::span<const std::byte>>();
            RUVIA_CHECK(first.has_value());
            RUVIA_CHECK_EQ(first->size(), std::size_t{3});
            RUVIA_CHECK((*first)[0] == std::byte{0});
            RUVIA_CHECK((*first)[1] == std::byte{0xff});
            state.pending.append("tail");
            state.reconcileProducerBodyBytes();
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{8});
            RUVIA_CHECK((*first)[2] == std::byte{0xc3});
            const auto next = co_await state.read<std::span<const std::byte>>();
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{5});
            RUVIA_CHECK_EQ(next->size(), std::size_t{5});
            RUVIA_CHECK((*next)[0] == std::byte{0xa9});
            RUVIA_CHECK(!(co_await state.read<std::string_view>()));
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(client_registry_aliases_share_the_worker_result_budget_domain) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    LoopbackResponseServer server(io, worker.handle, {"one", "two"});
    auto* const resource = std::pmr::get_default_resource();
    const auto config = localHttpClientConfig(server.port());
    const ruvia::detail::HttpClientDefinition definitions[]{
        {std::pmr::string("first", resource),
            ruvia::detail::HttpClientConfigStorage(config, resource)},
        {std::pmr::string("second", resource),
            ruvia::detail::HttpClientConfigStorage(config, resource)},
    };
    auto budgetDomain = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
        ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 3});
    ruvia::detail::HttpClientRegistry registry(
        io, worker.handle, resource, definitions, budgetDomain);
    ruvia::detail::ScopedOperationScope scope;
    const auto firstClient = registry.get("first", scope);
    const auto secondClient = registry.get("second", scope);
    RUVIA_CHECK_EQ(firstClient.host(), std::string_view("127.0.0.1"));
    RUVIA_CHECK_EQ(secondClient.host(), std::string_view("127.0.0.1"));
    RUVIA_CHECK_EQ(firstClient.port(), server.port());
    RUVIA_CHECK_EQ(secondClient.port(), server.port());

    server.start();
    auto operation = [&]() -> ruvia::Task<void> {
        auto firstResponse = co_await firstClient.send({.target = "/first"});
        std::optional<ruvia::HttpClientResponseBytes> retained;
        retained.emplace(co_await firstResponse.body().readAll(3));
        RUVIA_CHECK_EQ(std::string_view(
                           reinterpret_cast<const char*>(retained->bytes().data()),
                           retained->bytes().size()),
            std::string_view("one"));
        RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{3});

        auto secondResponse = co_await secondClient.send({.target = "/second"});
        bool rejected = false;
        try {
            (void)co_await secondResponse.body().readAll(3);
        } catch (const ruvia::HttpClientError& error) {
            rejected = error.code() == ruvia::HttpClientError::Code::kResultBudgetExceeded;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{3});

        retained.reset();
        RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{0});
        auto retried = co_await secondResponse.body().readAll(3);
        RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{3});
        RUVIA_CHECK_EQ(std::string_view(
                           reinterpret_cast<const char*>(retried.bytes().data()),
                           retried.bytes().size()),
            std::string_view("two"));
        co_await server.wait();
        registry.closeNow();
        co_await registry.join();
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(budgetDomain->retainedBytes(), std::size_t{0});
}

RUVIA_TEST(worker_capabilities_keep_result_budgets_independent) {
    std::optional<ruvia::HttpClientResponseBytes> firstResult;
    {
        auto& firstIo = ruvia::test::newTestIoContext();
        TestWorker firstWorker(firstIo);
        LoopbackResponseServer firstServer(firstIo, firstWorker.handle, {"one"});
        ruvia::WorkerMemory firstMemory;
        auto* const firstResource = firstMemory.resource();
        const auto firstConfig = localHttpClientConfig(firstServer.port());
        const ruvia::detail::HttpClientDefinition firstDefinition[]{
            {std::pmr::string("first", firstResource),
                ruvia::detail::HttpClientConfigStorage(firstConfig, firstResource)},
        };
        const ruvia::detail::WorkerCapabilityDefinitions firstDefinitions{
            .httpClients = firstDefinition};
        const ruvia::detail::WorkerCapabilityOptions options{
            .httpClientResultBudget = {.maxRetainedBytes = 3}};
        ruvia::detail::WorkerCapabilities firstCapabilities(
            firstIo, firstWorker.handle, firstResource, firstDefinitions, options);
        ruvia::detail::ScopedOperationScope scope;
        const ruvia::StopToken stopToken;
        const auto client = firstCapabilities.clientRegistries().httpClient(
            "first", scope, stopToken);

        firstServer.start();
        auto operation = [&]() -> ruvia::Task<void> {
            auto response = co_await client.send({.target = "/first"});
            firstResult.emplace(co_await response.body().readAll(3));
            co_await firstServer.wait();
            firstCapabilities.closeNow();
            co_await firstCapabilities.join();
        };
        runOperation(firstWorker, firstIo, operation);
    }

    RUVIA_CHECK(firstResult.has_value());
    RUVIA_CHECK_EQ(firstResult->bytes().size(), std::size_t{3});
    {
        auto& secondIo = ruvia::test::newTestIoContext();
        TestWorker secondWorker(secondIo);
        LoopbackResponseServer secondServer(secondIo, secondWorker.handle, {"two"});
        ruvia::WorkerMemory secondMemory;
        auto* const secondResource = secondMemory.resource();
        const auto secondConfig = localHttpClientConfig(secondServer.port());
        const ruvia::detail::HttpClientDefinition definition[]{
            {std::pmr::string("second", secondResource),
                ruvia::detail::HttpClientConfigStorage(secondConfig, secondResource)},
        };
        const ruvia::detail::WorkerCapabilityDefinitions definitions{
            .httpClients = definition};
        const ruvia::detail::WorkerCapabilityOptions options{
            .httpClientResultBudget = {.maxRetainedBytes = 3}};
        ruvia::detail::WorkerCapabilities capabilities(
            secondIo, secondWorker.handle, secondResource, definitions, options);
        ruvia::detail::ScopedOperationScope scope;
        const ruvia::StopToken stopToken;
        const auto client = capabilities.clientRegistries().httpClient(
            "second", scope, stopToken);

        secondServer.start();
        auto operation = [&]() -> ruvia::Task<void> {
            auto response = co_await client.send({.target = "/second"});
            auto result = co_await response.body().readAll(3);
            RUVIA_CHECK_EQ(result.bytes().size(), std::size_t{3});
            co_await secondServer.wait();
            capabilities.closeNow();
            co_await capabilities.join();
        };
        runOperation(secondWorker, secondIo, operation);
    }
    firstResult.reset();
}

RUVIA_TEST(client_body_consumed_buffer_wakes_backpressured_producer_before_waiting_for_data) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        state.buffered.assign(1024, 'a');
        bool produced = false;
        bool watchdogNeeded = false;
        ruvia::TaskScope tasks(worker.handle, {.resource = &resource});
        auto producer = [&]() -> ruvia::Task<void> {
            co_await state.spaceSignal.wait();
            RUVIA_CHECK(state.buffered.empty());
            state.pending.assign("next");
            produced = true;
            state.complete = true;
            state.dataSignal.notify();
        };
        auto watchdog = [&]() -> ruvia::Task<void> {
            (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(100));
            if (!produced) {
                watchdogNeeded = true;
                state.spaceSignal.notify();
            }
        };
        tasks.spawn(producer());
        tasks.spawn(watchdog());
        const auto first = co_await state.read<std::string_view>();
        RUVIA_CHECK(first && first->size() == 1024 && first->front() == 'a');
        RUVIA_CHECK(!produced);  // Reading a view alone does not release it.
        const auto second = co_await state.read<std::string_view>();
        RUVIA_CHECK(second && *second == "next");
        co_await tasks.join();
        RUVIA_CHECK(produced && !watchdogNeeded);
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(client_body_collection_reclaims_temporaries_and_retains_results_and_headers) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    std::optional<ruvia::HttpClientResponseBytes> retained;
    ruvia::detail::Http3ClientBodyBudget receiveBudget(4096);
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        auto resultBudget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 4096});
        state.resultBudgetDomain = &resultBudget;
        const std::string payload(1024, '\xff');
        const std::string header(128, 'h');
        state.headers.push_back(ruvia::HttpHeader::copyOf("x-retained", header, &resource));
        state.buffered.assign(payload);
        state.pending.reserve(payload.size());
        RUVIA_CHECK(state.bindHttp3BodyBudget(receiveBudget));
        RUVIA_CHECK_EQ(receiveBudget.used(), payload.size());
        state.complete = true;
        auto operation = [&]() -> ruvia::Task<void> {
            const auto coldAllocationCount = resource.allocationCount();
            const auto coldBudget = resultBudget->retainedBytes();
            {
                auto cold = state.readAll(4096);
            }
            RUVIA_CHECK(!state.collectAll);
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), coldBudget);
            RUVIA_CHECK_EQ(receiveBudget.used(), payload.size());
            RUVIA_CHECK_EQ(resource.allocationCount(), coldAllocationCount);
            RUVIA_CHECK_EQ(std::string_view(state.buffered), payload);
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
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
            const auto baseline = resource.liveAllocations();
            const auto retainedBudgetBaseline = resultBudget->retainedBytes();
            for (int i = 0; i < 64; ++i) {
                state.offset = 0;
                state.buffered.assign(payload);
                state.pending.assign("\0\x80", 2);
                state.reconcileProducerBodyBytes();
                RUVIA_CHECK_EQ(receiveBudget.used(), payload.size() + 2);
                {
                    auto bytes = co_await state.readAll(4096);
                    const auto view = bytes.bytes();
                    RUVIA_CHECK_EQ(view.size(), payload.size() + 2);
                    RUVIA_CHECK(view[payload.size()] == std::byte{0});
                    RUVIA_CHECK(view.back() == std::byte{0x80});
                }
                RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
                RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
                RUVIA_CHECK_EQ(resultBudget->retainedBytes(), retainedBudgetBaseline);
                RUVIA_CHECK_EQ(state.headers.front().value(), std::string_view(header));
                RUVIA_CHECK_EQ(retained->size(), payload.size());
                RUVIA_CHECK(retained->bytes().front() == std::byte{0xff});
            }
            state.buffered.assign(payload);
            state.pending.assign("tail");
            state.offset = 0;
            state.reconcileProducerBodyBytes();
            RUVIA_CHECK_EQ(receiveBudget.used(), payload.size() + 4);
            const auto limitedBaseline = resource.liveAllocations();
            bool limited = false;
            try {
                (void)co_await state.readAll(1);
            } catch (const ruvia::HttpClientError& error) {
                limited = error.code() == ruvia::HttpClientError::Code::kResponseTooLarge;
            }
            RUVIA_CHECK(limited);
            RUVIA_CHECK_EQ(resource.liveAllocations(), limitedBaseline);
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), retainedBudgetBaseline);
            RUVIA_CHECK_EQ(std::string_view(state.buffered), payload);
            RUVIA_CHECK_EQ(std::string_view(state.pending), "tail");
            auto retried = co_await state.readAll(4096);
            const auto retriedBytes = retried.bytes();
            RUVIA_CHECK_EQ(retriedBytes.size(), payload.size() + 4);
            RUVIA_CHECK(retriedBytes.front() == std::byte{0xff});
            RUVIA_CHECK(retriedBytes[payload.size()] == std::byte{'t'});
            RUVIA_CHECK(retriedBytes.back() == std::byte{'l'});
            RUVIA_CHECK_EQ(std::string_view(state.buffered), "");
            RUVIA_CHECK_EQ(std::string_view(state.pending), "");

            state.buffered.assign("preserved on failure");
            state.reconcileProducerBodyBytes();
            state.failure = std::make_exception_ptr(std::runtime_error("transport failed"));
            bool failed = false;
            try {
                (void)co_await state.readAll(4096);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(std::string_view(state.buffered), "preserved on failure");
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
    RUVIA_CHECK(retained->bytes().back() == std::byte{0xff});
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    const auto workerAllocations = resource.allocationCount();
    const auto workerDeallocations = resource.deallocationCount();
    retained.reset();
    RUVIA_CHECK_EQ(resource.allocationCount(), workerAllocations);
    RUVIA_CHECK_EQ(resource.deallocationCount(), workerDeallocations);
}

RUVIA_TEST(client_body_result_survives_response_client_worker_and_cross_thread_destruction) {
    static_assert(!std::is_copy_constructible_v<ruvia::HttpClientResponseBytes>);
    static_assert(!std::is_copy_assignable_v<ruvia::HttpClientResponseBytes>);
    static_assert(std::is_nothrow_move_constructible_v<ruvia::HttpClientResponseBytes>);
    static_assert(std::is_nothrow_move_assignable_v<ruvia::HttpClientResponseBytes>);

    std::optional<ruvia::HttpClientResponseBytes> retained;
    std::weak_ptr<ruvia::detail::HttpClientResultBudgetDomain> budgetLifetime;
    std::string expected;
    expected.reserve(1024);
    expected.push_back('\0');
    expected.push_back(static_cast<char>(0xff));
    expected.append(1022, 'r');
    {
        auto& io = ruvia::test::newTestIoContext();
        {
            TestWorker worker(io);
            {
                ruvia::HttpClient client(worker.attachment.loop(), {.host = "example.test"},
                    {.maxRetainedBytes = 4096});
                {
                    ruvia::test::CountingMemoryResource resource;
                    auto resultBudget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
                        ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 4096});
                    budgetLifetime = resultBudget;
                    {
                        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
                        state.resultBudgetDomain = &resultBudget;
                        state.buffered.assign(512, 'a');
                        state.complete = true;
                        auto operation = [&]() -> ruvia::Task<void> {
                            auto first = co_await state.readAll(4096);
                            RUVIA_CHECK_EQ(first.size(), std::size_t{512});
                            state.buffered.assign(expected);
                            auto replacement = co_await state.readAll(4096);
                            const auto* const firstAddress = first.bytes().data();
                            retained.emplace(std::move(first));
                            RUVIA_CHECK(retained->bytes().data() == firstAddress);
                            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{512} + expected.size());
                            const auto* const replacementAddress = replacement.bytes().data();
                            *retained = std::move(replacement);
                            RUVIA_CHECK(retained->bytes().data() == replacementAddress);
                            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), expected.size());
                            RUVIA_CHECK(first.empty());
                            RUVIA_CHECK(replacement.empty());
                            RUVIA_CHECK_EQ(retained->bytes().size(), expected.size());
                            co_await client.shutdown();
                        };
                        runOperation(worker, io, operation);
                    }
                    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
                    RUVIA_CHECK(resource.allocationCount() > 0);
                    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
                }
            }
        }
    }

    RUVIA_CHECK(!budgetLifetime.expired());
    std::atomic_bool contentsMatched{false};
    std::thread destroyer([bytes = std::move(*retained), expected = std::move(expected),
                              &contentsMatched]() {
        const auto actual = bytes.bytes();
        const auto wanted = std::as_bytes(std::span(expected.data(), expected.size()));
        contentsMatched.store(actual.size() == wanted.size() &&
                                  std::equal(actual.begin(), actual.end(), wanted.begin()),
            std::memory_order_release);
    });
    destroyer.join();
    RUVIA_CHECK(contentsMatched.load(std::memory_order_acquire));
    RUVIA_CHECK(retained->empty());
    RUVIA_CHECK(budgetLifetime.expired());
}

RUVIA_TEST(client_body_result_uses_pool_budget_after_client_and_worker_teardown) {
    std::optional<ruvia::HttpClientResponseBytes> retained;
    std::string expected(
        "\0\xff"
        "data",
        6);
    {
        auto& io = ruvia::test::newTestIoContext();
        {
            TestWorker worker(io);
            asio::ip::tcp::acceptor acceptor(
                io, {asio::ip::tcp::v4(), std::uint16_t{0}});
            asio::ip::tcp::socket serverSocket(io);
            ruvia::WorkerSignal serverDone(worker.handle);
            std::exception_ptr serverFailure;
            const std::string responseWire =
                "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n" +
                expected;
            acceptor.async_accept(serverSocket, [&](const std::error_code& error) {
                if (error) {
                    serverFailure = std::make_exception_ptr(std::system_error(error));
                    serverDone.notify();
                    return;
                }
                asio::async_write(serverSocket, asio::buffer(responseWire),
                    [&](const std::error_code& writeError, std::size_t) {
                        if (writeError) {
                            serverFailure = std::make_exception_ptr(std::system_error(writeError));
                        }
                        serverDone.notify();
                    });
            });
            {
                ruvia::HttpClient client(worker.attachment.loop(),
                    {.scheme = ruvia::HttpScheme::kHttp,
                        .host = "127.0.0.1",
                        .port = acceptor.local_endpoint().port(),
                        .protocol = ruvia::HttpClientProtocol::kHttp1Only},
                    {.maxRetainedBytes = expected.size()});
                auto operation = [&]() -> ruvia::Task<void> {
                    auto send = client.send({.target = "/"});
                    auto response = co_await std::move(send);
                    auto bytes = co_await response.body().readAll();
                    retained.emplace(std::move(bytes));
                    co_await serverDone.wait();
                    if (serverFailure != nullptr) {
                        std::rethrow_exception(serverFailure);
                    }
                    RUVIA_CHECK_EQ(retained->bytes().size(), expected.size());
                    co_await client.shutdown();
                };
                runOperation(worker, io, operation);
            }
        }
    }

    std::atomic_bool contentsMatched{false};
    std::thread destroyer([bytes = std::move(*retained), expected = std::move(expected),
                              &contentsMatched]() {
        const auto actual = bytes.bytes();
        const auto wanted = std::as_bytes(std::span(expected.data(), expected.size()));
        contentsMatched.store(actual.size() == wanted.size() &&
                                  std::equal(actual.begin(), actual.end(), wanted.begin()),
            std::memory_order_release);
    });
    destroyer.join();
    RUVIA_CHECK(contentsMatched.load(std::memory_order_acquire));
}

RUVIA_TEST(client_body_result_budget_rejects_without_consuming_then_retries_after_release) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    {
        ruvia::detail::HttpClientResponseState state(worker.handle, &resource);
        auto resultBudget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 4});
        state.resultBudgetDomain = &resultBudget;
        state.buffered.assign("old");
        state.complete = true;
        auto operation = [&]() -> ruvia::Task<void> {
            std::optional<ruvia::HttpClientResponseBytes> oldResult;
            oldResult.emplace(co_await state.readAll(4));
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{3});

            state.buffered.assign("new");
            bool exhausted = false;
            try {
                (void)co_await state.readAll(4);
            } catch (const ruvia::HttpClientError& error) {
                exhausted = error.code() == ruvia::HttpClientError::Code::kResultBudgetExceeded;
            }
            RUVIA_CHECK(exhausted);
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{3});
            RUVIA_CHECK_EQ(std::string_view(state.buffered), "new");
            RUVIA_CHECK(std::string_view(state.pending).empty());

            oldResult.reset();
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{0});
            auto retry = co_await state.readAll(4);
            RUVIA_CHECK_EQ(retry.bytes().size(), std::size_t{3});
            RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{3});
        };
        runOperation(worker, io, operation);
        RUVIA_CHECK_EQ(resultBudget->retainedBytes(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(client_result_budget_validation_and_lease_exception_rollback) {
    bool rejectedZeroLimit = false;
    try {
        (void)std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
            ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 0});
    } catch (const std::invalid_argument&) {
        rejectedZeroLimit = true;
    }
    RUVIA_CHECK(rejectedZeroLimit);

    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    bool rejectedAtClientStartup = false;
    try {
        ruvia::HttpClient client(worker.attachment.loop(), {.host = "example.test"},
            {.maxRetainedBytes = 0});
    } catch (const std::invalid_argument&) {
        rejectedAtClientStartup = true;
    }
    RUVIA_CHECK(rejectedAtClientStartup);

    auto domain = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
        ruvia::HttpClientResultBudgetConfig{.maxRetainedBytes = 8});
    bool threw = false;
    try {
        auto reservation = ruvia::detail::HttpClientResultBudgetLease::tryAcquire(domain, 5);
        RUVIA_CHECK(reservation.has_value());
        throw std::bad_alloc{};
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(domain->retainedBytes(), std::size_t{0});
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
