#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/Http2Framing.h"

#include "test_harness.h"
namespace {
void transfer(ruvia::Http2Connection& from, ruvia::Http2Connection& to) {
    const auto bytes = from.pendingOutput();
    const auto status = to.feed(bytes);
    if (status != ruvia::Http2FeedResult::kAccepted && status != ruvia::Http2FeedResult::kNeedInput) {
        throw std::runtime_error("priority transfer failed");
    }
    (void)from.consumeOutput(bytes.size());
}
}  // namespace
RUVIA_TEST(http2_priority_update_client_submission_and_server_event) {
    auto client = ruvia::Http2Connection::client();
    auto server = ruvia::Http2Connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    RUVIA_CHECK(client.submitPriorityUpdate(1, {.urgency = 0, .incremental = true}) == ruvia::Http2SubmitStatus::kAccepted);
    transfer(client, server);
    const auto event = server.nextEvent();
    RUVIA_CHECK(event && event->priorityUpdate());
    RUVIA_CHECK_EQ(event->priorityUpdate()->elementId, 1u);
    RUVIA_CHECK(event->priorityUpdate()->fields.urgency == 0);
    RUVIA_CHECK(server.submitPriorityUpdate(1, {}) == ruvia::Http2SubmitStatus::kInvalidState);
    RUVIA_CHECK(client.submitPriorityUpdate(2, {}) == ruvia::Http2SubmitStatus::kInvalidState);
}
RUVIA_TEST(http2_priority_update_idle_stream_budget_and_wrong_peer_role) {
    auto client = ruvia::Http2Connection::client();
    auto server = ruvia::Http2Connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    for (std::uint32_t i = 0; i < 128; ++i) {
        RUVIA_CHECK(client.submitPriorityUpdate(i * 2 + 1, {.urgency = 3}) == ruvia::Http2SubmitStatus::kAccepted);
        transfer(client, server);
        RUVIA_CHECK(server.nextEvent()->priorityUpdate());
    }
    RUVIA_CHECK(client.submitPriorityUpdate(257, {}) == ruvia::Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(server.feed(client.pendingOutput()) == ruvia::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(server.connectionError() == ruvia::Http2ErrorCode::kProtocolError);
}

RUVIA_TEST(http2_priority_update_ignores_malformed_values_without_consuming_idle_budget) {
    auto client = ruvia::Http2Connection::client();
    auto server = ruvia::Http2Connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    for (std::uint32_t index = 0; index != 128; ++index) {
        RUVIA_CHECK(client.submitPriorityUpdate(index * 2 + 1, {.urgency = 3}) == ruvia::Http2SubmitStatus::kAccepted);
        transfer(client, server);
        RUVIA_CHECK(server.nextEvent()->priorityUpdate());
    }

    std::array<char, ruvia::kHttp2FrameHeaderBytes + 6> frame{};
    RUVIA_CHECK(ruvia::encodeHttp2FrameHeader(std::span(frame).first(ruvia::kHttp2FrameHeaderBytes),
        6, ruvia::Http2FrameType::kPriorityUpdate, 0, 0));
    // Stream 257 followed by a malformed Structured Fields dictionary.
    frame[ruvia::kHttp2FrameHeaderBytes + 2] = 1;
    frame[ruvia::kHttp2FrameHeaderBytes + 3] = 1;
    frame[ruvia::kHttp2FrameHeaderBytes + 4] = 'u';
    frame[ruvia::kHttp2FrameHeaderBytes + 5] = '=';
    const auto status = server.feed(std::string_view(frame.data(), frame.size()));
    RUVIA_CHECK(status == ruvia::Http2FeedResult::kAccepted || status == ruvia::Http2FeedResult::kNeedInput);
    RUVIA_CHECK(!server.connectionError().has_value());
    RUVIA_CHECK(!server.nextEvent());
    if (server.connectionError().has_value()) {
        return;
    }

    RUVIA_CHECK(client.submitPriorityUpdate(1, {.urgency = 0}) == ruvia::Http2SubmitStatus::kAccepted);
    transfer(client, server);
    const auto update = server.nextEvent();
    RUVIA_CHECK(update && update->priorityUpdate() && update->priorityUpdate()->fields.urgency == 0);

    RUVIA_CHECK(client.submitPriorityUpdate(257, {}) == ruvia::Http2SubmitStatus::kAccepted);
    RUVIA_CHECK(server.feed(client.pendingOutput()) == ruvia::Http2FeedResult::kProtocolFailure);
    RUVIA_CHECK(server.connectionError() == ruvia::Http2ErrorCode::kProtocolError);
}

RUVIA_TEST(http2_priority_update_rejects_invalid_ids_even_with_malformed_values) {
    for (const char id : {char{0}, char{2}}) {
        auto client = ruvia::Http2Connection::client();
        auto server = ruvia::Http2Connection::server();
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
        std::array<char, ruvia::kHttp2FrameHeaderBytes + 6> frame{};
        RUVIA_CHECK(ruvia::encodeHttp2FrameHeader(std::span(frame).first(ruvia::kHttp2FrameHeaderBytes),
            6, ruvia::Http2FrameType::kPriorityUpdate, 0, 0));
        frame[ruvia::kHttp2FrameHeaderBytes + 3] = id;
        frame[ruvia::kHttp2FrameHeaderBytes + 4] = 'u';
        frame[ruvia::kHttp2FrameHeaderBytes + 5] = '=';
        RUVIA_CHECK(server.feed(std::string_view(frame.data(), frame.size())) == ruvia::Http2FeedResult::kProtocolFailure);
        RUVIA_CHECK(server.connectionError() == ruvia::Http2ErrorCode::kProtocolError);
    }
}

RUVIA_TEST(http2_priority_update_zero_length_on_client_yields_protocol_error) {
    auto client = ruvia::Http2Connection::client();
    auto server = ruvia::Http2Connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);

    // Empty PRIORITY_UPDATE frame on stream 0 sent to client
    std::array<char, ruvia::kHttp2FrameHeaderBytes> frame{};
    RUVIA_CHECK(ruvia::encodeHttp2FrameHeader(
        std::span(frame.data(), frame.size()),
        0 /* length */,
        ruvia::Http2FrameType::kPriorityUpdate,
        0 /* flags */,
        0 /* streamId */));

    const auto feedStatus = client.feed(std::string_view(frame.data(), frame.size()));
    RUVIA_CHECK(feedStatus == ruvia::Http2FeedResult::kProtocolFailure);
    // RFC 9218 §7.1: client receiving PRIORITY_UPDATE MUST treat as PROTOCOL_ERROR,
    // not FRAME_SIZE_ERROR even though length is 0 (< 4).
    RUVIA_CHECK(client.connectionError() == ruvia::Http2ErrorCode::kProtocolError);
}
