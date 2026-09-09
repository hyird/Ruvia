#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

struct HttpHeaderAccess final {
    [[nodiscard]] static HttpHeader make(std::pmr::string name, std::pmr::string value) {
        return HttpHeader(std::move(name), std::move(value));
    }
    [[nodiscard]] static HttpHeader make(std::string_view name, std::string_view value,
        std::pmr::memory_resource* resource) {
        return HttpHeader(name, value, httpPmrResourceOrDefault(resource));
    }
};

}  // namespace ruvia::detail
