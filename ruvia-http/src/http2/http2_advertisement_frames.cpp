#include <algorithm>
#include <cstring>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"

#include "field/http_origin_payload.h"
#include "http2/http2_connection.h"
#include "util/geometric_reserve.h"

namespace ruvia {

http_origin_advertisement& http_origin_advertisement::operator=(const http_origin_advertisement& other) {
    if (this != &other) {
        std::pmr::vector<std::pmr::string> candidate(other.origins_, origins_.get_allocator());
        origins_.swap(candidate);
    }
    return *this;
}

http_origin_advertisement& http_origin_advertisement::operator=(http_origin_advertisement&& other) {
    if (this != &other) {
        // Nested strings retain their allocator objects, even for equivalent resources.
        if (origins_.get_allocator().resource() == other.origins_.get_allocator().resource()) {
            http_origin_advertisement candidate(std::move(other));
            origins_.swap(candidate.origins_);
        } else {
            *this = other;
        }
    }
    return *this;
}

http_alternative_service_advertisement& http_alternative_service_advertisement::operator=(
    const http_alternative_service_advertisement& other) {
    if (this != &other) {
        const auto stream_id = other.stream_id_;
        std::pmr::string origin(other.origin_, origin_.get_allocator());
        std::pmr::string field_value(other.field_value_, field_value_.get_allocator());
        origin_.swap(origin);
        field_value_.swap(field_value);
        stream_id_ = stream_id;
    }
    return *this;
}

http_alternative_service_advertisement& http_alternative_service_advertisement::operator=(
    http_alternative_service_advertisement&& other) {
    if (this != &other) {
        if (origin_.get_allocator() == other.origin_.get_allocator() &&
            field_value_.get_allocator() == other.field_value_.get_allocator()) {
            http_alternative_service_advertisement candidate(std::move(other));
            origin_.swap(candidate.origin_);
            field_value_.swap(candidate.field_value_);
            stream_id_ = candidate.stream_id_;
        } else {
            *this = other;
        }
    }
    return *this;
}

namespace {
using error_type = http_connection_advertisement_error;
constexpr std::size_t length_prefix_bytes = 2;
std::size_t length(std::span<const char> bytes_value) {
    return (static_cast<unsigned char>(bytes_value[0]) << 8) | static_cast<unsigned char>(bytes_value[1]);
}
// RFC 7838 §4: ALTSVC on stream 0 carries an origin; on a stream it does not.
bool is_valid_alternative_service_stream(std::uint32_t stream_id, bool has_origin) {
    return stream_id <= detail::http2_max_stream_id && (stream_id == 0) == has_origin;
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
            detail::reserve_geometric(result.origins_, result.origins_.size() + 1);
            result.origins_.emplace_back(origin);
        }
        payload_value = payload_value.subspan(size);
    }
    return result;
}
std::variant<std::pmr::vector<char>, error_type> encode_http2_origin_frame(std::span<const std::string_view> origins,
    std::uint32_t maximum, std::pmr::memory_resource* resource) {
    resource = detail::http_pmr_resource_or_default(resource);
    const auto payload_size =
        detail::http_origin_payload_size(origins, std::min(maximum, http2_max_frame_size));
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_http2_frame(http2_frame_type::origin, 0, std::get<0>(payload_size), resource);
    detail::write_http_origin_payload(std::span(bytes_value).subspan(http2_frame_header_bytes), origins);
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
    if (!reserve_http_prefixed_origin(remaining, origin) || field_value.size() > remaining) {
        return error_type::limit;
    }
    return length_prefix_bytes + origin.size() + field_value.size();
}
void http2_write_alternative_service_payload(
    std::span<char> payload, std::string_view origin, std::string_view field_value) noexcept {
    auto* out = write_http_prefixed_origin(payload.data(), origin);
    if (!field_value.empty()) {
        std::memcpy(out, field_value.data(), field_value.size());
    }
}
}  // namespace detail
}  // namespace ruvia
