#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia {

namespace detail {
struct RequestNameValueViewAccess;
struct RequestNameValueListAccess;
struct RequestValueGroupAccess;
struct RequestValueGroupListAccess;
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
    RequestNameValueList& operator=(RequestNameValueList&&) noexcept = default;

    [[nodiscard]] const_iterator begin() const noexcept {
        return items_.data();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return begin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return items_.data() + items_.size();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return end();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] const RequestNameValueView* data() const noexcept {
        return items_.data();
    }

    [[nodiscard]] const RequestNameValueView& operator[](std::size_t index) const noexcept {
        return items_[index];
    }

    // Duplicate fields are preserved in materialization order; scalar lookup uses
    // the last occurrence, matching Context request parsing semantics.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept {
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (it->name() == name) {
                return it->value();
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        std::size_t result = 0;
        for (const auto& item : items_) {
            if (item.name() == name) {
                ++result;
            }
        }
        return result;
    }

    [[nodiscard]] std::span<const RequestNameValueView> entries() const noexcept {
        return std::span<const RequestNameValueView>(items_.data(), items_.size());
    }

private:
    friend struct detail::RequestNameValueListAccess;

    explicit RequestNameValueList(std::pmr::memory_resource* resource)
        : items_(detail::pmrResourceOrDefault(resource)) {}

    void reserve(std::size_t count) {
        items_.reserve(count);
    }

    void pushBack(RequestNameValueView value) {
        items_.push_back(value);
    }

    std::pmr::vector<RequestNameValueView> items_;
};

class RequestValueGroup final {
public:
    RequestValueGroup(const RequestValueGroup&) = delete;
    RequestValueGroup& operator=(const RequestValueGroup&) = delete;
    RequestValueGroup(RequestValueGroup&&) noexcept = default;
    RequestValueGroup& operator=(RequestValueGroup&&) noexcept = default;

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::span<const std::string_view> values() const noexcept {
        return std::span<const std::string_view>(values_.data(), values_.size());
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return values_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return values_.empty();
    }

private:
    friend struct detail::RequestValueGroupAccess;

    RequestValueGroup(std::pmr::memory_resource* resource, std::string_view name)
        : name_(name),
          values_(detail::pmrResourceOrDefault(resource)) {}

    void add(std::string_view value) {
        values_.push_back(value);
    }

    std::string_view name_;
    std::pmr::vector<std::string_view> values_;
};

class RequestValueGroupList final {
public:
    using value_type = RequestValueGroup;
    using const_iterator = const RequestValueGroup*;

    RequestValueGroupList(const RequestValueGroupList&) = delete;
    RequestValueGroupList& operator=(const RequestValueGroupList&) = delete;
    RequestValueGroupList(RequestValueGroupList&&) noexcept = default;
    RequestValueGroupList& operator=(RequestValueGroupList&&) noexcept = default;

    [[nodiscard]] const_iterator begin() const noexcept {
        return groups_.data();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return begin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return groups_.data() + groups_.size();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return end();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return groups_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return groups_.empty();
    }

    [[nodiscard]] const RequestValueGroup* data() const noexcept {
        return groups_.data();
    }

    [[nodiscard]] const RequestValueGroup& operator[](std::size_t index) const noexcept {
        return groups_[index];
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        const auto* requestGroup = group(name);
        return requestGroup == nullptr ? 0 : requestGroup->size();
    }

    [[nodiscard]] std::span<const std::string_view> values(std::string_view name) const noexcept {
        const auto* requestGroup = group(name);
        if (requestGroup == nullptr) {
            return {};
        }
        return requestGroup->values();
    }

    [[nodiscard]] std::span<const RequestValueGroup> entries() const noexcept {
        return std::span<const RequestValueGroup>(groups_.data(), groups_.size());
    }

private:
    friend struct detail::RequestValueGroupListAccess;

    explicit RequestValueGroupList(std::pmr::memory_resource* resource)
        : groups_(detail::pmrResourceOrDefault(resource)) {}

    void reserve(std::size_t count) {
        groups_.reserve(count);
    }

    void pushBack(RequestValueGroup value) {
        groups_.push_back(std::move(value));
    }

    [[nodiscard]] const RequestValueGroup* group(std::string_view name) const noexcept {
        for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
            if (it->name() == name) {
                return &*it;
            }
        }
        return nullptr;
    }

    std::pmr::vector<RequestValueGroup> groups_;
};

}  // namespace ruvia
