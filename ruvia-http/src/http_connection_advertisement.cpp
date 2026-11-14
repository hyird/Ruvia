#include "http_connection_advertisement.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http_binary_message.h"
#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"

namespace ruvia {

namespace {

constexpr std::size_t advertisement_length_prefix_bytes = 2;
constexpr std::size_t advertisement_frame_header_bytes = 9;

[[nodiscard]] std::size_t read_advertisement_length(std::span<const char> bytes_value) noexcept {
    return (static_cast<unsigned char>(bytes_value[0]) << 8) | static_cast<unsigned char>(bytes_value[1]);
}

[[nodiscard]] std::vector<char> make_advertisement_http2_frame(std::uint8_t type, std::uint32_t stream_id, std::size_t payload_size) {
    std::vector<char> bytes(advertisement_frame_header_bytes + payload_size);
    bytes[0] = static_cast<char>((payload_size >> 16) & 0xff);
    bytes[1] = static_cast<char>((payload_size >> 8) & 0xff);
    bytes[2] = static_cast<char>(payload_size & 0xff);
    bytes[3] = static_cast<char>(type);
    bytes[4] = 0;
    bytes[5] = static_cast<char>((stream_id >> 24) & 0x7f);
    bytes[6] = static_cast<char>((stream_id >> 16) & 0xff);
    bytes[7] = static_cast<char>((stream_id >> 8) & 0xff);
    bytes[8] = static_cast<char>(stream_id & 0xff);
    return bytes;
}

}  // namespace

std::variant<http_origin_advertisement, http_connection_advertisement_error> decode_http_origin_advertisement(std::span<const char> payload_value) {
    http_origin_advertisement result;
    while (!payload_value.empty()) {
        if (payload_value.size() < advertisement_length_prefix_bytes) {
            return http_connection_advertisement_error::malformed_payload;
        }
        const auto size = read_advertisement_length(payload_value);
        payload_value = payload_value.subspan(advertisement_length_prefix_bytes);
        if (size > payload_value.size()) {
            return http_connection_advertisement_error::malformed_payload;
        }
        const std::string_view origin(payload_value.data(), size);
        if (is_valid_http_serialized_origin(origin)) {
            detail::reserve_geometric(result.origins_, result.origins_.size() + 1);
            result.origins_.emplace_back(origin);
        }
        payload_value = payload_value.subspan(size);
    }
    return result;
}

std::variant<std::vector<char>, http_connection_advertisement_error> encode_http2_origin_frame(std::span<const std::string_view> origins, std::uint32_t maximum) {
    const auto payload_size = detail::http_origin_payload_size(origins, std::min(maximum, detail::http2_max_frame_payload_size));
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_advertisement_http2_frame(0xc, 0, std::get<0>(payload_size));
    detail::write_http_origin_payload(std::span(bytes_value).subspan(advertisement_frame_header_bytes), origins);
    return bytes_value;
}

std::variant<http_alternative_service_advertisement, http_connection_advertisement_error> decode_http2_alternative_service(std::uint32_t stream_id, std::span<const char> payload_value) {
    if (payload_value.size() < advertisement_length_prefix_bytes) {
        return http_connection_advertisement_error::malformed_payload;
    }
    const auto size = read_advertisement_length(payload_value);
    payload_value = payload_value.subspan(advertisement_length_prefix_bytes);
    if (size > payload_value.size()) {
        return http_connection_advertisement_error::malformed_payload;
    }
    if (!detail::is_valid_alternative_service_stream(stream_id, size != 0)) {
        return http_connection_advertisement_error::invalid_stream;
    }
    const std::string_view origin(payload_value.data(), size);
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return http_connection_advertisement_error::invalid_origin;
    }
    const std::string_view value(payload_value.data() + size, payload_value.size() - size);
    if (!is_valid_http_header_value(value)) {
        return http_connection_advertisement_error::invalid_field;
    }
    http_alternative_service_advertisement result;
    result.stream_id_ = stream_id;
    result.origin_ = origin;
    result.field_value_ = value;
    return result;
}

std::variant<std::vector<char>, http_connection_advertisement_error> encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view value, std::uint32_t maximum) {
    const auto payload_size = detail::http2_alternative_service_payload_size(stream_id, origin, value, maximum);
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_advertisement_http2_frame(0xa, stream_id, std::get<0>(payload_size));
    detail::http2_write_alternative_service_payload(std::span(bytes_value).subspan(advertisement_frame_header_bytes), origin, value);
    return bytes_value;
}
}  // namespace ruvia
