#pragma once

#include "ruvia/http/HttpResponse.h"

#include <cstddef>
#include <memory_resource>
#include <string_view>
#include <utility>

namespace ruvia::detail {

struct HttpResponseBodyAccess final {
    static void setStaticView(HttpResponse& response, std::string_view value) noexcept {
        response.setBodyStaticView(value);
    }

    static void setOwned(HttpResponse& response, std::pmr::string&& value) {
        response.setBodyOwned(std::move(value));
    }

    static void materialize(HttpResponse& response) {
        response.materializeBody();
    }

    [[nodiscard]] static std::string_view bytes(const HttpResponse& response) noexcept {
        return response.bodyBytes();
    }

    [[nodiscard]] static std::size_t size(const HttpResponse& response) noexcept {
        return response.bodySize();
    }

};

inline void setResponseBodyStaticView(HttpResponse& response, std::string_view value) noexcept {
    HttpResponseBodyAccess::setStaticView(response, value);
}

inline void setResponseBodyOwned(HttpResponse& response, std::pmr::string&& value) {
    HttpResponseBodyAccess::setOwned(response, std::move(value));
}

inline void materializeResponseBody(HttpResponse& response) {
    HttpResponseBodyAccess::materialize(response);
}

[[nodiscard]] inline std::string_view responseBodyBytes(const HttpResponse& response) noexcept {
    return HttpResponseBodyAccess::bytes(response);
}

[[nodiscard]] inline std::size_t responseBodySize(const HttpResponse& response) noexcept {
    return HttpResponseBodyAccess::size(response);
}

}  // namespace ruvia::detail
