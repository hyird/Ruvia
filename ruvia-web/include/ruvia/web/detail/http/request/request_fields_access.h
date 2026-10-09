#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/request_fields.h"

namespace ruvia::detail {

struct request_name_value_view_access final {
    [[nodiscard]] static constexpr request_name_value_view_type make(
        borrowed_text name, borrowed_text value) noexcept {
        return request_name_value_view_type{name.view(), value.view()};
    }
};

struct request_name_value_list_access final {
    [[nodiscard]] static request_name_value_list make(std::pmr::memory_resource* resource) {
        return request_name_value_list(resource);
    }

    [[nodiscard]] static request_name_value_list borrow_headers(
        std::span<const http_header_view> headers) noexcept {
        return request_name_value_list(headers);
    }

    [[nodiscard]] static bool case_insensitive(const request_name_value_list& list) noexcept {
        return list.case_insensitive();
    }

    [[nodiscard]] static bool names_equal(const request_name_value_list& list,
        std::string_view left, std::string_view right) noexcept {
        return list.names_equal(left, right);
    }

    static void reserve(request_name_value_list& list, std::size_t count) {
        list.reserve(count);
    }

    static void push_back(request_name_value_list& list, request_name_value_view_type value) {
        list.push_back(value);
    }
};

}  // namespace ruvia::detail
