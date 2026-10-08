#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/Bytes.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"

#include "http3/Http3SansIoSessionEngine.h"
#include "http3/Http3ServerBodyBudget.h"
#include "memory_resource_fixture.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class SwitchableCountingMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations(bool value = true) noexcept {
        rejecting_ = value;
    }

    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

    [[nodiscard]] std::size_t deallocationCount() const noexcept {
        return deallocationCount_;
    }

    [[nodiscard]] std::size_t liveAllocations() const noexcept {
        return liveAllocations_;
    }

    [[nodiscard]] std::size_t rejectedAllocationCount() const noexcept {
        return rejectedAllocationCount_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (rejecting_) {
            ++rejectedAllocationCount_;
            throw std::bad_alloc();
        }
        void* const storage = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocationCount_;
        ++liveAllocations_;
        return storage;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        --liveAllocations_;
        ++deallocationCount_;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
    std::size_t allocationCount_{0};
    std::size_t deallocationCount_{0};
    std::size_t liveAllocations_{0};
    std::size_t rejectedAllocationCount_{0};
};

std::string frame(std::uint64_t type, std::string_view payload) {
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto encoded = ruvia::encodeHttp3FrameHeader(header, type, payload.size());
    if (!encoded) {
        throw std::runtime_error("fixture frame header failed to encode");
    }
    std::string result(header.data(), *encoded);
    result.append(payload);
    return result;
}

std::string requestHeaders(ruvia::WorkerMemory& worker, std::string_view method,
    std::string_view path, std::optional<std::uint64_t> bodyLength = std::nullopt,
    std::span<const ruvia::Http3FieldSectionFieldView> fields = {},
    std::string_view protocol = {}) {
    auto head = ruvia::encodeHttp3ClientRequestHead({.method = method,
                                                        .scheme = "https",
                                                        .authority = "example.test",
                                                        .path = path,
                                                        .fields = fields,
                                                        .bodyLength = bodyLength,
                                                        .protocol = protocol,
                                                        .peerEnableConnectProtocol = !protocol.empty()},
        {}, worker.resource());
    if (!head) {
        throw std::runtime_error("fixture request head failed to encode");
    }
    return frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(head->fieldSection.data(), head->fieldSection.size()));
}

std::string data(std::string_view payload) {
    return frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), payload);
}

}  // namespace

RUVIA_TEST(http3_session_routes_extension_method_by_exact_token) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    implementation.registerExtensionMethodRoute("PROPFIND",
        std::pmr::string("/dav", std::pmr::get_default_resource()),
        ruvia::detail::RouteHandler(nullptr, &routing_test::dummyHandler),
        ruvia::detail::RequestBodyMode::kBuffered,
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3SansIoSessionEngine session(implementation.routeTable(), worker);
    const auto head = requestHeaders(worker, "PROPFIND", "/dav");
    RUVIA_CHECK(session.feed(0, head, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);

    const auto* request = session.request(0);
    const auto* resolution = session.resolution(0);
    RUVIA_CHECK(request != nullptr && request->request().method() == "PROPFIND");
    RUVIA_CHECK(resolution != nullptr && resolution->resolved() != nullptr);
    if (resolution != nullptr && resolution->resolved() != nullptr) {
        RUVIA_CHECK(resolution->resolved()->route().path() == "/dav");
    }
}

RUVIA_TEST(http3_session_maps_websocket_connect_to_websocket_route) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    implementation.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/socket", std::pmr::get_default_resource()),
        ruvia::detail::RouteStreamHandler(nullptr, &routing_test::dummyStreamHandler),
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
        std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3SansIoSessionEngine session(implementation.routeTable(), worker);
    const auto head = requestHeaders(worker, "CONNECT", "/socket", std::nullopt, {}, "websocket");
    RUVIA_CHECK(session.feed(0, head, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);

    const auto* request = session.request(0);
    const auto* resolution = session.resolution(0);
    RUVIA_CHECK(request != nullptr && request->request().method() == "CONNECT");
    RUVIA_CHECK(request != nullptr && request->extendedConnectProtocol() == "websocket");
    RUVIA_CHECK(resolution != nullptr && resolution->resolved() != nullptr);
    if (resolution != nullptr && resolution->resolved() != nullptr) {
        RUVIA_CHECK(resolution->resolved()->route().endpoint().webSocket() != nullptr);
    }
}

RUVIA_TEST(http3_server_priority_updates_remain_live_through_request_lease) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kGet, "/priority");
    implementation.finalize();
    ruvia::WorkerMemory worker;
    ruvia::detail::Http3SansIoSessionEngine session(implementation.routeTable(), worker);
    auto prefixes = ruvia::Http3LocalCriticalStreams::create({});
    const auto prefix = prefixes->controlPrefix();
    RUVIA_CHECK(session.feed(2, std::string_view(prefix.data(), prefix.size())).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(session.feed(0, requestHeaders(worker, "GET", "/priority"), true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
    auto lease = session.acquireRequest(0);
    RUVIA_CHECK(lease.has_value());
    if (!lease) {
        return;
    }
    const auto* observed = session.requestPriorityUpdate(0);
    RUVIA_CHECK(observed && !*observed);
    std::array<char, 32> bytes{};
    for (const auto urgency : {1, 7, 2}) {
        const auto encoded = ruvia::encodeHttp3PriorityUpdate(bytes, {.elementId = 0,
                                                                         .fields = {.urgency = static_cast<std::uint8_t>(urgency), .incremental = urgency == 7}});
        RUVIA_CHECK(encoded.has_value());
        if (!encoded) {
            return;
        }
        RUVIA_CHECK(session.feed(2, std::string_view(bytes.data(), *encoded)).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(observed == session.requestPriorityUpdate(0));
        RUVIA_CHECK(observed->has_value() && observed->value().urgency == urgency && observed->value().incremental == (urgency == 7));
    }
    lease.reset();
    RUVIA_CHECK(session.release(0));
    RUVIA_CHECK(!session.requestPriorityUpdate(0));
}

RUVIA_TEST(http3BufferedSansIoSessionLeasePinsRequestsAcrossResetAndStopWithoutAllocation) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();
    ruvia::test::CountingMemoryResource allocations;
    {
        ruvia::WorkerMemory worker(allocations);
        Engine session(implementation.routeTable(), worker);
        const std::string payload(64 * 1024, 'p');
        const auto wire = data(std::string_view(payload));
        const std::array fields{ruvia::Http3FieldSectionFieldView{"x-lease", "retained"}};
        const auto head = requestHeaders(worker, "POST", "/items", payload.size(), fields);
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(!session.acquireRequest(0));
        RUVIA_CHECK(session.feed(0, wire, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
        const auto beforeAcquire = allocations.allocationCount();
        auto initial = session.acquireRequest(0);
        RUVIA_CHECK(initial.has_value());
        if (!initial) {
            return;
        }
        std::optional<Engine::RequestLease> held(std::in_place, std::move(*initial));
        initial.reset();
        RUVIA_CHECK_EQ(allocations.allocationCount(), beforeAcquire);
        RUVIA_CHECK(!session.acquireRequest(0) && !session.release(0));
        RUVIA_CHECK(held->resolution().resolved() != nullptr);
        std::optional<std::size_t> cacheBaseline;
        for (std::uint64_t step = 1; step <= 32; ++step) {
            const auto id = step * 4;
            RUVIA_CHECK(session.feed(id, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id, wire, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
            auto lease = session.acquireRequest(id);
            RUVIA_CHECK(lease.has_value());
            if (!lease) {
                return;
            }
            const auto withResult = allocations.liveAllocations();
            if (step % 3 != 0) {
                if (step % 3 == 1) {
                    (void)session.feed(id, {}, false, true);
                } else {
                    RUVIA_CHECK(session.cancelRequest(id));
                    RUVIA_CHECK(!session.cancelRequest(id));
                }
                RUVIA_CHECK(session.request(id) != nullptr);
                RUVIA_CHECK(lease->request().bodyBytes() == payload.size());
                lease.reset();
                RUVIA_CHECK(session.request(id) == nullptr);
            } else {
                lease.reset();
                RUVIA_CHECK(!session.acquireRequest(id));
                RUVIA_CHECK(session.release(id));
            }
            // The worker pool may retain blocks for later requests.
            RUVIA_CHECK(allocations.liveAllocations() <= withResult);
            if (cacheBaseline) {
                RUVIA_CHECK_EQ(allocations.liveAllocations(), *cacheBaseline);
            } else {
                cacheBaseline = allocations.liveAllocations();
            }
            const auto bytes = held->request().request().bodyBytes();
            RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == payload);
            RUVIA_CHECK(held->request().request().header("x-lease") == "retained");
        }
        const auto invalid = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kSettings), {});
        const auto failure = session.feed(132, invalid);
        RUVIA_CHECK(failure.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        session.stop();
        RUVIA_CHECK(session.feed(0, {}).code == failure.code);
        RUVIA_CHECK(session.terminated() && session.activeStreamCount() == 1);
        RUVIA_CHECK(!session.acquireRequest(0) && !session.release(0));
        RUVIA_CHECK(held->request().bodyComplete() && held->request().bodyBytes() == payload.size());
        const auto beforeReturn = allocations.liveAllocations();
        held.reset();
        RUVIA_CHECK_EQ(session.activeStreamCount(), std::size_t{0});
        RUVIA_CHECK(allocations.liveAllocations() <= beforeReturn);
    }
    RUVIA_CHECK_EQ(allocations.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(allocations.allocationCount(), allocations.deallocationCount());
}

RUVIA_TEST(http3BufferedSansIoSessionCancelsIncompleteHeadersAndBodiesWithoutDisturbingLease) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();
    ruvia::test::CountingMemoryResource allocations;
    {
        ruvia::WorkerMemory worker(allocations);
        Engine session(implementation.routeTable(), worker, {.maxLiveStreams = 2});
        const auto head = requestHeaders(worker, "POST", "/items");
        (void)session.feed(0, head);
        (void)session.feed(0, data("held"), true);
        auto held = session.acquireRequest(0);
        RUVIA_CHECK(held.has_value());
        if (!held) {
            return;
        }
        const auto wire = data(std::string_view(std::string(65536, 'p')));
        RUVIA_CHECK(!session.cancelRequest(2) && !session.cancelRequest(4));
        std::optional<std::size_t> cached;
        for (std::uint64_t step = 1; step <= 16; ++step) {
            const auto id = step * 8;
            RUVIA_CHECK(session.feed(id, std::string_view(head).substr(0, 3)).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.request(id) == nullptr);
            RUVIA_CHECK(session.cancelRequest(id));
            RUVIA_CHECK(!session.cancelRequest(id));
            RUVIA_CHECK(session.feed(id + 4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id + 4, wire).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(!session.acquireRequest(id + 4));
            const auto beforeCancel = allocations.liveAllocations();
            RUVIA_CHECK(session.cancelRequest(id + 4));
            RUVIA_CHECK(!session.cancelRequest(id + 4));
            RUVIA_CHECK(allocations.liveAllocations() <= beforeCancel);
            if (cached) {
                RUVIA_CHECK_EQ(allocations.liveAllocations(), *cached);
            } else {
                cached = allocations.liveAllocations();
            }
            RUVIA_CHECK_EQ(session.activeStreamCount(), std::size_t{1});
            const auto body = held->request().request().bodyBytes();
            RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) == "held");
        }
        RUVIA_CHECK(session.cancelRequest(0));
        RUVIA_CHECK(!session.cancelRequest(0));
        held.reset();
        RUVIA_CHECK_EQ(session.activeStreamCount(), std::size_t{0});
        session.stop();
        RUVIA_CHECK(!session.cancelRequest(0));
    }
    RUVIA_CHECK_EQ(allocations.allocationCount(), allocations.deallocationCount());
}

RUVIA_TEST(http3BufferedSansIoSessionStopReleasesPartialProtocolHeadWhileLeaseSurvives) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();
    ruvia::test::CountingMemoryResource allocations;
    {
        ruvia::WorkerMemory worker(allocations);
        Engine session(implementation.routeTable(), worker);
        const auto head = requestHeaders(worker, "POST", "/items");
        (void)session.feed(0, head);
        (void)session.feed(0, data("held"), true);
        auto lease = session.acquireRequest(0);
        RUVIA_CHECK(lease.has_value());
        if (!lease) {
            return;
        }
        const std::string value(60000, 'z');
        const std::array fields{ruvia::Http3FieldSectionFieldView{"x-large", value}};
        const auto large = requestHeaders(worker, "POST", "/items", std::nullopt, fields);
        RUVIA_CHECK(session.feed(4, std::string_view(large).substr(0, large.size() / 2)).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.request(4) == nullptr);
        const auto beforeStop = allocations.liveAllocations();
        session.stop();
        RUVIA_CHECK(allocations.liveAllocations() <= beforeStop);
        RUVIA_CHECK_EQ(session.activeStreamCount(), std::size_t{1});
        const auto body = lease->request().request().bodyBytes();
        RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) == "held");
        lease.reset();
        RUVIA_CHECK_EQ(session.activeStreamCount(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(allocations.allocationCount(), allocations.deallocationCount());
}

RUVIA_TEST(http3BufferedSansIoSessionRetiredLeaseKeepsItsBodyBudget) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();
    ruvia::WorkerMemory worker;
    Engine session(implementation.routeTable(), worker,
        {.max_buffered_body_bytes = 8, .maxBufferedBytesInFlight = 6});
    const auto head = requestHeaders(worker, "POST", "/items");
    (void)session.feed(0, head);
    (void)session.feed(0, data("abc"), true);
    auto lease = session.acquireRequest(0);
    RUVIA_CHECK(lease.has_value());
    if (!lease) {
        return;
    }
    (void)session.feed(0, {}, false, true);
    RUVIA_CHECK(!session.acquireRequest(0));
    (void)session.feed(4, head);
    (void)session.feed(4, data("abcd"), true);
    RUVIA_CHECK(session.rejection(4) == Engine::Rejection::kInFlightBodyCapacity);
    RUVIA_CHECK(session.release(4));
    lease.reset();
    RUVIA_CHECK(session.request(0) == nullptr);
    (void)session.feed(8, head);
    (void)session.feed(8, data("abcd"), true);
    RUVIA_CHECK(session.streamState(8) == Engine::StreamState::kReady);
    RUVIA_CHECK(session.release(8));
}

RUVIA_TEST(http3BufferedSansIoSessionsShareWorkerBodyBudgetAcrossConnections) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3ServerBodyBudget budget(6);
    const ruvia::detail::Http3SansIoSessionLimits limits{.max_buffered_body_bytes = 16,
        .maxLiveStreams = 4,
        .maxBufferedBytesInFlight = 16};
    Engine first(implementation.routeTable(), worker, budget, limits);
    Engine second(implementation.routeTable(), worker, budget, limits);
    const auto head = requestHeaders(worker, "POST", "/items");

    RUVIA_CHECK(first.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(first.feed(0, data("abcd"), true).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(budget.used(), 4U);
    RUVIA_CHECK_EQ(budget.available(), 2U);

    RUVIA_CHECK(second.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    const auto exhausted = second.feed(0, data("xyz"), true);
    RUVIA_CHECK(exhausted.scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(second.rejection(0) == Engine::Rejection::kWorkerBodyBudgetExhausted);
    RUVIA_CHECK(second.request(0) != nullptr && second.request(0)->bodyBytes() == 0);
    RUVIA_CHECK_EQ(budget.used(), 4U);
    RUVIA_CHECK(second.release(0));

    RUVIA_CHECK(first.release(0));
    RUVIA_CHECK_EQ(budget.used(), 0U);
    RUVIA_CHECK(second.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(second.feed(4, data("xyz"), true).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(second.request(4) != nullptr && second.request(4)->bodyBytes() == 3);
    RUVIA_CHECK_EQ(budget.used(), 3U);
    RUVIA_CHECK(second.release(4));
    RUVIA_CHECK_EQ(budget.used(), 0U);
}

RUVIA_TEST(http3BufferedSansIoSessionsKeepPerConnectionBodyLimitIndependent) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3ServerBodyBudget budget(32);
    const ruvia::detail::Http3SansIoSessionLimits limits{.max_buffered_body_bytes = 16,
        .maxLiveStreams = 4,
        .maxBufferedBytesInFlight = 3};
    Engine first(implementation.routeTable(), worker, budget, limits);
    Engine second(implementation.routeTable(), worker, budget, limits);
    const auto head = requestHeaders(worker, "POST", "/items");

    RUVIA_CHECK(first.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(first.feed(0, data("ab"), true).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(second.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(second.feed(0, data("cd"), true).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(budget.used(), 4U);

    RUVIA_CHECK(first.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(first.feed(4, data("ef"), true).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(first.rejection(4) == Engine::Rejection::kInFlightBodyCapacity);
    RUVIA_CHECK(first.request(4) != nullptr && first.request(4)->bodyBytes() == 0);
    RUVIA_CHECK_EQ(budget.used(), 4U);
    RUVIA_CHECK(first.release(4));
    RUVIA_CHECK(first.release(0));
    RUVIA_CHECK_EQ(budget.used(), 2U);
    RUVIA_CHECK(second.release(0));
    RUVIA_CHECK_EQ(budget.used(), 0U);
}

RUVIA_TEST(http3BufferedSansIoSessionResetAndStopKeepLeasedBodiesReserved) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3ServerBodyBudget budget(8);
    const ruvia::detail::Http3SansIoSessionLimits limits{.max_buffered_body_bytes = 8,
        .maxLiveStreams = 4,
        .maxBufferedBytesInFlight = 8};
    Engine resetSession(implementation.routeTable(), worker, budget, limits);
    Engine stoppedSession(implementation.routeTable(), worker, budget, limits);
    Engine probe(implementation.routeTable(), worker, budget, limits);
    const auto head = requestHeaders(worker, "POST", "/items");

    RUVIA_CHECK(resetSession.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(resetSession.feed(0, data("held"), true).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    auto resetLease = resetSession.acquireRequest(0);
    RUVIA_CHECK(resetLease.has_value());
    if (!resetLease) {
        return;
    }
    RUVIA_CHECK(resetSession.feed(0, {}, false, true).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(!resetSession.acquireRequest(0).has_value());
    RUVIA_CHECK_EQ(budget.used(), 4U);

    RUVIA_CHECK(stoppedSession.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(stoppedSession.feed(0, data("stay"), true).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    auto stoppedLease = stoppedSession.acquireRequest(0);
    RUVIA_CHECK(stoppedLease.has_value());
    if (!stoppedLease) {
        return;
    }
    stoppedSession.stop();
    RUVIA_CHECK_EQ(budget.used(), 8U);
    RUVIA_CHECK_EQ(resetLease->request().request().bodyBytes().size(), 4U);
    RUVIA_CHECK_EQ(stoppedLease->request().request().bodyBytes().size(), 4U);

    RUVIA_CHECK(probe.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    const auto exhausted = probe.feed(0, data("x"), true);
    RUVIA_CHECK(exhausted.scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(probe.rejection(0) == Engine::Rejection::kWorkerBodyBudgetExhausted);
    RUVIA_CHECK_EQ(budget.used(), 8U);
    RUVIA_CHECK(probe.release(0));

    resetLease.reset();
    RUVIA_CHECK_EQ(budget.used(), 4U);
    RUVIA_CHECK(probe.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(probe.feed(4, data("four"), true).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(budget.used(), 8U);
    RUVIA_CHECK(probe.release(4));
    RUVIA_CHECK_EQ(budget.used(), 4U);
    const auto retained = stoppedLease->request().request().bodyBytes();
    RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(retained.data()), retained.size()) ==
                "stay");
    stoppedLease.reset();
    RUVIA_CHECK_EQ(budget.used(), 0U);
}

RUVIA_TEST(http3BufferedSansIoSessionReturnsSharedBudgetOnRepeatedRejectCancelAndRelease) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::test::CountingMemoryResource allocations;
    {
        ruvia::WorkerMemory worker(allocations);
        ruvia::detail::Http3ServerBodyBudget budget(11);
        Engine session(implementation.routeTable(), worker, budget,
            {.max_buffered_body_bytes = 16, .maxLiveStreams = 4, .maxBufferedBytesInFlight = 20});
        const auto head = requestHeaders(worker, "POST", "/items");
        for (std::uint64_t step = 1; step <= 8; ++step) {
            const auto id = step * 16;
            RUVIA_CHECK(session.feed(id, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id, data("ab")).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK_EQ(budget.used(), 2U);
            RUVIA_CHECK(session.feed(id, data("1234567890")).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.rejection(id) == Engine::Rejection::kWorkerBodyBudgetExhausted);
            RUVIA_CHECK(session.request(id) != nullptr && session.request(id)->bodyBytes() == 0);
            RUVIA_CHECK_EQ(budget.used(), 0U);
            RUVIA_CHECK(session.feed(id, {}, true).status ==
                        ruvia::Http3ConnectionStatus::kMessageEnd);
            RUVIA_CHECK(session.release(id));
            RUVIA_CHECK_EQ(budget.used(), 0U);

            RUVIA_CHECK(session.feed(id + 4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id + 4, data("ok"), true).status ==
                        ruvia::Http3ConnectionStatus::kMessageEnd);
            RUVIA_CHECK_EQ(budget.used(), 2U);
            RUVIA_CHECK(session.release(id + 4));
            RUVIA_CHECK_EQ(budget.used(), 0U);

            RUVIA_CHECK(session.feed(id + 8, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id + 8, data("abc")).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK_EQ(budget.used(), 3U);
            RUVIA_CHECK(session.cancelRequest(id + 8));
            RUVIA_CHECK(!session.cancelRequest(id + 8));
            RUVIA_CHECK_EQ(budget.used(), 0U);
        }
        RUVIA_CHECK_EQ(session.activeStreamCount(), 0U);
        RUVIA_CHECK_EQ(budget.used(), 0U);
        {
            Engine abandoned(implementation.routeTable(), worker, budget,
                {.max_buffered_body_bytes = 16,
                    .maxLiveStreams = 4,
                    .maxBufferedBytesInFlight = 20});
            RUVIA_CHECK(abandoned.feed(1000, head).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(abandoned.feed(1000, data("held")).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK_EQ(budget.used(), 4U);
        }
        RUVIA_CHECK_EQ(budget.used(), 0U);
    }
    RUVIA_CHECK_EQ(allocations.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(allocations.allocationCount(), allocations.deallocationCount());
}

RUVIA_TEST(http3BufferedSansIoSessionRollsBackSharedReservationWhenAppendThrows) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    SwitchableCountingMemoryResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        ruvia::detail::Http3ServerBodyBudget budget(256 * 1024);
        Engine session(implementation.routeTable(), worker, budget,
            {.max_buffered_body_bytes = 256 * 1024,
                .maxLiveStreams = 4,
                .maxBufferedBytesInFlight = 256 * 1024});
        const auto head = requestHeaders(worker, "POST", "/items");
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, data("seed")).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK_EQ(budget.used(), 4U);
        const std::string payload(128 * 1024, 'b');
        const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), payload);
        upstream.rejectAllocations();
        const auto failure = session.feed(0, wire, true);
        RUVIA_CHECK(upstream.rejectedAllocationCount() != 0);
        RUVIA_CHECK(failure.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(failure.code == ruvia::Http3ConnectionErrorCode::kInternalError);
        RUVIA_CHECK(session.terminated());
        RUVIA_CHECK_EQ(budget.used(), 0U);
        RUVIA_CHECK_EQ(session.activeStreamCount(), 0U);
        upstream.rejectAllocations(false);
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3BufferedSansIoSessionHandlerLeaseSurvivesSuspensionAndUnwindsAfterJoin) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto workerHandle = attachment.loop().handle();
    ruvia::test::CountingMemoryResource allocations;
    {
        ruvia::WorkerMemory worker(allocations);
        std::exception_ptr failure;
        auto operation = [&]() -> ruvia::Task<void> {
            try {
                const std::string payload(65536, 'p');
                const auto wire = data(std::string_view(payload));
                const auto head = requestHeaders(worker, "POST", "/items", payload.size());
                for (unsigned scenario = 0; scenario < 3; ++scenario) {
                    Engine session(implementation.routeTable(), worker);
                    ruvia::WorkerSignal started(workerHandle), resume(workerHandle);
                    RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
                    RUVIA_CHECK(session.feed(0, wire, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
                    auto lease = session.acquireRequest(0);
                    if (!lease) {
                        throw std::runtime_error("request lease unavailable");
                    }
                    bool ran = false;
                    bool resumed = false;
                    auto handler = [&](Engine::RequestLease parameter) -> ruvia::Task<void> {
                        auto request = std::move(parameter);
                        ran = true;
                        started.notify();
                        co_await resume.wait();
                        const auto bytes = request.request().request().bodyBytes();
                        RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == payload);
                        resumed = true;
                        if (scenario == 2) {
                            throw std::runtime_error("handler failed");
                        }
                    };
                    if (scenario == 0) {
                        {
                            auto cold = handler(std::move(*lease));
                        }
                        RUVIA_CHECK(!ran);
                        RUVIA_CHECK(session.release(0));
                        continue;
                    }
                    ruvia::TaskScope tasks(workerHandle, {.resource = worker.resource()});
                    bool completed = false;
                    auto watchdog = [&]() -> ruvia::Task<void> {
                        const auto result = co_await ruvia::sleepFor(workerHandle,
                            std::chrono::milliseconds(500), tasks.stopToken());
                        if (!completed && result == ruvia::TimerSleepResult::kElapsed) {
                            RUVIA_CHECK(false);
                            session.stop();
                            started.notify();
                            resume.notify();
                        }
                    };
                    tasks.spawn(handler(std::move(*lease)));
                    tasks.spawn(watchdog());
                    co_await started.wait();
                    const auto beforeStop = allocations.liveAllocations();
                    if (scenario == 1) {
                        RUVIA_CHECK(session.cancelRequest(0));
                        RUVIA_CHECK(!session.terminated());
                    } else {
                        session.stop();
                    }
                    RUVIA_CHECK(ran && !resumed && session.activeStreamCount() == 1);
                    RUVIA_CHECK_EQ(allocations.liveAllocations(), beforeStop);
                    resume.notify();
                    completed = true;
                    tasks.requestStop();
                    bool handlerFailed = false;
                    try {
                        co_await tasks.join();
                    } catch (const std::runtime_error&) {
                        handlerFailed = true;
                    }
                    RUVIA_CHECK(resumed && handlerFailed == (scenario == 2));
                    RUVIA_CHECK_EQ(session.activeStreamCount(), std::size_t{0});
                    RUVIA_CHECK(allocations.liveAllocations() <= beforeStop);
                }
            } catch (...) {
                failure = std::current_exception();
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(operation());
        attachment.run();
        root.get();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    RUVIA_CHECK_EQ(allocations.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(allocations.allocationCount(), allocations.deallocationCount());
}

RUVIA_TEST(http3BufferedSansIoSessionStagesIndependentStreamsAndCopiesRequests) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3SansIoSessionEngine session(
        implementation.routeTable(), worker, {.max_buffered_body_bytes = 16});
    const auto headA = requestHeaders(worker, "POST", "/items", 3);
    const auto headB = requestHeaders(worker, "POST", "/missing");
    RUVIA_CHECK(session.feed(0, headA).status == ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK(session.feed(4, headB).status == ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK(session.activeStreamCount() == 2);
    RUVIA_CHECK(session.streamState(0) ==
                ruvia::detail::Http3SansIoSessionEngine::StreamState::kReceiving);
    RUVIA_CHECK(session.resolution(0) != nullptr && session.resolution(0)->resolved() != nullptr);
    RUVIA_CHECK(session.resolution(4) != nullptr && session.resolution(4)->notFound() != nullptr);

    const auto body = data("abc");
    RUVIA_CHECK(session.feed(0, body, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(session.streamState(0) ==
                ruvia::detail::Http3SansIoSessionEngine::StreamState::kReady);
    const auto* request = session.request(0);
    RUVIA_CHECK(request != nullptr && request->bodyComplete());
    RUVIA_CHECK(request != nullptr && request->request().bodyBytes().size() == 3);
    RUVIA_CHECK(session.streamState(4) ==
                ruvia::detail::Http3SansIoSessionEngine::StreamState::kReceiving);
    RUVIA_CHECK(!session.release(4));
    RUVIA_CHECK(session.release(0));
    (void)session.feed(4, {}, false, true);
    RUVIA_CHECK(session.request(4) == nullptr);
    RUVIA_CHECK(session.activeStreamCount() == 0);
}

RUVIA_TEST(http3BufferedSansIoSessionDropsFailedStreamWithoutTerminatingPeers) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3SansIoSessionEngine session(
        implementation.routeTable(), worker, {.max_buffered_body_bytes = 16});
    const auto badHead = requestHeaders(worker, "POST", "/items", 1);
    const auto goodHead = requestHeaders(worker, "POST", "/items");
    RUVIA_CHECK(session.feed(0, badHead).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(session.feed(4, goodHead).scope == ruvia::Http3ConnectionErrorScope::kNone);
    const auto tooLong = data("ab");
    const auto failure = session.feed(0, tooLong);
    RUVIA_CHECK(failure.scope == ruvia::Http3ConnectionErrorScope::kStream);
    RUVIA_CHECK(session.request(0) == nullptr);
    RUVIA_CHECK_EQ(session.activeStreamCount(), 1U);
    RUVIA_CHECK(!session.terminated());
    const auto remaining = data("ok");
    RUVIA_CHECK(session.feed(4, remaining, true).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(session.request(4) != nullptr && session.request(4)->bodyComplete());
}

RUVIA_TEST(http3BufferedSansIoSessionAppliesStrictBodyAndExpectationRejectionsEarly) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3SansIoSessionEngine session(
        implementation.routeTable(), worker, {.max_buffered_body_bytes = 3});
    const auto head = requestHeaders(worker, "POST", "/items");
    RUVIA_CHECK(session.feed(0, head).status == ruvia::Http3ConnectionStatus::kNeedMoreData);
    const auto first = data("ab");
    const auto second = data("cd");
    RUVIA_CHECK(session.feed(0, first).status == ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK(session.feed(0, second).status == ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK(session.rejection(0) ==
                ruvia::detail::Http3SansIoSessionEngine::Rejection::kBodyTooLarge);

    const std::array expect{ruvia::Http3FieldSectionFieldView{"expect", "custom-expectation"}};
    const auto expectHead = requestHeaders(worker, "POST", "/items", std::nullopt, expect);
    RUVIA_CHECK(session.feed(4, expectHead).status == ruvia::Http3ConnectionStatus::kNeedMoreData);
    RUVIA_CHECK(session.rejection(4) ==
                ruvia::detail::Http3SansIoSessionEngine::Rejection::kExpectationUnsupported);
    session.stop();
    RUVIA_CHECK(session.terminated());
    RUVIA_CHECK(session.activeStreamCount() == 0);
}

RUVIA_TEST(http3BufferedSansIoSessionBoundsLiveRuntimesAfterRequestFin) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3SansIoSessionEngine session(implementation.routeTable(), worker,
        {.max_buffered_body_bytes = 4, .maxLiveStreams = 1, .maxBufferedBytesInFlight = 8});
    const auto head = requestHeaders(worker, "POST", "/items");
    RUVIA_CHECK(session.feed(0, head, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(session.streamState(0) ==
                ruvia::detail::Http3SansIoSessionEngine::StreamState::kReady);
    RUVIA_CHECK_EQ(session.activeStreamCount(), 1U);
    const auto excess = session.feed(4, head, true);
    RUVIA_CHECK(excess.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(excess.code == ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
    RUVIA_CHECK(session.terminated());
    RUVIA_CHECK_EQ(session.activeStreamCount(), 0U);
    RUVIA_CHECK(session.feed(8, head).code == ruvia::Http3ConnectionErrorCode::kExcessiveLoad);
}

RUVIA_TEST(http3BufferedSansIoSessionReleasesAggregateBodyBudgetOnRejectionAndRetirement) {
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();

    ruvia::WorkerMemory worker;
    ruvia::detail::Http3SansIoSessionEngine session(implementation.routeTable(), worker,
        {.max_buffered_body_bytes = 4, .maxLiveStreams = 3, .maxBufferedBytesInFlight = 5});
    const auto head = requestHeaders(worker, "POST", "/items");
    RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    const auto four = data("abcd");
    RUVIA_CHECK(session.feed(0, four, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(session.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    const auto one = data("x");
    const auto two = data("yz");
    RUVIA_CHECK(session.feed(4, one).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(session.feed(4, two).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(session.rejection(4) ==
                ruvia::detail::Http3SansIoSessionEngine::Rejection::kInFlightBodyCapacity);
    RUVIA_CHECK(session.request(4) != nullptr && session.request(4)->bodyBytes() == 0);
    RUVIA_CHECK(!session.release(4));
    RUVIA_CHECK(session.release(0));
    RUVIA_CHECK(session.feed(8, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(session.feed(8, two, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(session.request(8) != nullptr && session.request(8)->bodyBytes() == 2);
    RUVIA_CHECK(session.feed(4, {}, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(session.release(4));
    RUVIA_CHECK_EQ(session.activeStreamCount(), 1U);
    RUVIA_CHECK(!session.terminated());
}

RUVIA_TEST(http3_body_allocations_share_worker_budget_and_remain_pinned_by_lease) {
    using Engine = ruvia::detail::Http3SansIoSessionEngine;
    ruvia::detail::Router router;
    auto& implementation = ruvia::detail::RouterImpl::from(router);
    routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
    implementation.finalize();
    ruvia::WorkerMemory worker;
    ruvia::detail::inbound_buffer_resource shared(worker.resource(), 1500);
    Engine first(implementation.routeTable(), worker,
        {.inbound_buffer_pool = &shared, .max_inbound_buffer_bytes = 1200});
    Engine second(implementation.routeTable(), worker,
        {.inbound_buffer_pool = &shared, .max_inbound_buffer_bytes = 1200});
    const auto head = requestHeaders(worker, "POST", "/items");
    const std::string payload(1024, 'a');
    const auto chunk = data(std::string_view(payload));
    RUVIA_CHECK(first.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    const auto first_result = first.feed(0, chunk, true);
    RUVIA_CHECK(first_result.status == ruvia::Http3ConnectionStatus::kMessageEnd);
    auto lease = first.acquireRequest(0);
    RUVIA_CHECK(lease.has_value());
    if (!lease) {
        return;
    }
    RUVIA_CHECK(shared.used() >= 1024);
    const auto held = shared.used();
    first.stop();
    RUVIA_CHECK_EQ(shared.used(), held);
    RUVIA_CHECK_EQ(lease->request().request().bodyBytes().size(), std::size_t{1024});
    RUVIA_CHECK(second.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    const auto second_empty_bytes = shared.used() - held;
    RUVIA_CHECK(second.feed(0, chunk, true).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(second.rejection(0) == Engine::Rejection::kWorkerBodyBudgetExhausted);
    RUVIA_CHECK(!second.terminated());
    RUVIA_CHECK_EQ(shared.used(), held + second_empty_bytes);
    RUVIA_CHECK(second.request(0) != nullptr && second.request(0)->bodyBytes() == 0);
    RUVIA_CHECK_EQ(ruvia::asChars(lease->request().request().bodyBytes()), std::string_view(payload));
    lease.reset();
    // The rejected stream keeps its empty containers until session retirement.
    RUVIA_CHECK_EQ(shared.used(), second_empty_bytes);
    RUVIA_CHECK(second.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(second.feed(4, chunk, true).status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(shared.used() >= 1024);
    second.stop();
    RUVIA_CHECK_EQ(shared.used(), std::size_t{0});
}
