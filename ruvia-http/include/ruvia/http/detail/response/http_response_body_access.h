#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

struct http_response_body_access final {
    static void set_borrowed_view(http_response& response, std::string_view value) noexcept {
        response.set_body_borrowed_view(value);
    }

    static void set_static_view(http_response& response, std::string_view value) noexcept {
        response.set_body_static_view(value);
    }

    static void set_owned(http_response& response, std::pmr::string&& value) {
        response.set_body_owned(std::move(value));
    }

    static void materialize(http_response& response) {
        response.materialize_body();
    }

    [[nodiscard]] static const http_response_body& body(const http_response& response) noexcept {
        return response.body_;
    }
    [[nodiscard]] static const http_response_body& body(const http_response&& response) = delete;
};

inline void set_response_body_borrowed_view(http_response& response, std::string_view value) noexcept {
    http_response_body_access::set_borrowed_view(response, value);
}

inline void set_response_body_static_view(http_response& response, std::string_view value) noexcept {
    http_response_body_access::set_static_view(response, value);
}

inline void set_response_body_owned(http_response& response, std::pmr::string&& value) {
    http_response_body_access::set_owned(response, std::move(value));
}

inline void materialize_response_body(http_response& response) {
    http_response_body_access::materialize(response);
}

[[nodiscard]] inline const http_response_body& response_body(const http_response& response) noexcept {
    return http_response_body_access::body(response);
}
[[nodiscard]] const http_response_body& response_body(const http_response&& response) = delete;

}  // namespace ruvia::detail
