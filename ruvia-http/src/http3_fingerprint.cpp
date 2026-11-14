#include <cstddef>
#include <cstdint>
#include <string_view>

#include "decimal_text.h"
#include "http3_connection.h"

namespace ruvia {

http3_fingerprint_collector::http3_fingerprint_collector(http3_fingerprint_options options) :
    max_frames_(options.max_frames_),
    max_bytes_(options.max_bytes_) {}

void http3_fingerprint_collector::reset() noexcept {
    control_frames_.clear();
    pseudo_headers_.clear();
    fingerprint_.clear();
    frames_seen_ = 0;
    bytes_seen_ = 0;
    first_request_stream_id_ = -1;
    saw_regular_header_ = false;
    phase_ = phase::collecting;
}

void http3_fingerprint_collector::observe_frame(const http3_frame_metadata& frame) noexcept {
    if (phase_ != phase::collecting) {
        return;
    }
    try {
        ++frames_seen_;
        if (frames_seen_ > max_frames_ || frame.payload_length_ > max_bytes_ - bytes_seen_) {
            phase_ = phase::failed;
            return;
        }
        bytes_seen_ += static_cast<std::size_t>(frame.payload_length_);
        // Client unidirectional control stream has stream_id % 4 == 2 (usually stream 2).
        if (frame.stream_id_ >= 0 && (frame.stream_id_ % 4) == 2) {
            if (!control_frames_.empty()) {
                control_frames_.push_back(';');
            }
            http_append_unsigned(control_frames_, static_cast<std::uint64_t>(frame.type_));
            control_frames_.push_back(':');
            http_append_unsigned(control_frames_, frame.payload_length_);
        }
    } catch (...) {
        phase_ = phase::failed;
    }
}

void http3_fingerprint_collector::observe_field(const http3_field_metadata& field) noexcept {
    if (phase_ != phase::collecting || field.section_ != http3_field_section_kind::header) {
        return;
    }
    try {
        switch (field.phase_) {
            case http3_field_observation_phase::begin:
                if (field.stream_id_ < 0 || (field.stream_id_ % 4) != 0) {
                    phase_ = phase::failed;
                    return;
                }
                if (first_request_stream_id_ < 0) {
                    first_request_stream_id_ = field.stream_id_;
                }
                break;
            case http3_field_observation_phase::field: {
                if (first_request_stream_id_ < 0) {
                    if (field.stream_id_ < 0 || (field.stream_id_ % 4) != 0) {
                        phase_ = phase::failed;
                        return;
                    }
                    first_request_stream_id_ = field.stream_id_;
                }
                if (field.stream_id_ != first_request_stream_id_) {
                    return;
                }
                const std::size_t field_bytes = field.name_.size() + field.value_.size();
                if (field_bytes > max_bytes_ - bytes_seen_) {
                    phase_ = phase::failed;
                    return;
                }
                bytes_seen_ += field_bytes;
                if (!field.name_.empty() && field.name_.front() == ':') {
                    if (saw_regular_header_) {
                        phase_ = phase::failed;
                        return;
                    }
                    std::string_view token{};
                    if (field.name_ == ":method") {
                        token = "m";
                    } else if (field.name_ == ":authority") {
                        token = "a";
                    } else if (field.name_ == ":scheme") {
                        token = "s";
                    } else if (field.name_ == ":path") {
                        token = "p";
                    } else if (field.name_ == ":protocol") {
                        token = "pr";
                    } else if (field.name_.size() > 1U) {
                        token = field.name_.substr(1, 1);
                    }
                    if (!token.empty()) {
                        if (!pseudo_headers_.empty()) {
                            pseudo_headers_.push_back(',');
                        }
                        pseudo_headers_.append(token);
                    }
                } else {
                    saw_regular_header_ = true;
                }
                break;
            }
            case http3_field_observation_phase::end:
                if (field.stream_id_ != first_request_stream_id_ || first_request_stream_id_ < 0) {
                    return;
                }
                if (pseudo_headers_.empty()) {
                    phase_ = phase::failed;
                    return;
                }
                fingerprint_.clear();
                fingerprint_.append(control_frames_);
                fingerprint_.push_back('|');
                http_append_unsigned(fingerprint_, static_cast<std::uint64_t>(first_request_stream_id_));
                fingerprint_.push_back('|');
                fingerprint_.append(pseudo_headers_);
                phase_ = phase::complete;
                break;
        }
    } catch (...) {
        phase_ = phase::failed;
    }
}

}  // namespace ruvia
