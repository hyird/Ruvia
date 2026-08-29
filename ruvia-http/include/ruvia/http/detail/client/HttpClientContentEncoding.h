#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

struct HttpContentDecodeResultAccess final {
    [[nodiscard]] static HttpContentDecodeResult failure(HttpContentDecodeError error) noexcept {
        return HttpContentDecodeResult::makeFailure(error);
    }
};

template <typename Headers>
[[nodiscard]] inline HttpContentCodingFieldResult httpClientContentCodingOf(
    const Headers& headers) noexcept {
    return httpContentCodingFromHeaders(headers);
}

[[nodiscard]] inline HttpContentCodingFieldResult httpClientResponseContentCoding(
    const HttpClientResponseHead& head) noexcept {
    return httpClientContentCodingOf(head.headers());
}

[[nodiscard]] inline HttpContentDecodeResult decodeHttpClientResponseContentEncoding(
    const HttpClientResponseHead& head, std::string_view encodedContent,
    std::size_t maxDecodedBytes, std::pmr::memory_resource* resource) {
    // The immutable parsed head and externally driven encoded bytes remain
    // separate. A decoded representation has different Content-Encoding and
    // Content-Length metadata, so return independently owned bytes.
    const auto parsedCoding = httpClientResponseContentCoding(head);
    const auto* coding = parsedCoding.coding();
    if (coding == nullptr) {
        return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kUnsupportedCoding);
    }
    return decodeHttpContent(
        *coding, encodedContent, {.maxDecodedBytes = maxDecodedBytes, .resource = resource});
}

}  // namespace ruvia::detail
