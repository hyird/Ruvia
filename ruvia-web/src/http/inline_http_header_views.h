#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"

namespace ruvia::detail {

// Field views held inline up to `inline_capacity`, otherwise in one vector
// sized up front; the chosen storage never changes after construction.
template <std::size_t inline_capacity = 8>
class inline_http_header_views final {
public:
    template <typename fields_type>
        requires requires(const fields_type& fields) { fields.size(); }
    inline_http_header_views(const fields_type& fields, std::pmr::memory_resource* resource)
        : inline_http_header_views(fields.size(), resource) {
        for (const auto& field : fields) {
            push_back(http_header_view{field.name(), field.value()});
        }
    }

    // Reserves room for `count` views; at most `count` push_back calls follow.
    inline_http_header_views(std::size_t count, std::pmr::memory_resource* resource)
        : overflow_(resource) {
        if (count > inline_.size()) {
            overflow_.reserve(count);
        }
    }

    inline_http_header_views(const inline_http_header_views&) = delete;
    inline_http_header_views& operator=(const inline_http_header_views&) = delete;
    inline_http_header_views(inline_http_header_views&&) = delete;
    inline_http_header_views& operator=(inline_http_header_views&&) = delete;

    void push_back(http_header_view field) {
        if (overflow_.capacity() != 0) {
            overflow_.push_back(field);
        } else {
            inline_[size_++] = field;
        }
    }

    [[nodiscard]] std::span<const http_header_view> view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (overflow_.capacity() != 0) {
            return overflow_;
        }
        return std::span<const http_header_view>(inline_.data(), size_);
    }
    std::span<const http_header_view> view() const&& = delete;

    [[nodiscard]] operator std::span<const http_header_view>() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    operator std::span<const http_header_view>() const&& = delete;

private:
    std::array<http_header_view, inline_capacity> inline_;
    std::pmr::vector<http_header_view> overflow_;
    std::size_t size_{0};
};

}  // namespace ruvia::detail
