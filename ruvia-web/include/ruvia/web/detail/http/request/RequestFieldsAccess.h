#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/http/BorrowedText.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/RequestFields.h"

namespace ruvia::detail {

struct RequestNameValueViewAccess final {
    [[nodiscard]] static constexpr RequestNameValueView make(
        BorrowedText name, BorrowedText value) noexcept {
        return RequestNameValueView{name.view(), value.view()};
    }
};

struct RequestNameValueListAccess final {
    [[nodiscard]] static RequestNameValueList make(std::pmr::memory_resource* resource) {
        return RequestNameValueList(resource);
    }

    [[nodiscard]] static RequestNameValueList borrowHeaders(
        std::span<const HttpHeaderView> headers) noexcept {
        return RequestNameValueList(headers);
    }

    [[nodiscard]] static bool caseInsensitive(const RequestNameValueList& list) noexcept {
        return list.caseInsensitive();
    }

    [[nodiscard]] static bool namesEqual(const RequestNameValueList& list,
        std::string_view left, std::string_view right) noexcept {
        return list.namesEqual(left, right);
    }

    static void reserve(RequestNameValueList& list, std::size_t count) {
        list.reserve(count);
    }

    static void pushBack(RequestNameValueList& list, RequestNameValueView value) {
        list.pushBack(value);
    }
};

}  // namespace ruvia::detail
