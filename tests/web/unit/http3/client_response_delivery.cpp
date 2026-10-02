#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <asio/co_spawn.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientResponseDecoding.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/client/HttpClientResultBudget.h"
#include "ruvia/web/detail/http3/Http3ClientBodyBudget.h"
#include "ruvia/web/detail/http3/Http3ClientReceiveDriver.h"
#include "ruvia/web/detail/http3/Http3ClientResponseDelivery.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using Delivery = ruvia::detail::Http3ClientResponseDelivery;
using Driver = ruvia::detail::Http3ClientReceiveDriver;
using Engine = ruvia::detail::Http3ClientSansIoSessionEngine;
using Read = ruvia::quic_stream_read_result;
using State = ruvia::detail::HttpClientResponseState;
using BodyBudgetLease = ruvia::detail::Http3ClientBodyBudget::Lease;

static_assert(!std::is_copy_constructible_v<BodyBudgetLease>);
static_assert(!std::is_copy_assignable_v<BodyBudgetLease>);
static_assert(std::is_nothrow_move_constructible_v<BodyBudgetLease>);
static_assert(std::is_nothrow_move_assignable_v<BodyBudgetLease>);

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

std::vector<char> frame(std::uint64_t type, std::span<const char> payload) {
    std::vector<char> output(16);
    const auto header = ruvia::encodeHttp3VarInt(output, type);
    const auto length = ruvia::encodeHttp3VarInt(
        std::span<char>(output).subspan(*header), payload.size());
    output.resize(*header + *length);
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

void appendFrame(std::vector<char>& wire, std::uint64_t type, std::string_view payload) {
    const auto encoded = frame(type, std::span<const char>(payload.data(), payload.size()));
    wire.insert(wire.end(), encoded.begin(), encoded.end());
}

std::vector<char> responseHead(std::string_view status = "200",
    std::optional<std::string_view> contentLength = {},
    std::optional<std::string_view> contentEncoding = {}) {
    std::pmr::monotonic_buffer_resource temp;
    std::vector<ruvia::Http3FieldSectionFieldView> fields{{":status", status}, {"x-owned", "copied"}};
    if (contentLength) {
        fields.push_back({"content-length", *contentLength});
    }
    if (contentEncoding) {
        fields.push_back({"content-encoding", *contentEncoding});
    }
    const auto encoded = ruvia::encodeHttp3FieldSection(fields, &temp);
    return frame(1, *encoded);
}

std::vector<char> responseTrailer() {
    std::pmr::monotonic_buffer_resource temp;
    constexpr std::array fields{ruvia::Http3FieldSectionFieldView{"x-trailer", "done"}};
    const auto encoded = ruvia::encodeHttp3FieldSection(fields, &temp);
    return frame(1, *encoded);
}

struct FakeRead final {
    std::vector<char> wire;
    std::size_t position{};
    std::size_t maxChunk{4096};
    bool fin{};
    bool reset{};

    Read operator()(std::uint64_t, std::span<char> destination) {
        if (reset) {
            return {.status = ruvia::quic_stream_read_status::reset};
        }
        if (position == wire.size()) {
            return {.status = fin ? ruvia::quic_stream_read_status::fin : ruvia::quic_stream_read_status::would_block};
        }
        const auto size = std::min({destination.size(), wire.size() - position, maxChunk});
        std::copy_n(wire.data() + position, size, destination.data());
        position += size;
        return {.status = ruvia::quic_stream_read_status::data, .size = size};
    }
};

struct WakeCounter final {
    std::size_t notifications{};

    static void notify(void* context) noexcept {
        ++static_cast<WakeCounter*>(context)->notifications;
    }
};

bool planMatches(const std::optional<ruvia::HttpResponseBodyPlan>& plan,
    ruvia::HttpKnownMethod method, std::uint16_t status,
    ruvia::HttpResponseContentSemantics semantics, bool bodySuppressed) {
    return plan && plan->requestMethod() == method &&
           plan->responseStatus() == ruvia::HttpStatusCode::fromValue(status) &&
           plan->contentSemantics() == semantics &&
           plan->bodySuppressed() == bodySuppressed;
}
}  // namespace

RUVIA_TEST(http3_client_response_delivery_copies_events_without_invalidating_returned_body_view) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    ruvia::detail::Http3ClientBodyBudget bodyBudget(4096);
    {
        State state(worker.handle, &resource);
        const auto beforeLeaseAllocations = resource.allocationCount();
        Delivery delivery(state, &bodyBudget);
        RUVIA_CHECK_EQ(resource.allocationCount(), beforeLeaseAllocations);
        Engine engine(&resource);
        Driver driver(engine);
        const std::string firstBody(512, 'a');
        const std::string secondBody(1024, 'b');
        std::string_view returnedView;

        auto operation = [&]() -> ruvia::Task<void> {
            RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                                  delivery.eventSink())
                            .scope == ruvia::Http3ConnectionErrorScope::kNone);

            FakeRead initial{.wire = responseHead()};
            appendFrame(initial.wire, 0, firstBody);
            const auto headAndBody = driver.drive(0, initial);
            RUVIA_CHECK(headAndBody.status == Driver::Status::kProgress);
            RUVIA_CHECK(state.headReady);
            RUVIA_CHECK(state.status.value() == 200);
            const auto plan = delivery.responseBodyPlan();
            RUVIA_CHECK(plan && plan->requestMethod() == ruvia::HttpKnownMethod::kGet &&
                        plan->responseStatus() == ruvia::http_status::kOk);
            RUVIA_CHECK(state.protocolVersion == ruvia::HttpProtocolVersion::kHttp3);
            RUVIA_CHECK(state.headers.size() == 1);
            RUVIA_CHECK(state.headers.front().name() == "x-owned");
            RUVIA_CHECK(state.headers.front().value() == "copied");
            RUVIA_CHECK(!state.complete);

            const auto first = co_await state.read<std::string_view>();
            RUVIA_CHECK(first.has_value());
            RUVIA_CHECK_EQ(first->size(), firstBody.size());
            returnedView = *first;
            const auto* const returnedAddress = returnedView.data();

            FakeRead later{.wire = {}};
            appendFrame(later.wire, 0, secondBody);
            const auto data = driver.drive(0, later);
            RUVIA_CHECK(data.status == Driver::Status::kProgress);
            RUVIA_CHECK_EQ(state.buffered.size(), firstBody.size());
            RUVIA_CHECK(state.buffered.data() == returnedAddress);
            RUVIA_CHECK(returnedView == firstBody);
            RUVIA_CHECK(std::string_view(state.pending) == secondBody);

            // Trailers are a separate HEADERS field section, so use a fresh input
            // after the body bytes have already been consumed by the first drive.
            FakeRead trailers{.wire = responseTrailer()};
            RUVIA_CHECK(driver.drive(0, trailers).status == Driver::Status::kProgress);
            RUVIA_CHECK(state.trailers.size() == 1);
            RUVIA_CHECK(state.trailers.front().name() == "x-trailer");
            RUVIA_CHECK(state.trailers.front().value() == "done");

            FakeRead end{.fin = true};
            const auto finished = driver.drive(0, end);
            RUVIA_CHECK(finished.status == Driver::Status::kResponseComplete);
            RUVIA_CHECK(!state.complete);
            RUVIA_CHECK(!state.failure);
            RUVIA_CHECK(!state.errorCode);
            RUVIA_CHECK(delivery.commit(finished) == Delivery::CommitStatus::kCommitted);
            RUVIA_CHECK(state.complete);
            RUVIA_CHECK(returnedView == firstBody);
            RUVIA_CHECK(std::string_view(state.pending) == secondBody);
            RUVIA_CHECK_EQ(bodyBudget.used(), firstBody.size() + secondBody.size());
            RUVIA_CHECK(engine.release(0));
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_keeps_final_plans_for_decoder_and_terminal_paths) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        struct Case final {
            ruvia::HttpKnownMethod method;
            std::string_view status;
            std::optional<std::string_view> contentLength;
            std::string_view body;
            std::uint16_t statusCode;
            ruvia::HttpResponseContentSemantics semantics;
            bool bodySuppressed;
        };
        constexpr std::array cases{
            Case{ruvia::HttpKnownMethod::kGet, "200", "3", "abc", 200,
                ruvia::HttpResponseContentSemantics::kWithContent, false},
            Case{ruvia::HttpKnownMethod::kGet, "200", {}, {}, 200,
                ruvia::HttpResponseContentSemantics::kWithContent, false},
            Case{ruvia::HttpKnownMethod::kHead, "200", "5", {}, 200,
                ruvia::HttpResponseContentSemantics::kWithoutContent, true},
            Case{ruvia::HttpKnownMethod::kGet, "204", {}, {}, 204,
                ruvia::HttpResponseContentSemantics::kWithoutContent, true},
            Case{ruvia::HttpKnownMethod::kGet, "304", "7", {}, 304,
                ruvia::HttpResponseContentSemantics::kWithoutContent, true},
        };
        for (const auto& item : cases) {
            State state(worker.handle, &resource);
            Delivery delivery(state);
            Engine engine(&resource);
            Driver driver(engine);
            RUVIA_CHECK(engine.registerRequest(0, item.method, delivery.eventSink()).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            FakeRead input{.wire = responseHead(item.status, item.contentLength), .fin = true};
            if (!item.body.empty()) {
                appendFrame(input.wire, 0, item.body);
            }
            RUVIA_CHECK(driver.drive(0, input).status == Driver::Status::kProgress);
            RUVIA_CHECK(planMatches(delivery.responseBodyPlan(), item.method, item.statusCode,
                item.semantics, item.bodySuppressed));
            if (item.method == ruvia::HttpKnownMethod::kHead) {
                const auto length = std::find_if(state.headers.begin(), state.headers.end(),
                    [](const ruvia::HttpHeader& header) { return header.name() == "content-length"; });
                RUVIA_CHECK(length != state.headers.end() && length->value() == "5");
            }
            if (!item.body.empty()) {
                RUVIA_CHECK(std::string_view(state.pending) == item.body);
            }
            const auto complete = driver.drive(0, input);
            RUVIA_CHECK(complete.status == Driver::Status::kResponseComplete);
            RUVIA_CHECK(delivery.commit(complete) == Delivery::CommitStatus::kCommitted);
            RUVIA_CHECK(planMatches(delivery.responseBodyPlan(), item.method, item.statusCode,
                item.semantics, item.bodySuppressed));
            RUVIA_CHECK(engine.release(0));
        }

        State informationalState(worker.handle, &resource);
        Delivery informationalDelivery(informationalState);
        Engine informationalEngine(&resource);
        Driver informationalDriver(informationalEngine);
        RUVIA_CHECK(informationalEngine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                                           informationalDelivery.eventSink())
                        .scope == ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead informational{.wire = responseHead("103")};
        RUVIA_CHECK(informationalDriver.drive(0, informational).status == Driver::Status::kProgress);
        RUVIA_CHECK(!informationalDelivery.responseBodyPlan());
        RUVIA_CHECK_EQ(informationalState.informational.size(), std::size_t{1});
        RUVIA_CHECK_EQ(informationalState.informational.front().status().value(), std::uint16_t{103});
        RUVIA_CHECK_EQ(informationalState.informational.front().headers().front().value(), "copied");
        FakeRead finalHead{.wire = responseHead("200"), .fin = true};
        RUVIA_CHECK(informationalDriver.drive(0, finalHead).status == Driver::Status::kProgress);
        RUVIA_CHECK(planMatches(informationalDelivery.responseBodyPlan(), ruvia::HttpKnownMethod::kGet,
            200, ruvia::HttpResponseContentSemantics::kWithContent, false));
        const auto informationalEnd = informationalDriver.drive(0, finalHead);
        RUVIA_CHECK(informationalDelivery.commit(informationalEnd) == Delivery::CommitStatus::kCommitted);
        RUVIA_CHECK(planMatches(informationalDelivery.responseBodyPlan(), ruvia::HttpKnownMethod::kGet,
            200, ruvia::HttpResponseContentSemantics::kWithContent, false));
        RUVIA_CHECK(informationalEngine.release(0));

        State resetState(worker.handle, &resource);
        Delivery resetDelivery(resetState);
        Engine resetEngine(&resource);
        Driver resetDriver(resetEngine);
        RUVIA_CHECK(resetEngine.registerRequest(0, ruvia::HttpKnownMethod::kGet, resetDelivery.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead observedHead{.wire = responseHead("200")};
        RUVIA_CHECK(resetDriver.drive(0, observedHead).status == Driver::Status::kProgress);
        RUVIA_CHECK(planMatches(resetDelivery.responseBodyPlan(), ruvia::HttpKnownMethod::kGet, 200,
            ruvia::HttpResponseContentSemantics::kWithContent, false));
        FakeRead reset{.reset = true};
        const auto resetResult = resetDriver.drive(0, reset);
        RUVIA_CHECK(resetDelivery.commit(resetResult) == Delivery::CommitStatus::kCommitted);
        RUVIA_CHECK(planMatches(resetDelivery.responseBodyPlan(), ruvia::HttpKnownMethod::kGet, 200,
            ruvia::HttpResponseContentSemantics::kWithContent, false));
        RUVIA_CHECK(resetEngine.release(0));

        State noHeadState(worker.handle, &resource);
        Delivery noHeadDelivery(noHeadState);
        Engine noHeadEngine(&resource);
        Driver noHeadDriver(noHeadEngine);
        RUVIA_CHECK(noHeadEngine.registerRequest(0, ruvia::HttpKnownMethod::kGet, noHeadDelivery.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead noHeadReset{.reset = true};
        RUVIA_CHECK(noHeadDelivery.commit(noHeadDriver.drive(0, noHeadReset)) ==
                    Delivery::CommitStatus::kCommitted);
        RUVIA_CHECK(!noHeadDelivery.responseBodyPlan());
        RUVIA_CHECK(noHeadEngine.release(0));
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_uses_physical_buffered_occupancy_and_defers_overflow) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    {
        State state(worker.handle, &resource);
        state.bufferedLimit = 8;
        state.buffered.assign("abcde");
        state.offset = 4;
        state.pending.assign("xy");
        Delivery delivery(state);
        Engine engine(&resource);
        Driver driver(engine);

        auto operation = [&]() -> ruvia::Task<void> {
            RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                                  delivery.eventSink())
                            .scope == ruvia::Http3ConnectionErrorScope::kNone);
            FakeRead head{.wire = responseHead()};
            RUVIA_CHECK(driver.drive(0, head).status == Driver::Status::kProgress);

            const auto allowance = delivery.readAllowance(64);
            RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
            RUVIA_CHECK_EQ(allowance.bytes, std::size_t{1});

            FakeRead body{.wire = {}};
            appendFrame(body.wire, 0, "zz");
            // A compliant producer must stop once its body storage is full.
            // Exercise overflow independently by deliberately delivering a
            // complete DATA block larger than the available single byte.
            const auto result = driver.drive(0, body);
            RUVIA_CHECK(result.status == Driver::Status::kProgress);

            RUVIA_CHECK(delivery.retirementReason() ==
                        Delivery::RetirementReason::kResponseTooLarge);
            const auto blocked = delivery.readAllowance(64);
            RUVIA_CHECK(blocked.status == Delivery::ReadStatus::kRetirementRequired);
            RUVIA_CHECK_EQ(blocked.bytes, std::size_t{0});
            RUVIA_CHECK(delivery.commit(result) == Delivery::CommitStatus::kRetirementRequired);
            RUVIA_CHECK(!state.complete);
            RUVIA_CHECK(!state.errorCode);
            RUVIA_CHECK_EQ(std::string_view(state.pending), std::string_view("xy"));
            RUVIA_CHECK_EQ(std::string_view(state.buffered), std::string_view("abcde"));

            // Represents the owner completing QUIC STOP_SENDING and parser
            // retirement after drive() returned; neither is performed by callback.
            RUVIA_CHECK(engine.cancelRequest(0));
            RUVIA_CHECK(delivery.commitRetirementFailure(
                ruvia::HttpClientError::Code::kResponseTooLarge));
            RUVIA_CHECK(state.complete);
            RUVIA_CHECK(state.errorCode == static_cast<std::uint8_t>(
                                               ruvia::HttpClientError::Code::kResponseTooLarge));
            RUVIA_CHECK(engine.release(0));
            co_return;
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_commits_reset_only_after_owner_observes_it) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    {
        State state(worker.handle, &resource);
        Delivery delivery(state);
        Engine engine(&resource);
        Driver driver(engine);

        auto operation = [&]() -> ruvia::Task<void> {
            RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                                  delivery.eventSink())
                            .scope == ruvia::Http3ConnectionErrorScope::kNone);
            FakeRead head{.wire = responseHead()};
            RUVIA_CHECK(driver.drive(0, head).status == Driver::Status::kProgress);
            FakeRead reset{.reset = true};
            const auto failed = driver.drive(0, reset);
            RUVIA_CHECK(failed.status == Driver::Status::kStreamReset);
            RUVIA_CHECK(!state.complete);
            RUVIA_CHECK(!state.errorCode);
            RUVIA_CHECK(delivery.commit(failed) == Delivery::CommitStatus::kCommitted);
            RUVIA_CHECK(state.complete);
            RUVIA_CHECK(state.errorCode == static_cast<std::uint8_t>(
                                               ruvia::HttpClientError::Code::kProtocolError));
            RUVIA_CHECK(engine.release(0));
            co_return;
        };
        runOperation(worker, io, operation);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_overflow_isolates_other_streams) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        State rejected(worker.handle, &resource);
        State accepted(worker.handle, &resource);
        rejected.bufferedLimit = 1;
        rejected.collectAll = accepted.collectAll = true;
        Delivery first(rejected);
        Delivery second(accepted);
        Engine engine(&resource);
        Driver driver(engine);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                              first.eventSink())
                        .scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(engine.registerRequest(4, ruvia::HttpKnownMethod::kGet,
                              second.eventSink())
                        .scope == ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead overflowing{.wire = responseHead()};
        appendFrame(overflowing.wire, 0, "too large");
        const auto overflow = driver.drive(0, overflowing);
        RUVIA_CHECK(overflow.status == Driver::Status::kProgress);
        RUVIA_CHECK(first.commit(overflow) == Delivery::CommitStatus::kRetirementRequired);
        RUVIA_CHECK(!rejected.complete && !accepted.complete);
        // Simulated transport ceases all delivery before local parser retirement.
        RUVIA_CHECK(engine.cancelRequest(0));
        RUVIA_CHECK(engine.release(0));
        RUVIA_CHECK(first.commitRetirementFailure(ruvia::HttpClientError::Code::kResponseTooLarge));

        FakeRead normal{.wire = responseHead(), .fin = true};
        appendFrame(normal.wire, 0, "survives");
        const auto progress = driver.drive(4, normal);
        RUVIA_CHECK(second.commit(progress) == Delivery::CommitStatus::kPending);
        const auto end = driver.drive(4, normal);
        RUVIA_CHECK(second.commit(end) == Delivery::CommitStatus::kCommitted);
        RUVIA_CHECK(accepted.complete && !accepted.errorCode && accepted.pending == "survives");
        RUVIA_CHECK(engine.release(4));
        RUVIA_CHECK(engine.registerRequest(8, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(engine.cancelRequest(8) && engine.release(8));
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_connection_error_precedes_local_overflow) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        State state(worker.handle, &resource);
        state.bufferedLimit = 0;
        state.collectAll = true;
        Delivery delivery(state);
        Engine engine(&resource);
        Driver driver(engine);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                              delivery.eventSink())
                        .scope == ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead input{.wire = responseHead()};
        appendFrame(input.wire, 0, "overflow");
        appendFrame(input.wire, 4, "");  // SETTINGS is illegal on a response stream.
        const auto result = driver.drive(0, input);
        RUVIA_CHECK(delivery.retirementReason() == Delivery::RetirementReason::kResponseTooLarge);
        RUVIA_CHECK(result.status == Driver::Status::kConnectionError);
        // The fake transport is now considered closed; only then commit failure.
        RUVIA_CHECK(delivery.commit(result) == Delivery::CommitStatus::kCommitted);
        RUVIA_CHECK(state.complete && state.errorCode ==
                                          static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kProtocolError));
        RUVIA_CHECK(!delivery.commitRetirementFailure(ruvia::HttpClientError::Code::kResponseTooLarge));
        RUVIA_CHECK(engine.release(0));
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http3_client_response_delivery_collect_all_wakes_paused_producer_and_preserves_retry) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto resultBudget = std::make_shared<ruvia::detail::HttpClientResultBudgetDomain>(
        ruvia::HttpClientResultBudgetConfig{});
    auto operation = [&]() -> ruvia::Task<void> {
        State state(worker.handle, &resource);
        state.resultBudgetDomain = &resultBudget;
        state.bufferedLimit = 3;
        Delivery delivery(state);
        Engine engine(&resource);
        Driver driver(engine);
        ruvia::TaskScope tasks(worker.handle, {.resource = &resource});
        ruvia::WorkerSignal paused(worker.handle);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                              delivery.eventSink())
                        .scope == ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead initial{.wire = responseHead()};
        appendFrame(initial.wire, 0, "abc");
        RUVIA_CHECK(driver.drive(0, initial).status == Driver::Status::kProgress);
        RUVIA_CHECK(state.pending == "abc" && !state.complete);
        FakeRead tail{.wire = responseTrailer(), .fin = true};
        bool waitedForSpace = false;
        auto producer = [&]() -> ruvia::Task<void> {
            try {
                for (std::size_t step = 0; step < tail.wire.size() + 4 && !state.complete; ++step) {
                    const auto allowance = delivery.readAllowance(Driver::kReadBlockBytes);
                    if (allowance.status == Delivery::ReadStatus::kBackpressured) {
                        waitedForSpace = true;
                        paused.notify();
                        co_await state.spaceSignal.wait();
                        continue;
                    }
                    if (allowance.status != Delivery::ReadStatus::kReady || allowance.bytes == 0) {
                        throw std::logic_error("collecting producer cannot make protocol progress");
                    }
                    const auto input = driver.drive(0, tail, allowance.bytes);
                    (void)delivery.commit(input);
                }
                if (!state.complete) {
                    throw std::logic_error("collecting producer exceeded work bound");
                }
            } catch (...) {
                // Simulated transport terminates delivery before engine teardown.
                (void)engine.stop();
                (void)delivery.commitFailure(std::current_exception());
                paused.notify();
            }
        };
        tasks.spawn(producer());
        co_await paused.wait();
        bool tooSmall = false;
        try {
            (void)co_await ruvia::detail::makeScopedOperation(state.bodyOperationScope, state.readAll(2));
        } catch (const ruvia::HttpClientError& error) {
            tooSmall = error.code() == ruvia::HttpClientError::Code::kResponseTooLarge;
        }
        co_await tasks.join();
        RUVIA_CHECK(waitedForSpace && tooSmall && state.complete);
        RUVIA_CHECK(!state.failure && !state.errorCode);
        RUVIA_CHECK(state.pending == "abc" && state.offset == 0);
        const auto body = co_await ruvia::detail::makeScopedOperation(state.bodyOperationScope, state.readAll(3));
        const auto bodyBytes = body.bytes();
        RUVIA_CHECK(bodyBytes.size() == 3 && bodyBytes.front() == std::byte{'a'} &&
                    bodyBytes.back() == std::byte{'c'});
        RUVIA_CHECK_EQ(state.trailers.size(), std::size_t{1});
        RUVIA_CHECK(engine.release(0));
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_collect_all_probes_through_trailers_and_fin) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        for (const auto limit : {std::size_t{0}, std::size_t{3}}) {
            for (std::size_t size = 0; size <= limit + 1; ++size) {
                State state(worker.handle, &resource);
                state.bufferedLimit = limit;
                state.collectAll = true;
                Delivery delivery(state);
                Engine engine(&resource);
                Driver driver(engine);
                RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                                      delivery.eventSink())
                                .scope == ruvia::Http3ConnectionErrorScope::kNone);
                FakeRead input{.wire = responseHead(), .fin = true};
                appendFrame(input.wire, 0, std::string(size, 'x'));
                appendFrame(input.wire, 0, "");
                const auto trailer = responseTrailer();
                input.wire.insert(input.wire.end(), trailer.begin(), trailer.end());
                bool settled = false;
                for (std::size_t step = 0; step < input.wire.size() + 2 && !settled; ++step) {
                    const auto allowance = delivery.readAllowance(4);
                    RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
                    if (allowance.bytes == 0) {
                        break;
                    }
                    const auto result = driver.drive(0, input, allowance.bytes);
                    const auto committed = delivery.commit(result);
                    if (committed == Delivery::CommitStatus::kRetirementRequired) {
                        RUVIA_CHECK(size > limit);
                        // Fake transport stops delivering this stream here.
                        if (!engine.response(0)) {
                            RUVIA_CHECK(engine.cancelRequest(0));
                        }
                        RUVIA_CHECK(delivery.commitRetirementFailure(
                            ruvia::HttpClientError::Code::kResponseTooLarge));
                    }
                    settled = state.complete;
                }
                RUVIA_CHECK(settled);
                RUVIA_CHECK(state.pending.size() <= limit);
                if (size <= limit) {
                    RUVIA_CHECK(!state.errorCode && !state.failure);
                    RUVIA_CHECK_EQ(state.pending.size(), size);
                    RUVIA_CHECK_EQ(state.trailers.size(), std::size_t{1});
                } else {
                    RUVIA_CHECK(state.errorCode == static_cast<std::uint8_t>(
                                                       ruvia::HttpClientError::Code::kResponseTooLarge));
                }
                RUVIA_CHECK(engine.release(0));
            }
        }
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_shared_budget_backpressures_per_stream_and_resumes_after_consumption) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::detail::Http3ClientBodyBudget budget(6);
        State firstState(worker.handle, &resource);
        State secondState(worker.handle, &resource);
        State waitingState(worker.handle, &resource);
        State framingState(worker.handle, &resource);
        firstState.bufferedLimit = secondState.bufferedLimit = 3;
        waitingState.bufferedLimit = framingState.bufferedLimit = 3;
        framingState.collectAll = true;
        Delivery first(firstState, &budget);
        Delivery second(secondState, &budget);
        Delivery waiting(waitingState, &budget);
        Delivery framing(framingState, &budget);
        Engine engine(&resource, budget);
        Driver driver(engine);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet, first.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(engine.registerRequest(4, ruvia::HttpKnownMethod::kGet, second.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead firstHead{.wire = responseHead()};
        RUVIA_CHECK(driver.drive(0, firstHead).status == Driver::Status::kProgress);
        FakeRead firstBody{.wire = frame(0, std::span<const char>("abc", 3))};
        for (std::size_t step = 0; step < firstBody.wire.size() + 2 && firstState.pending.size() != 3;
            ++step) {
            const auto allowance = first.readAllowance(Driver::kReadBlockBytes);
            RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
            RUVIA_CHECK(driver.drive(0, firstBody, allowance.bytes).status == Driver::Status::kProgress);
        }
        RUVIA_CHECK_EQ(firstState.pending, std::string_view("abc"));
        RUVIA_CHECK(first.readAllowance(Driver::kReadBlockBytes).status ==
                    Delivery::ReadStatus::kBackpressured);
        RUVIA_CHECK_EQ(budget.used(), std::size_t{3});

        FakeRead secondHead{.wire = responseHead()};
        RUVIA_CHECK(driver.drive(4, secondHead).status == Driver::Status::kProgress);
        RUVIA_CHECK(second.readAllowance(Driver::kReadBlockBytes).status ==
                    Delivery::ReadStatus::kReady);
        FakeRead secondBody{.wire = frame(0, std::span<const char>("xyz", 3))};
        for (std::size_t step = 0; step < secondBody.wire.size() + 2 && secondState.pending.size() != 3;
            ++step) {
            const auto allowance = second.readAllowance(Driver::kReadBlockBytes);
            RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
            RUVIA_CHECK(driver.drive(4, secondBody, allowance.bytes).status == Driver::Status::kProgress);
        }
        RUVIA_CHECK_EQ(secondState.pending, std::string_view("xyz"));
        RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
        RUVIA_CHECK(waiting.readAllowance(Driver::kReadBlockBytes).status ==
                    Delivery::ReadStatus::kBackpressured);
        const auto framingProbe = framing.readAllowance(Driver::kReadBlockBytes);
        RUVIA_CHECK(framingProbe.status == Delivery::ReadStatus::kReady && framingProbe.bytes == 1);

        const auto borrowed = co_await firstState.read<std::string_view>();
        RUVIA_CHECK(borrowed && *borrowed == "abc");
        RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
        firstState.releaseConsumedBodyPrefix();
        first.reconcileBodyBytes();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{3});
        RUVIA_CHECK(first.readAllowance(Driver::kReadBlockBytes).status ==
                    Delivery::ReadStatus::kReady);
        RUVIA_CHECK(waiting.readAllowance(Driver::kReadBlockBytes).status ==
                    Delivery::ReadStatus::kReady);
        RUVIA_CHECK(framing.readAllowance(Driver::kReadBlockBytes).status ==
                    Delivery::ReadStatus::kReady);

        RUVIA_CHECK(engine.cancelRequest(0) && engine.release(0));
        RUVIA_CHECK(engine.cancelRequest(4) && engine.release(4));
        firstState.discardResponseBody();
        first.reconcileBodyBytes();
        secondState.discardResponseBody();
        second.reconcileBodyBytes();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_two_collect_all_streams_parse_trailers_and_fin_at_shared_limit) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::detail::Http3ClientBodyBudget budget(6);
        State firstState(worker.handle, &resource);
        State secondState(worker.handle, &resource);
        firstState.bufferedLimit = secondState.bufferedLimit = 4;
        firstState.collectAll = secondState.collectAll = true;
        Delivery first(firstState, &budget);
        Delivery second(secondState, &budget);
        Engine engine(&resource, budget);
        Driver driver(engine);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet, first.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(engine.registerRequest(4, ruvia::HttpKnownMethod::kGet, second.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead firstHead{.wire = responseHead()};
        FakeRead secondHead{.wire = responseHead()};
        RUVIA_CHECK(driver.drive(0, firstHead).status == Driver::Status::kProgress);
        RUVIA_CHECK(driver.drive(4, secondHead).status == Driver::Status::kProgress);

        FakeRead firstBody{.wire = frame(0, std::span<const char>("abc", 3))};
        for (std::size_t step = 0; step < firstBody.wire.size() + 2 && firstState.pending.size() != 3;
            ++step) {
            const auto allowance = first.readAllowance(Driver::kReadBlockBytes);
            RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
            RUVIA_CHECK(driver.drive(0, firstBody, allowance.bytes).status == Driver::Status::kProgress);
        }
        FakeRead secondBody{.wire = frame(0, std::span<const char>("xyz", 3))};
        for (std::size_t step = 0; step < secondBody.wire.size() + 2 && secondState.pending.size() != 3;
            ++step) {
            const auto allowance = second.readAllowance(Driver::kReadBlockBytes);
            RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
            RUVIA_CHECK(driver.drive(4, secondBody, allowance.bytes).status == Driver::Status::kProgress);
        }
        RUVIA_CHECK_EQ(firstState.pending, std::string_view("abc"));
        RUVIA_CHECK_EQ(secondState.pending, std::string_view("xyz"));
        RUVIA_CHECK_EQ(budget.used(), std::size_t{6});

        const auto trailer = responseTrailer();
        FakeRead firstTail{.wire = trailer, .fin = true};
        FakeRead secondTail{.wire = trailer, .fin = true};
        auto finish = [&](std::uint64_t id, FakeRead& input, Delivery& delivery, State& state) {
            for (std::size_t step = 0; step < input.wire.size() + 2 && !state.complete; ++step) {
                const auto allowance = delivery.readAllowance(Driver::kReadBlockBytes);
                RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
                RUVIA_CHECK_EQ(allowance.bytes, std::size_t{1});
                const auto result = driver.drive(id, input, allowance.bytes);
                if (result.status == Driver::Status::kResponseComplete) {
                    RUVIA_CHECK(delivery.commit(result) == Delivery::CommitStatus::kCommitted);
                } else {
                    RUVIA_CHECK(result.status == Driver::Status::kProgress);
                }
            }
            RUVIA_CHECK(state.complete && state.trailers.size() == 1);
        };
        finish(0, firstTail, first, firstState);
        finish(4, secondTail, second, secondState);
        RUVIA_CHECK(engine.release(0));
        RUVIA_CHECK(engine.release(4));
        firstState.discardResponseBody();
        first.reconcileBodyBytes();
        secondState.discardResponseBody();
        second.reconcileBodyBytes();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_budget_survives_connection_generations_and_wakes_drivers) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    ruvia::detail::Http3ClientBodyBudget budget(32);
    WakeCounter oldGenerationWake;
    WakeCounter newGenerationWake;
    ruvia::detail::Http3ClientBodyBudget::WakeRegistration oldGenerationRegistration(
        budget, WakeCounter::notify, &oldGenerationWake);
    ruvia::detail::Http3ClientBodyBudget::WakeRegistration newGenerationRegistration(
        budget, WakeCounter::notify, &newGenerationWake);
    auto operation = [&]() -> ruvia::Task<void> {
        const std::string firstBody(32, 'a');
        const std::string secondBody(16, 'b');
        State oldGenerationState(worker.handle, &resource);
        State newGenerationState(worker.handle, &resource);
        Delivery oldGeneration(oldGenerationState, &budget);
        Delivery newGeneration(newGenerationState, &budget);
        Engine oldEngine(&resource);
        Engine newEngine(&resource);
        Driver oldDriver(oldEngine);
        Driver newDriver(newEngine);
        RUVIA_CHECK(oldEngine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                                 oldGeneration.eventSink())
                        .scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(newEngine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                                 newGeneration.eventSink())
                        .scope == ruvia::Http3ConnectionErrorScope::kNone);

        FakeRead firstInput{.wire = responseHead()};
        appendFrame(firstInput.wire, 0, firstBody);
        for (std::size_t step = 0; step < firstInput.wire.size() + 2 &&
                                   oldGenerationState.pending.size() != firstBody.size();
            ++step) {
            const auto allowance = oldGeneration.readAllowance(Driver::kReadBlockBytes);
            RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
            RUVIA_CHECK(oldDriver.drive(0, firstInput, allowance.bytes).status ==
                        Driver::Status::kProgress);
        }
        RUVIA_CHECK_EQ(budget.used(), firstBody.size());
        RUVIA_CHECK(newGeneration.readAllowance(Driver::kReadBlockBytes).status ==
                    Delivery::ReadStatus::kBackpressured);
        // Changing the read policy must wake the QUIC driver even if no
        // storage has yet been freed (the peer may have trailers or FIN).
        const auto oldBeforePolicy = oldGenerationWake.notifications;
        const auto newBeforePolicy = newGenerationWake.notifications;
        oldGenerationState.collectAll = true;
        oldGenerationState.notifyProducerSpace();
        RUVIA_CHECK_EQ(budget.used(), firstBody.size());
        RUVIA_CHECK_EQ(oldGenerationWake.notifications, oldBeforePolicy + 1);
        RUVIA_CHECK_EQ(newGenerationWake.notifications, newBeforePolicy + 1);

        const auto borrowed = co_await oldGenerationState.read<std::string_view>();
        RUVIA_CHECK(borrowed && *borrowed == firstBody);
        RUVIA_CHECK_EQ(budget.used(), firstBody.size());
        // The borrowed bytes remain charged until the next body operation frees
        // the consumed buffered prefix; this release wakes both generations.
        const auto oldBeforeRelease = oldGenerationWake.notifications;
        const auto newBeforeRelease = newGenerationWake.notifications;
        oldGenerationState.releaseConsumedBodyPrefix();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK_EQ(oldGenerationWake.notifications, oldBeforeRelease + 2);
        RUVIA_CHECK_EQ(newGenerationWake.notifications, newBeforeRelease + 2);
        RUVIA_CHECK(newGeneration.readAllowance(Driver::kReadBlockBytes).status ==
                    Delivery::ReadStatus::kReady);

        FakeRead secondInput{.wire = responseHead()};
        appendFrame(secondInput.wire, 0, secondBody);
        for (std::size_t step = 0; step < secondInput.wire.size() + 2 &&
                                   newGenerationState.pending.size() != secondBody.size();
            ++step) {
            const auto allowance = newGeneration.readAllowance(Driver::kReadBlockBytes);
            RUVIA_CHECK(allowance.status == Delivery::ReadStatus::kReady);
            RUVIA_CHECK(newDriver.drive(0, secondInput, allowance.bytes).status ==
                        Driver::Status::kProgress);
        }
        RUVIA_CHECK_EQ(budget.used(), secondBody.size());
        const auto oldBeforeDiscard = oldGenerationWake.notifications;
        const auto newBeforeDiscard = newGenerationWake.notifications;
        newGenerationState.discardResponseBody();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK_EQ(oldGenerationWake.notifications, oldBeforeDiscard + 2);
        RUVIA_CHECK_EQ(newGenerationWake.notifications, newBeforeDiscard + 2);
        RUVIA_CHECK(oldEngine.cancelRequest(0) && oldEngine.release(0));
        RUVIA_CHECK(newEngine.cancelRequest(0) && newEngine.release(0));
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_isolates_callback_bad_alloc_and_rolls_back_body_reservation) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::RejectingMemoryResource rejecting;
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::detail::Http3ClientBodyBudget budget(1024);
        State failingState(worker.handle, &rejecting);
        State siblingState(worker.handle, &resource);
        Delivery failing(failingState, &budget);
        Delivery sibling(siblingState, &budget);
        Engine engine(&resource, budget);
        Driver driver(engine);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet, failing.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(engine.registerRequest(4, ruvia::HttpKnownMethod::kGet, sibling.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead failingHead{.wire = responseHead()};
        RUVIA_CHECK(driver.drive(0, failingHead).status == Driver::Status::kProgress);
        rejecting.rejectAllocations(true, 32);
        const std::string body(64, 'x');
        FakeRead failingBody{.wire = frame(0, std::span<const char>(body.data(), body.size()))};
        RUVIA_CHECK(driver.drive(0, failingBody).status == Driver::Status::kProgress);
        RUVIA_CHECK(failing.retirementReason() == Delivery::RetirementReason::kCallbackFailure);
        RUVIA_CHECK(failing.callbackFailure() != nullptr);
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});

        FakeRead siblingInput{.wire = responseHead()};
        appendFrame(siblingInput.wire, 0, "safe");
        RUVIA_CHECK(driver.drive(4, siblingInput).status == Driver::Status::kProgress);
        RUVIA_CHECK_EQ(siblingState.pending, std::string_view("safe"));
        RUVIA_CHECK_EQ(budget.used(), std::size_t{4});

        RUVIA_CHECK(engine.cancelRequest(0));
        RUVIA_CHECK(failing.commitFailure(failing.callbackFailure()));
        RUVIA_CHECK(engine.release(0));
        FakeRead siblingFin{.fin = true};
        const auto complete = driver.drive(4, siblingFin);
        RUVIA_CHECK(complete.status == Driver::Status::kResponseComplete);
        RUVIA_CHECK(sibling.commit(complete) == Delivery::CommitStatus::kCommitted);
        RUVIA_CHECK(engine.release(4));
        siblingState.discardResponseBody();
        sibling.reconcileBodyBytes();
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        rejecting.rejectAllocations(false);
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_connection_protocol_error_wins_after_callback_allocation_failure) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::RejectingMemoryResource rejecting;
    ruvia::test::CountingMemoryResource resource;
    auto operation = [&]() -> ruvia::Task<void> {
        ruvia::detail::Http3ClientBodyBudget budget(1024);
        State state(worker.handle, &rejecting);
        Delivery delivery(state, &budget);
        Engine engine(&resource, budget);
        Driver driver(engine);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet, delivery.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead head{.wire = responseHead()};
        RUVIA_CHECK(driver.drive(0, head).status == Driver::Status::kProgress);

        rejecting.rejectAllocations(true, 32);
        const std::string body(64, 'x');
        auto wire = frame(0, std::span<const char>(body.data(), body.size()));
        appendFrame(wire, 4, {});
        FakeRead malformed{.wire = std::move(wire)};
        const auto result = driver.drive(0, malformed);
        RUVIA_CHECK(result.status == Driver::Status::kConnectionError);
        RUVIA_CHECK(result.protocol.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(delivery.retirementReason() == Delivery::RetirementReason::kCallbackFailure);
        RUVIA_CHECK(delivery.callbackFailure() != nullptr);
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK(delivery.commitFailure(delivery.callbackFailure()));
        (void)engine.stop();
        RUVIA_CHECK(engine.release(0));
        rejecting.rejectAllocations(false);
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_holds_compressed_bytes_until_fin_and_decode_success) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    ruvia::detail::Http3ClientBodyBudget receiveBudget(4096);
    auto operation = [&]() -> ruvia::Task<void> {
        constexpr std::string_view plain = "decoded after the final response byte";
        auto encoded = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, plain,
            {.maxEncodedBytes = 4096, .resource = &resource});
        RUVIA_CHECK(encoded.encoded() != nullptr);
        const auto encodedBytes = encoded.encoded()->bytes();
        State state(worker.handle, &resource);
        state.bufferedLimit = 1024;
        Delivery delivery(state, &receiveBudget);
        Engine engine(&resource);
        Driver driver(engine);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet, delivery.eventSink()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        FakeRead input{.wire = responseHead("200", {}, "gzip")};
        appendFrame(input.wire, 0, encodedBytes);
        RUVIA_CHECK(driver.drive(0, input).status == Driver::Status::kProgress);
        RUVIA_CHECK(state.bodyDecodeRequired && state.collectAll && !state.complete);
        RUVIA_CHECK_EQ(std::string_view(state.pending), encodedBytes);
        RUVIA_CHECK_EQ(receiveBudget.used(), encodedBytes.size());

        bool readReturned = false;
        bool watchdogExpired = false;
        std::optional<std::string_view> readValue;
        ruvia::TaskScope tasks(worker.handle, {.resource = &resource});
        auto consumer = [&]() -> ruvia::Task<void> {
            readValue = co_await state.read<std::string_view>();
            readReturned = true;
        };
        auto watchdog = [&]() -> ruvia::Task<void> {
            (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(100));
            if (!readReturned) {
                watchdogExpired = true;
                state.failure = std::make_exception_ptr(std::runtime_error("compressed read watchdog"));
                state.complete = true;
                state.dataSignal.notify();
            }
        };
        tasks.spawn(consumer());
        tasks.spawn(watchdog());
        (void)co_await ruvia::sleepFor(worker.handle, std::chrono::milliseconds(2));
        RUVIA_CHECK(!readReturned);

        FakeRead fin{.fin = true};
        const auto finished = driver.drive(0, fin);
        RUVIA_CHECK(finished.status == Driver::Status::kResponseComplete);
        ruvia::detail::decodeHttpClientResponseContentEncoding(state, true, 1024, &resource);
        RUVIA_CHECK(!state.bodyDecodeRequired);
        RUVIA_CHECK_EQ(std::string_view(state.buffered), plain);
        RUVIA_CHECK_EQ(receiveBudget.used(), plain.size());
        RUVIA_CHECK(delivery.commit(finished) == Delivery::CommitStatus::kCommitted);
        co_await tasks.join();
        RUVIA_CHECK(readReturned && !watchdogExpired && readValue && *readValue == plain);
        state.discardResponseBody();
        RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
        RUVIA_CHECK(engine.release(0));
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_decode_errors_and_decoded_limits_publish_no_encoded_bytes) {
    auto& io = ruvia::test::newTestIoContext();
    TestWorker worker(io);
    ruvia::test::CountingMemoryResource resource;
    ruvia::detail::Http3ClientBodyBudget receiveBudget(8192);
    auto operation = [&]() -> ruvia::Task<void> {
        struct Case final {
            std::string_view bytes;
            std::size_t decodedLimit;
            ruvia::HttpClientError::Code error;
        };
        constexpr std::array cases{
            Case{"not a gzip member", 1024, ruvia::HttpClientError::Code::kProtocolError},
            Case{"decoded output exceeds its bound", 4, ruvia::HttpClientError::Code::kResponseTooLarge},
        };
        for (const auto& item : cases) {
            std::string wireBody(item.bytes);
            if (item.error == ruvia::HttpClientError::Code::kResponseTooLarge) {
                auto compressed = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip,
                    item.bytes, {.maxEncodedBytes = 4096, .resource = &resource});
                RUVIA_CHECK(compressed.encoded() != nullptr);
                wireBody.assign(compressed.encoded()->bytes());
            }
            State state(worker.handle, &resource);
            State siblingState(worker.handle, &resource);
            state.bufferedLimit = siblingState.bufferedLimit = 1024;
            Delivery delivery(state, &receiveBudget);
            Delivery siblingDelivery(siblingState, &receiveBudget);
            Engine engine(&resource);
            Driver driver(engine);
            RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                                  delivery.eventSink())
                            .scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(engine.registerRequest(4, ruvia::HttpKnownMethod::kGet,
                                  siblingDelivery.eventSink())
                            .scope == ruvia::Http3ConnectionErrorScope::kNone);
            FakeRead input{.wire = responseHead("200", {}, "gzip")};
            appendFrame(input.wire, 0, wireBody);
            RUVIA_CHECK(driver.drive(0, input).status == Driver::Status::kProgress);
            RUVIA_CHECK(state.bodyDecodeRequired && state.collectAll);
            RUVIA_CHECK_EQ(receiveBudget.used(), wireBody.size());
            FakeRead fin{.fin = true};
            const auto finished = driver.drive(0, fin);
            RUVIA_CHECK(finished.status == Driver::Status::kResponseComplete);
            std::exception_ptr failure;
            try {
                ruvia::detail::decodeHttpClientResponseContentEncoding(
                    state, true, item.decodedLimit, &resource);
            } catch (const ruvia::HttpClientError& error) {
                RUVIA_CHECK(error.code() == item.error);
                failure = std::current_exception();
            }
            RUVIA_CHECK(failure != nullptr);
            RUVIA_CHECK(delivery.commitFailure(failure));
            RUVIA_CHECK(state.complete && state.buffered.empty() && state.pending.empty());
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
            bool readFailed = false;
            try {
                (void)co_await state.readAll(1024);
            } catch (const ruvia::HttpClientError& error) {
                readFailed = error.code() == item.error;
            }
            RUVIA_CHECK(readFailed);
            FakeRead siblingInput{.wire = responseHead()};
            appendFrame(siblingInput.wire, 0, "sibling");
            RUVIA_CHECK(driver.drive(4, siblingInput).status == Driver::Status::kProgress);
            FakeRead siblingFin{.fin = true};
            const auto siblingFinished = driver.drive(4, siblingFin);
            RUVIA_CHECK(siblingFinished.status == Driver::Status::kResponseComplete);
            RUVIA_CHECK(siblingDelivery.commit(siblingFinished) == Delivery::CommitStatus::kCommitted);
            RUVIA_CHECK(siblingState.complete && siblingState.pending == "sibling");
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{7});
            siblingState.discardResponseBody();
            RUVIA_CHECK_EQ(receiveBudget.used(), std::size_t{0});
            RUVIA_CHECK(engine.release(0));
            RUVIA_CHECK(engine.release(4));
        }
        co_return;
    };
    runOperation(worker, io, operation);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(http3_client_response_delivery_parser_registration_allocation_failure_leaves_no_entry) {
    ruvia::test::RejectingMemoryResource resource;
    Engine engine(&resource);
    std::size_t callbacks{};
    const auto sink = ruvia::detail::Http3ClientResponseEventSink{
        .callback = [](void* context, const ruvia::Http3ConnectionEvent&) {
            ++*static_cast<std::size_t*>(context);
        },
        .context = &callbacks,
    };
    const auto allocationsBefore = resource.allocationCount();
    resource.rejectAllocations();
    bool allocationFailed = false;
    try {
        (void)engine.registerRequest(0, ruvia::HttpKnownMethod::kGet, sink);
    } catch (const std::bad_alloc&) {
        allocationFailed = true;
    }
    RUVIA_CHECK(allocationFailed);
    RUVIA_CHECK_EQ(resource.allocationCount(), allocationsBefore + 1);
    RUVIA_CHECK_EQ(engine.liveStreamCount(), std::size_t{0});
    RUVIA_CHECK(!engine.response(0));
    RUVIA_CHECK_EQ(callbacks, std::size_t{0});

    resource.rejectAllocations(false);
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet, sink).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK_EQ(engine.liveStreamCount(), std::size_t{1});
    RUVIA_CHECK(engine.stop().status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
    RUVIA_CHECK(engine.response(0).has_value());
    RUVIA_CHECK(engine.release(0));
    RUVIA_CHECK_EQ(engine.liveStreamCount(), std::size_t{0});
    RUVIA_CHECK_EQ(callbacks, std::size_t{0});
}
