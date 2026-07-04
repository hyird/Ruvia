#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

struct FetchResponseHeaderAccess final {
    [[nodiscard]] static FetchResponseHeader make(std::pmr::string name, std::pmr::string value) {
        return FetchResponseHeader(std::move(name), std::move(value));
    }

    [[nodiscard]] static FetchResponseHeader make(
        std::string_view name,
        std::string_view value,
        std::pmr::memory_resource* resource) {
        return FetchResponseHeader(
            ResolvedPmrResourceTag{},
            name,
            value,
            pmrResourceOrDefault(resource));
    }
};

struct FetchResponseAccess final {
    [[nodiscard]] static FetchResponse make(std::pmr::memory_resource* resource) {
        return FetchResponse(resource);
    }

    static void setStatus(FetchResponse& response, std::uint16_t status) noexcept {
        response.status_ = status;
    }

    [[nodiscard]] static std::pmr::vector<FetchResponseHeader>& headers(FetchResponse& response) noexcept {
        return response.headers_;
    }

    [[nodiscard]] static std::pmr::string& body(FetchResponse& response) noexcept {
        return response.body_;
    }
};

struct FetchResponseStreamAccess final {
    [[nodiscard]] static FetchResponseStream make(
        std::unique_ptr<FetchStreamSource, FetchStreamSourceDeleter> source) noexcept {
        return FetchResponseStream(std::move(source));
    }
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
