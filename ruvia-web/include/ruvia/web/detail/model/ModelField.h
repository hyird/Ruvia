#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/ModelOptions.h"
#include "ruvia/web/detail/model/parse/FieldAssign.h"

namespace ruvia::detail::model {

template <typename DerivedT, typename... DescriptorTs>
class ModelStorage;

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
    static_assert(std::is_empty_v<OptionsT>, "model field options must be stateless metadata");

public:
    using value_type = ValueT;
    static constexpr bool required = Required;
    static constexpr bool nullable = OptionsT::nullable;

    constexpr ModelField() noexcept = default;

    [[nodiscard]] constexpr std::string_view wireName() const noexcept {
        return WireName.view();
    }

    [[nodiscard]] detail::ModelFieldState state() const noexcept {
        return state_;
    }

    // Input provenance survives defaults and later application mutations.
    [[nodiscard]] bool isPresent() const noexcept {
        return present_;
    }

    [[nodiscard]] bool isNull() const noexcept {
        return state_ == detail::ModelFieldState::kNull;
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
            value_.emplace(
                detail::makeRequestValue<ValueT>(detail::ResolvedPmrResourceTag{}, resource));
        }
        state_ = detail::ModelFieldState::kParsed;
        return *value_;
    }

    template <typename InputT>
    void assign(InputT&& input, std::pmr::memory_resource* resource) {
        if constexpr (std::is_null_pointer_v<std::remove_cvref_t<InputT>>) {
            static_assert(nullable, "setting null requires RUVIA_NULLABLE");
            assignNull();
        } else {
            if constexpr (detail::isRuviaJsonValue<InputT>) {
                if (input.isNull()) {
                    if constexpr (nullable) {
                        assignNull();
                        return;
                    } else {
                        throw std::invalid_argument("setting JSON null requires RUVIA_NULLABLE");
                    }
                }
            }
            assignFieldValue(value_, std::forward<InputT>(input), resource);
            state_ = detail::ModelFieldState::kParsed;
        }
    }

    void reset() noexcept {
        state_ = detail::ModelFieldState::kMissing;
        value_.reset();
    }

    void applyDefault(std::pmr::memory_resource* resource) {
        // A default never satisfies a required input field.
        if (Required || state_ != detail::ModelFieldState::kMissing) {
            return;
        }
        OptionsT::applyDefault([this, resource]<typename InputT>(InputT&& value) {
            this->assign(std::forward<InputT>(value), resource);
        });
    }

    [[nodiscard]] constexpr bool emitNull() const noexcept {
        return OptionsT::emitNull();
    }

    [[nodiscard]] constexpr bool omitEmpty() const noexcept {
        return OptionsT::omitEmpty();
    }

    void markDuplicate() noexcept {
        present_ = true;
        state_ = detail::ModelFieldState::kDuplicate;
    }

    void markInvalidType() noexcept {
        present_ = true;
        state_ = detail::ModelFieldState::kInvalidType;
    }

    void markNull() noexcept {
        present_ = true;
        assignNull();
    }

private:
    template <typename DerivedT, typename... DescriptorTs>
    friend class ModelStorage;
    friend struct ::ruvia::detail::ModelValueFactory;

    void assignNull() noexcept {
        value_.reset();
        state_ = detail::ModelFieldState::kNull;
    }

    void emplaceParsed(ValueT&& value) {
        value_.emplace(std::move(value));
        present_ = true;
        state_ = detail::ModelFieldState::kParsed;
    }

    [[nodiscard]] ModelField rebindForModel(
        const ModelField& source, std::pmr::memory_resource* resource) const {
        ModelField rebound;
        rebound.state_ = source.state_;
        rebound.present_ = source.present_;
        if (source.value_) {
            rebound.value_.emplace(detail::rebindModelValue(*source.value_, resource));
        }
        return rebound;
    }

    void resetAfterMove() noexcept {
        present_ = false;
        value_.reset();
        state_ = detail::ModelFieldState::kMissing;
    }

    detail::ModelFieldState state_{detail::ModelFieldState::kMissing};
    bool present_{false};
    std::optional<ValueT> value_;
};

}  // namespace ruvia::detail::model
