#include "ruvia/http/HttpConnectionAdvertisement.h"

#include <algorithm>
#include <array>
#include <limits>

#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpOrigin.h"

namespace ruvia {
namespace {
using Error = HttpConnectionAdvertisementError;
void appendLength(std::pmr::vector<char>& bytes, std::size_t length) {
    bytes.push_back(static_cast<char>(length >> 8));
    bytes.push_back(static_cast<char>(length));
}
std::size_t length(std::span<const char> bytes) {
    return (static_cast<unsigned char>(bytes[0]) << 8) | static_cast<unsigned char>(bytes[1]);
}
std::expected<std::size_t, Error> origin_payload_size(std::span<const std::string_view> origins,
    std::size_t maximum) {
    std::size_t size = 0;
    for (const auto origin : origins) {
        if (!is_valid_http_serialized_origin(origin)) {
            return std::unexpected(Error::kInvalidOrigin);
        }
        if (origin.size() > 65535 || size > maximum || maximum - size < 2 || origin.size() > maximum - size - 2) {
            return std::unexpected(Error::kLimit);
        }
        size += 2 + origin.size();
    }
    return size;
}
void append_origin_payload(std::pmr::vector<char>& bytes, std::span<const std::string_view> origins) {
    for (const auto origin : origins) {
        appendLength(bytes, origin.size());
        bytes.insert(bytes.end(), origin.begin(), origin.end());
    }
}
std::expected<std::pmr::vector<char>, Error> make_http2_frame(std::uint8_t type, std::uint32_t streamId,
    std::size_t payload_size, std::pmr::memory_resource* resource) {
    std::pmr::vector<char> bytes(resource);
    bytes.reserve(9 + payload_size);
    bytes.resize(9);
    if (!encodeHttp2FrameHeader(bytes, static_cast<std::uint32_t>(payload_size), static_cast<Http2FrameType>(type), 0, streamId)) {
        return std::unexpected(Error::kInvalidStream);
    }
    return bytes;
}
}  // namespace
std::expected<HttpOriginAdvertisement, Error> decodeHttpOriginAdvertisement(std::span<const char> payload,
    std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    HttpOriginAdvertisement result(resource);
    while (!payload.empty()) {
        if (payload.size() < 2) {
            return std::unexpected(Error::kMalformedPayload);
        }
        const auto size = length(payload);
        payload = payload.subspan(2);
        if (size > payload.size()) {
            return std::unexpected(Error::kMalformedPayload);
        }
        const std::string_view origin(payload.data(), size);
        if (is_valid_http_serialized_origin(origin)) {
            result.origins.emplace_back(origin);
        }
        payload = payload.subspan(size);
    }
    return result;
}
std::expected<std::pmr::vector<char>, Error> encodeHttp2OriginFrame(std::span<const std::string_view> origins,
    std::uint32_t maximum, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    const auto payload = origin_payload_size(origins, std::min(maximum, 0xffffffU));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto bytes = make_http2_frame(0xc, 0, *payload, resource);
    if (bytes) {
        append_origin_payload(*bytes, origins);
    }
    return bytes;
}
std::expected<std::pmr::vector<char>, Error> encodeHttp3OriginFrame(std::span<const std::string_view> origins,
    std::size_t maximum, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    const auto payload = origin_payload_size(origins, maximum);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    std::array<char, 16> header_bytes{};
    const auto header = encodeHttp3FrameHeader(header_bytes, 0xc, *payload);
    if (!header) {
        return std::unexpected(Error::kLimit);
    }
    if (*payload > (std::numeric_limits<std::size_t>::max)() - *header) {
        return std::unexpected(Error::kLimit);
    }
    std::pmr::vector<char> bytes(resource);
    bytes.reserve(*header + *payload);
    const auto encoded_header = std::span(header_bytes).first(*header);
    bytes.insert(bytes.end(), encoded_header.begin(), encoded_header.end());
    append_origin_payload(bytes, origins);
    return bytes;
}
std::expected<HttpAlternativeServiceAdvertisement, Error> decodeHttp2AlternativeService(std::uint32_t streamId,
    std::span<const char> payload, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    if (payload.size() < 2) {
        return std::unexpected(Error::kMalformedPayload);
    }
    const auto size = length(payload);
    payload = payload.subspan(2);
    if (size > payload.size()) {
        return std::unexpected(Error::kMalformedPayload);
    }
    if (streamId > 0x7fffffff || ((streamId == 0) != (size != 0))) {
        return std::unexpected(Error::kInvalidStream);
    }
    const std::string_view origin(payload.data(), size);
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return std::unexpected(Error::kInvalidOrigin);
    }
    const std::string_view value(payload.data() + size, payload.size() - size);
    if (!isValidHttpHeaderValue(value)) {
        return std::unexpected(Error::kInvalidField);
    }
    HttpAlternativeServiceAdvertisement result(resource);
    result.streamId = streamId;
    result.origin = origin;
    result.fieldValue = value;
    return result;
}
std::expected<std::pmr::vector<char>, Error> encodeHttp2AlternativeServiceFrame(std::uint32_t streamId,
    std::string_view origin, std::string_view value, std::uint32_t maximum, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    if (streamId > 0x7fffffff || ((streamId == 0) != (!origin.empty()))) {
        return std::unexpected(Error::kInvalidStream);
    }
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return std::unexpected(Error::kInvalidOrigin);
    }
    if (!isValidHttpHeaderValue(value)) {
        return std::unexpected(Error::kInvalidField);
    }
    const auto payload_limit = std::min(maximum, 0xffffffU);
    if (origin.size() > 65535 || payload_limit < 2 || origin.size() > payload_limit - 2 || value.size() > payload_limit - 2 - origin.size()) {
        return std::unexpected(Error::kLimit);
    }
    auto bytes = make_http2_frame(0xa, streamId, 2 + origin.size() + value.size(), resource);
    if (bytes) {
        appendLength(*bytes, origin.size());
        bytes->insert(bytes->end(), origin.begin(), origin.end());
        bytes->insert(bytes->end(), value.begin(), value.end());
    }
    return bytes;
}
}  // namespace ruvia
