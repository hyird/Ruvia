#include "ruvia/http/http3_data_write_plan.h"

#include <variant>

#include "ruvia/http/http3_var_int.h"

namespace ruvia {

http3_data_write_plan::http3_data_write_plan(http_response_body_plan body_plan,
    std::optional<std::uint64_t> declared_content_length) noexcept
    : response_body_plan_(body_plan),
      declared_content_length_(declared_content_length) {}

http3_data_write_plan::http3_data_write_plan(http3_client_request_body_plan body_plan) noexcept
    : declared_content_length_(body_plan.expected_length_),
      request_body_(true) {}

bool http3_data_write_plan::body_allowed() const noexcept {
    return request_body_ ||
           response_body_plan_->content_semantics() == http_response_content_semantics::connect_tunnel ||
           (response_body_plan_->status_allows_body() && !response_body_plan_->body_suppressed());
}

bool http3_data_write_plan::fin_allowed() const noexcept {
    return !finished_ && !write_pending_ &&
           (request_body_ ||
               !(response_body_plan_->content_semantics() == http_response_content_semantics::connect_tunnel &&
                   declared_content_length_)) &&
           (!body_allowed() || !declared_content_length_ ||
               committed_payload_bytes_ == *declared_content_length_);
}

std::uint64_t http3_data_write_plan::committed_payload_bytes() const noexcept {
    return committed_payload_bytes_;
}

bool http3_data_write_plan::finished() const noexcept {
    return finished_;
}

std::variant<http3_data_write_plan::chunk_type, http3_data_write_error> http3_data_write_plan::plan_chunk(
    std::span<const char> payload_value, bool finishing) noexcept {
    if (finished_) {
        return http3_data_write_error::already_finished;
    }
    if (write_pending_) {
        return http3_data_write_error::write_already_pending;
    }
    if (!request_body_ &&
        response_body_plan_->content_semantics() == http_response_content_semantics::connect_tunnel &&
        declared_content_length_) {
        return http3_data_write_error::content_length_forbidden;
    }
    if (!payload_value.empty() && !body_allowed()) {
        return http3_data_write_error::body_not_allowed;
    }
    if (payload_value.size() > http3_var_int_max) {
        return http3_data_write_error::var_int_out_of_range;
    }
    const auto bytes_value = static_cast<std::uint64_t>(payload_value.size());
    if (bytes_value > UINT64_MAX - committed_payload_bytes_) {
        return http3_data_write_error::content_length_overflow;
    }
    const auto total = committed_payload_bytes_ + bytes_value;
    if (declared_content_length_ && total > *declared_content_length_) {
        return http3_data_write_error::content_length_mismatch;
    }
    if (finishing && declared_content_length_ && total != *declared_content_length_ &&
        (request_body_ || body_allowed())) {
        return http3_data_write_error::content_length_mismatch;
    }

    chunk_type chunk{.payload_ = payload_value,
        .emits_data_ = !payload_value.empty(),
        .finishing_ = finishing};
    if (chunk.emits_data_) {
        const auto encoded = encode_http3_frame_header(chunk.frame_header_,
            static_cast<std::uint64_t>(http3_frame_type::data), bytes_value);
        if ((encoded.index() != 0)) {
            return std::get<1>(encoded) == http3_codec_error::value_out_of_range
                       ? http3_data_write_error::var_int_out_of_range
                       : http3_data_write_error::frame_header_encoding;
        }
        chunk.frame_header_size_ = std::get<0>(encoded);
    }
    pending_payload_bytes_ = bytes_value;
    pending_finishing_ = finishing;
    write_pending_ = true;
    return chunk;
}

std::variant<std::monostate, http3_data_write_error> http3_data_write_plan::commit_payload(
    std::uint64_t bytes_value, bool finishing) noexcept {
    if (finished_) {
        return http3_data_write_error::already_finished;
    }
    if (!write_pending_) {
        return http3_data_write_error::no_write_pending;
    }
    if (bytes_value != pending_payload_bytes_ || finishing != pending_finishing_) {
        return http3_data_write_error::commit_does_not_match_plan;
    }
    // The planned total was checked before any bytes were committed.
    committed_payload_bytes_ += bytes_value;
    write_pending_ = false;
    pending_payload_bytes_ = 0;
    pending_finishing_ = false;
    finished_ = finishing;
    return {};
}

}  // namespace ruvia
