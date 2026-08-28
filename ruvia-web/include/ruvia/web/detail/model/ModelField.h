#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/parse/FieldAssign.h"
#include "ruvia/web/detail/model/ModelOptions.h"

namespace ruvia::detail::model {

[[nodiscard]] constexpr std::uint64_t modelFieldNameHash(std::string_view name) noexcept {
    // FNV-1a is only a dispatch prefilter. The parser still compares the full
    // decoded key before binding, so collisions cannot change JSON semantics.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : name) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <typename ValueT, bool Required, typename OptionsT, FixedString WireName>
class ModelField final {
public:
    using value_type = ValueT;
    static constexpr bool required = Required;

    constexpr ModelField() noexcept = default;

    [[nodiscard]] constexpr std::string_view wireName() const noexcept {
        return WireName.view();
    }

    [[nodiscard]] detail::ModelFieldState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::optional<ValueT>& value() const& noexcept {
        return value_;
    }

    [[nodiscard]] const std::optional<ValueT>& value() const&& = delete;

    [[nodiscard]] const ValueT& requiredValue() const&
        requires Required
    {
        if (!value_) {
            throw std::logic_error("required model field has no value");
        }
        return *value_;
    }

    [[nodiscard]] const ValueT& requiredValue() const&&
        requires Required
    = delete;

    [[nodiscard]] ValueT& ensure(std::pmr::memory_resource* resource) {
        if (!value_) {
            value_.emplace(detail::makeRequestValue<ValueT>(detail::ResolvedPmrResourceTag{}, resource));
        }
        state_ = detail::ModelFieldState::kParsed;
        return *value_;
    }

    template <typename InputT>
    void assign(InputT&& input, std::pmr::memory_resource* resource) {
        assignFieldValue(value_, std::forward<InputT>(input), resource);
        state_ = detail::ModelFieldState::kParsed;
    }

    void reset() noexcept {
        state_ = detail::ModelFieldState::kMissing;
        value_.reset();
    }

    void applyDefault(std::pmr::memory_resource* resource) {
        if (state_ != detail::ModelFieldState::kMissing) {
            return;
        }
        options_.applyDefault(value_, resource);
        if (value_) {
            state_ = detail::ModelFieldState::kParsed;
        }
    }

    [[nodiscard]] constexpr bool emitNull() const noexcept {
        return options_.emitNull();
    }

    [[nodiscard]] constexpr bool omitEmpty() const noexcept {
        return options_.omitEmpty();
    }

    void markDuplicate() noexcept {
        state_ = detail::ModelFieldState::kDuplicate;
    }

    void markInvalidType() noexcept {
        state_ = detail::ModelFieldState::kInvalidType;
    }

    void emplaceParsed(ValueT&& value) {
        value_.emplace(std::move(value));
        state_ = detail::ModelFieldState::kParsed;
    }

private:
    OptionsT options_;
    detail::ModelFieldState state_{detail::ModelFieldState::kMissing};
    std::optional<ValueT> value_;
};

}  // namespace ruvia::detail::model
