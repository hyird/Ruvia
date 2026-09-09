#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpProtocolVersion.h"

namespace ruvia::detail {

struct HttpClientResponseHeadAccess final {
    [[nodiscard]] static HttpClientResponseHead make(HttpStatusCode status,
        HttpProtocolVersion protocolVersion, std::pmr::memory_resource* resource) {
        return HttpClientResponseHead(status, protocolVersion, resource);
    }

    [[nodiscard]] static std::pmr::vector<HttpHeader>& headers(
        HttpClientResponseHead& head) noexcept {
        return head.headers_;
    }
};

}  // namespace ruvia::detail
