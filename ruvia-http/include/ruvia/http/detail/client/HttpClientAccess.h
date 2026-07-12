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

struct HttpClientResponseAccess final {
    [[nodiscard]] static HttpClientResponse make(
        HttpProtocolVersion protocolVersion,
        std::pmr::memory_resource* resource) {
        return HttpClientResponse(protocolVersion, resource);
    }

    static void setStatus(HttpClientResponse& response, std::uint16_t status) noexcept {
        response.status_ = status;
    }

    [[nodiscard]] static std::pmr::vector<HttpClientResponseHeader>& headers(
        HttpClientResponse& response) noexcept {
        return response.headers_;
    }

    [[nodiscard]] static std::pmr::string& body(HttpClientResponse& response) noexcept {
        return response.body_;
    }
};

}  // namespace ruvia::detail
