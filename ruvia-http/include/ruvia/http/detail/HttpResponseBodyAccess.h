#pragma once

#include "ruvia/http/HttpResponse.h"

#include <memory_resource>
#include <string_view>
#include <utility>

namespace ruvia::detail {

struct HttpResponseBodyAccess final {
    static void setBorrowedView(HttpResponse& response, std::string_view value) noexcept {
        response.setBodyBorrowedView(value);
    }

    static void setStaticView(HttpResponse& response, std::string_view value) noexcept {
        response.setBodyStaticView(value);
    }

    static void setOwned(HttpResponse& response, std::pmr::string&& value) {
        response.setBodyOwned(std::move(value));
    }

    static void materialize(HttpResponse& response) {
        response.materializeBody();
    }

    [[nodiscard]] static const HttpResponseBody& body(
        const HttpResponse& response) noexcept {
        return response.body_;
    }
};

inline void setResponseBodyBorrowedView(HttpResponse& response, std::string_view value) noexcept {
    HttpResponseBodyAccess::setBorrowedView(response, value);
}

inline void setResponseBodyStaticView(HttpResponse& response, std::string_view value) noexcept {
    HttpResponseBodyAccess::setStaticView(response, value);
}

inline void setResponseBodyOwned(HttpResponse& response, std::pmr::string&& value) {
    HttpResponseBodyAccess::setOwned(response, std::move(value));
}

inline void materializeResponseBody(HttpResponse& response) {
    HttpResponseBodyAccess::materialize(response);
}

[[nodiscard]] inline const HttpResponseBody& responseBody(
    const HttpResponse& response) noexcept {
    return HttpResponseBodyAccess::body(response);
}

}  // namespace ruvia::detail
