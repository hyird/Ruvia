#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/attributes.h"

namespace ruvia {

namespace detail {
struct request_name_value_view_access;
struct request_name_value_list_access;
}  // namespace detail

// Query, cookie, and route fields use the same name/value view as HTTP headers.
using request_name_value_view_type = http_header_view;

class request_name_value_list final {
public:
    using value_type = request_name_value_view_type;
    using const_iterator = const request_name_value_view_type*;

    request_name_value_list(const request_name_value_list&) = delete;
    request_name_value_list& operator=(const request_name_value_list&) = delete;
    request_name_value_list(request_name_value_list&& other) noexcept
        : owned_(std::move(other.owned_)),
          case_insensitive_(other.case_insensitive_) {
        items_ = owned_.empty() ? other.items_ : std::span<const request_name_value_view_type>(owned_);
        other.items_ = {};
    }
    request_name_value_list& operator=(request_name_value_list&&) = delete;

    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const_iterator begin() const&& = delete;

    [[nodiscard]] const_iterator cbegin() const& noexcept RUVIA_LIFETIMEBOUND {
        return begin();
    }
    [[nodiscard]] const_iterator cbegin() const&& = delete;

    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data() + items_.size();
    }
    [[nodiscard]] const_iterator end() const&& = delete;

    [[nodiscard]] const_iterator cend() const& noexcept RUVIA_LIFETIMEBOUND {
        return end();
    }
    [[nodiscard]] const_iterator cend() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] const request_name_value_view_type* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const request_name_value_view_type* data() const&& = delete;

    [[nodiscard]] const request_name_value_view_type& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return items_[index];
    }
    [[nodiscard]] const request_name_value_view_type& operator[](std::size_t) const&& = delete;

    // Duplicate fields are preserved in materialization order; scalar lookup uses
    // the last occurrence. Header lists compare names case-insensitively;
    // other field sources compare exactly. Enumeration preserves name spelling.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept {
        for (std::size_t i = items_.size(); i > 0; --i) {
            if (names_equal(items_[i - 1].name(), name)) {
                return items_[i - 1].value();
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        std::size_t result_value = 0;
        for (const auto& item : items_) {
            if (names_equal(item.name(), name)) {
                ++result_value;
            }
        }
        return result_value;
    }

    [[nodiscard]] std::span<const request_name_value_view_type> entries() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_;
    }
    [[nodiscard]] std::span<const request_name_value_view_type> entries() const&& = delete;

private:
    friend struct detail::request_name_value_list_access;

    explicit request_name_value_list(std::pmr::memory_resource* resource, bool case_insensitive = false)
        : owned_(detail::pmr_resource_or_default(resource)),
          items_(owned_),
          case_insensitive_(case_insensitive) {}

    explicit request_name_value_list(std::span<const http_header_view> headers) noexcept
        : items_(headers),
          case_insensitive_(true) {}

    [[nodiscard]] bool case_insensitive() const noexcept {
        return case_insensitive_;
    }

    [[nodiscard]] bool names_equal(std::string_view left, std::string_view right) const noexcept {
        return case_insensitive_ ? http_ascii_equals_ignore_case(left, right) : left == right;
    }

    void reserve(std::size_t count) {
        owned_.reserve(count);
        items_ = owned_;
    }

    void push_back(request_name_value_view_type value) {
        owned_.push_back(value);
        items_ = owned_;
    }

    std::pmr::vector<http_header_view> owned_{};
    std::span<const http_header_view> items_{};
    bool case_insensitive_{false};
};

}  // namespace ruvia
