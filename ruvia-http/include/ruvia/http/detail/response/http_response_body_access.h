#pragma once

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

struct http_response_body_access final {
    [[nodiscard]] static const http_response_body& body(const http_response& response) noexcept {
        return response.body_;
    }
    [[nodiscard]] static const http_response_body& body(const http_response&& response) = delete;
    static void set_head_metadata_only(http_response& response) noexcept {
        response.body_.set_head_metadata_only();
    }
};

[[nodiscard]] inline const http_response_body& response_body(const http_response& response) noexcept {
    return http_response_body_access::body(response);
}
[[nodiscard]] const http_response_body& response_body(const http_response&& response) = delete;

}  // namespace ruvia::detail
