#include <algorithm>
#include <array>
#include <cstring>
#include <variant>

#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"

#include "http2/http2_connection.h"
#include "http2/http2_remote_receive_semantics.h"

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
            events_.push_back(http2_event(std::move(std::get<0>(advertisement))));
        }
    } else {
        if (header_value.stream_id_ != 0) {
            const auto* stream = find_stream(header_value.stream_id_);
            if (stream == nullptr || http2_stream_is_closed(*stream) ||
                stream->tunnel().not_connect() == nullptr) {
                return true;
            }
        }
        auto advertisement = decode_http2_alternative_service(header_value.stream_id_, {payload_value.data(), payload_value.size()}, resource_);
        if ((advertisement.index() == 0)) {
            reserve_event_slots(1);
            events_.push_back(http2_event(std::move(std::get<0>(advertisement))));
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
    const std::size_t payload_limit = std::min(peer_settings_.max_frame_size(), http2_max_frame_size);
    std::size_t remaining = payload_limit;
    for (const auto origin : origins) {
        if (!is_valid_http_serialized_origin(origin) || origin.size() > 0xffffU ||
            remaining < 2U || origin.size() > remaining - 2U) {
            return http2_submit_status::invalid_message;
        }
        remaining -= 2U + origin.size();
    }
    const std::size_t payload_size = payload_limit - remaining;
    if (origins.size() == 1U) {
        const auto origin = origins.front();
        const std::array<char, 2> prefix{
            static_cast<char>(origin.size() >> 8), static_cast<char>(origin.size())};
        output_.append_frame(http2_frame_type::origin, 0, 0,
            std::string_view(prefix.data(), prefix.size()), origin);
        return http2_submit_status::accepted;
    }
    static constexpr std::size_t inline_payload_capacity = 256;
    if (payload_size <= inline_payload_capacity) {
        std::array<char, inline_payload_capacity> payload{};
        std::size_t offset = 0;
        for (const auto origin : origins) {
            payload[offset++] = static_cast<char>(origin.size() >> 8);
            payload[offset++] = static_cast<char>(origin.size());
            if (!origin.empty()) {
                std::memcpy(payload.data() + offset, origin.data(), origin.size());
                offset += origin.size();
            }
        }
        output_.append_frame(
            http2_frame_type::origin, 0, 0, std::string_view(payload.data(), payload_size));
        return http2_submit_status::accepted;
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
    const auto* stream = stream_id != 0 ? find_stream(stream_id) : nullptr;
    // A promised stream leaves reserved (local) with its first response
    // HEADERS, interim or final (RFC 9113 §8.4.2); only the still-reserved
    // stream may not carry ALTSVC (RFC 7838 §4).
    if (preface_phase_ != preface_phase_type::ready ||
        (stream_id != 0 &&
            (stream == nullptr || http2_stream_is_closed(*stream) ||
                stream->push_reservation() == http2_push_reservation::local ||
                !http2_remote_final_head_decoded(*stream) || stream->tunnel().not_connect() == nullptr))) {
        return http2_submit_status::invalid_state;
    }
    if (stream_id > 0x7fffffffU || (stream_id == 0) != (!origin.empty()) ||
        (!origin.empty() && !is_valid_http_serialized_origin(origin)) ||
        !is_valid_http_header_value(value)) {
        return http2_submit_status::invalid_message;
    }
    const std::size_t payload_limit = std::min(peer_settings_.max_frame_size(), http2_max_frame_size);
    if (origin.size() > 0xffffU || payload_limit < 2U || origin.size() > payload_limit - 2U ||
        value.size() > payload_limit - 2U - origin.size()) {
        return http2_submit_status::invalid_message;
    }
    static constexpr std::size_t inline_prefix_capacity = 256;
    if (2U + origin.size() <= inline_prefix_capacity) {
        std::array<char, inline_prefix_capacity> prefix{};
        prefix[0] = static_cast<char>(origin.size() >> 8);
        prefix[1] = static_cast<char>(origin.size());
        if (!origin.empty()) {
            std::memcpy(prefix.data() + 2, origin.data(), origin.size());
        }
        output_.append_frame(http2_frame_type::alternative_service, 0, stream_id,
            std::string_view(prefix.data(), 2U + origin.size()), value);
        return http2_submit_status::accepted;
    }
    const auto bytes_value = encode_http2_alternative_service_frame(stream_id, origin, value, peer_settings_.max_frame_size(), resource_);
    if ((bytes_value.index() != 0)) {
        return http2_submit_status::invalid_message;
    }
    output_.append_bytes({std::get<0>(bytes_value).data(), std::get<0>(bytes_value).size()});
    return http2_submit_status::accepted;
}
}  // namespace ruvia::detail
