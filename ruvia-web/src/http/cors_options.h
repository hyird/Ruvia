#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>

#include "ruvia/web/server_config.h"

namespace ruvia::detail {

struct cors_options final {
    explicit cors_options(std::pmr::memory_resource* resource)
        : origin_(resource),
          request_headers_(resource),
          expose_headers_(resource) {}

    cors_origin_mode origin_mode_{cors_origin_mode::any};
    std::pmr::string origin_;
    cors_request_headers_mode request_headers_mode_{cors_request_headers_mode::reflect};
    std::pmr::string request_headers_;
    std::pmr::string expose_headers_;
    std::optional<std::chrono::seconds> max_age_;
};

[[nodiscard]] cors_options make_cors_options(
    const cors_config& config, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
