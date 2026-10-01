#include "http2_connection_fixture.h"

RUVIA_TEST(http2_request_trailers_finish_content_and_reject_framing_fields) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);
    const auto request = client.submitRegularRequestHead("POST", "https", "example.test", "/upload",
        {}, Http2RequestContent::knownLength(2));
    const auto id = submittedRequestStreamId(request);
    using Finish = ruvia::Http2FinishRequestStatus;
    RUVIA_CHECK(client.finishRequest(id, {}) == Finish::kContentLengthIncomplete);
    RUVIA_CHECK(client.submitData(id, "ok", Http2EndStream::kKeepOpen) == Http2DataSubmitStatus::kAccepted);
    const std::array forbidden{ruvia::HttpHeaderView{"content-length", "2"}};
    RUVIA_CHECK(client.finishRequest(id, forbidden) == Finish::kInvalidTrailer);
    client.consumeOutput(client.pendingOutput().size());
    const std::array trailers{ruvia::HttpHeaderView{"x-checksum", "123"}};
    RUVIA_CHECK(client.finishRequest(id, trailers) == Finish::kAccepted);
    const auto frame = ruvia::detail::http2ParseFrameHeader(client.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    RUVIA_CHECK(client.submitData(id, "", Http2EndStream::kKeepOpen) == Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK(client.finishRequest(id, trailers) == Finish::kInvalidState);
}

RUVIA_TEST(http2_request_trailers_follow_window_blocked_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);
    const auto request = client.submitRegularRequestHead("POST", "https", "example.test", "/upload",
        {}, Http2RequestContent::knownLength(70'000));
    const auto id = submittedRequestStreamId(request);
    const std::string body(70'000, 'x');
    RUVIA_CHECK(client.submitData(id, body, Http2EndStream::kKeepOpen) == Http2DataSubmitStatus::kQueued);
    const std::array trailers{ruvia::HttpHeaderView{"x-checksum", "done"}};
    RUVIA_CHECK(client.finishRequest(id, trailers) == ruvia::Http2FinishRequestStatus::kQueued);
    client.consumeOutput(client.pendingOutput().size());
    std::array<char, 26> updates{};
    ruvia::detail::http2WriteWindowUpdate(updates.data(), 0, 70'000);
    ruvia::detail::http2WriteWindowUpdate(updates.data() + 13, id, 70'000);
    RUVIA_CHECK(client.feed(std::string_view(updates.data(), updates.size())) == Http2FeedResult::kAccepted);
    auto bytes = client.pendingOutput();
    std::size_t payload = 0;
    bool sawTrailer = false;
    while (!bytes.empty()) {
        const auto frame = ruvia::detail::http2ParseFrameHeader(bytes.substr(0, 9));
        if (frame.type == static_cast<std::uint8_t>(Http2FrameType::kData)) {
            RUVIA_CHECK(!sawTrailer);
            RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
            payload += frame.length;
        } else if (frame.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders)) {
            sawTrailer = true;
            RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
        }
        bytes.remove_prefix(9 + frame.length);
    }
    RUVIA_CHECK(payload > 0);
    RUVIA_CHECK(sawTrailer);
}
