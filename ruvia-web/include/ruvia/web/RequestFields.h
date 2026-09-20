#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/Attributes.h"

namespace ruvia {

namespace detail {
struct RequestNameValueViewAccess;
struct RequestNameValueListAccess;
}  // namespace detail

// Read-only request fields materialized by the Web Context. These views may
// represent headers, query parameters, cookies, or route parameters and borrow
// request-owned storage.
class RequestNameValueView final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return value_;
    }

private:
    friend struct detail::RequestNameValueViewAccess;

    constexpr RequestNameValueView(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    std::string_view name_;
    std::string_view value_;
};

class RequestNameValueList final {
public:
    using value_type = RequestNameValueView;
    using const_iterator = const RequestNameValueView*;

    RequestNameValueList(const RequestNameValueList&) = delete;
    RequestNameValueList& operator=(const RequestNameValueList&) = delete;
    RequestNameValueList(RequestNameValueList&&) noexcept = default;
    RequestNameValueList& operator=(RequestNameValueList&&) = delete;

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

    [[nodiscard]] const RequestNameValueView* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const RequestNameValueView* data() const&& = delete;

    [[nodiscard]] const RequestNameValueView& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return items_[index];
    }
    [[nodiscard]] const RequestNameValueView& operator[](std::size_t) const&& = delete;

    // Duplicate fields are preserved in materialization order; scalar lookup uses
    // the last occurrence. Header lists compare names case-insensitively;
    // other field sources compare exactly. Enumeration preserves name spelling.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept {
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (namesEqual(it->name(), name)) {
                return it->value();
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        std::size_t result = 0;
        for (const auto& item : items_) {
            if (namesEqual(item.name(), name)) {
                ++result;
            }
        }
        return result;
    }

    [[nodiscard]] std::span<const RequestNameValueView> entries() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_;
    }
    [[nodiscard]] std::span<const RequestNameValueView> entries() const&& = delete;

private:
    friend struct detail::RequestNameValueListAccess;

    explicit RequestNameValueList(std::pmr::memory_resource* resource, bool caseInsensitive = false)
        : items_(detail::pmrResourceOrDefault(resource)),
          caseInsensitive_(caseInsensitive) {}

    [[nodiscard]] bool namesEqual(std::string_view left, std::string_view right) const noexcept {
        return caseInsensitive_ ? detail::httpAsciiEqualsIgnoreCase(left, right) : left == right;
    }

    void reserve(std::size_t count) {
        items_.reserve(count);
    }

    void pushBack(RequestNameValueView value) {
        items_.push_back(value);
    }

    std::pmr::vector<RequestNameValueView> items_;
    bool caseInsensitive_{false};
};

}  // namespace ruvia
