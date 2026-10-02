#include <array>
#include <cstring>
#include <memory_resource>
#include <string>

#include "ruvia/http/Hpack.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"

#include "test_harness.h"

namespace {
void transfer(ruvia::Http2Connection& from, ruvia::Http2Connection& to) {
    const auto bytes = from.pendingOutput();
    const auto status = to.feed(bytes);
    if (status != ruvia::Http2FeedResult::kAccepted && status != ruvia::Http2FeedResult::kNeedInput) {
        throw std::runtime_error("HTTP/2 push transfer failed, status " + std::to_string(static_cast<int>(status)) +
                                 ", error " + std::to_string(to.connectionError() ? static_cast<int>(*to.connectionError()) : -1));
    }
    (void)from.consumeOutput(bytes.size());
}
}  // namespace

RUVIA_TEST(http2_push_promise_fragmentation_response_and_detached_metadata) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::Http2Connection::client({.resource = &resource, .enablePush = true});
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{.authority = "example.test"});
    RUVIA_CHECK(submitted.submitted() != nullptr);
    transfer(client, server);
    auto request = server.nextEvent();
    RUVIA_CHECK(request && request->requestHead());
    auto requestEnd = server.nextEvent();
    RUVIA_CHECK(requestEnd && requestEnd->messageEnd());
    const std::string value(20'000, 'x');
    const std::array fields{ruvia::HttpHeaderView{"x-long", value}};
    const auto push = server.submitPushPromise(1, {.authority = "example.test", .path = "/asset", .headers = fields});
    RUVIA_CHECK(push.has_value());
    RUVIA_CHECK_EQ(*push, 2u);
    transfer(server, client);
    auto promise = client.nextEvent();
    RUVIA_CHECK(promise && promise->pushPromise());
    RUVIA_CHECK(promise->pushPromise()->request.path == "/asset");
    RUVIA_CHECK(promise->pushPromise()->request.headers[0].value() == value);
    ruvia::HttpResponse response({.resource = &resource});
    response.status(ruvia::http_status::kOk);
    response.body("asset");
    RUVIA_CHECK(server.submitBufferedResponse(2, response) == ruvia::Http2SubmitStatus::kAccepted);
    transfer(server, client);
    auto head = client.nextEvent();
    RUVIA_CHECK(head && head->responseHead());
    RUVIA_CHECK_EQ(head->responseHead()->streamId(), 2u);
    auto data = client.nextEvent();
    RUVIA_CHECK(data && data->messageBodyChunk());
    RUVIA_CHECK(data->messageBodyChunk()->bytes() == "asset");
    auto credit = data->messageBodyChunk()->takeCredit();
    RUVIA_CHECK(client.acknowledge(std::move(credit)) == ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
    auto end = client.nextEvent();
    RUVIA_CHECK(end && end->messageEnd());
    RUVIA_CHECK(promise->pushPromise()->request.headers[0].value() == value);
    RUVIA_CHECK(!client.connectionError());
}

RUVIA_TEST(http2_push_disabled_peer_and_client_cancellation) {
    for (const bool enabled : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = ruvia::Http2Connection::client({.resource = &resource, .enablePush = enabled});
        auto server = ruvia::Http2Connection::server({.resource = &resource});
        transfer(client, server);
        transfer(server, client);
        const auto submitted = client.submitRequestHead(
            ruvia::Http2RegularRequestHeadView{.authority = "example.test"});
        RUVIA_CHECK(submitted.submitted() != nullptr);
        transfer(client, server);
        auto request = server.nextEvent();
        auto end = server.nextEvent();
        const auto push = server.submitPushPromise(1, {.authority = "example.test"});
        if (!enabled) {
            RUVIA_CHECK(!push && push.error() == ruvia::Http2PushSubmitError::kPushDisabled);
            continue;
        }
        RUVIA_CHECK(push.has_value());
        transfer(server, client);
        RUVIA_CHECK(client.nextEvent()->pushPromise());
        RUVIA_CHECK(client.submitReset(*push, ruvia::Http2ErrorCode::kCancel) == ruvia::Http2SubmitStatus::kAccepted);
        transfer(client, server);
        auto closed = server.nextEvent();
        RUVIA_CHECK(closed && closed->streamClosed());
        RUVIA_CHECK_EQ(closed->streamClosed()->streamId(), *push);
    }
}

RUVIA_TEST(http2_push_promise_unsafe_method_resets_promised_stream_without_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = ruvia::Http2Connection::client({.resource = &resource, .enablePush = true});
    auto server = ruvia::Http2Connection::server({.resource = &resource});
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);

    const auto submitted = client.submitRequestHead(
        ruvia::Http2RegularRequestHeadView{.authority = "example.test", .target = "/"});
    RUVIA_CHECK(submitted.submitted() != nullptr);
    transfer(client, server);

    // Prepare HPACK block with unsafe method (POST)
    std::pmr::string block(&resource);
    ruvia::HpackEncoder::encodeHeader(block, ":method", "POST");
    ruvia::HpackEncoder::encodeHeader(block, ":scheme", "https");
    ruvia::HpackEncoder::encodeHeader(block, ":authority", "example.test");
    ruvia::HpackEncoder::encodeHeader(block, ":path", "/upload");

    std::string frame;
    frame.resize(ruvia::kHttp2FrameHeaderBytes + 4 + block.size());
    const auto payloadLen = static_cast<std::uint32_t>(4 + block.size());
    RUVIA_CHECK(ruvia::encodeHttp2FrameHeader(
        std::span(frame.data(), ruvia::kHttp2FrameHeaderBytes),
        payloadLen,
        ruvia::Http2FrameType::kPushPromise,
        0x4 /* END_HEADERS */,
        1 /* associatedStreamId */));

    // Promised stream ID = 2
    frame[ruvia::kHttp2FrameHeaderBytes + 0] = 0x00;
    frame[ruvia::kHttp2FrameHeaderBytes + 1] = 0x00;
    frame[ruvia::kHttp2FrameHeaderBytes + 2] = 0x00;
    frame[ruvia::kHttp2FrameHeaderBytes + 3] = 0x02;
    std::memcpy(frame.data() + ruvia::kHttp2FrameHeaderBytes + 4, block.data(), block.size());

    // Feed to client
    const auto feedStatus = client.feed(frame);
    RUVIA_CHECK(feedStatus == ruvia::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!client.connectionError());

    // Client should have emitted RST_STREAM on promised stream 2
    const auto pending = client.pendingOutput();
    RUVIA_CHECK(pending.size() >= ruvia::kHttp2FrameHeaderBytes + 4);
    const auto parsed = ruvia::parseHttp2FrameHeader(std::span(pending.data(), pending.size()));
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK_EQ(parsed->type, static_cast<std::uint8_t>(ruvia::Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(parsed->streamId, 2u);
    RUVIA_CHECK_EQ(parsed->length, 4u);
}

RUVIA_TEST(http2_push_reservation_does_not_consume_peer_concurrency_until_response_headers) {
    auto client = ruvia::Http2Connection::client({.enablePush = true});
    auto server = ruvia::Http2Connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{.authority = "example.test"});
    RUVIA_CHECK(submitted.submitted());
    transfer(client, server);
    auto head = server.nextEvent();
    auto end = server.nextEvent();
    const auto setLimit = [&](std::uint32_t limit) {
        std::array<char, 15> frame{};
        RUVIA_CHECK(ruvia::encodeHttp2FrameHeader(std::span(frame).first(9), 6, ruvia::Http2FrameType::kSettings, 0, 0));
        frame[10] = 3;
        for (std::size_t i = 0; i < 4; ++i) {
            frame[11 + i] = static_cast<char>(limit >> (24 - i * 8));
        }
        RUVIA_CHECK(server.feed(std::string_view(frame.data(), frame.size())) == ruvia::Http2FeedResult::kAccepted);
        (void)server.consumeOutput(server.pendingOutput().size());
    };
    setLimit(0);
    const auto first = server.submitPushPromise(1, {.authority = "example.test", .path = "/one"});
    const auto second = server.submitPushPromise(1, {.authority = "example.test", .path = "/two"});
    RUVIA_CHECK(first && second);
    if (!first || !second) {
        return;
    }
    RUVIA_CHECK(server.submitStreamingResponseHead(*first, ruvia::HttpResponse{}) == ruvia::Http2SubmitStatus::kPeerCapabilityUnavailable);
    setLimit(1);
    RUVIA_CHECK(server.submitStreamingResponseHead(*first, ruvia::HttpResponse{}) == ruvia::Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(server.submitStreamingResponseHead(*second, ruvia::HttpResponse{}) == ruvia::Http2SubmitStatus::kPeerCapabilityUnavailable);
    RUVIA_CHECK(server.submitData(*first, {}, ruvia::Http2EndStream::kEndStream) == ruvia::Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK(server.submitStreamingResponseHead(*second, ruvia::HttpResponse{}) == ruvia::Http2SubmitStatus::kAccepted);
}

RUVIA_TEST(http2_server_goaway_releases_unprocessed_local_push_streams) {
    auto client = ruvia::Http2Connection::client({.enablePush = true});
    auto server = ruvia::Http2Connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{.authority = "example.test"});
    RUVIA_CHECK(submitted.submitted());
    transfer(client, server);
    auto head = server.nextEvent();
    auto end = server.nextEvent();
    const auto push = server.submitPushPromise(1, {.authority = "example.test"});
    RUVIA_CHECK(push.has_value());
    const std::array<char, 17> goaway{0, 0, 8, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    RUVIA_CHECK(server.feed(std::string_view(goaway.data(), goaway.size())) == ruvia::Http2FeedResult::kAccepted);
    auto draining = server.nextEvent();
    auto closed = server.nextEvent();
    RUVIA_CHECK(draining && draining->goaway());
    RUVIA_CHECK(closed && closed->streamClosed() && closed->streamClosed()->streamId() == *push);
    RUVIA_CHECK(server.submitStreamingResponseHead(*push, ruvia::HttpResponse{}) == ruvia::Http2SubmitStatus::kClosed);
}

namespace {
struct PushResource final : std::pmr::memory_resource {
    std::size_t live_bytes{0};
    std::size_t live_allocations{0};
    bool fail{false};
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (fail) {
            throw std::bad_alloc();
        }
        auto* pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        live_bytes += bytes;
        ++live_allocations;
        return pointer;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        if (live_allocations == 0 || live_bytes < bytes) {
            std::terminate();
        }
        live_bytes -= bytes;
        --live_allocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace
RUVIA_TEST(http2_push_repeated_response_and_cancellation_release_storage_and_preserve_promises) {
    PushResource resource;
    {
        auto client = ruvia::Http2Connection::client({.resource = &resource, .enablePush = true});
        auto server = ruvia::Http2Connection::server({.resource = &resource});
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
        const auto submitted = client.submitRequestHead(ruvia::Http2RegularRequestHeadView{.authority = "example.test"});
        RUVIA_CHECK(submitted.submitted());
        transfer(client, server);
        auto request = server.nextEvent();
        auto requestEnd = server.nextEvent();
        const auto first = server.submitPushPromise(1, {.authority = "example.test", .path = "/retained"});
        RUVIA_CHECK(first);
        transfer(server, client);
        auto retained = client.nextEvent();
        RUVIA_CHECK(retained && retained->pushPromise());
        RUVIA_CHECK(client.submitReset(*first, ruvia::Http2ErrorCode::kCancel) == ruvia::Http2SubmitStatus::kAccepted);
        transfer(client, server);
        auto closed = server.nextEvent();
        closed.reset();
        std::size_t baseline_bytes = 0;
        std::size_t baseline_allocations = 0;
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto push = server.submitPushPromise(1, {.authority = "example.test", .path = "/asset"});
                RUVIA_CHECK(push);
                transfer(server, client);
                auto promise = client.nextEvent();
                RUVIA_CHECK(promise && promise->pushPromise());
                if (i % 2 == 0) {
                    RUVIA_CHECK(client.submitReset(*push, ruvia::Http2ErrorCode::kCancel) == ruvia::Http2SubmitStatus::kAccepted);
                    transfer(client, server);
                    auto canceled = server.nextEvent();
                    RUVIA_CHECK(canceled && canceled->streamClosed());
                } else {
                    ruvia::HttpResponse response({.resource = &resource});
                    response.body("asset");
                    RUVIA_CHECK(server.submitBufferedResponse(*push, response) == ruvia::Http2SubmitStatus::kAccepted);
                    transfer(server, client);
                    auto head = client.nextEvent();
                    auto data = client.nextEvent();
                    RUVIA_CHECK(data && data->messageBodyChunk());
                    if (data && data->messageBodyChunk()) {
                        auto credit = data->messageBodyChunk()->takeCredit();
                        RUVIA_CHECK(client.acknowledge(std::move(credit)) == ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged);
                    }
                    auto end = client.nextEvent();
                    RUVIA_CHECK(head && head->responseHead());
                    RUVIA_CHECK(end && end->messageEnd());
                }
            }
            // Drain consumed event storage before comparing allocations.
            RUVIA_CHECK(!client.nextEvent());
            RUVIA_CHECK(!server.nextEvent());
            if (i == 3) {
                // Retained connection/container storage (including implementation-specific
                // sentinels) is the stable baseline; per-push storage must return to it.
                baseline_bytes = resource.live_bytes;
                baseline_allocations = resource.live_allocations;
            }
            if (i > 3) {
                RUVIA_CHECK_EQ(resource.live_bytes, baseline_bytes);
                RUVIA_CHECK_EQ(resource.live_allocations, baseline_allocations);
            }
            RUVIA_CHECK(retained->pushPromise()->request.path == "/retained");
        }
        const auto outputSize = server.pendingOutput().size();
        resource.fail = true;
        bool threw = false;
        try {
            (void)server.submitPushPromise(1, {.authority = "example.test", .path = "/allocation-failure"});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        resource.fail = false;
        RUVIA_CHECK_EQ(server.pendingOutput().size(), outputSize);
    }
    RUVIA_CHECK_EQ(resource.live_bytes, 0u);
    RUVIA_CHECK_EQ(resource.live_allocations, 0u);
}
