#pragma once

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "RequestBodyDecoding.h"
#include "HttpClientAccess.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

// A single decodable Content-Encoding, or kNone if absent, unknown, identity, or listed more
// than once (a multi-coding stack is delivered as received). Shared by the buffered decode and
// the streaming decoder.
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
        coding = requestContentCoding(header.value());
    }
    return coding;
}

[[nodiscard]] inline HttpContentCoding httpClientResponseContentCoding(
    const FetchResponse& response) noexcept {
    return httpClientContentCodingOf(response.headers());
}

inline void decodeHttpClientResponseContentEncoding(
    FetchResponse& response,
    std::size_t maxDecodedBytes) {
    const auto coding = httpClientResponseContentCoding(response);
    if (coding == HttpContentCoding::kNone || response.body().empty()) {
        return;
    }
    auto& body = FetchResponseAccess::body(response);
    std::pmr::string decoded(body.get_allocator().resource());
    if (!decodeRequestContentEncoding(coding, body, decoded, maxDecodedBytes)) {
        throw std::runtime_error("http client: failed to decode response Content-Encoding");
    }
    body = std::move(decoded);
}

}  // namespace ruvia::detail
