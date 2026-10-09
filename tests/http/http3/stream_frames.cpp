#include <array>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3ConnectionError.h"
#include "ruvia/http/Http3StreamFrames.h"

#include "test_harness.h"

namespace {

using ruvia::Http3ConnectionErrorCode;
using ruvia::Http3StreamFrameEvent;
using ruvia::Http3StreamFrameEventKind;
using ruvia::Http3StreamFrames;
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

void append_varint(std::vector<char>& wire, std::uint64_t value, std::size_t width) {
    const auto begin = wire.size();
    wire.resize(begin + width);
    for (std::size_t i = width; i != 0; --i) {
        wire[begin + i - 1] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
    wire[begin] |= static_cast<char>(width == 1 ? 0 : width == 2 ? 0x40
                                                  : width == 4   ? 0x80
                                                                 : 0xc0);
}

void append_frame(std::vector<char>& wire, std::uint64_t type, std::string_view payload,
    std::size_t type_width, std::size_t length_width) {
    append_varint(wire, type, type_width);
    append_varint(wire, payload.size(), length_width);
    wire.insert(wire.end(), payload.begin(), payload.end());
}

struct paused_headers final {
    Http3StreamFrames& decoder;
    Events events;
    bool pause_next = true;
};

void collect_and_pause(void* context, Http3StreamFrameEvent event) {
    auto& state = *static_cast<paused_headers*>(context);
    collect(&state.events, event);
    if (event.kind == Http3StreamFrameEventKind::kHeaders && state.pause_next) {
        state.pause_next = false;
        state.decoder.pause();
    }
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

RUVIA_TEST(http3_stream_frames_accepts_all_varint_widths_at_every_input_split) {
    std::pmr::unsynchronized_pool_resource resource;
    for (std::size_t type_width : {1, 2, 4, 8}) {
        for (std::size_t length_width : {1, 2, 4, 8}) {
            std::vector<char> storage{'!'};
            append_frame(storage, 1, "head", type_width, length_width);
            append_frame(storage, 0, "body", type_width, length_width);
            append_frame(storage, 1, "tail", type_width, length_width);
            const auto wire = std::span<const char>(storage).subspan(1);
            for (std::size_t split = 0; split <= wire.size(); ++split) {
                Http3StreamFrames decoder(Http3StreamKind::kRequest, &resource);
                Events events;
                RUVIA_CHECK(decoder.feed(wire.first(split), false, collect, &events) == Http3StreamFrameStatus::kNeedMoreData);
                RUVIA_CHECK_EQ(decoder.consumedBytes(), split);
                RUVIA_CHECK(decoder.feed(wire.subspan(split), true, collect, &events) == Http3StreamFrameStatus::kMessageEnd);
                RUVIA_CHECK_EQ(decoder.consumedBytes(), wire.size() - split);
                std::string body;
                std::size_t headers = 0;
                std::size_t data_ends = 0;
                for (std::size_t i = 0; i < events.kinds.size(); ++i) {
                    if (events.kinds[i] == Http3StreamFrameEventKind::kHeaders) {
                        RUVIA_CHECK_EQ(events.payloads[i], headers == 0 ? std::string("head") : std::string("tail"));
                        RUVIA_CHECK_EQ(events.trailers[i], headers != 0);
                        RUVIA_CHECK(events.ends[i]);
                        ++headers;
                    } else {
                        RUVIA_CHECK(events.kinds[i] == Http3StreamFrameEventKind::kData);
                        body += events.payloads[i];
                        data_ends += events.ends[i];
                    }
                }
                RUVIA_CHECK_EQ(headers, std::size_t{2});
                RUVIA_CHECK_EQ(data_ends, std::size_t{1});
                RUVIA_CHECK_EQ(body, std::string("body"));
            }
        }
    }
}

RUVIA_TEST(http3_stream_frames_rejects_fin_inside_any_frame_header_width) {
    std::pmr::monotonic_buffer_resource resource;
    for (std::size_t type_width : {1, 2, 4, 8}) {
        for (std::size_t length_width : {1, 2, 4, 8}) {
            std::vector<char> wire;
            append_frame(wire, 1, "", type_width, length_width);
            for (std::size_t prefix = 1; prefix < wire.size(); ++prefix) {
                Http3StreamFrames decoder(Http3StreamKind::kRequest, &resource);
                Events events;
                RUVIA_CHECK(decoder.feed(std::span<const char>(wire).first(prefix), true, collect, &events) == Http3StreamFrameStatus::kFrameError);
                RUVIA_CHECK_EQ(decoder.consumedBytes(), prefix);
                RUVIA_CHECK(events.kinds.empty());
            }
        }
    }
}

RUVIA_TEST(http3_stream_frames_retains_paused_headers_before_consuming_following_frames) {
    std::pmr::unsynchronized_pool_resource resource;
    for (std::size_t width : {1, 2, 4, 8}) {
        const auto header_size = 2 * width;
        for (std::size_t split = 0; split < header_size; ++split) {
            std::vector<char> wire;
            append_frame(wire, 1, "head", width, width);
            const auto first_frame_size = wire.size();
            append_frame(wire, 0, "body", width, width);
            Http3StreamFrames decoder(Http3StreamKind::kRequest, &resource);
            paused_headers state{decoder};
            RUVIA_CHECK(decoder.feed(std::span<const char>(wire).first(split), false, collect_and_pause, &state) == Http3StreamFrameStatus::kNeedMoreData);
            RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(split), true, collect_and_pause, &state) == Http3StreamFrameStatus::kPaused);
            RUVIA_CHECK_EQ(decoder.consumedBytes(), first_frame_size - split);
            RUVIA_CHECK(decoder.paused());
            for (std::size_t i = 0; i < first_frame_size; ++i) {
                wire[i] = '!';
            }
            RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(first_frame_size), true, collect_and_pause, &state) == Http3StreamFrameStatus::kMessageEnd);
            RUVIA_CHECK_EQ(decoder.consumedBytes(), wire.size() - first_frame_size);
            RUVIA_CHECK(!decoder.paused());
            RUVIA_CHECK_EQ(state.events.kinds.size(), std::size_t{3});
            if (state.events.kinds.size() == 3) {
                RUVIA_CHECK_EQ(state.events.payloads[0], std::string("head"));
                RUVIA_CHECK_EQ(state.events.payloads[1], std::string("head"));
                RUVIA_CHECK_EQ(state.events.payloads[2], std::string("body"));
                RUVIA_CHECK(!state.events.fins[0]);
                RUVIA_CHECK(!state.events.fins[1]);
                RUVIA_CHECK(state.events.fins[2]);
            }
        }
    }
}
