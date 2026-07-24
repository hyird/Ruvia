#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/parse/FieldAssign.h"
#include "ruvia/web/detail/model/ModelOptions.h"

namespace ruvia::detail::model {

template <typename ValueT, bool Required, typename OptionsT>
class ModelField final {
public:
    using value_type = ValueT;
    static constexpr bool required = Required;

    constexpr ModelField(std::string_view wireName, OptionsT options) noexcept
        : wireName_(wireName),
          options_(std::move(options)) {
        static_assert(detail::isRequestModelField<ValueT>, "RUVIA_FIELD type must be a Ruvia value type or nested RUVIA_MODEL");
    }

    [[nodiscard]] constexpr std::string_view wireName() const noexcept {
        return wireName_;
    }

    [[nodiscard]] detail::ModelFieldState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::optional<ValueT>& value() const& noexcept {
        return value_;
    }

    [[nodiscard]] const std::optional<ValueT>& value() const&& = delete;

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
    std::string_view wireName_;
    OptionsT options_;
    detail::ModelFieldState state_{detail::ModelFieldState::kMissing};
    std::optional<ValueT> value_;
};

}  // namespace ruvia::detail::model
