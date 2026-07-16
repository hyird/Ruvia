#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/web/RequestFields.h"
#include "ruvia/web/detail/BorrowedView.h"

namespace ruvia::detail {

struct RequestNameValueViewAccess final {
    [[nodiscard]] static constexpr RequestNameValueView make(
        std::string_view name,
        std::string_view value) noexcept {
        return RequestNameValueView(name, value);
    }

    template <RvalueCharBasicString Name>
    static RequestNameValueView make(Name&&, std::string_view) = delete;

    template <RvalueCharBasicString Value>
    static RequestNameValueView make(std::string_view, Value&&) = delete;
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
