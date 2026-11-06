#include <algorithm>
#include <variant>

#include "ruvia/http/http_connection_advertisement.h"

#include "http2/http2_advertisement_frames.h"
#include "http2/http2_connection.h"
#include "http2/http2_remote_receive_semantics.h"

// ORIGIN (RFC 8336) and ALTSVC (RFC 7838) leave through nghttp2 as extension
// frames whose payload the single in-tree advertisement encoder writes.

namespace ruvia::detail {
namespace {

// nghttp2 packs an extension payload into a buffer of at least 16 KiB
// (nghttp2_pack_extension_callback2), so larger peer frame limits do not apply.
[[nodiscard]] std::uint32_t extension_payload_limit(std::uint32_t peer_max_frame_size) noexcept {
    return (std::min)(peer_max_frame_size, http2_default_max_frame_size);
}

}  // namespace

http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    if (role_ != http2_role::server) {
        return http2_submit_status::invalid_state;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_) {
        return http2_submit_status::closed;
    }
    if (!received_peer_settings_) {
        return http2_submit_status::invalid_state;
    }
    const auto payload_size =
        http2_origin_payload_size(origins, extension_payload_limit(peer_setting(NGHTTP2_SETTINGS_MAX_FRAME_SIZE)));
    if (payload_size.index() != 0) {
        return http2_submit_status::invalid_message;
    }
    http2_extension_payload payload{
        .size_ = std::get<0>(payload_size),
        .write_ = [](const http2_extension_payload& value, std::span<char> output) noexcept {
            http2_write_origin_payload(output, value.origins_);
        },
        .origins_ = origins,
    };
    check_nghttp2(nghttp2_submit_extension(
        session_.get(), static_cast<std::uint8_t>(http2_frame_type::origin), NGHTTP2_FLAG_NONE, 0, &payload));
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id,
    std::string_view origin, std::string_view field_value) {
    if (role_ != http2_role::server) {
        return http2_submit_status::invalid_state;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_) {
        return http2_submit_status::closed;
    }
    const auto* stream = stream_id != 0 ? streams_.find(stream_id) : nullptr;
    // A promised stream leaves reserved (local) with its first response
    // HEADERS (RFC 9113 §8.4.2); only the still-reserved stream may not carry
    // ALTSVC (RFC 7838 §4), and a CONNECT tunnel never does.
    if (!received_peer_settings_ ||
        (stream_id != 0 &&
            (stream == nullptr || http2_stream_is_closed(*stream) ||
                stream->push_reservation() == http2_push_reservation::local ||
                !http2_remote_final_head_decoded(*stream) || stream->tunnel().not_connect() == nullptr))) {
        return http2_submit_status::invalid_state;
    }
    const auto payload_size = http2_alternative_service_payload_size(
        stream_id, origin, field_value, extension_payload_limit(peer_setting(NGHTTP2_SETTINGS_MAX_FRAME_SIZE)));
    if (payload_size.index() != 0) {
        return http2_submit_status::invalid_message;
    }
    http2_extension_payload payload{
        .size_ = std::get<0>(payload_size),
        .write_ = [](const http2_extension_payload& value, std::span<char> output) noexcept {
            http2_write_alternative_service_payload(output, value.origin_, value.field_value_);
        },
        .origin_ = origin,
        .field_value_ = field_value,
    };
    check_nghttp2(nghttp2_submit_extension(session_.get(), static_cast<std::uint8_t>(http2_frame_type::alternative_service),
        NGHTTP2_FLAG_NONE, static_cast<std::int32_t>(stream_id), &payload));
    flush_session();
    return http2_submit_status::accepted;
}

}  // namespace ruvia::detail
