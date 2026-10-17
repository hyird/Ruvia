#pragma once

#include <initializer_list>
#include <memory_resource>
#include <vector>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/http_client_response_head.h"

namespace ruvia::detail {

struct http_client_response_head_access final {
    [[nodiscard]] static http_client_response_head make(http_status_code status,
        http_protocol_version protocol_version, std::pmr::memory_resource* resource) noexcept {
        return http_client_response_head(status, protocol_version, resource);
    }

    [[nodiscard]] static std::pmr::vector<http_header>& headers(
        http_client_response_head& head) {
        if (head.headers_ == nullptr) {
            auto* const resource = head.headers_.get_deleter().resource_;
            head.headers_ = make_http_pmr_object<std::pmr::vector<http_header>>(
                resource, std::initializer_list<http_header>{}, resource);
        }
        return *head.headers_;
    }
};

}  // namespace ruvia::detail
