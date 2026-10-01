#include "ruvia/http/HttpConnectionAdvertisement.h"

#include <algorithm>
#include <array>
#include <limits>

#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/parser/HttpSerializedOrigin.h"

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
std::expected<std::pmr::vector<char>, Error> originPayload(std::span<const std::string_view> origins,
    std::size_t maximum, std::pmr::memory_resource* resource) {
    std::size_t size = 0;
    for (const auto origin : origins) {
        if (!detail::isValidHttpSerializedOrigin(origin)) {
            return std::unexpected(Error::kInvalidOrigin);
        }
        if (origin.size() > 65535 || size > maximum || maximum - size < 2 || origin.size() > maximum - size - 2) {
            return std::unexpected(Error::kLimit);
        }
        size += 2 + origin.size();
    }
    std::pmr::vector<char> payload(resource);
    payload.reserve(size);
    for (const auto origin : origins) {
        appendLength(payload, origin.size());
        payload.insert(payload.end(), origin.begin(), origin.end());
    }
    return payload;
}
std::expected<std::pmr::vector<char>, Error> http2Frame(std::uint8_t type, std::uint32_t streamId,
    std::span<const char> payload, std::uint32_t maximum, std::pmr::memory_resource* resource) {
    if (payload.size() > std::min(maximum, 0xffffffU)) {
        return std::unexpected(Error::kLimit);
    }
    std::pmr::vector<char> bytes(resource);
    bytes.resize(9);
    if (!encodeHttp2FrameHeader(bytes, static_cast<std::uint32_t>(payload.size()), static_cast<Http2FrameType>(type), 0, streamId)) {
        return std::unexpected(Error::kInvalidStream);
    }
    bytes.insert(bytes.end(), payload.begin(), payload.end());
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
        if (detail::isValidHttpSerializedOrigin(origin)) {
            result.origins.emplace_back(origin);
        }
        payload = payload.subspan(size);
    }
    return result;
}
std::expected<std::pmr::vector<char>, Error> encodeHttp2OriginFrame(std::span<const std::string_view> origins,
    std::uint32_t maximum, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    const auto payload = originPayload(origins, std::min(maximum, 0xffffffU), resource);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return http2Frame(0xc, 0, *payload, maximum, resource);
}
std::expected<std::pmr::vector<char>, Error> encodeHttp3OriginFrame(std::span<const std::string_view> origins,
    std::size_t maximum, std::pmr::memory_resource* resource) {
    resource = resource ? resource : std::pmr::get_default_resource();
    const auto payload = originPayload(origins, maximum, resource);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    std::pmr::vector<char> bytes(resource);
    bytes.resize(16);
    const auto header = encodeHttp3FrameHeader(bytes, 0xc, payload->size());
    if (!header) {
        return std::unexpected(Error::kLimit);
    }
    bytes.resize(*header);
    bytes.insert(bytes.end(), payload->begin(), payload->end());
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
    if (!origin.empty() && !detail::isValidHttpSerializedOrigin(origin)) {
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
    if (!origin.empty() && !detail::isValidHttpSerializedOrigin(origin)) {
        return std::unexpected(Error::kInvalidOrigin);
    }
    if (!isValidHttpHeaderValue(value)) {
        return std::unexpected(Error::kInvalidField);
    }
    if (origin.size() > 65535 || maximum < 2 || origin.size() > maximum - 2 || value.size() > maximum - 2 - origin.size()) {
        return std::unexpected(Error::kLimit);
    }
    std::pmr::vector<char> payload(resource);
    payload.reserve(2 + origin.size() + value.size());
    appendLength(payload, origin.size());
    payload.insert(payload.end(), origin.begin(), origin.end());
    payload.insert(payload.end(), value.begin(), value.end());
    return http2Frame(0xa, streamId, payload, maximum, resource);
}
}  // namespace ruvia
