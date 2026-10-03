#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/web/detail/http3/Http3SansIoSessionEngine.h"
#include "ruvia/web/detail/http3/Http3ServerStreamInput.h"
#include "ruvia/web/detail/http3/Http3StreamMailbox.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"

#include "memory_resource_fixture.h"
#include "routing_fixture.h"
#include "test_harness.h"

namespace {

using Input = ruvia::detail::Http3ServerStreamInput;
using Mailbox = ruvia::detail::Http3StreamMailbox;
using MessageId = ruvia::detail::Http3StreamMessageId;
using Control = ruvia::detail::Http3StreamControl;
using Engine = ruvia::detail::Http3SansIoSessionEngine;

constexpr std::uint64_t kEpoch = 17;
constexpr std::uint64_t kGeneration = 29;

struct Routes final {
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& implementation{ruvia::detail::RouterImpl::from(router)};

    Routes() {
        routing_test::addRoute(implementation, ruvia::HttpKnownMethod::kPost, "/items");
        implementation.finalize();
    }
};

struct Fixture final {
    Routes routes;
    ruvia::WorkerMemory worker;
    Engine session;
    Mailbox mailbox;
    Input input;

    explicit Fixture(std::size_t capacity = 16, ruvia::Http3ConnectionConfig connection = {.enableConnectProtocol = true})
        : routes(),
          worker(),
          session(routes.implementation.routeTable(), worker, {.connection = connection}),
          mailbox(16, 16, 8),
          input(session, worker, kEpoch, kGeneration, capacity) {}
};

std::string frame(std::uint64_t type, std::string_view payload) {
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto encoded = ruvia::encodeHttp3FrameHeader(header, type, payload.size());
    if (!encoded) {
        throw std::runtime_error("HTTP/3 test frame header encoding failed");
    }
    std::string result(header.data(), *encoded);
    result.append(payload);
    return result;
}

std::string requestHeaders(ruvia::WorkerMemory& worker, std::string_view method,
    std::string_view path, std::optional<std::uint64_t> bodyLength = std::nullopt) {
    auto head = ruvia::encodeHttp3ClientRequestHead({.method = method,
                                                        .scheme = "https",
                                                        .authority = "example.test",
                                                        .path = path,
                                                        .bodyLength = bodyLength},
        {}, worker.resource());
    if (!head) {
        throw std::runtime_error("HTTP/3 test request head encoding failed");
    }
    return frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(head->fieldSection.data(), head->fieldSection.size()));
}

std::string requestWire(ruvia::WorkerMemory& worker, std::string_view body) {
    std::string wire = requestHeaders(worker, "POST", "/items", body.size());
    wire += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), body);
    return wire;
}

bool queueData(Fixture& fixture, MessageId id, std::string_view bytes) {
    const auto input = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
    const auto result = fixture.mailbox.trySend(id, input);
    return result == Mailbox::SendResult::kSent || result == Mailbox::SendResult::kSentNotifyPeer;
}

bool queueControl(Fixture& fixture, const Control& control) {
    const auto result = fixture.mailbox.trySendControl(control);
    return result == Mailbox::ControlResult::kSent ||
           result == Mailbox::ControlResult::kSentNotifyPeer;
}

std::optional<Input::Result> receiveData(Fixture& fixture) {
    Mailbox::BorrowedBlock block;
    if (!fixture.mailbox.tryReceive(block)) {
        return std::nullopt;
    }
    auto result = fixture.input.acceptData(block);
    block.release();
    (void)fixture.mailbox.drainReturns();
    return result;
}

std::optional<Input::Result> receiveControl(Fixture& fixture) {
    Control control;
    if (!fixture.mailbox.tryReceiveControl(control)) {
        return std::nullopt;
    }
    return fixture.input.acceptControl(control);
}

Control fin(std::uint64_t streamId, std::uint64_t finalSize) {
    return {Control::Kind::kStreamFin, {kEpoch, kGeneration, streamId}, finalSize};
}

Control reset(std::uint64_t streamId, std::uint64_t publishedBytes,
    ruvia::Http3ConnectionErrorCode errorCode = ruvia::Http3ConnectionErrorCode::kRequestCancelled) {
    return {.kind = Control::Kind::kStreamReset,
        .id = {kEpoch, kGeneration, streamId},
        .value = publishedBytes,
        .streamResetErrorCode = errorCode};
}

bool bodyMatches(const Engine& session, std::uint64_t streamId, std::string_view expected) {
    const auto* request = session.request(streamId);
    if (request == nullptr) {
        return false;
    }
    const auto body = request->request().bodyBytes();
    return std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) == expected;
}

}  // namespace

RUVIA_TEST(http3ServerStreamInputCountsRequestStreamsButNotPeerUnidirectionalStreams) {
    Fixture fixture;
    RUVIA_CHECK(fixture.input.acceptControl(reset(2, 1)).status ==
                Input::Status::kDeferredReset);
    RUVIA_CHECK_EQ(fixture.input.observedRequestStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(fixture.input.activeRequestStreamCount(), std::size_t{0});

    RUVIA_CHECK(fixture.input.acceptControl(reset(0, 0)).status == Input::Status::kReset);
    RUVIA_CHECK_EQ(fixture.input.observedRequestStreamCount(), std::size_t{1});
    RUVIA_CHECK_EQ(fixture.input.activeRequestStreamCount(), std::size_t{0});
}

RUVIA_TEST(http3ServerStreamInputDefersFinAcrossIndependentMailboxLanesAndInterleavedStreams) {
    Fixture fixture;
    const auto first = requestWire(fixture.worker, "alpha");
    const auto second = requestWire(fixture.worker, "bravo");
    const auto firstCut = first.size() / 3;
    const auto secondCut = second.size() / 3;
    RUVIA_CHECK(firstCut > 0 && secondCut > 0);

    RUVIA_CHECK(queueData(fixture,
        {kEpoch, kGeneration, 0, {}, true}, std::string_view(first).substr(0, firstCut)));
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 4}, std::string_view(second).substr(0, secondCut)));
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, std::string_view(first).substr(firstCut, firstCut)));
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 4}, std::string_view(second).substr(secondCut, secondCut)));
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, std::string_view(first).substr(firstCut * 2)));
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 4}, std::string_view(second).substr(secondCut * 2)));
    RUVIA_CHECK(queueControl(fixture, fin(0, first.size())));
    RUVIA_CHECK(queueControl(fixture, fin(4, second.size())));

    const auto firstFin = receiveControl(fixture);
    const auto secondFin = receiveControl(fixture);
    RUVIA_CHECK(firstFin.has_value() && firstFin->status == Input::Status::kDeferredFin);
    RUVIA_CHECK(secondFin.has_value() && secondFin->status == Input::Status::kDeferredFin);

    std::array<Input::Status, 6> statuses{};
    for (auto& status : statuses) {
        const auto result = receiveData(fixture);
        RUVIA_CHECK(result.has_value());
        status = result ? result->status : Input::Status::kInvalidInput;
    }
    RUVIA_CHECK(statuses[0] == Input::Status::kFed && statuses[1] == Input::Status::kFed);
    RUVIA_CHECK(statuses[2] == Input::Status::kFed && statuses[3] == Input::Status::kFed);
    RUVIA_CHECK(statuses[4] == Input::Status::kFinished && statuses[5] == Input::Status::kFinished);
    RUVIA_CHECK(bodyMatches(fixture.session, 0, "alpha"));
    RUVIA_CHECK(bodyMatches(fixture.session, 4, "bravo"));
    RUVIA_CHECK(fixture.input.receivedEarlyData(0));
    RUVIA_CHECK(!fixture.input.receivedEarlyData(4));
    RUVIA_CHECK(fixture.input.trackedStreamCount() == 2);
    RUVIA_CHECK_EQ(fixture.input.observedRequestStreamCount(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.input.activeRequestStreamCount(), std::size_t{0});
}

RUVIA_TEST(http3ServerStreamInputDefersPeerResetUntilPublishedHeadersAreConsumed) {
    Fixture fixture;
    const auto headers = requestHeaders(fixture.worker, "POST", "/items");
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, headers));
    RUVIA_CHECK(queueControl(fixture, reset(0, headers.size())));

    const auto resetResult = receiveControl(fixture);
    RUVIA_CHECK(resetResult.has_value() && resetResult->status == Input::Status::kDeferredReset);
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
    const auto appliedReset = receiveData(fixture);
    RUVIA_CHECK(appliedReset.has_value() && appliedReset->status == Input::Status::kReset);
    RUVIA_CHECK(fixture.session.request(0) == nullptr);

    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, headers));
    const auto stillClosed = receiveData(fixture);
    RUVIA_CHECK(stillClosed.has_value() && stillClosed->status == Input::Status::kFinalSizeError);
    RUVIA_CHECK(fixture.input.trackedStreamCount() == 1);
    RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
}

RUVIA_TEST(http3ServerStreamInputDefersResetAcrossMultipleDataBlocksAndChecksTheBarrier) {
    Fixture fixture;
    const auto wire = requestWire(fixture.worker, "barrier");
    const auto firstCut = wire.size() / 3;
    const auto secondCut = firstCut * 2;
    RUVIA_CHECK(firstCut > 0 && secondCut < wire.size());
    const MessageId id{kEpoch, kGeneration, 0};
    RUVIA_CHECK(queueData(fixture, id, std::string_view(wire).substr(0, firstCut)));
    RUVIA_CHECK(queueData(fixture, id,
        std::string_view(wire).substr(firstCut, secondCut - firstCut)));
    RUVIA_CHECK(queueData(fixture, id, std::string_view(wire).substr(secondCut)));
    RUVIA_CHECK(queueControl(fixture, reset(0, wire.size(),
                                          ruvia::Http3ConnectionErrorCode::kRequestRejected)));

    const auto deferred = receiveControl(fixture);
    RUVIA_CHECK(deferred && deferred->status == Input::Status::kDeferredReset);
    const auto first = receiveData(fixture);
    const auto second = receiveData(fixture);
    const auto last = receiveData(fixture);
    RUVIA_CHECK(first && first->status == Input::Status::kDeferredReset);
    RUVIA_CHECK(second && second->status == Input::Status::kDeferredReset);
    RUVIA_CHECK(last && last->status == Input::Status::kReset);
    RUVIA_CHECK(fixture.input.acceptControl(reset(0, wire.size(),
                                                ruvia::Http3ConnectionErrorCode::kRequestRejected))
                    .status == Input::Status::kClosedStream);
    RUVIA_CHECK(!fixture.input.stopped());
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
}

RUVIA_TEST(http3ServerStreamInputDefersCriticalStreamResetUntilItsTypeIsFed) {
    Fixture fixture;
    const MessageId id{kEpoch, kGeneration, 2};
    const auto resetControl = Control{.kind = Control::Kind::kStreamReset,
        .id = id,
        .value = 1,
        .streamResetErrorCode = ruvia::Http3ConnectionErrorCode::kRequestCancelled};
    RUVIA_CHECK(fixture.input.acceptControl(resetControl).status == Input::Status::kDeferredReset);
    const std::array<char, 1> controlType{0};
    RUVIA_CHECK(queueData(fixture, id, std::string_view(controlType.data(), controlType.size())));
    const auto closedCritical = receiveData(fixture);
    RUVIA_CHECK(closedCritical && closedCritical->status == Input::Status::kProtocolError);
    RUVIA_CHECK(closedCritical->protocol.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(closedCritical->protocol.code == ruvia::Http3ConnectionErrorCode::kClosedCriticalStream);
    RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
}

RUVIA_TEST(http3ServerStreamInputRejectsResetBarrierOverrunAndConflictingDuplicatesLocally) {
    {
        Fixture fixture;
        const auto headers = requestHeaders(fixture.worker, "POST", "/items");
        RUVIA_CHECK(fixture.input.acceptControl(reset(0, headers.size() - 1)).status ==
                    Input::Status::kDeferredReset);
        RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, headers));
        const auto overrun = receiveData(fixture);
        RUVIA_CHECK(overrun && overrun->status == Input::Status::kFinalSizeError);
        RUVIA_CHECK(overrun->protocol.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(fixture.session.request(0) == nullptr);
        RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
    }
    {
        Fixture fixture;
        const auto original = reset(0, 8, ruvia::Http3ConnectionErrorCode::kRequestRejected);
        RUVIA_CHECK(fixture.input.acceptControl(original).status == Input::Status::kDeferredReset);
        RUVIA_CHECK(fixture.input.acceptControl(original).status == Input::Status::kDeferredReset);
        const auto conflict = fixture.input.acceptControl(
            reset(0, 9, ruvia::Http3ConnectionErrorCode::kRequestRejected));
        RUVIA_CHECK(conflict.status == Input::Status::kFinalSizeError);
        RUVIA_CHECK(conflict.protocol.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
    }
    {
        Fixture fixture;
        const auto firstReset = reset(0, 8, ruvia::Http3ConnectionErrorCode::kRequestRejected);
        RUVIA_CHECK(fixture.input.acceptControl(firstReset).status == Input::Status::kDeferredReset);
        const auto codeConflict = fixture.input.acceptControl(
            reset(0, 8, ruvia::Http3ConnectionErrorCode::kMessageError));
        RUVIA_CHECK(codeConflict.status == Input::Status::kFinalSizeError);
        RUVIA_CHECK(codeConflict.protocol.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
    }
    {
        Fixture fixture;
        const auto tooLargeForVarInt = fixture.input.acceptControl(
            reset(0, ruvia::kHttp3VarIntMax + 1));
        RUVIA_CHECK(tooLargeForVarInt.status == Input::Status::kFinalSizeError);
        RUVIA_CHECK(tooLargeForVarInt.protocol.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
    }
    {
        Fixture fixture;
        RUVIA_CHECK(fixture.input.acceptControl(reset(0, 0)).status == Input::Status::kReset);
        RUVIA_CHECK(fixture.input.acceptControl(reset(0, 0)).status == Input::Status::kClosedStream);
        const auto conflict = fixture.input.acceptControl(reset(0, 1));
        RUVIA_CHECK(conflict.status == Input::Status::kFinalSizeError);
        RUVIA_CHECK(conflict.protocol.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
    }
}

RUVIA_TEST(http3ServerStreamInputStoresDeferredResetInItsPreallocatedStreamSlot) {
    ruvia::test::CountingMemoryResource upstream;
    {
        Routes routes;
        ruvia::WorkerMemory worker(upstream);
        Engine session(routes.implementation.routeTable(), worker);
        Input input(session, worker, kEpoch, kGeneration, 4);
        const auto allocations = upstream.allocationCount();
        const auto deferred = input.acceptControl(reset(0, 12));
        RUVIA_CHECK(deferred.status == Input::Status::kDeferredReset);
        RUVIA_CHECK_EQ(upstream.allocationCount(), allocations);
        RUVIA_CHECK_EQ(input.trackedStreamCount(), std::size_t{1});
        input.stop();
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerStreamInputLocalCancellationTombstonesDeferredAndCompletedRequests) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        Fixture fixture;
        const auto wire = requestWire(fixture.worker, "retained");
        RUVIA_CHECK(fixture.input.cancelRequest(2).status == Input::Status::kInvalidInput);
        RUVIA_CHECK(fixture.input.acceptControl(fin(0, wire.size())).status == Input::Status::kDeferredFin);
        std::optional<Engine::RequestLease> lease;
        if (scenario > 0) {
            const auto bytes = std::string_view(wire).substr(0, scenario == 1 ? wire.size() - 1 : wire.size());
            RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, bytes));
            const auto result = receiveData(fixture);
            RUVIA_CHECK(result && result->status == (scenario == 1 ? Input::Status::kFed : Input::Status::kFinished));
            if (scenario == 2) {
                auto acquired = fixture.session.acquireRequest(0);
                RUVIA_CHECK(acquired.has_value());
                if (acquired) {
                    lease.emplace(std::move(*acquired));
                }
            }
        }
        RUVIA_CHECK(fixture.input.cancelRequest(0).status == Input::Status::kLocalCancelled);
        RUVIA_CHECK(fixture.input.cancelRequest(0).status == Input::Status::kClosedStream);
        RUVIA_CHECK(fixture.input.acceptControl(fin(0, wire.size() + 1)).status == Input::Status::kClosedStream);
        RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, wire));
        const auto late = receiveData(fixture);
        RUVIA_CHECK(late && late->status == Input::Status::kClosedStream);
        if (lease) {
            RUVIA_CHECK(bodyMatches(fixture.session, 0, "retained"));
        }
        lease.reset();
        RUVIA_CHECK(fixture.session.request(0) == nullptr);
        RUVIA_CHECK(!fixture.input.stopped());
        RUVIA_CHECK(fixture.input.acceptControl(fin(4, wire.size())).status == Input::Status::kDeferredFin);
        RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 4}, wire));
        const auto sibling = receiveData(fixture);
        RUVIA_CHECK(sibling && sibling->status == Input::Status::kFinished);
        RUVIA_CHECK(bodyMatches(fixture.session, 4, "retained"));
    }
}

RUVIA_TEST(http3ServerStreamInputRejectsForeignAndStaleMailboxIdentityWithoutMutation) {
    Fixture fixture;
    const auto headers = requestHeaders(fixture.worker, "POST", "/items");
    const Control foreignReset{.kind = Control::Kind::kStreamReset,
        .id = {kEpoch + 1, kGeneration, 0},
        .value = 0};
    const Control staleReset{.kind = Control::Kind::kStreamReset,
        .id = {kEpoch, kGeneration + 1, 0},
        .value = 0};
    RUVIA_CHECK(fixture.input.acceptControl(foreignReset).status == Input::Status::kForeignEpoch);
    RUVIA_CHECK(fixture.input.acceptControl(staleReset).status == Input::Status::kStaleConnection);
    RUVIA_CHECK(fixture.input.trackedStreamCount() == 0);

    RUVIA_CHECK(queueData(fixture, {kEpoch + 1, kGeneration, 0}, headers));
    const auto foreignData = receiveData(fixture);
    RUVIA_CHECK(foreignData.has_value() && foreignData->status == Input::Status::kForeignEpoch);
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration + 1, 0}, headers));
    const auto staleData = receiveData(fixture);
    RUVIA_CHECK(staleData.has_value() && staleData->status == Input::Status::kStaleConnection);
    RUVIA_CHECK(fixture.input.trackedStreamCount() == 0);
    RUVIA_CHECK(fixture.session.request(0) == nullptr);

    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, headers));
    const auto currentData = receiveData(fixture);
    RUVIA_CHECK(currentData.has_value() && currentData->status == Input::Status::kFed);
    RUVIA_CHECK(fixture.input.trackedStreamCount() == 1);
    RUVIA_CHECK(fixture.session.request(0) != nullptr);
}

RUVIA_TEST(http3ServerStreamInputRejectsInvalidBorrowAndStreamIdWithoutAdmission) {
    Fixture fixture;
    Mailbox::BorrowedBlock empty;
    RUVIA_CHECK(fixture.input.acceptData(empty).status == Input::Status::kInvalidInput);
    RUVIA_CHECK(fixture.input.acceptControl(reset(ruvia::kHttp3VarIntMax + 1, 0)).status ==
                Input::Status::kInvalidInput);
    const auto headers = requestHeaders(fixture.worker, "POST", "/items");
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, ruvia::kHttp3VarIntMax + 1}, headers));
    const auto invalid = receiveData(fixture);
    RUVIA_CHECK(invalid && invalid->status == Input::Status::kInvalidInput);
    RUVIA_CHECK(fixture.input.trackedStreamCount() == 0 && fixture.session.activeStreamCount() == 0);
    RUVIA_CHECK(!fixture.input.stopped());
}

RUVIA_TEST(http3ServerStreamInputRoutesSharedMailboxWithoutConsumingForeignBlocks) {
    Routes routes;
    ruvia::WorkerMemory worker;
    Engine firstSession(routes.implementation.routeTable(), worker);
    Engine secondSession(routes.implementation.routeTable(), worker);
    Mailbox mailbox(4, 4, 2);
    Input firstInput(firstSession, worker, kEpoch, kGeneration, 4);
    Input secondInput(secondSession, worker, kEpoch, kGeneration + 1, 4);
    const auto headers = requestHeaders(worker, "POST", "/items");
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(headers.data()), headers.size());
    const auto firstSent = mailbox.trySend({kEpoch, kGeneration, 0}, bytes);
    const auto secondSent = mailbox.trySend({kEpoch, kGeneration + 1, 4}, bytes);
    RUVIA_CHECK(firstSent == Mailbox::SendResult::kSent ||
                firstSent == Mailbox::SendResult::kSentNotifyPeer);
    RUVIA_CHECK(secondSent == Mailbox::SendResult::kSent ||
                secondSent == Mailbox::SendResult::kSentNotifyPeer);

    Mailbox::BorrowedBlock block;
    RUVIA_CHECK(mailbox.tryReceive(block));
    RUVIA_CHECK(secondInput.acceptData(block).status == Input::Status::kStaleConnection);
    RUVIA_CHECK(secondInput.trackedStreamCount() == 0);
    RUVIA_CHECK(firstInput.acceptData(block).status == Input::Status::kFed);
    block.release();
    (void)mailbox.drainReturns();

    RUVIA_CHECK(mailbox.tryReceive(block));
    RUVIA_CHECK(firstInput.acceptData(block).status == Input::Status::kStaleConnection);
    RUVIA_CHECK(firstInput.trackedStreamCount() == 1);
    RUVIA_CHECK(firstSession.request(4) == nullptr);
    RUVIA_CHECK(secondInput.acceptData(block).status == Input::Status::kFed);
    block.release();
    (void)mailbox.drainReturns();
    RUVIA_CHECK(firstInput.trackedStreamCount() == 1);
    RUVIA_CHECK(secondInput.trackedStreamCount() == 1);
    RUVIA_CHECK(secondSession.request(4) != nullptr);
}

RUVIA_TEST(http3ServerStreamInputValidatesFinalSizeAndNeverFeedsPastIt) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        Fixture fixture;
        const auto headers = requestHeaders(fixture.worker, "POST", "/items");
        std::optional<Engine::RequestLease> lease;
        Input::Result failure;
        if (scenario < 2) {
            RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, headers));
            const auto fed = receiveData(fixture);
            RUVIA_CHECK(fed && fed->status == Input::Status::kFed);
            if (scenario == 1) {
                RUVIA_CHECK(fixture.input.acceptControl(fin(0, headers.size())).status == Input::Status::kFinished);
                RUVIA_CHECK(fixture.input.acceptControl(fin(0, headers.size())).status == Input::Status::kDuplicateFin);
                auto acquired = fixture.session.acquireRequest(0);
                RUVIA_CHECK(acquired.has_value());
                if (acquired) {
                    lease.emplace(std::move(*acquired));
                }
            }
            failure = fixture.input.acceptControl(fin(0, scenario == 0 ? headers.size() - 1 : headers.size() + 1));
        } else if (scenario == 2) {
            RUVIA_CHECK(fixture.input.acceptControl(fin(0, 1)).status == Input::Status::kDeferredFin);
            RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, headers));
            const auto overrun = receiveData(fixture);
            RUVIA_CHECK(overrun.has_value());
            if (overrun) {
                failure = *overrun;
            }
        } else if (scenario == 3) {
            failure = fixture.input.acceptControl(fin(0, ruvia::kHttp3VarIntMax + 1));
        } else {
            RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, headers));
            (void)receiveData(fixture);
            RUVIA_CHECK(fixture.input.acceptControl(fin(0, headers.size())).status == Input::Status::kFinished);
            RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, "x"));
            const auto excess = receiveData(fixture);
            RUVIA_CHECK(excess.has_value());
            if (excess) {
                failure = *excess;
            }
        }
        RUVIA_CHECK(failure.status == Input::Status::kFinalSizeError);
        RUVIA_CHECK(failure.protocol.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
        if (lease) {
            RUVIA_CHECK(lease->request().bodyComplete());
        }
        lease.reset();
        RUVIA_CHECK(fixture.session.request(0) == nullptr);
        RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, "late"));
        const auto late = receiveData(fixture);
        RUVIA_CHECK(late && late->status == Input::Status::kStopped);
    }
}

RUVIA_TEST(http3ServerStreamInputResetAfterFinRetiresRequestWithoutInvalidatingLease) {
    Fixture fixture;
    const auto wire = requestWire(fixture.worker, "retained");
    RUVIA_CHECK(fixture.input.acceptControl(fin(0, wire.size())).status == Input::Status::kDeferredFin);
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, wire));
    const auto completed = receiveData(fixture);
    RUVIA_CHECK(completed && completed->status == Input::Status::kFinished);
    auto lease = fixture.session.acquireRequest(0);
    RUVIA_CHECK(lease.has_value());
    RUVIA_CHECK(fixture.input.acceptControl(reset(0, wire.size())).status == Input::Status::kReset);
    RUVIA_CHECK(bodyMatches(fixture.session, 0, "retained"));
    lease.reset();
    RUVIA_CHECK(fixture.session.request(0) == nullptr);
    RUVIA_CHECK(fixture.input.acceptControl(reset(0, wire.size())).status == Input::Status::kClosedStream);
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, wire));
    const auto late = receiveData(fixture);
    RUVIA_CHECK(late && late->status == Input::Status::kFinalSizeError);
    RUVIA_CHECK(fixture.input.stopped() && fixture.session.terminated());
}

RUVIA_TEST(http3ServerStreamInputReturnsCompleteSessionFeedErrorScope) {
    Fixture fixture;
    // The frame carries one byte while the request advertises two, producing
    // an HTTP message error when the queued FIN is finally fed.
    auto invalid = requestHeaders(fixture.worker, "POST", "/items", 2);
    invalid += frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kData), "x");
    RUVIA_CHECK(queueControl(fixture, fin(0, invalid.size())));
    const auto deferred = receiveControl(fixture);
    RUVIA_CHECK(deferred.has_value() && deferred->status == Input::Status::kDeferredFin);
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, invalid));
    const auto failure = receiveData(fixture);
    RUVIA_CHECK(failure.has_value() && failure->status == Input::Status::kProtocolError);
    RUVIA_CHECK(failure.has_value() && failure->protocol.status == ruvia::Http3ConnectionStatus::kStreamError);
    RUVIA_CHECK(failure.has_value() && failure->protocol.scope == ruvia::Http3ConnectionErrorScope::kStream);
    RUVIA_CHECK(failure.has_value() &&
                failure->protocol.code == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(!fixture.session.terminated());
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, "ignored"));
    const auto terminal = receiveData(fixture);
    RUVIA_CHECK(terminal.has_value() && terminal->status == Input::Status::kClosedStream);
}

RUVIA_TEST(http3ServerStreamInputBoundsTombstonesAndConnectionStopDropsLateData) {
    Fixture full(1);
    RUVIA_CHECK(queueControl(full, reset(0, 0)));
    const auto resetResult = receiveControl(full);
    RUVIA_CHECK(resetResult.has_value() && resetResult->status == Input::Status::kReset);
    const auto headers = requestHeaders(full.worker, "POST", "/items");
    RUVIA_CHECK(queueData(full, {kEpoch, kGeneration, 4}, headers));
    const auto capacity = receiveData(full);
    RUVIA_CHECK(capacity.has_value() && capacity->status == Input::Status::kCapacityExhausted);
    RUVIA_CHECK(full.input.trackedStreamCount() == 1);
    RUVIA_CHECK(full.session.request(4) == nullptr);
    RUVIA_CHECK(full.input.stopped() && full.session.terminated());

    Fixture stopped;
    RUVIA_CHECK(queueData(stopped, {kEpoch, kGeneration, 0}, headers));
    const auto beforeStop = receiveData(stopped);
    RUVIA_CHECK(beforeStop.has_value() && beforeStop->status == Input::Status::kFed);
    const Control connectionClosed{Control::Kind::kConnectionClosed,
        {kEpoch, kGeneration, 0}, 0};
    RUVIA_CHECK(queueControl(stopped, connectionClosed));
    const auto closed = receiveControl(stopped);
    RUVIA_CHECK(closed.has_value() && closed->status == Input::Status::kConnectionClosed);
    RUVIA_CHECK(stopped.input.stopped() && stopped.session.terminated());
    RUVIA_CHECK(queueData(stopped, {kEpoch, kGeneration, 0}, headers));
    const auto late = receiveData(stopped);
    RUVIA_CHECK(late.has_value() && late->status == Input::Status::kStopped);
    RUVIA_CHECK(stopped.input.trackedStreamCount() == 1);
}

RUVIA_TEST(http3ServerStreamInputConsumesBoundedCrossThreadMailboxWithCapacityWake) {
    Routes routes;
    ruvia::WorkerMemory encodingMemory;
    const std::string payload(65536, 'p');
    const auto wire = requestWire(encodingMemory, payload);
    std::counting_semaphore<64> incoming(0), space(0);
    std::atomic<bool> ok{true};
    ruvia::test::CountingMemoryResource upstream;
    Mailbox mailbox(2, 2, 1, nullptr, {.context = &space, .notify = [](void* pointer) noexcept { static_cast<std::counting_semaphore<64>*>(pointer)->release(); }});
    std::jthread consumer([&] {
        try {
            ruvia::WorkerMemory worker(upstream);
            Engine session(routes.implementation.routeTable(), worker);
            Input input(session, worker, kEpoch, kGeneration, 8);
            bool complete = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!complete && ok && std::chrono::steady_clock::now() < deadline) {
                if (!incoming.try_acquire_for(std::chrono::seconds(1))) {
                    ok = false;
                    break;
                }
                bool again;
                do {
                    Control control;
                    while (mailbox.tryReceiveControl(control)) {
                        const auto result = input.acceptControl(control);
                        if (result.status == Input::Status::kFinished) {
                            complete = true;
                        } else if (result.status != Input::Status::kDeferredFin) {
                            ok = false;
                        }
                    }
                    Mailbox::BorrowedBlock block;
                    while (mailbox.tryReceive(block)) {
                        const auto result = input.acceptData(block);
                        if (result.status == Input::Status::kFinished) {
                            complete = true;
                        } else if (result.status != Input::Status::kFed) {
                            ok = false;
                        }
                    }
                    again = mailbox.finishDrain();
                } while (again && ok && std::chrono::steady_clock::now() < deadline);
            }
            if (!complete || !bodyMatches(session, 0, payload)) {
                ok = false;
            }
            auto lease = session.acquireRequest(0);
            if (!lease) {
                ok = false;
            }
            input.stop();
            if (lease && !bodyMatches(session, 0, payload)) {
                ok = false;
            }
            lease.reset();
            if (session.activeStreamCount() != 0) {
                ok = false;
            }
        } catch (...) {
            ok = false;
        }
    });
    const auto waitForCapacity = [&](bool control) {
        const auto result = mailbox.armCapacityWait(control ? Mailbox::CapacityInterest::kControl
                                                            : Mailbox::CapacityInterest::kData);
        if (result == Mailbox::CapacityWaitResult::kReady) {
            return true;
        }
        return result == Mailbox::CapacityWaitResult::kArmed && space.try_acquire_for(std::chrono::seconds(1));
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::size_t offset = 0;
    while (offset < wire.size() && ok && std::chrono::steady_clock::now() < deadline) {
        const auto count = std::min(Mailbox::kMaxBlockBytes, wire.size() - offset);
        const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(wire.data() + offset), count);
        const auto sent = mailbox.trySend({kEpoch, kGeneration, 0}, bytes);
        if (sent == Mailbox::SendResult::kSent || sent == Mailbox::SendResult::kSentNotifyPeer) {
            offset += count;
            if (sent == Mailbox::SendResult::kSentNotifyPeer) {
                incoming.release();
            }
        } else if ((sent != Mailbox::SendResult::kFull && sent != Mailbox::SendResult::kNoBlock) || !waitForCapacity(false)) {
            ok = false;
        }
    }
    bool finSent = false;
    while (offset == wire.size() && ok && std::chrono::steady_clock::now() < deadline) {
        const auto sent = mailbox.trySendControl(fin(0, wire.size()));
        if (sent == Mailbox::ControlResult::kSent || sent == Mailbox::ControlResult::kSentNotifyPeer) {
            finSent = true;
            if (sent == Mailbox::ControlResult::kSentNotifyPeer) {
                incoming.release();
            }
            break;
        }
        if (sent != Mailbox::ControlResult::kFull || !waitForCapacity(true)) {
            ok = false;
            break;
        }
    }
    if (!finSent) {
        ok = false;
    }
    consumer.join();
    (void)mailbox.drainReturns();
    RUVIA_CHECK(mailbox.stop());
    RUVIA_CHECK(ok && finSent);
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3ServerStreamInputReturnsWorkerPmrStateAtConnectionRetirement) {
    ruvia::test::CountingMemoryResource upstream;
    {
        Routes routes;
        {
            ruvia::WorkerMemory worker(upstream);
            {
                Engine session(routes.implementation.routeTable(), worker);
                {
                    Input input(session, worker, kEpoch, kGeneration, 8);
                    const auto startupAllocations = upstream.allocationCount();
                    for (std::uint64_t streamId = 0; streamId < 24; streamId += 4) {
                        const auto result = input.acceptControl(reset(streamId, 0));
                        RUVIA_CHECK(result.status == Input::Status::kReset);
                    }
                    RUVIA_CHECK(input.trackedStreamCount() == 6);
                    RUVIA_CHECK_EQ(upstream.allocationCount(), startupAllocations);
                    input.stop();
                }
            }
        }
    }
    RUVIA_CHECK(upstream.liveAllocations() == 0);
    RUVIA_CHECK(upstream.allocationCount() == upstream.deallocationCount());
}

RUVIA_TEST(http3ServerStreamInputRetainsQpackBlockedSuffixAndFinUntilEncoderAdvances) {
    Fixture fixture(16, {.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2, .enableConnectProtocol = true});
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 256, .maxBlockedStreams = 2}, fixture.worker.resource());
    const std::array<ruvia::Http3FieldSectionFieldView, 1> fields{{{"x-dynamic", "retained"}}};
    auto head = ruvia::encodeHttp3ClientRequestHead(encoder, 0,
        {.method = "POST", .scheme = "https", .authority = "example.test", .path = "/items", .fields = fields, .bodyLength = 7},
        {}, fixture.worker.resource());
    RUVIA_CHECK(head.has_value());
    if (!head) {
        return;
    }
    auto wire = frame(1, std::string_view(head->fieldSection.data(), head->fieldSection.size())) + frame(0, "payload");
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, wire));
    Mailbox::BorrowedBlock block;
    RUVIA_CHECK(fixture.mailbox.tryReceive(block));
    const auto accepted = fixture.input.acceptData(block);
    block.release();
    RUVIA_CHECK(accepted.status == Input::Status::kDeferredQpack);
    RUVIA_CHECK(!fixture.input.canAcceptInput(0));
    RUVIA_CHECK(fixture.input.acceptControl({.kind = Control::Kind::kStreamFin, .id = {kEpoch, kGeneration, 0}, .value = wire.size()}).status == Input::Status::kDeferredQpack);
    RUVIA_CHECK(!fixture.input.resumeQpack());
    std::string instructions(1, char{2});
    const auto pending = encoder.pendingEncoderOutput();
    instructions.append(pending.data(), pending.size());
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 6}, instructions));
    RUVIA_CHECK(fixture.mailbox.tryReceive(block));
    RUVIA_CHECK(fixture.input.acceptData(block).status == Input::Status::kFed);
    block.release();
    const auto resumed = fixture.input.resumeQpack();
    RUVIA_CHECK(resumed && resumed->streamId == 0 && resumed->result.status == Input::Status::kFinished);
    RUVIA_CHECK(fixture.input.canAcceptInput(0));
    RUVIA_CHECK(fixture.session.streamState(0) == Engine::StreamState::kReady);
    const auto* request = fixture.session.request(0);
    RUVIA_CHECK(request != nullptr);
    if (request) {
        const auto body = request->request().bodyBytes();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "payload");
        RUVIA_CHECK_EQ(request->request().header("x-dynamic").value_or(""), "retained");
    }
    RUVIA_CHECK(!fixture.input.resumeQpack());
}

RUVIA_TEST(http3ServerStreamInputResetAfterBlockedFinReleasesSuffixAndCancelsDecoderSection) {
    Fixture fixture(16, {.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2, .enableConnectProtocol = true});
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 256, .maxBlockedStreams = 2}, fixture.worker.resource());
    const std::array fields{ruvia::Http3FieldSectionFieldView{"x-dynamic", "retained"}};
    auto head = ruvia::encodeHttp3ClientRequestHead(encoder, 0,
        {.method = "POST", .scheme = "https", .authority = "example.test", .path = "/items", .fields = fields, .bodyLength = 7}, {}, fixture.worker.resource());
    RUVIA_CHECK(head.has_value());
    if (!head) {
        return;
    }
    const auto wire = frame(1, {head->fieldSection.data(), head->fieldSection.size()}) + frame(0, "payload");
    RUVIA_CHECK(queueData(fixture, {kEpoch, kGeneration, 0}, wire));
    Mailbox::BorrowedBlock block;
    RUVIA_CHECK(fixture.mailbox.tryReceive(block));
    RUVIA_CHECK(fixture.input.acceptData(block).status == Input::Status::kDeferredQpack);
    block.release();
    RUVIA_CHECK(fixture.input.acceptControl({Control::Kind::kStreamFin, {kEpoch, kGeneration, 0}, wire.size()}).status == Input::Status::kDeferredQpack);
    RUVIA_CHECK(fixture.input.acceptControl({Control::Kind::kStreamReset, {kEpoch, kGeneration, 0}, wire.size()}).status == Input::Status::kReset);
    RUVIA_CHECK(!fixture.input.stopped());
    RUVIA_CHECK(!fixture.input.resumeQpack());
    RUVIA_CHECK_EQ(fixture.input.activeRequestStreamCount(), std::size_t{0});
    RUVIA_CHECK(!fixture.session.pendingQpackDecoderOutput().empty());
}
