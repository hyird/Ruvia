#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

struct http_response_headers_access final {
    using iterator = http_response_header*;

    [[nodiscard]] static iterator begin(http_response_headers& headers) noexcept {
        return headers.begin();
    }

    [[nodiscard]] static iterator end(http_response_headers& headers) noexcept {
        return headers.end();
    }

    static void release(http_response_headers& headers, http_response_header& header_value) noexcept {
        headers.release_header(header_value);
    }

    static void truncate(http_response_headers& headers, iterator begin, iterator write) {
        if (headers.spilled_) {
            headers.heap_.erase(headers.heap_.begin() + static_cast<std::ptrdiff_t>(write - begin),
                headers.heap_.end());
            return;
        }
        headers.size_ = static_cast<std::size_t>(write - begin);
    }

    [[nodiscard]] static http_response_header& add(http_response_headers& headers,
        std::string_view name, std::string_view value, std::uint32_t known_bit) {
        return headers.add(name, value, known_bit);
    }

    static void assign(http_response_headers& headers, http_response_header& header_value,
        std::string_view name, std::string_view value, std::uint32_t known_bit) {
        headers.assign(header_value, name, value, known_bit);
    }

    [[nodiscard]] static http_response_header& add_uninitialized_value(http_response_headers& headers,
        std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
        return headers.add_uninitialized_value(name, value_size, known_bit);
    }

    [[nodiscard]] static http_response_header& add_stable_view(http_response_headers& headers,
        std::string_view name, std::string_view value, std::uint32_t known_bit) {
        return headers.add_stable_view(name, value, known_bit);
    }

    static void assign_stable_view(http_response_headers& headers, http_response_header& header_value,
        std::string_view name, std::string_view value, std::uint32_t known_bit) {
        headers.assign_stable_view(header_value, name, value, known_bit);
    }
};

}  // namespace ruvia::detail
