#include <cstddef>
#include <cstdint>
#include <string_view>

#include "decimal_text.h"
#include "http2_connection.h"
#include "http2_frame.h"

namespace ruvia {
http2_akamai_fingerprint_collector::http2_akamai_fingerprint_collector(http2_akamai_fingerprint_options options) :
    max_frames_(options.max_frames_),
    max_bytes_(options.max_bytes_) {}

void http2_akamai_fingerprint_collector::reset() noexcept {
    settings_.clear();
    priorities_.clear();
    pseudo_headers_.clear();
    fingerprint_.clear();
    frames_seen_ = 0;
    bytes_seen_ = 0;
    window_update_ = 0;
    first_request_stream_id_ = 0;
    has_window_update_ = false;
    saw_regular_header_ = false;
    phase_ = phase::collecting;
}

void http2_akamai_fingerprint_collector::observe_frame(const http2_frame_metadata& frame) noexcept {
    if (phase_ != phase::collecting) {
        return;
    }
    try {
        ++frames_seen_;
        const std::size_t frame_bytes = 9U + static_cast<std::size_t>(frame.payload_length_);
        if (frames_seen_ > max_frames_ || frame_bytes > max_bytes_ - bytes_seen_) {
            phase_ = phase::failed;
            return;
        }
        bytes_seen_ += frame_bytes;

        const auto append_priority = [&](const http2_received_priority& pri) {
            if (!priorities_.empty()) {
                priorities_.push_back(',');
            }
            http_append_unsigned(priorities_, pri.stream_id_);
            priorities_.push_back(':');
            priorities_.push_back(pri.exclusive_ ? '1' : '0');
            priorities_.push_back(':');
            http_append_unsigned(priorities_, pri.stream_dependency_);
            priorities_.push_back(':');
            http_append_unsigned(priorities_, pri.weight_);
        };

        switch (frame.type_) {
            case http2_frame_type::settings:
                if ((frame.flags_ & 0x01U) != 0) {
                    break;
                }
                if (!frame.settings_complete_ || frame.stream_id_ != 0) {
                    phase_ = phase::failed;
                    return;
                }
                for (const auto& entry : frame.settings()) {
                    if (!settings_.empty()) {
                        settings_.push_back(';');
                    }
                    http_append_unsigned(settings_, static_cast<std::uint16_t>(entry.id_));
                    settings_.push_back(':');
                    http_append_unsigned(settings_, entry.value_);
                }
                break;
            case http2_frame_type::window_update:
                if (frame.stream_id_ == 0 && frame.window_increment_.has_value()) {
                    window_update_ = *frame.window_increment_;
                    has_window_update_ = true;
                }
                break;
            case http2_frame_type::priority:
                if (frame.priority_.has_value()) {
                    append_priority(*frame.priority_);
                }
                break;
            case http2_frame_type::headers:
                if (frame.priority_.has_value() && (first_request_stream_id_ == 0 || frame.stream_id_ == first_request_stream_id_)) {
                    append_priority(*frame.priority_);
                }
                break;
            default:
                break;
        }
    } catch (...) {
        phase_ = phase::failed;
    }
}

void http2_akamai_fingerprint_collector::observe_header(const http2_header_metadata& header) noexcept {
    if (phase_ != phase::collecting) {
        return;
    }
    try {
        switch (header.phase_) {
            case http2_header_observation_phase::begin:
                if (header.stream_id_ == 0) {
                    phase_ = phase::failed;
                    return;
                }
                if (first_request_stream_id_ == 0) {
                    first_request_stream_id_ = header.stream_id_;
                }
                break;
            case http2_header_observation_phase::field: {
                if (first_request_stream_id_ == 0) {
                    if (header.stream_id_ == 0) {
                        phase_ = phase::failed;
                        return;
                    }
                    first_request_stream_id_ = header.stream_id_;
                }
                if (header.stream_id_ != first_request_stream_id_) {
                    return;
                }
                const std::size_t field_bytes = header.name_.size() + header.value_.size();
                if (field_bytes > max_bytes_ - bytes_seen_) {
                    phase_ = phase::failed;
                    return;
                }
                bytes_seen_ += field_bytes;
                if (!saw_regular_header_ && !header.name_.empty() && header.name_.front() == ':') {
                    std::string_view token{};
                    if (header.name_ == ":method") {
                        token = "m";
                    } else if (header.name_ == ":authority") {
                        token = "a";
                    } else if (header.name_ == ":scheme") {
                        token = "s";
                    } else if (header.name_ == ":path") {
                        token = "p";
                    } else if (header.name_.size() > 1U) {
                        token = header.name_.substr(1, 1);
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
            case http2_header_observation_phase::end:
                if (header.stream_id_ != first_request_stream_id_ || first_request_stream_id_ == 0) {
                    return;
                }
                if (pseudo_headers_.empty()) {
                    phase_ = phase::failed;
                    return;
                }
                fingerprint_.clear();
                fingerprint_.append(settings_);
                fingerprint_.push_back('|');
                if (has_window_update_) {
                    http_append_unsigned(fingerprint_, window_update_);
                } else {
                    fingerprint_.append("00");
                }
                fingerprint_.push_back('|');
                if (priorities_.empty()) {
                    fingerprint_.push_back('0');
                } else {
                    fingerprint_.append(priorities_);
                }
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
