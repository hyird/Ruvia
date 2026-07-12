#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

// A single decodable Content-Encoding, or kNone if absent, unknown, identity,
// or listed more than once (a multi-coding stack is delivered as received).
template <typename Headers>
[[nodiscard]] inline HttpContentCoding httpClientContentCodingOf(const Headers& headers) noexcept {
    bool seen = false;
    HttpContentCoding coding = HttpContentCoding::kNone;
    for (const auto& header : headers) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "content-encoding")) {
            continue;
        }
        if (seen) {
            return HttpContentCoding::kNone;
        }
        seen = true;
        coding = httpContentCodingFromFieldValue(header.value());
    }
    return coding;
}

[[nodiscard]] inline HttpContentCoding httpClientResponseContentCoding(
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
    const auto coding = httpClientResponseContentCoding(response);
    return decodeHttpContent(
        coding,
        response.body(),
        maxDecodedBytes,
        resource);
}

}  // namespace ruvia::detail
