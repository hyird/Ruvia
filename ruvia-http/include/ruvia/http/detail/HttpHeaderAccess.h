#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

struct HttpHeaderAccess final {
    [[nodiscard]] static HttpHeader make(const std::pmr::string& name, const std::pmr::string& value) {
        return HttpHeader(std::string_view(name), std::string_view(value), name.get_allocator().resource());
    }
    [[nodiscard]] static HttpHeader make(std::string_view name, std::string_view value,
        std::pmr::memory_resource* resource) {
        return HttpHeader(name, value, httpPmrResourceOrDefault(resource));
    }
};

}  // namespace ruvia::detail
