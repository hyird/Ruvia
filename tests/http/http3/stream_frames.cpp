#include <array>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3ConnectionError.h"
#include "ruvia/http/Http3StreamFrames.h"

#include "test_harness.h"

namespace {

using ruvia::Http3StreamFrameEvent;
using ruvia::Http3StreamFrameEventKind;
using ruvia::Http3StreamFrames;
using ruvia::Http3ConnectionErrorCode;
using ruvia::Http3StreamFrameStatus;
using ruvia::Http3StreamKind;

struct Events final {
    std::vector<Http3StreamFrameEventKind> kinds;
    std::vector<std::string> payloads;
    std::vector<bool> ends;
    std::vector<bool> fins;
    std::vector<bool> trailers;
};

void collect(void* context, Http3StreamFrameEvent event) {
    auto& events = *static_cast<Events*>(context);
    events.kinds.push_back(event.kind);
    events.payloads.emplace_back(event.payload.data(), event.payload.size());
    events.ends.push_back(event.endFrame);
    events.fins.push_back(event.fin);
    events.trailers.push_back(event.trailers);
}

}  // namespace

RUVIA_TEST(http3_stream_frame_status_maps_to_connection_errors_only_for_errors) {
    RUVIA_CHECK(!ruvia::http3ConnectionErrorCodeForStreamFrameStatus(
        Http3StreamFrameStatus::kNeedMoreData));
    RUVIA_CHECK(!ruvia::http3ConnectionErrorCodeForStreamFrameStatus(
        Http3StreamFrameStatus::kMessageEnd));
    RUVIA_CHECK(
        ruvia::http3ConnectionErrorCodeForStreamFrameStatus(
            Http3StreamFrameStatus::kFrameUnexpected) ==
        Http3ConnectionErrorCode::kFrameUnexpected);
    RUVIA_CHECK(
        ruvia::http3ConnectionErrorCodeForStreamFrameStatus(
            Http3StreamFrameStatus::kPushPromise) ==
        Http3ConnectionErrorCode::kFrameUnexpected);
    RUVIA_CHECK(
        ruvia::http3ConnectionErrorCodeForStreamFrameStatus(
            Http3StreamFrameStatus::kMissingSettings) ==
        Http3ConnectionErrorCode::kMissingSettings);
    RUVIA_CHECK(
        ruvia::http3ConnectionErrorCodeForStreamFrameStatus(
            Http3StreamFrameStatus::kFrameError) ==
        Http3ConnectionErrorCode::kFrameError);
    RUVIA_CHECK(
        ruvia::http3ConnectionErrorCodeForStreamFrameStatus(
            Http3StreamFrameStatus::kClosedCriticalStream) ==
        Http3ConnectionErrorCode::kClosedCriticalStream);
    RUVIA_CHECK(
        ruvia::http3ConnectionErrorCodeForStreamFrameStatus(
            Http3StreamFrameStatus::kLimit) ==
        Http3ConnectionErrorCode::kExcessiveLoad);
}

RUVIA_TEST(http3_stream_frames_incrementally_delivers_data_and_aggregates_headers) {
    std::pmr::monotonic_buffer_resource resource;
    Http3StreamFrames decoder(Http3StreamKind::kRequest, &resource);
    Events events;
    constexpr std::array<char, 12> wire{0x1, 0x2, 'h', '1', 0x0, 0x3, 'a', 'b', 'c',
        0x1, 0x1, 't'};

    RUVIA_CHECK(decoder.feed(std::span<const char>(wire).first(1), false, collect, &events) ==
                Http3StreamFrameStatus::kNeedMoreData);
    RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(1, 5), false, collect, &events) ==
                Http3StreamFrameStatus::kNeedMoreData);
    RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(6, 3), false, collect, &events) ==
                Http3StreamFrameStatus::kNeedMoreData);
    RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(9), true, collect, &events) ==
                Http3StreamFrameStatus::kMessageEnd);
    RUVIA_CHECK_EQ(events.kinds.size(), std::size_t{3});
    if (events.kinds.size() == 3) {
        RUVIA_CHECK(events.kinds[0] == Http3StreamFrameEventKind::kHeaders);
        RUVIA_CHECK_EQ(events.payloads[0], std::string("h1"));
        RUVIA_CHECK(events.ends[0]);
        RUVIA_CHECK(events.kinds[1] == Http3StreamFrameEventKind::kData);
        RUVIA_CHECK_EQ(events.payloads[1], std::string("abc"));
        RUVIA_CHECK(events.ends[1]);
        RUVIA_CHECK(events.kinds[2] == Http3StreamFrameEventKind::kHeaders);
        RUVIA_CHECK_EQ(events.payloads[2], std::string("t"));
        RUVIA_CHECK(events.trailers[2]);
        RUVIA_CHECK(events.fins[2]);
    }
}

RUVIA_TEST(http3_stream_frames_delivers_informational_and_final_response_headers) {
    std::pmr::monotonic_buffer_resource resource;
    Http3StreamFrames decoder(Http3StreamKind::kResponse, &resource);
    Events events;
    constexpr std::array<char, 9> wire{0x1, 0x1, 'i', 0x1, 0x1, 'f', 0x0, 0x1, 'x'};
    RUVIA_CHECK(decoder.feed(wire, true, collect, &events) == Http3StreamFrameStatus::kMessageEnd);
    RUVIA_CHECK_EQ(events.kinds.size(), std::size_t{3});
    if (events.kinds.size() == 3) {
        RUVIA_CHECK_EQ(events.payloads[0], std::string("i"));
        RUVIA_CHECK_EQ(events.payloads[1], std::string("f"));
        RUVIA_CHECK(!events.trailers[1]);
        RUVIA_CHECK_EQ(events.payloads[2], std::string("x"));
        RUVIA_CHECK(events.fins[2]);
    }
}

RUVIA_TEST(http3_stream_frames_rejects_invalid_request_frame_order_and_truncation) {
    std::pmr::monotonic_buffer_resource resource;
    Events events;
    constexpr std::array<char, 2> dataFirst{0x0, 0x0};
    Http3StreamFrames dataDecoder(Http3StreamKind::kRequest, &resource);
    RUVIA_CHECK(dataDecoder.feed(dataFirst, false, collect, &events) ==
                Http3StreamFrameStatus::kFrameUnexpected);

    constexpr std::array<char, 3> truncated{0x1, 0x2, 'x'};
    Http3StreamFrames truncatedDecoder(Http3StreamKind::kRequest, &resource);
    RUVIA_CHECK(truncatedDecoder.feed(truncated, true, collect, &events) ==
                Http3StreamFrameStatus::kFrameError);

    constexpr std::array<char, 6> afterTrailers{0x1, 0x0, 0x1, 0x0, 0x0, 0x0};
    Http3StreamFrames orderDecoder(Http3StreamKind::kRequest, &resource);
    RUVIA_CHECK(orderDecoder.feed(afterTrailers, false, collect, &events) ==
                Http3StreamFrameStatus::kFrameUnexpected);
}

RUVIA_TEST(http3_stream_frames_requires_settings_and_rejects_fin_on_control_stream) {
    std::pmr::monotonic_buffer_resource resource;
    Events events;
    Http3StreamFrames missing(Http3StreamKind::kControl, &resource);
    constexpr std::array<char, 2> wrongFirst{0x0, 0x0};
    RUVIA_CHECK(missing.feed(wrongFirst, false, collect, &events) ==
                Http3StreamFrameStatus::kMissingSettings);

    Http3StreamFrames critical(Http3StreamKind::kControl, &resource);
    constexpr std::array<char, 2> settings{0x4, 0x0};
    RUVIA_CHECK(critical.feed(settings, true, collect, &events) ==
                Http3StreamFrameStatus::kClosedCriticalStream);
}

RUVIA_TEST(http3_stream_frames_bounds_settings_payload_before_delivery) {
    std::pmr::monotonic_buffer_resource resource;
    Events events;
    Http3StreamFrames decoder(Http3StreamKind::kControl, &resource,
        {.maxSettingsPayloadBytes = 2});
    constexpr std::array<char, 2> header{0x4, 0x3};
    RUVIA_CHECK(decoder.feed(header, false, collect, &events) ==
                Http3StreamFrameStatus::kLimit);
    RUVIA_CHECK(events.kinds.empty());
}

RUVIA_TEST(http3_stream_frames_accepts_cancel_push_on_control_but_rejects_reserved_types) {
    std::pmr::monotonic_buffer_resource resource;
    Events events;
    Http3StreamFrames control(Http3StreamKind::kControl, &resource);
    constexpr std::array<char, 4> settingsAndCancel{0x4, 0x0, 0x3, 0x0};
    RUVIA_CHECK(control.feed(settingsAndCancel, false, collect, &events) ==
                Http3StreamFrameStatus::kNeedMoreData);
    constexpr std::array<char, 2> reserved{0x2, 0x0};
    RUVIA_CHECK(control.feed(reserved, false, collect, &events) ==
                Http3StreamFrameStatus::kFrameUnexpected);

    Http3StreamFrames request(Http3StreamKind::kRequest, &resource);
    constexpr std::array<char, 4> headersAndReserved{0x1, 0x0, 0x6, 0x0};
    RUVIA_CHECK(request.feed(headersAndReserved, false, collect, &events) ==
                Http3StreamFrameStatus::kFrameUnexpected);
}

RUVIA_TEST(http3_stream_frames_skips_unknown_payload_without_buffering) {
    std::pmr::monotonic_buffer_resource resource;
    Http3StreamFrames decoder(Http3StreamKind::kRequest, &resource);
    Events events;
    constexpr std::array<char, 2> headers{0x1, 0x0};
    RUVIA_CHECK(decoder.feed(headers, false, collect, &events) == Http3StreamFrameStatus::kNeedMoreData);
    constexpr std::array<char, 5> extensionPrefix{0x21, static_cast<char>(0x80), 0x00, 0x40, 0x00};
    RUVIA_CHECK(decoder.feed(extensionPrefix, false, collect, &events) ==
                Http3StreamFrameStatus::kNeedMoreData);
    std::array<char, 4096> payload{};
    for (int i = 0; i < 4; ++i) {
        RUVIA_CHECK(decoder.feed(payload, false, collect, &events) == Http3StreamFrameStatus::kNeedMoreData);
    }
    RUVIA_CHECK_EQ(events.kinds.size(), std::size_t{1});
}
