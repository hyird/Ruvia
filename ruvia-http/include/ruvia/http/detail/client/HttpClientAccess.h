#pragma once

#include "ruvia/http/HttpProtocolVersion.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

struct HttpClientResponseHeaderAccess final {
    [[nodiscard]] static HttpClientResponseHeader make(
        std::pmr::string name,
        std::pmr::string value) {
        return HttpClientResponseHeader(std::move(name), std::move(value));
    }

    [[nodiscard]] static HttpClientResponseHeader make(
        std::string_view name,
        std::string_view value,
        std::pmr::memory_resource* resource) {
        return HttpClientResponseHeader(
            HttpResolvedPmrResourceTag{},
            name,
            value,
            httpPmrResourceOrDefault(resource));
    }
};

struct HttpClientResponseHeadAccess final {
    [[nodiscard]] static HttpClientResponseHead make(
        std::uint16_t status,
        HttpProtocolVersion protocolVersion,
        std::pmr::memory_resource* resource) {
        return HttpClientResponseHead(status, protocolVersion, resource);
    }

    [[nodiscard]] static std::pmr::vector<HttpClientResponseHeader>& headers(
        HttpClientResponseHead& head) noexcept {
        return head.headers_;
    }
};

}  // namespace ruvia::detail
