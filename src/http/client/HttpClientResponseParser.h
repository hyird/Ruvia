#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "../HeaderAcceptUtils.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

struct HttpClientResponseHead final {
    std::size_t bodyOffset{0};
    std::size_t contentLength{0};
    // Response Content-Encoding, if it is a single coding we can decode; kNone otherwise
    // (identity, unknown, or a multi-coding list — the body is delivered as received).
    HttpContentCoding contentCoding{HttpContentCoding::kNone};
    bool hasContentLength{false};
    bool hasContentEncoding{false};
    bool hasTransferEncoding{false};
    bool isChunked{false};
    bool closeAfterResponse{false};
    bool responseMayHaveBody{false};
};

[[nodiscard]] HttpClientResponseHead parseHttpClientResponseHead(
    std::string_view method,
    std::string_view headerSection,
    FetchResponse& response,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
