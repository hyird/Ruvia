#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "../RequestBodyDecoding.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

[[nodiscard]] inline HttpContentCoding httpClientResponseContentCoding(
    const FetchResponse& response) noexcept {
    bool seen = false;
    HttpContentCoding coding = HttpContentCoding::kNone;
    for (const auto& header : response.headers()) {
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

#endif  // RUVIA_ENABLE_HTTP_CLIENT
