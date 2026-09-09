#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Hpack.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/http2/flow/Http2ReceiveWindowCredit.h"

#include "test_harness.h"

namespace {

class AccountingAllocationResource final : public std::pmr::memory_resource {
public:
    explicit AccountingAllocationResource(
        std::size_t failAt = (std::numeric_limits<std::size_t>::max)()) noexcept
        : failAt_(failAt) {}

    [[nodiscard]] std::size_t attempts() const noexcept {
        return attempts_;
    }
    [[nodiscard]] std::size_t liveAllocations() const noexcept {
        return liveAllocations_;
    }
    void switchDefaultOnAllocation(std::pmr::memory_resource* resource) noexcept {
        nextDefault_ = resource;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (nextDefault_ != nullptr) {
            std::pmr::set_default_resource(nextDefault_);
            nextDefault_ = nullptr;
        }
        if (attempts_++ == failAt_) {
            throw std::bad_alloc();
        }
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++liveAllocations_;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        if (liveAllocations_ == 0) {
            std::terminate();
        }
        --liveAllocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t failAt_;
    std::size_t attempts_{0};
    std::size_t liveAllocations_{0};
    std::pmr::memory_resource* nextDefault_{nullptr};
};

class ToggleAllocationResource final : public std::pmr::memory_resource {
public:
    void reject(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};

void appendFrame(std::pmr::string& wire, ruvia::Http2FrameType type, std::uint8_t flags,
    std::uint32_t streamId, std::string_view payload) {
    std::array<char, ruvia::kHttp2FrameHeaderBytes> header{};
    if (!ruvia::encodeHttp2FrameHeader(
            header, static_cast<std::uint32_t>(payload.size()), type, flags, streamId)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

void appendPeerSettings(std::pmr::string& wire) {
    appendFrame(wire, ruvia::Http2FrameType::kSettings, 0, 0, {});
}

void appendResponse(std::pmr::string& wire, std::pmr::memory_resource* resource,
    std::uint32_t streamId, std::string_view status, std::string_view body = {},
    std::string_view trailerName = {}, std::string_view trailerValue = {}) {
    std::pmr::string block(resource);
    ruvia::HpackEncoder::encodeHeader(block, ":status", status);
    const auto headFlags = static_cast<std::uint8_t>(0x4 | (body.empty() && trailerName.empty() ? 0x1 : 0));
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, headFlags, streamId, block);
    if (!body.empty()) {
        appendFrame(wire, ruvia::Http2FrameType::kData,
            static_cast<std::uint8_t>(trailerName.empty() ? 0x1 : 0), streamId, body);
    }
    if (!trailerName.empty()) {
        block.clear();
        ruvia::HpackEncoder::encodeHeader(block, trailerName, trailerValue);
        appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x5, streamId, block);
    }
}

std::pmr::string clientResponseWire(std::pmr::memory_resource* resource, std::string_view body,
    bool includeHeader = false, bool endStream = true) {
    std::pmr::string block(resource);
    ruvia::HpackEncoder::encodeStatus(block, ruvia::http_status::kOk);
    if (includeHeader) {
        ruvia::HpackEncoder::encodeHeader(block, "x-test", "value");
    }

    std::pmr::string wire(resource);
    appendPeerSettings(wire);
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x4, 1, block);
    appendFrame(wire, ruvia::Http2FrameType::kData, endStream ? 0x1 : 0, 1, body);
    return wire;
}

std::pmr::string clientResponseWithTrailersWire(std::pmr::memory_resource* resource) {
    std::pmr::string wire(resource);
    appendPeerSettings(wire);
    std::pmr::string block(resource);
    ruvia::HpackEncoder::encodeStatus(block, ruvia::http_status::kOk);
    ruvia::HpackEncoder::encodeHeader(block, "x-test", "owned");
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x4, 1, block);
    block.clear();
    ruvia::HpackEncoder::encodeHeader(block, "x-trace", "done");
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x5, 1, block);
    return wire;
}

std::pmr::string clientWindowThresholdResponseWire(
    std::pmr::memory_resource* resource, bool endStream) {
    constexpr std::size_t kFramePayloadBytes = 16'384;

    constexpr auto kFrameCount =
        ruvia::detail::kHttp2ReceiveWindowUpdateThreshold / kFramePayloadBytes;
    std::pmr::string block(resource);
    ruvia::HpackEncoder::encodeStatus(block, ruvia::http_status::kOk);
    std::string payload(kFramePayloadBytes, 'x');

    std::pmr::string wire(resource);
    appendPeerSettings(wire);
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x4, 1, block);
    for (std::size_t index = 0; index < kFrameCount; ++index) {
        const auto flags =
            static_cast<std::uint8_t>(endStream && index + 1 == kFrameCount ? 0x1U : 0U);
        appendFrame(wire, ruvia::Http2FrameType::kData, flags, 1, payload);
    }
    return wire;
}

std::pmr::string serverRequestWire(std::pmr::memory_resource* resource, std::string_view body) {
    std::pmr::string block(resource);
    ruvia::HpackEncoder::encodeHeader(block, ":method", "POST");
    ruvia::HpackEncoder::encodeHeader(block, ":scheme", "https");
    ruvia::HpackEncoder::encodeHeader(block, ":authority", "example.test");
    ruvia::HpackEncoder::encodeHeader(block, ":path", "/upload");
    ruvia::HpackEncoder::encodeHeader(block, "content-length", body.empty() ? "0" : "1");

    std::pmr::string wire(ruvia::kHttp2ClientPreface, resource);
    appendPeerSettings(wire);
    const auto headFlags = static_cast<std::uint8_t>(0x4 | (body.empty() ? 0x1 : 0));
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, headFlags, 1, block);
    if (!body.empty()) {
        appendFrame(wire, ruvia::Http2FrameType::kData, 0x1, 1, body);
    }
    return wire;
}

std::pmr::string serverRequestWithTrailersWire(std::pmr::memory_resource* resource) {
    std::pmr::string block(resource);
    ruvia::HpackEncoder::encodeHeader(block, ":method", "POST");
    ruvia::HpackEncoder::encodeHeader(block, ":scheme", "https");
    ruvia::HpackEncoder::encodeHeader(block, ":authority", "example.test");
    ruvia::HpackEncoder::encodeHeader(block, ":path", "/upload");
    ruvia::HpackEncoder::encodeHeader(block, "content-length", "1");

    std::pmr::string wire(ruvia::kHttp2ClientPreface, resource);
    appendPeerSettings(wire);
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x4, 1, block);
    appendFrame(wire, ruvia::Http2FrameType::kData, 0, 1, "x");
    block.clear();
    ruvia::HpackEncoder::encodeHeader(block, "x-request-trace", "done");
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x5, 1, block);
    return wire;
}

ruvia::Http2Connection preparedClient(std::pmr::memory_resource* resource) {
    auto client = ruvia::Http2Connection::client({.resource = resource});
    (void)client.consumeOutput(client.pendingOutput().size());
    const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "GET", .scheme = "https", .authority = "example.test", .target = "/"});
    if (submitted.submitted() == nullptr) {
        throw std::logic_error("test request was not submitted");
    }
    (void)client.consumeOutput(client.pendingOutput().size());
    return client;
}

ruvia::Http2Connection preparedClientMethod(std::pmr::memory_resource* resource,
    std::string_view method) {
    auto client = ruvia::Http2Connection::client({.resource = resource});
    (void)client.consumeOutput(client.pendingOutput().size());
    const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = method, .scheme = "https", .authority = "example.test", .target = "/"});
    if (submitted.submitted() == nullptr) {
        throw std::logic_error("test request was not submitted");
    }
    (void)client.consumeOutput(client.pendingOutput().size());
    return client;
}

}  // namespace

RUVIA_TEST(http2_public_default_resource_is_resolved_once) {
    for (bool clientRole : {false, true}) {
        AccountingAllocationResource original;
        AccountingAllocationResource replacement;
        struct RestoreDefault {
            std::pmr::memory_resource* previous;
            ~RestoreDefault() {
                std::pmr::set_default_resource(previous);
            }
        } restore{std::pmr::set_default_resource(&original)};
        original.switchDefaultOnAllocation(&replacement);
        {
            auto connection = clientRole ? ruvia::Http2Connection::client({})
                                         : ruvia::Http2Connection::server({});
            RUVIA_CHECK(original.liveAllocations() > 0);
            RUVIA_CHECK(replacement.attempts() == 0);
        }
        RUVIA_CHECK(original.liveAllocations() == 0);
        RUVIA_CHECK(replacement.liveAllocations() == 0);
    }
}

RUVIA_TEST(http2_public_construction_failure_returns_all_allocations) {
    for (const auto role : {ruvia::Http2Role::kClient, ruvia::Http2Role::kServer}) {
        AccountingAllocationResource baseline;
        {
            auto connection = role == ruvia::Http2Role::kClient
                                  ? ruvia::Http2Connection::client({.resource = &baseline})
                                  : ruvia::Http2Connection::server({.resource = &baseline});
            RUVIA_CHECK(connection.wantsWrite());
        }
        RUVIA_CHECK(baseline.liveAllocations() == 0);
        for (std::size_t failAt = 0; failAt < baseline.attempts(); ++failAt) {
            AccountingAllocationResource resource(failAt);
            bool threw = false;
            try {
                auto connection = role == ruvia::Http2Role::kClient
                                      ? ruvia::Http2Connection::client({.resource = &resource})
                                      : ruvia::Http2Connection::server({.resource = &resource});
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            RUVIA_CHECK(threw);
            RUVIA_CHECK(resource.liveAllocations() == 0);
        }
    }
}

RUVIA_TEST(http2_public_escaped_events_return_storage_to_original_resource_after_move_assignment) {
    AccountingAllocationResource original;
    AccountingAllocationResource replacement;
    auto wire = serverRequestWire(std::pmr::new_delete_resource(), "x");
    std::optional<ruvia::Http2Event> request;
    std::optional<ruvia::Http2ReceivedDataCredit> credit;
    {
        auto connection = ruvia::Http2Connection::server({.resource = &original});
        RUVIA_CHECK(connection.feed(wire) == ruvia::Http2FeedResult::kAccepted);
        request.emplace(std::move(*connection.nextEvent()));
        auto body = connection.nextEvent();
        RUVIA_CHECK(body && body->messageBodyChunk() != nullptr);
        credit.emplace(body->messageBodyChunk()->takeCredit());
        connection = ruvia::Http2Connection::server({.resource = &replacement});
        RUVIA_CHECK(original.liveAllocations() != 0);
        RUVIA_CHECK(request->requestHead()->request().method() == "POST");
    }
    RUVIA_CHECK(replacement.liveAllocations() == 0);
    request.reset();
    RUVIA_CHECK(original.liveAllocations() != 0);
    credit.reset();
    RUVIA_CHECK(original.liveAllocations() == 0);
}

RUVIA_TEST(http2_public_client_terminal_event_preserves_unacknowledged_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = preparedClient(&resource);
    const auto wire = clientResponseWire(&resource, "x");
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    const auto head = client.nextEvent();
    auto chunk = client.nextEvent();
    auto* body = chunk ? chunk->messageBodyChunk() : nullptr;
    RUVIA_CHECK(head && head->responseHead() != nullptr);
    RUVIA_CHECK(body != nullptr && body->bytes() == "x");
    auto credit = body->takeCredit();

    const auto end = client.nextEvent();
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    RUVIA_CHECK(client.acknowledge(std::move(credit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
    RUVIA_CHECK(client.acknowledge(std::move(credit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kInvalidCredit);
}

RUVIA_TEST(http2_public_dropped_data_credit_returns_debt_and_releases_closed_stream) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = preparedClient(&resource);
    const auto wire = clientResponseWire(&resource, "x");
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    (void)client.nextEvent();
    {
        auto chunk = client.nextEvent();
        auto* body = chunk ? chunk->messageBodyChunk() : nullptr;
        RUVIA_CHECK(body != nullptr);
        auto credit = body->takeCredit();
        const auto end = client.nextEvent();
        RUVIA_CHECK(end && end->messageEnd() != nullptr);
        RUVIA_CHECK(credit.valid());
    }

    RUVIA_CHECK(
        client.submitReset(1, ruvia::Http2ErrorCode::kCancel) == ruvia::Http2SubmitStatus::kClosed);
}

RUVIA_TEST(http2_public_terminal_waits_for_exact_credit_before_window_update) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = preparedClient(&resource);
    const auto wire = clientWindowThresholdResponseWire(&resource, true);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    std::pmr::string drained(&resource);
    client.takeOutput(drained);

    (void)client.nextEvent();
    std::vector<ruvia::Http2ReceivedDataCredit> credits;
    constexpr auto kFrameCount = ruvia::detail::kHttp2ReceiveWindowUpdateThreshold / 16'384;
    credits.reserve(kFrameCount);
    for (std::size_t index = 0; index < kFrameCount; ++index) {
        auto chunk = client.nextEvent();
        credits.push_back(chunk->messageBodyChunk()->takeCredit());
    }
    const auto end = client.nextEvent();
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    RUVIA_CHECK(client.pendingOutput().empty());

    for (std::size_t index = 0; index + 1 < credits.size(); ++index) {
        RUVIA_CHECK(client.acknowledge(std::move(credits[index])) ==
                    ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
        RUVIA_CHECK(client.pendingOutput().empty());
    }
    RUVIA_CHECK(credits.back().valid());
    credits.pop_back();

    const auto output = client.pendingOutput();
    const auto update =
        ruvia::parseHttp2FrameHeader(std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(update.has_value());
    RUVIA_CHECK(
        update && update->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kWindowUpdate));
    RUVIA_CHECK(update && update->streamId == 0);
    RUVIA_CHECK(output.size() == ruvia::kHttp2FrameHeaderBytes + 4);
}

RUVIA_TEST(http2_public_client_reset_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = preparedClient(&resource);
    const auto wire = clientResponseWire(&resource, "x", false, false);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    (void)client.nextEvent();
    auto chunk = client.nextEvent();
    auto* body = chunk ? chunk->messageBodyChunk() : nullptr;
    RUVIA_CHECK(body != nullptr);
    auto credit = body->takeCredit();

    RUVIA_CHECK(client.submitReset(1, ruvia::Http2ErrorCode::kCancel) ==
                ruvia::Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(client.acknowledge(std::move(credit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
}

RUVIA_TEST(http2_public_peer_reset_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = preparedClient(&resource);
    auto wire = clientResponseWire(&resource, "x", false, false);
    constexpr std::array<char, 4> kCancelPayload{0, 0, 0, 8};
    appendFrame(wire, ruvia::Http2FrameType::kRstStream, 0, 1,
        std::string_view(kCancelPayload.data(), kCancelPayload.size()));
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    (void)client.nextEvent();
    auto chunk = client.nextEvent();
    auto* body = chunk ? chunk->messageBodyChunk() : nullptr;
    RUVIA_CHECK(body != nullptr);
    auto credit = body->takeCredit();
    const auto closed = client.nextEvent();
    RUVIA_CHECK(closed && closed->streamClosed() != nullptr);

    RUVIA_CHECK(client.acknowledge(std::move(credit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
}

RUVIA_TEST(http2_public_dropped_credit_retries_failed_window_update_once) {
    ToggleAllocationResource resource;
    auto client = preparedClient(&resource);
    const auto wire = clientWindowThresholdResponseWire(&resource, false);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    std::pmr::string drained(&resource);
    client.takeOutput(drained);

    (void)client.nextEvent();
    std::vector<ruvia::Http2ReceivedDataCredit> credits;
    constexpr auto kFrameCount = ruvia::detail::kHttp2ReceiveWindowUpdateThreshold / 16'384;
    credits.reserve(kFrameCount);
    for (std::size_t index = 0; index < kFrameCount; ++index) {
        auto chunk = client.nextEvent();
        credits.push_back(chunk->messageBodyChunk()->takeCredit());
    }
    resource.reject();
    credits.clear();

    resource.reject(false);
    const auto output = client.pendingOutput();
    RUVIA_CHECK(output.size() == 2 * (ruvia::kHttp2FrameHeaderBytes + 4));
    const auto connectionUpdate =
        ruvia::parseHttp2FrameHeader(std::span<const char>(output.data(), output.size()));
    const auto streamOffset = ruvia::kHttp2FrameHeaderBytes + 4;
    const auto streamUpdate = ruvia::parseHttp2FrameHeader(
        std::span<const char>(output.data() + streamOffset, output.size() - streamOffset));
    RUVIA_CHECK(
        connectionUpdate &&
        connectionUpdate->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kWindowUpdate));
    RUVIA_CHECK(connectionUpdate && connectionUpdate->streamId == 0);
    RUVIA_CHECK(streamUpdate && streamUpdate->type == static_cast<std::uint8_t>(
                                                          ruvia::Http2FrameType::kWindowUpdate));
    RUVIA_CHECK(streamUpdate && streamUpdate->streamId == 1);
    const auto creditedOutputBytes = output.size();
    RUVIA_CHECK(client.pendingOutput().size() == creditedOutputBytes);

    RUVIA_CHECK(client.submitReset(1, ruvia::Http2ErrorCode::kCancel) ==
                ruvia::Http2SubmitStatus::kAccepted);
}

RUVIA_TEST(http2_public_server_release_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    const auto wire = serverRequestWire(&resource, "x");
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    auto request = server.nextEvent();
    auto chunk = server.nextEvent();
    const auto end = server.nextEvent();
    auto* requestHead = request ? request->requestHead() : nullptr;
    auto* body = chunk ? chunk->messageBodyChunk() : nullptr;
    RUVIA_CHECK(requestHead != nullptr);
    RUVIA_CHECK(body != nullptr && body->bytes() == "x");
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    auto credit = body->takeCredit();

    ruvia::HttpResponse response({.resource = &resource});
    RUVIA_CHECK(server.submitBufferedResponse(1, response) == ruvia::Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(server.release(std::move(*requestHead)) ==
                ruvia::Http2ServerRequestReleaseStatus::kReleased);
    RUVIA_CHECK(server.acknowledge(std::move(credit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
}

RUVIA_TEST(http2_public_server_submits_streaming_response_head_and_data) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    const auto wire = serverRequestWire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    auto request = server.nextEvent();
    const auto end = server.nextEvent();
    auto* requestHead = request ? request->requestHead() : nullptr;
    RUVIA_CHECK(requestHead != nullptr);
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    (void)server.consumeOutput(server.pendingOutput().size());

    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    RUVIA_CHECK(server.submitStreamingResponseHead(1, std::move(response)) ==
                ruvia::Http2SubmitStatus::kAccepted);

    const auto headOutput = server.pendingOutput();
    const auto head = ruvia::parseHttp2FrameHeader(
        std::span<const char>(headOutput.data(), headOutput.size()));
    RUVIA_CHECK(head.has_value());
    RUVIA_CHECK(head && head->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kHeaders));
    RUVIA_CHECK(head && head->streamId == 1);
    RUVIA_CHECK(head && (head->flags & 0x1U) == 0);
    (void)server.consumeOutput(headOutput.size());

    RUVIA_CHECK(server.submitData(1, "event: update\n\ndata: ok\n\n",
                    ruvia::Http2EndStream::kEndStream) ==
                ruvia::Http2DataSubmitStatus::kAccepted);
    const auto dataOutput = server.pendingOutput();
    const auto data = ruvia::parseHttp2FrameHeader(
        std::span<const char>(dataOutput.data(), dataOutput.size()));
    RUVIA_CHECK(data.has_value());
    RUVIA_CHECK(data && data->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData));
    RUVIA_CHECK(data && (data->flags & 0x1U) != 0);

    RUVIA_CHECK(server.release(std::move(*requestHead)) ==
                ruvia::Http2ServerRequestReleaseStatus::kReleased);
}

RUVIA_TEST(http2_public_streaming_response_head_maps_submission_failures) {
    std::pmr::monotonic_buffer_resource resource;

    auto closed = ruvia::Http2Connection::server({.resource = &resource});
    ruvia::HttpResponse closedResponse({.resource = &resource});
    RUVIA_CHECK(closed.submitStreamingResponseHead(1, std::move(closedResponse)) ==
                ruvia::Http2SubmitStatus::kClosed);

    auto client = preparedClient(&resource);
    ruvia::HttpResponse clientResponse({.resource = &resource});
    RUVIA_CHECK(client.submitStreamingResponseHead(1, std::move(clientResponse)) ==
                ruvia::Http2SubmitStatus::kInvalidState);

    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    const auto wire = serverRequestWire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    auto request = server.nextEvent();
    const auto end = server.nextEvent();
    auto* requestHead = request ? request->requestHead() : nullptr;
    RUVIA_CHECK(requestHead != nullptr);
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    (void)server.consumeOutput(server.pendingOutput().size());

    ruvia::HttpResponse invalidResponse({.resource = &resource});
    invalidResponse.header("Content-Length", "invalid");
    RUVIA_CHECK(server.submitStreamingResponseHead(1, std::move(invalidResponse)) ==
                ruvia::Http2SubmitStatus::kInvalidMessage);
    RUVIA_CHECK(server.pendingOutput().empty());
    RUVIA_CHECK(server.release(std::move(*requestHead)) ==
                ruvia::Http2ServerRequestReleaseStatus::kReleased);
}

RUVIA_TEST(http2_public_dropped_request_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    const auto wire = serverRequestWire(&resource, "x");
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    auto request = server.nextEvent();
    auto chunk = server.nextEvent();
    auto* body = chunk ? chunk->messageBodyChunk() : nullptr;
    RUVIA_CHECK(request && request->requestHead() != nullptr);
    RUVIA_CHECK(body != nullptr);
    auto credit = body->takeCredit();

    request.reset();
    RUVIA_CHECK(server.acknowledge(std::move(credit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
}

RUVIA_TEST(http2_public_dropped_request_event_abandons_its_stream) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    const auto wire = serverRequestWire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    (void)server.consumeOutput(server.pendingOutput().size());

    {
        const auto request = server.nextEvent();
        RUVIA_CHECK(request && request->requestHead() != nullptr);
    }

    const auto output = server.pendingOutput();
    const auto frame =
        ruvia::parseHttp2FrameHeader(std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(frame.has_value());
    RUVIA_CHECK(
        frame && frame->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kRstStream));
}

RUVIA_TEST(http2_public_dropped_request_retries_failed_abandonment) {
    ToggleAllocationResource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    std::pmr::string initialOutput(&resource);
    server.takeOutput(initialOutput);
    const auto wire = serverRequestWire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead() != nullptr);
    resource.reject();
    request.reset();

    resource.reject(false);
    const auto output = server.pendingOutput();
    constexpr auto kSettingsAckBytes = ruvia::kHttp2FrameHeaderBytes;
    RUVIA_CHECK(output.size() == kSettingsAckBytes + ruvia::kHttp2FrameHeaderBytes + 4);
    const auto reset = ruvia::parseHttp2FrameHeader(std::span<const char>(
        output.data() + kSettingsAckBytes, output.size() - kSettingsAckBytes));
    RUVIA_CHECK(
        reset && reset->type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kRstStream));
    RUVIA_CHECK(reset && reset->streamId == 1);
}

RUVIA_TEST(http2_public_request_endpoint_survives_connection_destruction_without_aba) {
    std::pmr::monotonic_buffer_resource resource;
    auto escaped = [&]() {
        auto server = ruvia::Http2Connection::server({.resource = &resource});
        (void)server.consumeOutput(server.pendingOutput().size());
        const auto wire = serverRequestWire(&resource, {});
        (void)server.feed(wire);
        return server.nextEvent();
    }();
    RUVIA_CHECK(escaped && escaped->requestHead() != nullptr);

    auto other = ruvia::Http2Connection::server({.resource = &resource});
    RUVIA_CHECK(other.release(std::move(*escaped->requestHead())) ==
                ruvia::Http2ServerRequestReleaseStatus::kInvalidLease);
    escaped.reset();
}

RUVIA_TEST(http2_public_data_credit_endpoint_survives_connection_destruction_without_aba) {
    std::pmr::monotonic_buffer_resource resource;
    auto escapedCredit = [&]() {
        auto client = preparedClient(&resource);
        const auto wire = clientResponseWire(&resource, "x");
        (void)client.feed(wire);
        (void)client.nextEvent();
        auto chunk = client.nextEvent();
        return chunk->messageBodyChunk()->takeCredit();
    }();
    RUVIA_CHECK(escapedCredit.valid());

    auto other = preparedClient(&resource);
    RUVIA_CHECK(other.acknowledge(std::move(escapedCredit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kInvalidCredit);
    RUVIA_CHECK(escapedCredit.valid());
}

RUVIA_TEST(http2_public_request_views_survive_connection_move_assignment) {
    auto* resource = std::pmr::new_delete_resource();
    // Keep caller-owned input alive so this test isolates the connection's decoded storage.
    const auto wire = serverRequestWire(resource, "x");
    std::optional<ruvia::Http2Event> escapedRequest;
    std::optional<ruvia::Http2Event> escapedChunk;

    auto server = ruvia::Http2Connection::server({.resource = resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead() != nullptr);
    if (request) {
        escapedRequest.emplace(std::move(*request));
    }
    auto chunk = server.nextEvent();
    RUVIA_CHECK(chunk && chunk->messageBodyChunk() != nullptr);
    if (chunk) {
        escapedChunk.emplace(std::move(*chunk));
    }

    // Move assignment destroys the old implementation while the public events still hold leases.
    auto replacement = ruvia::Http2Connection::server({.resource = resource});
    server = std::move(replacement);

    const auto* requestHead = escapedRequest ? escapedRequest->requestHead() : nullptr;
    RUVIA_CHECK(requestHead != nullptr);
    if (requestHead != nullptr) {
        const auto& materialized = requestHead->request();
        RUVIA_CHECK(materialized.method() == "POST");
        RUVIA_CHECK(materialized.target() == "/upload");
        RUVIA_CHECK(materialized.authority() == "example.test");
        const auto contentLength = materialized.header("content-length");
        RUVIA_CHECK(contentLength && *contentLength == "1");
    }

    const auto* body = escapedChunk ? escapedChunk->messageBodyChunk() : nullptr;
    RUVIA_CHECK(body != nullptr);
    if (body != nullptr) {
        RUVIA_CHECK(body->bytes() == "x");
    }
}

#if !defined(_MSC_VER)
RUVIA_TEST(http2_public_response_materialization_failure_keeps_event_retryable) {
    ToggleAllocationResource resource;
    auto client = preparedClient(&resource);
    const auto wire = clientResponseWire(&resource, {}, true);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    resource.reject();
    bool threw = false;
    try {
        (void)client.nextEvent();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);

    resource.reject(false);
    const auto retried = client.nextEvent();
    RUVIA_CHECK(retried && retried->responseHead() != nullptr);
    RUVIA_CHECK(retried && retried->responseHead()->head().headers().size() == 1);
}

RUVIA_TEST(http2_public_request_materialization_failure_keeps_event_retryable) {
    ToggleAllocationResource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    const auto wire = serverRequestWire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    resource.reject();
    bool threw = false;
    try {
        (void)server.nextEvent();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);

    resource.reject(false);
    const auto retried = server.nextEvent();
    RUVIA_CHECK(retried && retried->requestHead() != nullptr);
    RUVIA_CHECK(retried && retried->requestHead()->request().method() == "POST");
}
#endif

RUVIA_TEST(http2_public_received_peer_settings_reports_handshake_readiness) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = ruvia::Http2Connection::client({.resource = &resource});
    RUVIA_CHECK(!client.receivedPeerSettings());

    std::pmr::string wire(&resource);
    appendPeerSettings(wire);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.receivedPeerSettings());
}

RUVIA_TEST(http2_public_response_head_and_trailers_are_owned_by_events) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = preparedClient(&resource);
    auto wire = clientResponseWithTrailersWire(&resource);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    auto headEvent = client.nextEvent();
    RUVIA_CHECK(headEvent && headEvent->responseHead() != nullptr);
    auto ownedHead = std::move(*headEvent->responseHead()).takeHead();
    RUVIA_CHECK(ownedHead.status() == ruvia::http_status::kOk);
    RUVIA_CHECK(ownedHead.headers().size() == 1);
    RUVIA_CHECK(ownedHead.headers().front().name() == "x-test");

    auto endEvent = client.nextEvent();
    RUVIA_CHECK(endEvent && endEvent->messageEnd() != nullptr);
    RUVIA_CHECK(endEvent->messageEnd()->trailers().size() == 1);
    auto ownedTrailers = std::move(*endEvent->messageEnd()).takeTrailers();
    RUVIA_CHECK(ownedTrailers.size() == 1);
    RUVIA_CHECK(ownedTrailers.front().name() == "x-trace");
    RUVIA_CHECK(ownedTrailers.front().value() == "done");

    auto replacement = ruvia::Http2Connection::client({.resource = &resource});
    client = std::move(replacement);
    RUVIA_CHECK(ownedHead.headers().front().value() == "owned");
    RUVIA_CHECK(ownedTrailers.front().value() == "done");
}

RUVIA_TEST(http2_public_message_end_reports_metadata_only_and_empty_trailers) {
    for (const auto method : {std::string_view("HEAD"), std::string_view("GET")}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = preparedClientMethod(&resource, method);
        std::pmr::string wire(&resource);
        appendPeerSettings(wire);
        appendResponse(wire, &resource, 1, method == "HEAD" ? "200" : "304");
        RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
        auto head = client.nextEvent();
        RUVIA_CHECK(head && head->responseHead() != nullptr);
        auto end = client.nextEvent();
        RUVIA_CHECK(end && end->messageEnd() != nullptr);
        RUVIA_CHECK(end->messageEnd()->trailers().empty());
        RUVIA_CHECK(end->messageEnd()->contentSemantics() ==
                    ruvia::Http2MessageContentSemantics::kMetadataOnly);
    }

    std::pmr::monotonic_buffer_resource resource;
    auto client = preparedClientMethod(&resource, "GET");
    std::pmr::string wire(&resource);
    appendPeerSettings(wire);
    appendResponse(wire, &resource, 1, "200", "body");
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().has_value());
    RUVIA_CHECK(client.nextEvent().has_value());
    const auto end = client.nextEvent();
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    RUVIA_CHECK(end->messageEnd()->contentSemantics() ==
                ruvia::Http2MessageContentSemantics::kContent);
}

RUVIA_TEST(http2_public_trailer_decode_failure_is_retryable_and_event_transfer_does_not_allocate) {
    ToggleAllocationResource resource;
    auto client = preparedClient(&resource);
    std::pmr::string wire(&resource);
    appendPeerSettings(wire);
    std::pmr::string block(&resource);
    ruvia::HpackEncoder::encodeStatus(block, ruvia::http_status::kOk);
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x4, 1, block);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.nextEvent().has_value());
    block.clear();
    ruvia::HpackEncoder::encodeHeader(block, "x-trace", "done");
    std::pmr::string trailers(&resource);
    appendFrame(trailers, ruvia::Http2FrameType::kHeaders, 0x5, 1, block);
    resource.reject();
    bool threw = false;
    try {
        (void)client.feed(trailers);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    resource.reject(false);
    RUVIA_CHECK(client.feed(trailers) == ruvia::Http2FeedResult::kAccepted);
    resource.reject();
    auto retried = client.nextEvent();
    resource.reject(false);
    RUVIA_CHECK(retried && retried->messageEnd() != nullptr);
    RUVIA_CHECK(retried->messageEnd()->trailers().size() == 1);
    RUVIA_CHECK(retried->messageEnd()->trailers().front().value() == "done");
}

RUVIA_TEST(http2_public_data_credit_merge_is_allocation_free_and_linear) {
    ToggleAllocationResource resource;
    auto client = preparedClient(&resource);
    auto wire = clientWindowThresholdResponseWire(&resource, true);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    std::pmr::string drained(&resource);
    client.takeOutput(drained);
    RUVIA_CHECK(client.nextEvent().has_value());
    auto first = client.nextEvent();
    RUVIA_CHECK(first && first->messageBodyChunk() != nullptr);
    auto firstCredit = first->messageBodyChunk()->takeCredit();
    constexpr auto frameCount = ruvia::detail::kHttp2ReceiveWindowUpdateThreshold / 16'384;
    for (std::size_t index = 1; index < frameCount; ++index) {
        auto chunk = client.nextEvent();
        RUVIA_CHECK(chunk && chunk->messageBodyChunk() != nullptr);
        auto nextCredit = chunk->messageBodyChunk()->takeCredit();
        resource.reject();
        RUVIA_CHECK(firstCredit.merge(std::move(nextCredit)) ==
                    ruvia::Http2ReceivedDataCreditMergeStatus::kMerged);
        RUVIA_CHECK(firstCredit.valid());
        RUVIA_CHECK(!nextCredit.valid());
        RUVIA_CHECK(client.pendingOutput().empty());
        resource.reject(false);
    }
    auto end = client.nextEvent();
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.acknowledge(std::move(firstCredit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
    const auto output = client.pendingOutput();
    RUVIA_CHECK(output.size() == ruvia::kHttp2FrameHeaderBytes + sizeof(std::uint32_t));
    const auto update = ruvia::parseHttp2FrameHeader(
        std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(update && update->streamId == 0);
    RUVIA_CHECK(update && update->type ==
                              static_cast<std::uint8_t>(ruvia::Http2FrameType::kWindowUpdate));
    RUVIA_CHECK(client.submitReset(1, ruvia::Http2ErrorCode::kCancel) ==
                ruvia::Http2SubmitStatus::kClosed);
}

RUVIA_TEST(http2_public_data_credit_merge_rejects_different_connection_unchanged) {
    std::pmr::monotonic_buffer_resource resource;
    auto firstClient = preparedClient(&resource);
    std::pmr::string firstWire(&resource);
    appendPeerSettings(firstWire);
    appendResponse(firstWire, &resource, 1, "200", "a", {}, {});
    RUVIA_CHECK(firstClient.feed(firstWire) == ruvia::Http2FeedResult::kAccepted);
    (void)firstClient.nextEvent();
    auto firstEvent = firstClient.nextEvent();
    auto firstCredit = firstEvent->messageBodyChunk()->takeCredit();

    auto secondClient = preparedClient(&resource);
    std::pmr::string secondWire(&resource);
    appendPeerSettings(secondWire);
    appendResponse(secondWire, &resource, 1, "200", "b", {}, {});
    RUVIA_CHECK(secondClient.feed(secondWire) == ruvia::Http2FeedResult::kAccepted);
    (void)secondClient.nextEvent();
    auto secondEvent = secondClient.nextEvent();
    auto secondCredit = secondEvent->messageBodyChunk()->takeCredit();

    RUVIA_CHECK(firstCredit.merge(std::move(secondCredit)) ==
                ruvia::Http2ReceivedDataCreditMergeStatus::kDifferentStream);
    RUVIA_CHECK(firstCredit.valid());
    RUVIA_CHECK(secondCredit.valid());
    RUVIA_CHECK(firstClient.acknowledge(std::move(firstCredit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
    RUVIA_CHECK(secondClient.acknowledge(std::move(secondCredit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
}

RUVIA_TEST(http2_public_data_credit_merge_rejects_different_stream_unchanged) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = preparedClient(&resource);
    const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "GET", .scheme = "https", .authority = "example.test", .target = "/two"});
    RUVIA_CHECK(submitted.submitted() != nullptr);

    std::pmr::string wire(&resource);
    appendPeerSettings(wire);
    appendResponse(wire, &resource, 1, "200", "a");
    appendResponse(wire, &resource, 3, "200", "b");
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    (void)client.nextEvent();
    auto firstEvent = client.nextEvent();
    RUVIA_CHECK(firstEvent && firstEvent->messageBodyChunk() != nullptr);
    auto firstCredit = firstEvent->messageBodyChunk()->takeCredit();
    (void)client.nextEvent();
    (void)client.nextEvent();
    auto secondEvent = client.nextEvent();
    RUVIA_CHECK(secondEvent && secondEvent->messageBodyChunk() != nullptr);
    auto secondCredit = secondEvent->messageBodyChunk()->takeCredit();

    RUVIA_CHECK(firstCredit.merge(std::move(secondCredit)) ==
                ruvia::Http2ReceivedDataCreditMergeStatus::kDifferentStream);
    RUVIA_CHECK(firstCredit.valid());
    RUVIA_CHECK(secondCredit.valid());
    RUVIA_CHECK(client.acknowledge(std::move(firstCredit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
    RUVIA_CHECK(client.acknowledge(std::move(secondCredit)) ==
                ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
}

RUVIA_TEST(http2_public_server_message_end_owns_request_trailers) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    auto wire = serverRequestWithTrailersWire(&resource);
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead() != nullptr);
    RUVIA_CHECK(!request->requestHead()->request().header("x-request-trace"));
    const auto chunk = server.nextEvent();
    RUVIA_CHECK(chunk && chunk->messageBodyChunk() != nullptr);
    auto end = server.nextEvent();
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    RUVIA_CHECK(end->messageEnd()->contentSemantics() ==
                ruvia::Http2MessageContentSemantics::kContent);
    RUVIA_CHECK(end->messageEnd()->trailers().size() == 1);
    auto trailers = std::move(*end->messageEnd()).takeTrailers();
    RUVIA_CHECK(trailers.size() == 1);
    if (!trailers.empty()) {
        RUVIA_CHECK(trailers.front().name() == "x-request-trace");
        RUVIA_CHECK(trailers.front().value() == "done");
    }
    RUVIA_CHECK(server.release(std::move(*request->requestHead())) ==
                ruvia::Http2ServerRequestReleaseStatus::kReleased);
}

RUVIA_TEST(http2_public_request_headers_remain_valid_while_trailers_arrive) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    const std::string initialValue(700, 'a');
    const std::string trailerValue(3000, 'b');
    std::pmr::string block(&resource);
    ruvia::HpackEncoder::encodeHeader(block, ":method", "POST");
    ruvia::HpackEncoder::encodeHeader(block, ":scheme", "https");
    ruvia::HpackEncoder::encodeHeader(block, ":authority", "example.test");
    ruvia::HpackEncoder::encodeHeader(block, ":path", "/");
    ruvia::HpackEncoder::encodeHeader(block, "x-initial", initialValue);
    std::pmr::string wire(ruvia::kHttp2ClientPreface, &resource);
    appendPeerSettings(wire);
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x4, 1, block);
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead() != nullptr);
    const auto borrowed = request->requestHead()->request().header("x-initial");
    RUVIA_CHECK(borrowed && *borrowed == initialValue);

    block.clear();
    ruvia::HpackEncoder::encodeHeader(block, "x-trailer", trailerValue);
    std::pmr::string trailers(&resource);
    appendFrame(trailers, ruvia::Http2FrameType::kHeaders, 0x5, 1, block);
    RUVIA_CHECK(server.feed(trailers) == ruvia::Http2FeedResult::kAccepted);
    auto end = server.nextEvent();
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    RUVIA_CHECK(borrowed && *borrowed == initialValue);
    RUVIA_CHECK(!request->requestHead()->request().header("x-trailer"));
    const auto fields = end->messageEnd()->trailers();
    RUVIA_CHECK(fields.size() == 1);
    if (!fields.empty()) {
        RUVIA_CHECK(fields.front().value() == trailerValue);
    }
    RUVIA_CHECK(server.release(std::move(*request->requestHead())) ==
                ruvia::Http2ServerRequestReleaseStatus::kReleased);
}

RUVIA_TEST(http2_public_server_release_before_message_end_keeps_terminal_event) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    auto wire = serverRequestWire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);

    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead() != nullptr);
    ruvia::HttpResponse response({.resource = &resource});
    RUVIA_CHECK(server.submitBufferedResponse(1, response) ==
                ruvia::Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(server.release(std::move(*request->requestHead())) ==
                ruvia::Http2ServerRequestReleaseStatus::kReleased);

    auto end = server.nextEvent();
    RUVIA_CHECK(end && end->messageEnd() != nullptr);
    RUVIA_CHECK(end->messageEnd()->streamId() == 1);
    RUVIA_CHECK(end->messageEnd()->trailers().empty());
}

RUVIA_TEST(http2_public_dropped_request_before_message_end_keeps_terminal_event) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    (void)server.consumeOutput(server.pendingOutput().size());
    auto wire = serverRequestWire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead() != nullptr);
    request.reset();

    bool sawEnd = false;
    while (const auto event = server.nextEvent()) {
        if (event->messageEnd() != nullptr) {
            sawEnd = true;
            RUVIA_CHECK(event->messageEnd()->streamId() == 1);
            RUVIA_CHECK(event->messageEnd()->trailers().empty());
        }
    }
    RUVIA_CHECK(sawEnd);
}

RUVIA_TEST(http2_public_client_reset_before_terminal_events_drain_keeps_events_readable) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = ruvia::Http2Connection::client({.resource = &resource});
    const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "POST", .scheme = "https", .authority = "example.test", .target = "/", .content = ruvia::Http2RequestContent::streaming()});
    RUVIA_CHECK(submitted.submitted() != nullptr);
    auto wire = clientResponseWire(&resource, {}, false, true);
    RUVIA_CHECK(client.feed(wire) == ruvia::Http2FeedResult::kAccepted);
    const auto reset = client.submitReset(1, ruvia::Http2ErrorCode::kCancel);
    RUVIA_CHECK(reset == ruvia::Http2SubmitStatus::kAccepted);

    bool sawHead = false;
    bool sawEnd = false;
    while (const auto event = client.nextEvent()) {
        if (event->responseHead() != nullptr) {
            sawHead = true;
            RUVIA_CHECK(event->responseHead()->head().status() == ruvia::http_status::kOk);
        }
        if (event->messageEnd() != nullptr) {
            sawEnd = true;
            RUVIA_CHECK(event->messageEnd()->trailers().empty());
        }
    }
    RUVIA_CHECK(sawHead);
    RUVIA_CHECK(sawEnd);
}
