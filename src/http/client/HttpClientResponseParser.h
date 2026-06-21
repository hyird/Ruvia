#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

struct HttpClientResponseHead final {
    std::size_t bodyOffset{0};
    std::size_t contentLength{0};
    bool hasContentLength{false};
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
