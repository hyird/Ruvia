#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

template <typename Headers>
[[nodiscard]] inline HttpContentCodingFieldResult httpClientContentCodingOf(
    const Headers& headers) noexcept {
    return httpContentCodingFromHeaders(headers);
}

[[nodiscard]] inline HttpContentCodingFieldResult httpClientResponseContentCoding(
    const HttpClientResponse& response) noexcept {
    return httpClientContentCodingOf(response.headers());
}

[[nodiscard]] inline HttpContentDecodeResult
decodeHttpClientResponseContentEncoding(
    const HttpClientResponse& response,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    // The parsed wire response remains untouched. A decoded representation has
    // different Content-Encoding/Content-Length metadata, so returning owned
    // bytes avoids constructing an internally contradictory response object.
    const auto parsedCoding = httpClientResponseContentCoding(response);
    const auto* coding = parsedCoding.coding();
    if (coding == nullptr) {
        return HttpContentDecodeResult::makeFailure(
            HttpContentDecodeError::kUnsupportedCoding);
    }
    return decodeHttpContent(
        *coding,
        response.body(),
        maxDecodedBytes,
        resource);
}

}  // namespace ruvia::detail
