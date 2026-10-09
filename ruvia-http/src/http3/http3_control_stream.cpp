#include "ruvia/http/http3_control_stream.h"

#include <array>
#include <stdexcept>
#include <variant>

#include "ruvia/http/http3_var_int.h"

namespace ruvia {

http3_control_stream::http3_control_stream(http3_control_role role, std::pmr::memory_resource* resource,
    http3_stream_frames_config config) noexcept
    : role_(role),
      resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      frames_(http3_stream_kind::control, resource_, config),
      settings_payload_(resource_) {}

void http3_control_stream::on_frame(void* context_value, http3_stream_frame_event event) {
    static_cast<http3_control_stream*>(context_value)->consume(event);
}

void http3_control_stream::consume(http3_stream_frame_event event) {
    if (error_ != http3_control_stream_status::need_more_data) {
        return;
    }
    if (event.kind_ == http3_stream_frame_event_kind::origin) {
        if (role_ != http3_control_role::client) {
            return;
        }
        settings_payload_.insert(settings_payload_.end(), event.payload_.begin(), event.payload_.end());
        if (!event.end_frame_) {
            return;
        }
        const auto advertisement = decode_http_origin_advertisement(settings_payload_, resource_);
        if ((advertisement.index() == 0) && callback_) {
            callback_(context_, {.kind_ = event.kind_, .id_ = 0, .origin_advertisement_ = &std::get<0>(advertisement)});
        }
        settings_payload_.clear();
        return;
    }
    if (event.kind_ == http3_stream_frame_event_kind::request_priority_update || event.kind_ == http3_stream_frame_event_kind::push_priority_update) {
        if (role_ != http3_control_role::server) {
            error_ = http3_control_stream_status::frame_unexpected;
            return;
        }
        settings_payload_.insert(settings_payload_.end(), event.payload_.begin(), event.payload_.end());
        if (!event.end_frame_) {
            return;
        }
        const auto id = decode_http3_var_int(settings_payload_);
        const bool push = event.kind_ == http3_stream_frame_event_kind::push_priority_update;
        if ((id.index() != 0)) {
            error_ = http3_control_stream_status::frame_error;
            return;
        }
        if ((!push && (std::get<0>(id).value_ & 3) != 0) || (push && (!max_push_id_ || std::get<0>(id).value_ > *max_push_id_))) {
            error_ = http3_control_stream_status::id_error;
            return;
        }
        auto fields_value = parse_http_priority(std::string_view(settings_payload_.data() + std::get<0>(id).encoded_bytes_,
            settings_payload_.size() - std::get<0>(id).encoded_bytes_));
        if (callback_) {
            callback_(context_, {event.kind_, std::get<0>(id).value_,
                                    (fields_value.index() == 0) ? std::optional(http_priority_update{std::get<0>(id).value_, push, std::get<0>(fields_value)}) : std::nullopt});
        }
        settings_payload_.clear();
        return;
    }
    if (event.kind_ == http3_stream_frame_event_kind::settings) {
        if (event.payload_.size() > frames_.config().max_settings_payload_bytes_ ||
            settings_payload_.size() > frames_.config().max_settings_payload_bytes_ - event.payload_.size()) {
            error_ = http3_control_stream_status::limit;
            return;
        }
        settings_payload_.insert(settings_payload_.end(), event.payload_.begin(), event.payload_.end());
        if (!event.end_frame_) {
            return;
        }
        auto decoded = decode_http3_settings(settings_payload_, resource_);
        if ((decoded.index() != 0)) {
            error_ = http3_control_stream_status::settings_error;
            return;
        }
        settings_ = std::move(std::get<0>(decoded));
        std::pmr::vector<char> released(resource_);
        settings_payload_.swap(released);
        return;
    }
    if (event.kind_ != http3_stream_frame_event_kind::cancel_push &&
        event.kind_ != http3_stream_frame_event_kind::goaway &&
        event.kind_ != http3_stream_frame_event_kind::max_push_id) {
        return;
    }

    if (fixed_payload_size_ == 0) {
        fixed_kind_ = event.kind_;
    }
    if (fixed_kind_ != event.kind_ || event.payload_.size() > fixed_payload_.size() - fixed_payload_size_) {
        error_ = http3_control_stream_status::frame_error;
        return;
    }
    for (char byte : event.payload_) {
        fixed_payload_[fixed_payload_size_++] = byte;
    }
    if (!event.end_frame_) {
        return;
    }
    if (fixed_payload_size_ == 0) {
        error_ = http3_control_stream_status::frame_error;
        return;
    }
    const auto decoded = decode_http3_var_int(std::span<const char>(fixed_payload_).first(fixed_payload_size_));
    if ((decoded.index() != 0) || std::get<0>(decoded).encoded_bytes_ != fixed_payload_size_) {
        error_ = http3_control_stream_status::frame_error;
        return;
    }
    const auto value = std::get<0>(decoded).value_;
    switch (fixed_kind_) {
        case http3_stream_frame_event_kind::cancel_push:
            if (role_ == http3_control_role::server && (!max_push_id_ || value > *max_push_id_)) {
                error_ = http3_control_stream_status::id_error;
            } else {
                cancel_push_id_ = value;
            }
            break;
        case http3_stream_frame_event_kind::goaway:
            if (role_ == http3_control_role::client && (value & 0x3U) != 0) {
                error_ = http3_control_stream_status::id_error;
            } else if (goaway_id_ && value > *goaway_id_) {
                error_ = http3_control_stream_status::id_error;
            } else {
                goaway_id_ = value;
            }
            break;
        case http3_stream_frame_event_kind::max_push_id:
            if (role_ == http3_control_role::client || (max_push_id_ && value < *max_push_id_)) {
                error_ = role_ == http3_control_role::client
                             ? http3_control_stream_status::frame_unexpected
                             : http3_control_stream_status::id_error;
            } else {
                max_push_id_ = value;
            }
            break;
        default:
            break;
    }
    if (error_ == http3_control_stream_status::need_more_data && callback_) {
        callback_(context_, {fixed_kind_, value});
    }
    fixed_payload_size_ = 0;
}

http3_control_stream_status http3_control_stream::feed(std::span<const char> input, bool fin, http3_control_stream_callback_type callback_value, void* context_value) {
    if (feeding_) {
        throw std::logic_error("recursive http3_control_stream::feed()");
    }
    if (error_ != http3_control_stream_status::need_more_data) {
        return error_;
    }
    struct guard {
        http3_control_stream& owner_;
        int exceptions_{std::uncaught_exceptions()};
        ~guard() {
            owner_.feeding_ = false;
            owner_.callback_ = nullptr;
            owner_.context_ = nullptr;
            if (std::uncaught_exceptions() > exceptions_) {
                owner_.error_ = http3_control_stream_status::frame_error;
            }
        }
    } guard_value{*this};
    feeding_ = true;
    callback_ = callback_value;
    context_ = context_value;
    const auto status = frames_.feed(input, fin, on_frame, this);
    if (error_ != http3_control_stream_status::need_more_data) {
        return error_;
    }
    switch (status) {
        case http3_stream_frame_status::need_more_data:
        case http3_stream_frame_status::paused:
        case http3_stream_frame_status::message_end:
            return http3_control_stream_status::need_more_data;
        case http3_stream_frame_status::closed_critical_stream:
            error_ = http3_control_stream_status::closed_critical_stream;
            break;
        case http3_stream_frame_status::missing_settings:
            error_ = http3_control_stream_status::missing_settings;
            break;
        case http3_stream_frame_status::frame_unexpected:
        case http3_stream_frame_status::push_promise:
            error_ = http3_control_stream_status::frame_unexpected;
            break;
        case http3_stream_frame_status::limit:
            error_ = http3_control_stream_status::limit;
            break;
        case http3_stream_frame_status::frame_error:
            error_ = http3_control_stream_status::frame_error;
            break;
    }
    return error_;
}

}  // namespace ruvia
