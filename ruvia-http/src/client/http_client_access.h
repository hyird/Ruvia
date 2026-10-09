#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/http_client_response_head.h"

namespace ruvia::detail {

struct http_client_response_head_access final {
    [[nodiscard]] static http_client_response_head make(http_status_code status,
        http_protocol_version protocol_version, std::pmr::memory_resource* resource) {
        return http_client_response_head(status, protocol_version, resource);
    }

    [[nodiscard]] static std::pmr::vector<http_header>& headers(
        http_client_response_head& head) noexcept {
        return *head.headers_;
    }
};

}  // namespace ruvia::detail
