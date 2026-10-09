#include "http3/http3_response_stream_sink.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http_response_stream.h"

#include "http3/http3_buffered_request_dispatch.h"

namespace ruvia::detail {
http3_response_stream_sink::http3_response_stream_sink(http3_buffered_request_dispatch& publisher,
    const worker_handle& worker_value, http_known_method method, http_response_stream_kind kind,
    std::pmr::memory_resource* resource, http_response_coding_selection coding,
    http_response_coding_availability availability)
    : publisher_(publisher),
      worker_(worker_value),
      method_(method),
      kind_(kind),
      resource_(resource),
      compression_(resource, coding, availability) {}

bool http3_response_stream_sink::aborted() const noexcept {
    return state_.aborted() || publisher_.response_aborted();
}
void http3_response_stream_sink::require_active() const {
    if (aborted()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
}
task<timer_sleep_result> http3_response_stream_sink::sleep(std::chrono::milliseconds duration, const stop_token& stop) {
    require_active();
    co_return co_await sleep_for(worker_, duration, stop);
}
task<void> http3_response_stream_sink::commit(http_response_trailer_intent trailers) {
    require_active();
    if (state_.committed()) {
        if (trailers == http_response_trailer_intent::present) {
            state_.ensure_trailers_allowed(http_response_stream_trailer_framing::http3_trailing_headers);
        }
        co_return;
    }
    auto response = co_await state_.streaming_head();
    require_active();
    compression_.prepare(method_, response, kind_);
    auto prepared = publisher_.encode_streaming_response_head(std::move(response), method_, kind_, trailers);
    if ((prepared.index() != 0)) {
        if (std::get<1>(prepared).kind_ == http3_response_head_error::peer_field_section_limit) {
            publisher_.reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 streaming response head");
    }
    if (!publisher_.response_field_section_allowed(std::get<0>(prepared).head_.field_section_.decoded_field_section_size())) {
        publisher_.reject_peer_field_section();
    }
    data_.emplace(std::get<0>(prepared).head_.body_plan_, std::get<0>(prepared).head_.declared_content_length_);
    compression_.activate(std::get<0>(prepared).head_.body_plan_);
    // A handoff can accept a prefix before cancellation. Mark commitment before
    // entering that operation so recovery never emits a second response head.
    publisher_.commit_final_response();
    state_.mark_committed(std::get<0>(prepared).commit_plan_);
    co_await publisher_.publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(prepared).head_.field_section_.field_section_);
    if (std::get<0>(prepared).commit_plan_.head_disposition() == http_response_stream_head_disposition::message_ended) {
        co_await publisher_.finish_response();
    }
}
task<void> http3_response_stream_sink::write_encoded(std::string_view bytes_value) {
    constexpr std::size_t block_bytes = 16 * 1024;
    for (std::size_t offset = 0; offset < bytes_value.size();) {
        require_active();
        const auto count = std::min(block_bytes, bytes_value.size() - offset);
        const auto planned = data_->plan_chunk(std::span<const char>(bytes_value.data() + offset, count), false);
        if ((planned.index() != 0)) {
            throw std::length_error("HTTP/3 response content length exceeded");
        }
        co_await publisher_.publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::data), std::get<0>(planned).payload_);
        if ((data_->commit_payload(count, false).index() != 0)) {
            std::terminate();
        }
        offset += count;
    }
}
task<void> http3_response_stream_sink::write(std::string_view bytes_value) {
    require_active();
    if (bytes_value.empty()) {
        co_return;
    }
    co_await commit(http_response_trailer_intent::none);
    if (state_.body_suppressed_complete()) {
        co_await sleep_for(worker_, std::chrono::steady_clock::duration(1));
    }
    state_.ensure_body_allowed();
    if (!compression_.active()) {
        co_await write_encoded(bytes_value);
        co_return;
    }
    constexpr std::size_t block_bytes = 16 * 1024;
    for (std::size_t offset = 0; offset < bytes_value.size();) {
        const auto count = std::min(block_bytes, bytes_value.size() - offset);
        try {
            compression_.write(bytes_value.substr(offset, count));
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
        co_await write_encoded(compression_.output());
        offset += count;
    }
}
task<void> http3_response_stream_sink::end(std::span<const http_header_view> trailers) {
    if (state_.ended()) {
        if (!trailers.empty()) {
            throw std::logic_error("HTTP/3 response stream already ended");
        }
        co_return;
    }
    require_active();
    const auto section = validate_http_response_trailers(trailers);
    co_await commit(response_trailer_intent(section));
    if (state_.ended()) {
        co_return;
    }
    if (compression_.active()) {
        try {
            compression_.finish();
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
        co_await write_encoded(compression_.output());
    }
    const auto finishing = data_->plan_chunk({}, true);
    if ((finishing.index() != 0)) {
        throw std::length_error("HTTP/3 response content length incomplete");
    }
    if (!trailers.empty()) {
        state_.ensure_trailers_allowed(http_response_stream_trailer_framing::http3_trailing_headers);
        std::pmr::vector<http3_field_section_field_view> fields(resource_);
        for (const auto& field : trailers) {
            fields.push_back({field.name(), field.value(), false});
        }
        auto encoded = publisher_.encode_response_trailers(fields);
        if ((encoded.index() != 0)) {
            if (std::get<1>(encoded).kind_ == http3_response_head_error::peer_field_section_limit) {
                publisher_.reject_peer_field_section();
            }
            throw std::invalid_argument("invalid HTTP/3 response trailers");
        }
        if (!publisher_.response_field_section_allowed(std::get<0>(encoded).decoded_field_section_size())) {
            publisher_.reject_peer_field_section();
        }
        co_await publisher_.publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(encoded).field_section_);
    }
    co_await publisher_.finish_response();
    if ((data_->commit_payload(0, true).index() != 0)) {
        std::terminate();
    }
    state_.mark_ended();
}
}  // namespace ruvia::detail
