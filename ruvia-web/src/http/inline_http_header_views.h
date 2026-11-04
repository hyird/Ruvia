#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <vector>

#include "ruvia/http/http_header.h"

namespace ruvia::detail {

template <std::size_t inline_capacity = 8>
class inline_http_header_views final {
public:
    template <typename fields_type>
    inline_http_header_views(const fields_type& fields, std::pmr::memory_resource* resource)
        : overflow_(resource),
          size_(fields.size()) {
        if (fields.size() <= inline_.size()) {
            std::size_t i = 0;
            for (const auto& field : fields) {
                inline_[i++] = http_header_view{field.name(), field.value()};
            }
            return;
        }
        overflow_.reserve(fields.size());
        for (const auto& field : fields) {
            overflow_.emplace_back(field.name(), field.value());
        }
    }

    [[nodiscard]] std::span<const http_header_view> view() const noexcept {
        if (size_ <= inline_.size()) {
            return std::span<const http_header_view>(inline_.data(), size_);
        }
        return overflow_;
    }

    [[nodiscard]] operator std::span<const http_header_view>() const noexcept {
        return view();
    }

private:
    std::array<http_header_view, inline_capacity> inline_;
    std::pmr::vector<http_header_view> overflow_;
    std::size_t size_{0};
};

}  // namespace ruvia::detail
