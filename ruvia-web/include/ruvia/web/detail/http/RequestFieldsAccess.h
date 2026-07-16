#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>
#include "ruvia/web/RequestFields.h"

namespace ruvia::detail {

struct RequestNameValueViewAccess final {
    [[nodiscard]] static constexpr RequestNameValueView make(
        std::string_view name,
        std::string_view value) noexcept {
        return RequestNameValueView(name, value);
    }
};

struct RequestNameValueListAccess final {
    [[nodiscard]] static RequestNameValueList make(std::pmr::memory_resource* resource) {
        return RequestNameValueList(resource);
    }

    static void reserve(RequestNameValueList& list, std::size_t count) {
        list.reserve(count);
    }

    static void pushBack(RequestNameValueList& list, RequestNameValueView value) {
        list.pushBack(value);
    }
};

}  // namespace ruvia::detail
