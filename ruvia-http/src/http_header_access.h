#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_header.h"

namespace ruvia::detail {

struct http_header_access final {
    [[nodiscard]] static http_header make(const std::pmr::string& name, const std::pmr::string& value) {
        return http_header(std::string_view(name), std::string_view(value), name.get_allocator().resource());
    }
    [[nodiscard]] static http_header make(std::string_view name, std::string_view value,
        std::pmr::memory_resource* resource) {
        return http_header(name, value, http_pmr_resource_or_default(resource));
    }
};

}  // namespace ruvia::detail
