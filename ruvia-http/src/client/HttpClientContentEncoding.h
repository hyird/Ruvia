#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpContentCodec.h"

#include "coding/HttpContentCodecResult.h"
#include "coding/HttpContentCoding.h"

namespace ruvia::detail {

template <typename Headers>
[[nodiscard]] inline HttpContentCodingFieldResult httpClientContentCodingOf(
    const Headers& headers, std::pmr::memory_resource* resource) {
    return httpContentCodingFromHeaders(headers, resource);
}

[[nodiscard]] inline HttpContentCodingFieldResult httpClientResponseContentCoding(
    const HttpClientResponseHead& head, std::pmr::memory_resource* resource) {
    return httpClientContentCodingOf(head.headers(), resource);
}

[[nodiscard]] inline HttpContentDecodeResult decodeHttpClientResponseContentEncoding(
    const HttpClientResponseHead& head, std::string_view encodedContent,
    std::size_t maxDecodedBytes, std::pmr::memory_resource* resource) {
    // The immutable parsed head and externally driven encoded bytes remain
    // separate. A decoded representation has different Content-Encoding and
    // Content-Length metadata, so return independently owned bytes.
    const auto parsedCoding = httpClientContentCodingOf(head.headers(), resource);
    if (parsedCoding.invalid() != nullptr || parsedCoding.unsupported() != nullptr) {
        return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kUnsupportedCoding);
    }
    return decodeHttpContent(
        parsedCoding.codings(), encodedContent,
        {.maxDecodedBytes = maxDecodedBytes, .resource = resource});
}

}  // namespace ruvia::detail
