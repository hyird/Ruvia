#pragma once

#include "ruvia/http/ModelTypes.h"
#include "ruvia/http/detail/model/Traits.h"
#include "ruvia/memory/PmrResource.h"

#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>

namespace ruvia {

template <typename T>
class ModelFieldConstRef final {
public:
    explicit ModelFieldConstRef(const std::optional<T>& value) noexcept
        : value_(&value) {}

    [[nodiscard]] bool has_value() const noexcept {
        return value_->has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] const T* operator->() const noexcept {
        return value_->operator->();
    }

    [[nodiscard]] const T& operator*() const noexcept {
        return **value_;
    }

    [[nodiscard]] const T& value() const {
        return value_->value();
    }

    [[nodiscard]] const std::optional<T>& optional() const noexcept {
        return *value_;
    }

    [[nodiscard]] operator const std::optional<T>&() const noexcept {
        return *value_;
    }

private:
    const std::optional<T>* value_{nullptr};
};

template <typename T>
class ModelFieldRef final {
public:
    ModelFieldRef(
        std::optional<T>& value,
        detail::ModelFieldState& state,
        std::pmr::memory_resource* resource) noexcept
        : value_(&value),
          state_(&state),
          resource_(detail::pmrResourceOrDefault(resource)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return value_->has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T* operator->() noexcept {
        return value_->operator->();
    }

    [[nodiscard]] const T* operator->() const noexcept {
        return value_->operator->();
    }

    [[nodiscard]] T& operator*() noexcept {
        return **value_;
    }

    [[nodiscard]] const T& operator*() const noexcept {
        return **value_;
    }

    [[nodiscard]] T& value() {
        return value_->value();
    }

    [[nodiscard]] const T& value() const {
        return value_->value();
    }

    [[nodiscard]] std::optional<T>& optional() noexcept {
        return *value_;
    }

    [[nodiscard]] const std::optional<T>& optional() const noexcept {
        return *value_;
    }

    [[nodiscard]] operator std::optional<T>&() noexcept {
        return *value_;
    }

    [[nodiscard]] operator const std::optional<T>&() const noexcept {
        return *value_;
    }

    [[nodiscard]] T& ensure() {
        if (!*value_) {
            emplaceMissingValue();
        }
        *state_ = detail::ModelFieldState::kParsed;
        return **value_;
    }

    void reset() noexcept {
        *state_ = detail::ModelFieldState::kMissing;
        value_->reset();
    }

    void clear()
        requires detail::isRuviaString<T>
    {
        ensure().resetOwned().clear();
    }

    void clear()
        requires (!detail::isRuviaString<T> && requires (T& value) { value.clear(); })
    {
        ensure().clear();
    }

    template <typename... Args>
    decltype(auto) emplace(Args&&... args)
        requires requires (T& value) { value.emplace(std::forward<Args>(args)...); }
    {
        return ensure().emplace(std::forward<Args>(args)...);
    }

    template <typename... Args>
    decltype(auto) emplace_back(Args&&... args)
        requires requires (T& value) { value.emplace_back(std::forward<Args>(args)...); }
    {
        return ensure().emplace_back(std::forward<Args>(args)...);
    }

    void assignView(std::string_view value) noexcept
        requires detail::isRuviaString<T>
    {
        if (!*value_) {
            emplaceMissingValue();
        }
        (*value_)->assignView(value);
        *state_ = detail::ModelFieldState::kParsed;
    }

    void assignOwned(std::string_view value)
        requires detail::isRuviaString<T>
    {
        if (!*value_) {
            emplaceMissingValue();
        }
        (*value_)->assignOwned(value);
        *state_ = detail::ModelFieldState::kParsed;
    }

    void assignOwned(std::pmr::string&& value)
        requires detail::isRuviaString<T>
    {
        if (!*value_) {
            emplaceMissingValue();
        }
        (*value_)->assignOwned(std::move(value));
        *state_ = detail::ModelFieldState::kParsed;
    }

private:
    void emplaceMissingValue() {
        value_->emplace(detail::makeRequestValue<T>(
            detail::ResolvedPmrResourceTag{},
            resource_));
    }

    std::optional<T>* value_{nullptr};
    detail::ModelFieldState* state_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

}  // namespace ruvia
