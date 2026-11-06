#include "http2/http2_advertisement_frames.h"

#include <algorithm>
#include <cstring>
#include <variant>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"

#include "http2/http2_local_settings.h"

namespace ruvia {
namespace {
using error_type = http_connection_advertisement_error;
constexpr std::size_t length_prefix_bytes = 2;
constexpr std::size_t max_prefixed_length = 0xffff;
std::size_t length(std::span<const char> bytes_value) {
    return (static_cast<unsigned char>(bytes_value[0]) << 8) | static_cast<unsigned char>(bytes_value[1]);
}
// RFC 7838 §4: ALTSVC on stream 0 carries an origin; on a stream it does not.
bool is_valid_alternative_service_stream(std::uint32_t stream_id, bool has_origin) {
    return stream_id <= detail::http2_max_stream_id && (stream_id == 0) == has_origin;
}
// Consumes the space of one length-prefixed origin from `remaining`; false when it does not fit.
bool reserve_prefixed_origin(std::size_t& remaining, std::string_view origin) {
    if (origin.size() > max_prefixed_length || remaining < length_prefix_bytes || origin.size() > remaining - length_prefix_bytes) {
        return false;
    }
    remaining -= length_prefix_bytes + origin.size();
    return true;
}
char* write_prefixed_origin(char* out, std::string_view origin) noexcept {
    *out++ = static_cast<char>(origin.size() >> 8);
    *out++ = static_cast<char>(origin.size());
    if (!origin.empty()) {
        std::memcpy(out, origin.data(), origin.size());
    }
    return out + origin.size();
}
std::pmr::vector<char> make_http2_frame(http2_frame_type type, std::uint32_t stream_id, std::size_t payload_size,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<char> bytes(http2_frame_header_bytes + payload_size, resource);
    (void)encode_http2_frame_header(
        std::span(bytes).first(http2_frame_header_bytes), static_cast<std::uint32_t>(payload_size), type, 0, stream_id);
    return bytes;
}
}  // namespace
std::variant<http_origin_advertisement, error_type> decode_http_origin_advertisement(std::span<const char> payload_value,
    std::pmr::memory_resource* resource) {
    resource = detail::http_pmr_resource_or_default(resource);
    std::size_t candidate_count = 0;
    for (auto scan = payload_value; !scan.empty();) {
        if (scan.size() < length_prefix_bytes) {
            return error_type::malformed_payload;
        }
        const auto size = length(scan);
        scan = scan.subspan(length_prefix_bytes);
        if (size > scan.size()) {
            return error_type::malformed_payload;
        }
        ++candidate_count;
        scan = scan.subspan(size);
    }
    http_origin_advertisement result(resource);
    result.origins_.reserve(candidate_count);
    while (!payload_value.empty()) {
        const auto size = length(payload_value);
        payload_value = payload_value.subspan(length_prefix_bytes);
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
    resource = detail::http_pmr_resource_or_default(resource);
    const auto payload_size = detail::http2_origin_payload_size(origins, maximum);
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_http2_frame(http2_frame_type::origin, 0, std::get<0>(payload_size), resource);
    detail::http2_write_origin_payload(std::span(bytes_value).subspan(http2_frame_header_bytes), origins);
    return bytes_value;
}
std::variant<http_alternative_service_advertisement, error_type> decode_http2_alternative_service(std::uint32_t stream_id,
    std::span<const char> payload_value, std::pmr::memory_resource* resource) {
    resource = detail::http_pmr_resource_or_default(resource);
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
    resource = detail::http_pmr_resource_or_default(resource);
    const auto payload_size = detail::http2_alternative_service_payload_size(stream_id, origin, value, maximum);
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value =
        make_http2_frame(http2_frame_type::alternative_service, stream_id, std::get<0>(payload_size), resource);
    detail::http2_write_alternative_service_payload(
        std::span(bytes_value).subspan(http2_frame_header_bytes), origin, value);
    return bytes_value;
}
namespace detail {
std::variant<std::size_t, error_type> http2_origin_payload_size(
    std::span<const std::string_view> origins, std::uint32_t max_frame_size) {
    const std::size_t payload_limit = std::min(max_frame_size, http2_max_frame_size);
    std::size_t remaining = payload_limit;
    for (const auto origin : origins) {
        if (!is_valid_http_serialized_origin(origin)) {
            return error_type::invalid_origin;
        }
        if (!reserve_prefixed_origin(remaining, origin)) {
            return error_type::limit;
        }
    }
    return payload_limit - remaining;
}
void http2_write_origin_payload(std::span<char> payload, std::span<const std::string_view> origins) noexcept {
    auto* out = payload.data();
    for (const auto origin : origins) {
        out = write_prefixed_origin(out, origin);
    }
}
std::variant<std::size_t, error_type> http2_alternative_service_payload_size(
    std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size) {
    if (!is_valid_alternative_service_stream(stream_id, !origin.empty())) {
        return error_type::invalid_stream;
    }
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return error_type::invalid_origin;
    }
    if (!is_valid_http_header_value(field_value)) {
        return error_type::invalid_field;
    }
    std::size_t remaining = std::min(max_frame_size, http2_max_frame_size);
    if (!reserve_prefixed_origin(remaining, origin) || field_value.size() > remaining) {
        return error_type::limit;
    }
    return length_prefix_bytes + origin.size() + field_value.size();
}
void http2_write_alternative_service_payload(
    std::span<char> payload, std::string_view origin, std::string_view field_value) noexcept {
    auto* out = write_prefixed_origin(payload.data(), origin);
    if (!field_value.empty()) {
        std::memcpy(out, field_value.data(), field_value.size());
    }
}
}  // namespace detail
}  // namespace ruvia
