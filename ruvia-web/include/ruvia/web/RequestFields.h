#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/Attributes.h"

namespace ruvia {

namespace detail {
struct RequestNameValueViewAccess;
struct RequestNameValueListAccess;
}  // namespace detail

// Query, cookie, and route fields use the same name/value view as HTTP headers.
using RequestNameValueView = HttpHeaderView;

class RequestNameValueList final {
public:
    using value_type = RequestNameValueView;
    using const_iterator = const RequestNameValueView*;

    RequestNameValueList(const RequestNameValueList&) = delete;
    RequestNameValueList& operator=(const RequestNameValueList&) = delete;
    RequestNameValueList(RequestNameValueList&& other) noexcept
        : owned_(std::move(other.owned_)),
          caseInsensitive_(other.caseInsensitive_) {
        items_ = owned_.empty() ? other.items_ : std::span<const RequestNameValueView>(owned_);
        other.items_ = {};
    }
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
        for (std::size_t i = items_.size(); i > 0; --i) {
            if (namesEqual(items_[i - 1].name(), name)) {
                return items_[i - 1].value();
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
        : owned_(detail::pmrResourceOrDefault(resource)),
          items_(owned_),
          caseInsensitive_(caseInsensitive) {}

    explicit RequestNameValueList(std::span<const HttpHeaderView> headers) noexcept
        : items_(headers),
          caseInsensitive_(true) {}

    [[nodiscard]] bool caseInsensitive() const noexcept {
        return caseInsensitive_;
    }

    [[nodiscard]] bool namesEqual(std::string_view left, std::string_view right) const noexcept {
        return caseInsensitive_ ? httpAsciiEqualsIgnoreCase(left, right) : left == right;
    }

    void reserve(std::size_t count) {
        owned_.reserve(count);
        items_ = owned_;
    }

    void pushBack(RequestNameValueView value) {
        owned_.push_back(value);
        items_ = owned_;
    }

    std::pmr::vector<HttpHeaderView> owned_{};
    std::span<const HttpHeaderView> items_{};
    bool caseInsensitive_{false};
};

}  // namespace ruvia
