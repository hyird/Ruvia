#include <algorithm>
#include <variant>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"

namespace ruvia {
namespace {
using error_type = http_connection_advertisement_error;
constexpr std::size_t length_prefix_bytes = 2;
constexpr std::size_t max_prefixed_length = 0xffff;
void append_length(std::pmr::vector<char>& bytes_value, std::size_t length) {
    bytes_value.push_back(static_cast<char>(length >> 8));
    bytes_value.push_back(static_cast<char>(length));
}
std::size_t length(std::span<const char> bytes_value) {
    return (static_cast<unsigned char>(bytes_value[0]) << 8) | static_cast<unsigned char>(bytes_value[1]);
}
// RFC 7838 §4: ALTSVC on stream 0 carries an origin; on a stream it does not.
bool is_valid_alternative_service_stream(std::uint32_t stream_id, bool has_origin) {
    return stream_id <= 0x7fffffffU && (stream_id == 0) == has_origin;
}
// Consumes the space of one length-prefixed origin from `remaining`; false when it does not fit.
bool reserve_prefixed_origin(std::size_t& remaining, std::string_view origin) {
    if (origin.size() > max_prefixed_length || remaining < length_prefix_bytes || origin.size() > remaining - length_prefix_bytes) {
        return false;
    }
    remaining -= length_prefix_bytes + origin.size();
    return true;
}
void append_prefixed_origin(std::pmr::vector<char>& bytes_value, std::string_view origin) {
    append_length(bytes_value, origin.size());
    bytes_value.insert(bytes_value.end(), origin.begin(), origin.end());
}
std::variant<std::pmr::vector<char>, error_type> make_http2_frame(http2_frame_type type, std::uint32_t stream_id,
    std::size_t payload_size, std::pmr::memory_resource* resource) {
    std::pmr::vector<char> bytes(resource);
    bytes.reserve(http2_frame_header_bytes + payload_size);
    bytes.resize(http2_frame_header_bytes);
    if (!encode_http2_frame_header(bytes, static_cast<std::uint32_t>(payload_size), type, 0, stream_id)) {
        return error_type::invalid_stream;
    }
    return bytes;
}
}  // namespace
std::variant<http_origin_advertisement, error_type> decode_http_origin_advertisement(std::span<const char> payload_value,
    std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    http_origin_advertisement result(resource);
    while (!payload_value.empty()) {
        if (payload_value.size() < length_prefix_bytes) {
            return error_type::malformed_payload;
        }
        const auto size = length(payload_value);
        payload_value = payload_value.subspan(length_prefix_bytes);
        if (size > payload_value.size()) {
            return error_type::malformed_payload;
        }
        const std::string_view origin(payload_value.data(), size);
        if (is_valid_http_serialized_origin(origin)) {
            result.origins_.emplace_back(origin);
        }
        payload_value = payload_value.subspan(size);
    }
    return result;
}
std::variant<std::pmr::vector<char>, error_type> encode_http2_origin_frame(std::span<const std::string_view> origins,
    std::uint32_t maximum, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    const std::size_t payload_limit = std::min(maximum, http2_max_frame_size);
    std::size_t remaining = payload_limit;
    for (const auto origin : origins) {
        if (!is_valid_http_serialized_origin(origin)) {
            return error_type::invalid_origin;
        }
        if (!reserve_prefixed_origin(remaining, origin)) {
            return error_type::limit;
        }
    }
    auto bytes_value = make_http2_frame(http2_frame_type::origin, 0, payload_limit - remaining, resource);
    if ((bytes_value.index() == 0)) {
        for (const auto origin : origins) {
            append_prefixed_origin(std::get<0>(bytes_value), origin);
        }
    }
    return bytes_value;
}
std::variant<http_alternative_service_advertisement, error_type> decode_http2_alternative_service(std::uint32_t stream_id,
    std::span<const char> payload_value, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    if (payload_value.size() < length_prefix_bytes) {
        return error_type::malformed_payload;
    }
    const auto size = length(payload_value);
    payload_value = payload_value.subspan(length_prefix_bytes);
    if (size > payload_value.size()) {
        return error_type::malformed_payload;
    }
    if (!is_valid_alternative_service_stream(stream_id, size != 0)) {
        return error_type::invalid_stream;
    }
    const std::string_view origin(payload_value.data(), size);
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return error_type::invalid_origin;
    }
    const std::string_view value(payload_value.data() + size, payload_value.size() - size);
    if (!is_valid_http_header_value(value)) {
        return error_type::invalid_field;
    }
    http_alternative_service_advertisement result(resource);
    result.stream_id_ = stream_id;
    result.origin_ = origin;
    result.field_value_ = value;
    return result;
}
std::variant<std::pmr::vector<char>, error_type> encode_http2_alternative_service_frame(std::uint32_t stream_id,
    std::string_view origin, std::string_view value, std::uint32_t maximum, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    if (!is_valid_alternative_service_stream(stream_id, !origin.empty())) {
        return error_type::invalid_stream;
    }
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return error_type::invalid_origin;
    }
    if (!is_valid_http_header_value(value)) {
        return error_type::invalid_field;
    }
    std::size_t remaining = std::min(maximum, http2_max_frame_size);
    if (!reserve_prefixed_origin(remaining, origin) || value.size() > remaining) {
        return error_type::limit;
    }
    auto bytes_value = make_http2_frame(http2_frame_type::alternative_service, stream_id, length_prefix_bytes + origin.size() + value.size(), resource);
    if ((bytes_value.index() == 0)) {
        append_prefixed_origin(std::get<0>(bytes_value), origin);
        std::get<0>(bytes_value).insert(std::get<0>(bytes_value).end(), value.begin(), value.end());
    }
    return bytes_value;
}
}  // namespace ruvia
