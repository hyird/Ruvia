#include "ruvia/http/http3_stream_frames.h"

#include <algorithm>
#include <limits>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_var_int.h"

namespace ruvia {

std::size_t http3_stream_frames::field_section_buffer_limit() const noexcept {
    const auto overhead = frame_type_ == 0x5 ? std::size_t{8} : std::size_t{0};
    return config_.max_field_section_size_ + std::min(overhead,
                                                 std::numeric_limits<std::size_t>::max() - config_.max_field_section_size_);
}

std::optional<http3_connection_error_code> http3_connection_error_code_for_stream_frame_status(
    http3_stream_frame_status status) noexcept {
    switch (status) {
        case http3_stream_frame_status::need_more_data:
        case http3_stream_frame_status::paused:
        case http3_stream_frame_status::message_end:
            return std::nullopt;
        case http3_stream_frame_status::frame_unexpected:
        case http3_stream_frame_status::push_promise:
            return http3_connection_error_code::frame_unexpected;
        case http3_stream_frame_status::missing_settings:
            return http3_connection_error_code::missing_settings;
        case http3_stream_frame_status::frame_error:
            return http3_connection_error_code::frame_error;
        case http3_stream_frame_status::closed_critical_stream:
            return http3_connection_error_code::closed_critical_stream;
        case http3_stream_frame_status::limit:
            return http3_connection_error_code::excessive_load;
    }
    return http3_connection_error_code::frame_error;
}

http3_stream_frames::http3_stream_frames(http3_stream_kind kind, std::pmr::memory_resource* resource,
    http3_stream_frames_config config) noexcept
    : kind_(kind),
      config_(config),
      resource_(resource),
      field_section_(resource) {}

http3_stream_frame_status http3_stream_frames::begin_frame(std::uint64_t type, std::uint64_t length) {
    frame_type_ = type;
    frame_length_ = length;
    remaining_ = frame_length_;

    // HTTP/2 frame types are reserved in HTTP/3 and cannot be skipped as extensions.
    if (frame_type_ == 0x2 || frame_type_ == 0x6 || frame_type_ == 0x8 || frame_type_ == 0x9) {
        return http3_stream_frame_status::frame_unexpected;
    }
    const bool known_frame = frame_type_ == 0x0 || frame_type_ == 0x1 || frame_type_ == 0x3 ||
                             frame_type_ == 0x4 || frame_type_ == 0x5 || frame_type_ == 0x7 ||
                             frame_type_ == 0xd || frame_type_ == 0xf0700 || frame_type_ == 0xf0701;
    if (kind_ == http3_stream_kind::control) {
        if (first_frame_ && frame_type_ != 0x4) {
            return http3_stream_frame_status::missing_settings;
        }
        if ((frame_type_ == 0xc || frame_type_ == 0xf0700 || frame_type_ == 0xf0701) && frame_length_ > config_.max_settings_payload_bytes_) {
            return http3_stream_frame_status::limit;
        }
        if (frame_type_ == 0x4) {
            if (settings_seen_) {
                return http3_stream_frame_status::frame_unexpected;
            }
            if (frame_length_ > config_.max_settings_payload_bytes_) {
                return http3_stream_frame_status::limit;
            }
            settings_seen_ = true;
        } else if (frame_type_ == 0x0 || frame_type_ == 0x1 || frame_type_ == 0x5) {
            return http3_stream_frame_status::frame_unexpected;
        }
    } else {
        if (kind_ == http3_stream_kind::response && frame_type_ == 0x5 && !config_.allow_push_ && !first_frame_) {
            return http3_stream_frame_status::push_promise;
        }
        if (known_frame && frame_type_ != 0x0 && frame_type_ != 0x1 &&
            !(frame_type_ == 0x5 && kind_ == http3_stream_kind::response && config_.allow_push_)) {
            return http3_stream_frame_status::frame_unexpected;
        }
        if (first_frame_ && frame_type_ != 0x1 &&
            !(frame_type_ == 0x5 && kind_ == http3_stream_kind::response && config_.allow_push_)) {
            return http3_stream_frame_status::frame_unexpected;
        }
        if (frame_type_ == 0x1 || frame_type_ == 0x5) {
            if (kind_ == http3_stream_kind::request && trailers_seen_) {
                return http3_stream_frame_status::frame_unexpected;
            }
            if (kind_ == http3_stream_kind::request && headers_seen_) {
                trailers_seen_ = true;
            }
            if (frame_length_ > field_section_buffer_limit()) {
                return http3_stream_frame_status::limit;
            }
            field_section_.clear();
        } else if (frame_type_ == 0x0) {
            if (!headers_seen_ || (kind_ == http3_stream_kind::request && trailers_seen_)) {
                return http3_stream_frame_status::frame_unexpected;
            }
        }
    }
    first_frame_ = false;
    phase_ = phase_type::frame_payload;
    return http3_stream_frame_status::need_more_data;
}

http3_stream_frame_status http3_stream_frames::finish_frame() noexcept {
    if (kind_ != http3_stream_kind::control && frame_type_ == 0x1) {
        headers_seen_ = true;
    }
    phase_ = phase_type::frame_header;
    header_bytes_used_ = 0;
    frame_length_ = 0;
    remaining_ = 0;
    return http3_stream_frame_status::need_more_data;
}

http3_stream_frame_status http3_stream_frames::feed(std::span<const char> input, bool fin,
    http3_stream_frame_callback_type callback_value, void* context_value) {
    if (phase_ == phase_type::failed) {
        return http3_stream_frame_status::frame_error;
    }
    if (phase_ == phase_type::ended) {
        return http3_stream_frame_status::frame_error;
    }
    if (resource_ == nullptr || callback_value == nullptr) {
        phase_ = phase_type::failed;
        return http3_stream_frame_status::frame_error;
    }

    consumed_ = 0;
    paused_ = false;
    std::size_t offset = 0;
    struct consume_guard {
        std::size_t& consumed_;
        std::size_t& offset_;
        ~consume_guard() {
            consumed_ = offset_;
        }
    } guard_value{consumed_, offset};
    while (offset < input.size() || (phase_ == phase_type::frame_payload && remaining_ == 0)) {
        if (phase_ == phase_type::frame_header) {
            if (offset == input.size()) {
                break;
            }
            // Complete headers borrow the current input. Only a fragmented
            // header needs to survive this feed in decoder-owned storage.
            if (header_bytes_used_ == 0) {
                if (const auto decoded = decode_http3_frame_header(input.subspan(offset)); decoded.index() == 0) {
                    offset += std::get<0>(decoded).encoded_bytes_;
                    const auto status = begin_frame(std::get<0>(decoded).type_, std::get<0>(decoded).length_);
                    if (status != http3_stream_frame_status::need_more_data) {
                        phase_ = phase_type::failed;
                        return status;
                    }
                    continue;
                }
            }
            header_[header_bytes_used_++] = input[offset++];
            const auto type_width = std::size_t{1} << (static_cast<std::uint8_t>(header_[0]) >> 6);
            if (header_bytes_used_ <= type_width) {
                continue;
            }
            const auto length_width = std::size_t{1} << (static_cast<std::uint8_t>(header_[type_width]) >> 6);
            if (header_bytes_used_ < type_width + length_width) {
                continue;
            }
            const auto decoded = decode_http3_frame_header(std::span<const char>(header_, header_bytes_used_));
            const auto status = begin_frame(std::get<0>(decoded).type_, std::get<0>(decoded).length_);
            if (status != http3_stream_frame_status::need_more_data) {
                phase_ = phase_type::failed;
                return status;
            }
            continue;
        }

        if (phase_ == phase_type::frame_payload) {
            if (remaining_ == 0) {
                if (frame_type_ == 0x5) {
                    const auto id = decode_http3_var_int(field_section_);
                    if ((id.index() != 0) || field_section_.size() - std::get<0>(id).encoded_bytes_ > config_.max_field_section_size_) {
                        phase_ = phase_type::failed;
                        return (id.index() == 0) ? http3_stream_frame_status::limit : http3_stream_frame_status::frame_error;
                    }
                }
                if (frame_type_ == 0x0 || frame_type_ == 0x1 || frame_type_ == 0x4 ||
                    frame_type_ == 0x3 || frame_type_ == 0x7 || frame_type_ == 0xd || frame_type_ == 0x5 || frame_type_ == 0xf0700 || frame_type_ == 0xf0701 || (frame_type_ == 0xc && kind_ == http3_stream_kind::control)) {
                    callback_value(context_value, http3_stream_frame_event{
                                                      .kind_ = frame_type_ == 0xc       ? http3_stream_frame_event_kind::origin
                                                               : frame_type_ == 0xf0700 ? http3_stream_frame_event_kind::request_priority_update
                                                               : frame_type_ == 0xf0701 ? http3_stream_frame_event_kind::push_priority_update
                                                               : frame_type_ == 0x5     ? http3_stream_frame_event_kind::push_promise
                                                               : frame_type_ == 0x1     ? http3_stream_frame_event_kind::headers
                                                               : frame_type_ == 0x4     ? http3_stream_frame_event_kind::settings
                                                               : frame_type_ == 0x3     ? http3_stream_frame_event_kind::cancel_push
                                                               : frame_type_ == 0x7     ? http3_stream_frame_event_kind::goaway
                                                               : frame_type_ == 0xd     ? http3_stream_frame_event_kind::max_push_id
                                                                                        : http3_stream_frame_event_kind::data,
                                                      .payload_ = (frame_type_ == 0x1 || frame_type_ == 0x5) ? std::span<const char>(field_section_.data(), field_section_.size())
                                                                                                             : std::span<const char>{},
                                                      .trailers_ = frame_type_ == 0x1 && kind_ == http3_stream_kind::request && trailers_seen_,
                                                      .end_frame_ = true,
                                                      .fin_ = fin && offset == input.size(),
                                                  });
                }
                if (paused_) {
                    return http3_stream_frame_status::paused;
                }
                finish_frame();
                continue;
            }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, input.size() - offset));
            const auto chunk = input.subspan(offset, count);
            const bool end_frame = count == remaining_;
            if (frame_type_ == 0x1 || frame_type_ == 0x5) {
                if (count > field_section_buffer_limit() || field_section_.size() > field_section_buffer_limit() - count) {
                    phase_ = phase_type::failed;
                    return http3_stream_frame_status::limit;
                }
                field_section_.insert(field_section_.end(), chunk.begin(), chunk.end());
            } else if (frame_type_ == 0x0 || frame_type_ == 0x4 || frame_type_ == 0x3 ||
                       frame_type_ == 0x7 || frame_type_ == 0xd || frame_type_ == 0xf0700 || frame_type_ == 0xf0701 || (frame_type_ == 0xc && kind_ == http3_stream_kind::control)) {
                callback_value(context_value, http3_stream_frame_event{
                                                  .kind_ = frame_type_ == 0xc       ? http3_stream_frame_event_kind::origin
                                                           : frame_type_ == 0xf0700 ? http3_stream_frame_event_kind::request_priority_update
                                                           : frame_type_ == 0xf0701 ? http3_stream_frame_event_kind::push_priority_update
                                                           : frame_type_ == 0x0     ? http3_stream_frame_event_kind::data
                                                           : frame_type_ == 0x4     ? http3_stream_frame_event_kind::settings
                                                           : frame_type_ == 0x3     ? http3_stream_frame_event_kind::cancel_push
                                                           : frame_type_ == 0x7     ? http3_stream_frame_event_kind::goaway
                                                                                    : http3_stream_frame_event_kind::max_push_id,
                                                  .payload_ = chunk,
                                                  .trailers_ = false,
                                                  .end_frame_ = end_frame,
                                                  .fin_ = fin && end_frame && offset + count == input.size(),
                                              });
            }
            offset += count;
            remaining_ -= count;
            if (remaining_ == 0 && frame_type_ != 0x1 && frame_type_ != 0x5) {
                if (count == 0) {
                    continue;
                }
                finish_frame();
            }
        }
    }

    if (fin) {
        if (phase_ != phase_type::frame_header || header_bytes_used_ != 0) {
            phase_ = phase_type::failed;
            return http3_stream_frame_status::frame_error;
        }
        if (kind_ == http3_stream_kind::control) {
            phase_ = phase_type::failed;
            return http3_stream_frame_status::closed_critical_stream;
        }
        if (!headers_seen_) {
            phase_ = phase_type::failed;
            return http3_stream_frame_status::frame_unexpected;
        }
        phase_ = phase_type::ended;
        return http3_stream_frame_status::message_end;
    }
    return http3_stream_frame_status::need_more_data;
}

}  // namespace ruvia
