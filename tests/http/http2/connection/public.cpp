#include "test_harness.h"

#include <array>
#include <cstddef>
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

namespace {

class ToggleAllocationResource final : public std::pmr::memory_resource {
public:
    void reject(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject_) throw std::bad_alloc();
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

std::pmr::string clientResponseWire(std::pmr::memory_resource* resource, std::string_view body,
    bool includeHeader = false, bool endStream = true) {
    std::pmr::string block(resource);
    ruvia::HpackEncoder::encodeStatus(block, ruvia::http_status::kOk);
    if (includeHeader) ruvia::HpackEncoder::encodeHeader(block, "x-test", "value");

    std::pmr::string wire(resource);
    appendPeerSettings(wire);
    appendFrame(wire, ruvia::Http2FrameType::kHeaders, 0x4, 1, block);
    appendFrame(wire, ruvia::Http2FrameType::kData, endStream ? 0x1 : 0, 1, body);
    return wire;
}

std::pmr::string clientWindowThresholdResponseWire(
    std::pmr::memory_resource* resource, bool endStream) {
    constexpr std::size_t kFramePayloadBytes = 16'384;
    static_assert(ruvia::detail::kHttp2ReceiveWindowUpdateThreshold % kFramePayloadBytes == 0);
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
    if (!body.empty()) appendFrame(wire, ruvia::Http2FrameType::kData, 0x1, 1, body);
    return wire;
}

ruvia::Http2Connection preparedClient(std::pmr::memory_resource* resource) {
    auto client = ruvia::Http2Connection::client({.resource = resource});
    (void)client.consumeOutput(client.pendingOutput().size());
    const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "GET", .scheme = "https", .authority = "example.test", .target = "/"});
    if (submitted.submitted() == nullptr) throw std::logic_error("test request was not submitted");
    (void)client.consumeOutput(client.pendingOutput().size());
    return client;
}

}  // namespace

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
