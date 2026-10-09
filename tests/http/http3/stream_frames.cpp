#include <array>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http3_connection_error.h"
#include "ruvia/http/http3_stream_frames.h"

#include "test_harness.h"

namespace {

using ruvia::http3_connection_error_code;
using ruvia::http3_stream_frame_event;
using ruvia::http3_stream_frame_event_kind;
using ruvia::http3_stream_frame_status;
using ruvia::http3_stream_frames;
using ruvia::http3_stream_kind;

struct events final {
    std::vector<http3_stream_frame_event_kind> kinds_;
    std::vector<std::string> payloads_;
    std::vector<bool> ends_;
    std::vector<bool> fins_;
    std::vector<bool> trailers_;
};

void collect(void* context_value, http3_stream_frame_event event) {
    auto& events_value = *static_cast<events*>(context_value);
    events_value.kinds_.push_back(event.kind_);
    events_value.payloads_.emplace_back(event.payload_.data(), event.payload_.size());
    events_value.ends_.push_back(event.end_frame_);
    events_value.fins_.push_back(event.fin_);
    events_value.trailers_.push_back(event.trailers_);
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

void append_frame(std::vector<char>& wire, std::uint64_t type, std::string_view payload_value,
    std::size_t type_width, std::size_t length_width) {
    append_varint(wire, type, type_width);
    append_varint(wire, payload_value.size(), length_width);
    wire.insert(wire.end(), payload_value.begin(), payload_value.end());
}

struct paused_headers final {
    http3_stream_frames& decoder_;
    events events_;
    bool pause_next_ = true;
};

void collect_and_pause(void* context_value, http3_stream_frame_event event) {
    auto& state_value = *static_cast<paused_headers*>(context_value);
    collect(&state_value.events_, event);
    if (event.kind_ == http3_stream_frame_event_kind::headers && state_value.pause_next_) {
        state_value.pause_next_ = false;
        state_value.decoder_.pause();
    }
}

}  // namespace

RUVIA_TEST(http3_stream_frame_status_maps_to_connection_errors_only_for_errors) {
    RUVIA_CHECK(!ruvia::http3_connection_error_code_for_stream_frame_status(
        http3_stream_frame_status::need_more_data));
    RUVIA_CHECK(!ruvia::http3_connection_error_code_for_stream_frame_status(
        http3_stream_frame_status::message_end));
    RUVIA_CHECK(
        ruvia::http3_connection_error_code_for_stream_frame_status(
            http3_stream_frame_status::frame_unexpected) ==
        http3_connection_error_code::frame_unexpected);
    RUVIA_CHECK(
        ruvia::http3_connection_error_code_for_stream_frame_status(
            http3_stream_frame_status::push_promise) ==
        http3_connection_error_code::frame_unexpected);
    RUVIA_CHECK(
        ruvia::http3_connection_error_code_for_stream_frame_status(
            http3_stream_frame_status::missing_settings) ==
        http3_connection_error_code::missing_settings);
    RUVIA_CHECK(
        ruvia::http3_connection_error_code_for_stream_frame_status(
            http3_stream_frame_status::frame_error) ==
        http3_connection_error_code::frame_error);
    RUVIA_CHECK(
        ruvia::http3_connection_error_code_for_stream_frame_status(
            http3_stream_frame_status::closed_critical_stream) ==
        http3_connection_error_code::closed_critical_stream);
    RUVIA_CHECK(
        ruvia::http3_connection_error_code_for_stream_frame_status(
            http3_stream_frame_status::limit) ==
        http3_connection_error_code::excessive_load);
}

RUVIA_TEST(http3_stream_frames_incrementally_delivers_data_and_aggregates_headers) {
    std::pmr::monotonic_buffer_resource resource;
    http3_stream_frames decoder(http3_stream_kind::request, &resource);
    events events;
    constexpr std::array<char, 12> wire{0x1, 0x2, 'h', '1', 0x0, 0x3, 'a', 'b', 'c',
        0x1, 0x1, 't'};

    RUVIA_CHECK(decoder.feed(std::span<const char>(wire).first(1), false, collect, &events) ==
                http3_stream_frame_status::need_more_data);
    RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(1, 5), false, collect, &events) ==
                http3_stream_frame_status::need_more_data);
    RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(6, 3), false, collect, &events) ==
                http3_stream_frame_status::need_more_data);
    RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(9), true, collect, &events) ==
                http3_stream_frame_status::message_end);
    RUVIA_CHECK_EQ(events.kinds_.size(), std::size_t{3});
    if (events.kinds_.size() == 3) {
        RUVIA_CHECK(events.kinds_[0] == http3_stream_frame_event_kind::headers);
        RUVIA_CHECK_EQ(events.payloads_[0], std::string("h1"));
        RUVIA_CHECK(events.ends_[0]);
        RUVIA_CHECK(events.kinds_[1] == http3_stream_frame_event_kind::data);
        RUVIA_CHECK_EQ(events.payloads_[1], std::string("abc"));
        RUVIA_CHECK(events.ends_[1]);
        RUVIA_CHECK(events.kinds_[2] == http3_stream_frame_event_kind::headers);
        RUVIA_CHECK_EQ(events.payloads_[2], std::string("t"));
        RUVIA_CHECK(events.trailers_[2]);
        RUVIA_CHECK(events.fins_[2]);
    }
}

RUVIA_TEST(http3_stream_frames_delivers_informational_and_final_response_headers) {
    std::pmr::monotonic_buffer_resource resource;
    http3_stream_frames decoder(http3_stream_kind::response, &resource);
    events events;
    constexpr std::array<char, 9> wire{0x1, 0x1, 'i', 0x1, 0x1, 'f', 0x0, 0x1, 'x'};
    RUVIA_CHECK(decoder.feed(wire, true, collect, &events) == http3_stream_frame_status::message_end);
    RUVIA_CHECK_EQ(events.kinds_.size(), std::size_t{3});
    if (events.kinds_.size() == 3) {
        RUVIA_CHECK_EQ(events.payloads_[0], std::string("i"));
        RUVIA_CHECK_EQ(events.payloads_[1], std::string("f"));
        RUVIA_CHECK(!events.trailers_[1]);
        RUVIA_CHECK_EQ(events.payloads_[2], std::string("x"));
        RUVIA_CHECK(events.fins_[2]);
    }
}

RUVIA_TEST(http3_stream_frames_rejects_invalid_request_frame_order_and_truncation) {
    std::pmr::monotonic_buffer_resource resource;
    events events;
    constexpr std::array<char, 2> data_first{0x0, 0x0};
    http3_stream_frames data_decoder(http3_stream_kind::request, &resource);
    RUVIA_CHECK(data_decoder.feed(data_first, false, collect, &events) ==
                http3_stream_frame_status::frame_unexpected);

    constexpr std::array<char, 3> truncated{0x1, 0x2, 'x'};
    http3_stream_frames truncated_decoder(http3_stream_kind::request, &resource);
    RUVIA_CHECK(truncated_decoder.feed(truncated, true, collect, &events) ==
                http3_stream_frame_status::frame_error);

    constexpr std::array<char, 6> after_trailers{0x1, 0x0, 0x1, 0x0, 0x0, 0x0};
    http3_stream_frames order_decoder(http3_stream_kind::request, &resource);
    RUVIA_CHECK(order_decoder.feed(after_trailers, false, collect, &events) ==
                http3_stream_frame_status::frame_unexpected);
}

RUVIA_TEST(http3_stream_frames_requires_settings_and_rejects_fin_on_control_stream) {
    std::pmr::monotonic_buffer_resource resource;
    events events;
    http3_stream_frames missing(http3_stream_kind::control, &resource);
    constexpr std::array<char, 2> wrong_first{0x0, 0x0};
    RUVIA_CHECK(missing.feed(wrong_first, false, collect, &events) ==
                http3_stream_frame_status::missing_settings);

    http3_stream_frames critical(http3_stream_kind::control, &resource);
    constexpr std::array<char, 2> settings{0x4, 0x0};
    RUVIA_CHECK(critical.feed(settings, true, collect, &events) ==
                http3_stream_frame_status::closed_critical_stream);
}

RUVIA_TEST(http3_stream_frames_bounds_settings_payload_before_delivery) {
    std::pmr::monotonic_buffer_resource resource;
    events events;
    http3_stream_frames decoder(http3_stream_kind::control, &resource,
        {.max_settings_payload_bytes_ = 2});
    constexpr std::array<char, 2> header_value{0x4, 0x3};
    RUVIA_CHECK(decoder.feed(header_value, false, collect, &events) ==
                http3_stream_frame_status::limit);
    RUVIA_CHECK(events.kinds_.empty());
}

RUVIA_TEST(http3_stream_frames_accepts_cancel_push_on_control_but_rejects_reserved_types) {
    std::pmr::monotonic_buffer_resource resource;
    events events;
    http3_stream_frames control(http3_stream_kind::control, &resource);
    constexpr std::array<char, 4> settings_and_cancel{0x4, 0x0, 0x3, 0x0};
    RUVIA_CHECK(control.feed(settings_and_cancel, false, collect, &events) ==
                http3_stream_frame_status::need_more_data);
    constexpr std::array<char, 2> reserved{0x2, 0x0};
    RUVIA_CHECK(control.feed(reserved, false, collect, &events) ==
                http3_stream_frame_status::frame_unexpected);

    http3_stream_frames request(http3_stream_kind::request, &resource);
    constexpr std::array<char, 4> headers_and_reserved{0x1, 0x0, 0x6, 0x0};
    RUVIA_CHECK(request.feed(headers_and_reserved, false, collect, &events) ==
                http3_stream_frame_status::frame_unexpected);
}

RUVIA_TEST(http3_stream_frames_skips_unknown_payload_without_buffering) {
    std::pmr::monotonic_buffer_resource resource;
    http3_stream_frames decoder(http3_stream_kind::request, &resource);
    events events;
    constexpr std::array<char, 2> headers{0x1, 0x0};
    RUVIA_CHECK(decoder.feed(headers, false, collect, &events) == http3_stream_frame_status::need_more_data);
    constexpr std::array<char, 5> extension_prefix{0x21, static_cast<char>(0x80), 0x00, 0x40, 0x00};
    RUVIA_CHECK(decoder.feed(extension_prefix, false, collect, &events) ==
                http3_stream_frame_status::need_more_data);
    std::array<char, 4096> payload_value{};
    for (int i = 0; i < 4; ++i) {
        RUVIA_CHECK(decoder.feed(payload_value, false, collect, &events) == http3_stream_frame_status::need_more_data);
    }
    RUVIA_CHECK_EQ(events.kinds_.size(), std::size_t{1});
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
                http3_stream_frames decoder(http3_stream_kind::request, &resource);
                events events;
                RUVIA_CHECK(decoder.feed(wire.first(split), false, collect, &events) == http3_stream_frame_status::need_more_data);
                RUVIA_CHECK_EQ(decoder.consumed_bytes(), split);
                RUVIA_CHECK(decoder.feed(wire.subspan(split), true, collect, &events) == http3_stream_frame_status::message_end);
                RUVIA_CHECK_EQ(decoder.consumed_bytes(), wire.size() - split);
                std::string body;
                std::size_t headers = 0;
                std::size_t data_ends = 0;
                for (std::size_t i = 0; i < events.kinds_.size(); ++i) {
                    if (events.kinds_[i] == http3_stream_frame_event_kind::headers) {
                        RUVIA_CHECK_EQ(events.payloads_[i], headers == 0 ? std::string("head") : std::string("tail"));
                        RUVIA_CHECK_EQ(events.trailers_[i], headers != 0);
                        RUVIA_CHECK(events.ends_[i]);
                        ++headers;
                    } else {
                        RUVIA_CHECK(events.kinds_[i] == http3_stream_frame_event_kind::data);
                        body += events.payloads_[i];
                        data_ends += events.ends_[i];
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
                http3_stream_frames decoder(http3_stream_kind::request, &resource);
                events events;
                RUVIA_CHECK(decoder.feed(std::span<const char>(wire).first(prefix), true, collect, &events) == http3_stream_frame_status::frame_error);
                RUVIA_CHECK_EQ(decoder.consumed_bytes(), prefix);
                RUVIA_CHECK(events.kinds_.empty());
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
            http3_stream_frames decoder(http3_stream_kind::request, &resource);
            paused_headers state_value{decoder};
            RUVIA_CHECK(decoder.feed(std::span<const char>(wire).first(split), false, collect_and_pause, &state_value) == http3_stream_frame_status::need_more_data);
            RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(split), true, collect_and_pause, &state_value) == http3_stream_frame_status::paused);
            RUVIA_CHECK_EQ(decoder.consumed_bytes(), first_frame_size - split);
            RUVIA_CHECK(decoder.paused());
            for (std::size_t i = 0; i < first_frame_size; ++i) {
                wire[i] = '!';
            }
            RUVIA_CHECK(decoder.feed(std::span<const char>(wire).subspan(first_frame_size), true, collect_and_pause, &state_value) == http3_stream_frame_status::message_end);
            RUVIA_CHECK_EQ(decoder.consumed_bytes(), wire.size() - first_frame_size);
            RUVIA_CHECK(!decoder.paused());
            RUVIA_CHECK_EQ(state_value.events_.kinds_.size(), std::size_t{3});
            if (state_value.events_.kinds_.size() == 3) {
                RUVIA_CHECK_EQ(state_value.events_.payloads_[0], std::string("head"));
                RUVIA_CHECK_EQ(state_value.events_.payloads_[1], std::string("head"));
                RUVIA_CHECK_EQ(state_value.events_.payloads_[2], std::string("body"));
                RUVIA_CHECK(!state_value.events_.fins_[0]);
                RUVIA_CHECK(!state_value.events_.fins_[1]);
                RUVIA_CHECK(state_value.events_.fins_[2]);
            }
        }
    }
}
