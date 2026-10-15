#include "ruvia/http/http_connection_advertisement.h"

#include <algorithm>
#include <array>
#include <limits>
#include <variant>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_origin.h"

namespace ruvia {
namespace {
using error_type = http_connection_advertisement_error;
void append_length(std::pmr::vector<char>& bytes_value, std::size_t length) {
    bytes_value.push_back(static_cast<char>(length >> 8));
    bytes_value.push_back(static_cast<char>(length));
}
std::size_t length(std::span<const char> bytes_value) {
    return (static_cast<unsigned char>(bytes_value[0]) << 8) | static_cast<unsigned char>(bytes_value[1]);
}
std::variant<std::size_t, error_type> origin_payload_size(std::span<const std::string_view> origins,
    std::size_t maximum) {
    std::size_t size = 0;
    for (const auto origin : origins) {
        if (!is_valid_http_serialized_origin(origin)) {
            return error_type::invalid_origin;
        }
        if (origin.size() > 65535 || size > maximum || maximum - size < 2 || origin.size() > maximum - size - 2) {
            return error_type::limit;
        }
        size += 2 + origin.size();
    }
    return size;
}
void append_origin_payload(std::pmr::vector<char>& bytes_value, std::span<const std::string_view> origins) {
    for (const auto origin : origins) {
        append_length(bytes_value, origin.size());
        bytes_value.insert(bytes_value.end(), origin.begin(), origin.end());
    }
}
std::variant<std::pmr::vector<char>, error_type> make_http2_frame(std::uint8_t type, std::uint32_t stream_id,
    std::size_t payload_size, std::pmr::memory_resource* resource) {
    std::pmr::vector<char> bytes(resource);
    bytes.reserve(9 + payload_size);
    bytes.resize(9);
    if (!encode_http2_frame_header(bytes, static_cast<std::uint32_t>(payload_size), static_cast<http2_frame_type>(type), 0, stream_id)) {
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
        if (payload_value.size() < 2) {
            return error_type::malformed_payload;
        }
        const auto size = length(payload_value);
        payload_value = payload_value.subspan(2);
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
    const auto payload_value = origin_payload_size(origins, std::min(maximum, 0xffffffU));
    if ((payload_value.index() != 0)) {
        return std::get<1>(payload_value);
    }
    auto bytes_value = make_http2_frame(0xc, 0, std::get<0>(payload_value), resource);
    if ((bytes_value.index() == 0)) {
        append_origin_payload(std::get<0>(bytes_value), origins);
    }
    return bytes_value;
}
std::variant<std::pmr::vector<char>, error_type> encode_http3_origin_frame(std::span<const std::string_view> origins,
    std::size_t maximum, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    const auto payload_value = origin_payload_size(origins, maximum);
    if ((payload_value.index() != 0)) {
        return std::get<1>(payload_value);
    }
    std::array<char, 16> header_bytes{};
    const auto header_value = encode_http3_frame_header(header_bytes, 0xc, std::get<0>(payload_value));
    if ((header_value.index() != 0)) {
        return error_type::limit;
    }
    if (std::get<0>(payload_value) > (std::numeric_limits<std::size_t>::max)() - std::get<0>(header_value)) {
        return error_type::limit;
    }
    std::pmr::vector<char> bytes(resource);
    bytes.reserve(std::get<0>(header_value) + std::get<0>(payload_value));
    const auto encoded_header = std::span(header_bytes).first(std::get<0>(header_value));
    bytes.insert(bytes.end(), encoded_header.begin(), encoded_header.end());
    append_origin_payload(bytes, origins);
    return bytes;
}
std::variant<http_alternative_service_advertisement, error_type> decode_http2_alternative_service(std::uint32_t stream_id,
    std::span<const char> payload_value, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    if (payload_value.size() < 2) {
        return error_type::malformed_payload;
    }
    const auto size = length(payload_value);
    payload_value = payload_value.subspan(2);
    if (size > payload_value.size()) {
        return error_type::malformed_payload;
    }
    if (stream_id > 0x7fffffff || ((stream_id == 0) != (size != 0))) {
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
    if (stream_id > 0x7fffffff || ((stream_id == 0) != (!origin.empty()))) {
        return error_type::invalid_stream;
    }
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return error_type::invalid_origin;
    }
    if (!is_valid_http_header_value(value)) {
        return error_type::invalid_field;
    }
    const auto payload_limit = std::min(maximum, 0xffffffU);
    if (origin.size() > 65535 || payload_limit < 2 || origin.size() > payload_limit - 2 || value.size() > payload_limit - 2 - origin.size()) {
        return error_type::limit;
    }
    auto bytes_value = make_http2_frame(0xa, stream_id, 2 + origin.size() + value.size(), resource);
    if ((bytes_value.index() == 0)) {
        append_length(std::get<0>(bytes_value), origin.size());
        std::get<0>(bytes_value).insert(std::get<0>(bytes_value).end(), origin.begin(), origin.end());
        std::get<0>(bytes_value).insert(std::get<0>(bytes_value).end(), value.begin(), value.end());
    }
    return bytes_value;
}
}  // namespace ruvia
