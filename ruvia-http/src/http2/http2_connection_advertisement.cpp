#include <variant>

#include "ruvia/http/http_connection_advertisement.h"

#include "http2/http2_connection.h"

namespace ruvia::detail {
bool http2_connection::process_advertisement(const http2_frame_header& header_value, std::string_view payload_value) {
    if (role_ != http2_role::client) {
        return true;
    }
    if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::origin)) {
        if (!receive_origin_advertisements_ || header_value.stream_id_ != 0 || (header_value.flags_ & 0xf) != 0) {
            return true;
        }
        auto advertisement = decode_http_origin_advertisement({payload_value.data(), payload_value.size()}, resource_);
        if ((advertisement.index() == 0)) {
            reserve_event_slots(1);
            events_.push_back(http2_event::origin_advertisement(std::move(std::get<0>(advertisement))));
        }
    } else {
        auto advertisement = decode_http2_alternative_service(header_value.stream_id_, {payload_value.data(), payload_value.size()}, resource_);
        if ((advertisement.index() == 0)) {
            reserve_event_slots(1);
            events_.push_back(http2_event::alternative_service_advertisement(std::move(std::get<0>(advertisement))));
        }
    }
    return true;
}
http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    if (role_ != http2_role::server) {
        return http2_submit_status::invalid_state;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_) {
        return http2_submit_status::closed;
    }
    if (preface_phase_ != preface_phase_type::ready) {
        return http2_submit_status::invalid_state;
    }
    const auto bytes_value = encode_http2_origin_frame(origins, peer_settings_.max_frame_size(), resource_);
    if ((bytes_value.index() != 0)) {
        return http2_submit_status::invalid_message;
    }
    output_.append_bytes({std::get<0>(bytes_value).data(), std::get<0>(bytes_value).size()});
    return http2_submit_status::accepted;
}
http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id,
    std::string_view origin, std::string_view value) {
    if (role_ != http2_role::server) {
        return http2_submit_status::invalid_state;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_) {
        return http2_submit_status::closed;
    }
    if (preface_phase_ != preface_phase_type::ready || (stream_id != 0 && find_stream(stream_id) == nullptr)) {
        return http2_submit_status::invalid_state;
    }
    const auto bytes_value = encode_http2_alternative_service_frame(stream_id, origin, value, peer_settings_.max_frame_size(), resource_);
    if ((bytes_value.index() != 0)) {
        return http2_submit_status::invalid_message;
    }
    output_.append_bytes({std::get<0>(bytes_value).data(), std::get<0>(bytes_value).size()});
    return http2_submit_status::accepted;
}
}  // namespace ruvia::detail
