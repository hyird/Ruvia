#include "http_connection_advertisement.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http2_frame.h"
#include "http_advertisement_payload.h"
#include "http_authority.h"
#include "http_field_text.h"
#include "http_header.h"

namespace ruvia {

namespace {

constexpr std::size_t advertisement_length_prefix_bytes = 2;

[[nodiscard]] std::size_t read_advertisement_length(byte_view bytes_value) noexcept {
    return (std::to_integer<std::size_t>(bytes_value[0]) << 8) | std::to_integer<std::size_t>(bytes_value[1]);
}

[[nodiscard]] std::vector<std::byte> make_advertisement_http2_frame(http2_frame_type type, std::uint32_t stream_id, std::size_t payload_size) {
    std::vector<std::byte> bytes(http2_frame_header_bytes + payload_size);
    (void)encode_http2_frame_header(mutable_byte_view(bytes).first(http2_frame_header_bytes), static_cast<std::uint32_t>(payload_size), type, 0, stream_id);
    return bytes;
}

}  // namespace

std::variant<http_origin_advertisement, http_connection_advertisement_error> decode_http_origin_advertisement(byte_view payload_value) {
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
        const std::string_view origin = as_chars(payload_value.first(size));
        if (is_valid_http_serialized_origin(origin)) {
            detail::reserve_geometric(result.origins_, result.origins_.size() + 1);
            result.origins_.emplace_back(origin);
        }
        payload_value = payload_value.subspan(size);
    }
    return result;
}

std::variant<std::size_t, http_connection_advertisement_error> http_origin_payload_size(std::span<const std::string_view> origins, std::size_t max_payload_bytes) {
    return detail::http_origin_payload_size(origins, max_payload_bytes);
}

std::variant<std::size_t, http_connection_advertisement_error> encode_http_origin_payload(mutable_byte_view output, std::span<const std::string_view> origins, std::size_t max_payload_bytes) {
    const auto payload_size = detail::http_origin_payload_size(origins, max_payload_bytes);
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    const auto size = std::get<0>(payload_size);
    if (output.size() < size) {
        return http_connection_advertisement_error::output_too_small;
    }
    detail::write_http_origin_payload(output.first(size), origins);
    return size;
}

std::variant<std::vector<std::byte>, http_connection_advertisement_error> encode_http2_origin_frame(std::span<const std::string_view> origins, std::uint32_t maximum) {
    const auto payload_size = detail::http_origin_payload_size(origins, std::min(maximum, detail::http2_max_frame_payload_size));
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_advertisement_http2_frame(http2_frame_type::origin, 0, std::get<0>(payload_size));
    detail::write_http_origin_payload(mutable_byte_view(bytes_value).subspan(http2_frame_header_bytes), origins);
    return bytes_value;
}

std::variant<http_alternative_service_advertisement, http_connection_advertisement_error> decode_http2_alternative_service(std::uint32_t stream_id, byte_view payload_value) {
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
    const std::string_view origin = as_chars(payload_value.first(size));
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return http_connection_advertisement_error::invalid_origin;
    }
    const std::string_view value = as_chars(payload_value.subspan(size));
    if (!is_valid_http_header_value(value)) {
        return http_connection_advertisement_error::invalid_field;
    }
    http_alternative_service_advertisement result;
    result.stream_id_ = stream_id;
    result.origin_ = origin;
    result.field_value_ = value;
    return result;
}

std::variant<std::size_t, http_connection_advertisement_error> http2_alternative_service_payload_size(std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size) {
    return detail::http2_alternative_service_payload_size(stream_id, origin, field_value, max_frame_size);
}

std::variant<std::size_t, http_connection_advertisement_error> encode_http2_alternative_service_payload(mutable_byte_view output, std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size) {
    const auto payload_size = detail::http2_alternative_service_payload_size(stream_id, origin, field_value, max_frame_size);
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    const auto size = std::get<0>(payload_size);
    if (output.size() < size) {
        return http_connection_advertisement_error::output_too_small;
    }
    detail::http2_write_alternative_service_payload(output.first(size), origin, field_value);
    return size;
}

std::variant<std::vector<std::byte>, http_connection_advertisement_error> encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view value, std::uint32_t maximum) {
    const auto payload_size = detail::http2_alternative_service_payload_size(stream_id, origin, value, maximum);
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_advertisement_http2_frame(http2_frame_type::alternative_service, stream_id, std::get<0>(payload_size));
    detail::http2_write_alternative_service_payload(mutable_byte_view(bytes_value).subspan(http2_frame_header_bytes), origin, value);
    return bytes_value;
}
}  // namespace ruvia
