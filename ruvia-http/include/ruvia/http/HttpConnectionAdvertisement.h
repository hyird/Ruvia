#pragma once

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {
// The resource must outlive the advertisement and all retained frame results.
struct HttpOriginAdvertisement final {
    std::pmr::vector<std::pmr::string> origins;
    explicit HttpOriginAdvertisement(std::pmr::memory_resource* resource)
        : origins(resource) {}
};
struct HttpAlternativeServiceAdvertisement final {
    std::uint32_t streamId{0};
    std::pmr::string origin;
    std::pmr::string fieldValue;
    explicit HttpAlternativeServiceAdvertisement(std::pmr::memory_resource* resource)
        : origin(resource),
          fieldValue(resource) {}
};
enum class HttpConnectionAdvertisementError : std::uint8_t {
    kMalformedPayload,
    kInvalidOrigin,
    kInvalidField,
    kInvalidStream,
    kLimit,
};
// RFC 8336 / RFC 9412 share the ORIGIN payload format. Invalid serialized
// origins are ignored; truncated length-delimited entries reject the payload.
// Origin authority and connection coalescing require the runtime's TLS identity.
[[nodiscard]] std::expected<HttpOriginAdvertisement, HttpConnectionAdvertisementError>
decodeHttpOriginAdvertisement(std::span<const char> payload,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::expected<std::pmr::vector<char>, HttpConnectionAdvertisementError>
encodeHttp2OriginFrame(std::span<const std::string_view> origins,
    std::uint32_t maxFrameSize = 16384, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::expected<std::pmr::vector<char>, HttpConnectionAdvertisementError>
encodeHttp3OriginFrame(std::span<const std::string_view> origins,
    std::size_t maxPayloadBytes = 64 * 1024, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
// RFC 7838 ALTSVC. Stream 0 carries an explicit origin; a request stream
// carries an empty origin and inherits the request origin. The value is an
// Alt-Svc field value; transport authority and cache policy are external.
[[nodiscard]] std::expected<HttpAlternativeServiceAdvertisement, HttpConnectionAdvertisementError>
decodeHttp2AlternativeService(std::uint32_t streamId, std::span<const char> payload,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::expected<std::pmr::vector<char>, HttpConnectionAdvertisementError>
encodeHttp2AlternativeServiceFrame(std::uint32_t streamId, std::string_view origin, std::string_view fieldValue,
    std::uint32_t maxFrameSize = 16384, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
}  // namespace ruvia
