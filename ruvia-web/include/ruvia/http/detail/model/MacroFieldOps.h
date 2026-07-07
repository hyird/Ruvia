#pragma once

#include "ruvia/http/detail/model/FieldAssign.h"
#include "ruvia/http/detail/model/MacroCore.h"

// Per-field storage, access, defaults, and request parsing fragments.

#define RUVIA_MODEL_TYPED_GET_BRANCH(T, x) \
    RUVIA_MODEL_TYPED_GET_BRANCH_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_TYPED_GET_BRANCH_I(...) RUVIA_MODEL_TYPED_GET_BRANCH_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_TYPED_GET_BRANCH_IMPL(type, field, wire, rules) \
    if constexpr (Field == ::ruvia::FixedString{#field}) { \
        return field(); \
    } else

#define RUVIA_MODEL_FIELD_STATE_BRANCH(T, x) \
    RUVIA_MODEL_FIELD_STATE_BRANCH_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_FIELD_STATE_BRANCH_I(...) RUVIA_MODEL_FIELD_STATE_BRANCH_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_FIELD_STATE_BRANCH_IMPL(type, field, wire, rules) \
    if constexpr (Field == ::ruvia::FixedString{#field}) { \
        return ruviaState_##field##_; \
    } else

#define RUVIA_MODEL_FIELD_STORAGE(T, x) \
    RUVIA_MODEL_FIELD_STORAGE_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_FIELD_STORAGE_I(...) RUVIA_MODEL_FIELD_STORAGE_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_FIELD_STORAGE_IMPL(type, field, wire, rules) \
    static_assert(::ruvia::detail::isModelField<RUVIA_MODEL_UNPAREN type>, \
        "RUVIA_MODEL field type must be a Ruvia model type such as ruvia::String, ruvia::List<T>, ruvia::Bool, ruvia::Int32, or nested RUVIA_MODEL"); \
    mutable ::ruvia::detail::ModelFieldState ruviaState_##field##_ {::ruvia::detail::ModelFieldState::kMissing}; \
    mutable ::std::optional<RUVIA_MODEL_UNPAREN type> ruviaField_##field##_ {};

#define RUVIA_MODEL_RESET_FIELD(T, x) \
    RUVIA_MODEL_RESET_FIELD_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_RESET_FIELD_I(...) RUVIA_MODEL_RESET_FIELD_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_RESET_FIELD_IMPL(type, field, wire, rules) \
    ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kMissing; \
    ruviaField_##field##_.reset();

#define RUVIA_MODEL_PARSE_JSON_FIELD(T, x) \
    RUVIA_MODEL_PARSE_JSON_FIELD_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_PARSE_JSON_FIELD_I(...) RUVIA_MODEL_PARSE_JSON_FIELD_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_PARSE_JSON_FIELD_IMPL(type, field, wire, rules) \
    if (key == ::std::string_view{wire}) { \
        if (ruviaState_##field##_ != ::ruvia::detail::ModelFieldState::kMissing) { \
            ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kDuplicate; \
            return; \
        } \
        auto ruviaValueInput = value; \
        if (auto ruviaValue = ::ruvia::detail::parseJsonValue<RUVIA_MODEL_UNPAREN type>(ruviaValueInput, ruviaResource); ruviaValue) { \
            ::ruvia::detail::skipJsonWhitespace(ruviaValueInput); \
            if (ruviaValueInput.empty()) { \
                ruviaField_##field##_.emplace(::std::move(*ruviaValue)); \
                ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kParsed; \
            } else { \
                ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kInvalidType; \
            } \
        } else { \
            ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kInvalidType; \
        } \
        return; \
    }

#define RUVIA_MODEL_PARSE_FORM_FIELD(T, x) \
    RUVIA_MODEL_PARSE_FORM_FIELD_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_PARSE_FORM_FIELD_I(...) RUVIA_MODEL_PARSE_FORM_FIELD_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_PARSE_FORM_FIELD_IMPL(type, field, wire, rules) \
    if constexpr (::ruvia::detail::isFormField<RUVIA_MODEL_UNPAREN type>) { \
        if (key == ::std::string_view{wire}) { \
            if (ruviaState_##field##_ != ::ruvia::detail::ModelFieldState::kMissing) { \
                ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kDuplicate; \
                return; \
            } \
            RUVIA_MODEL_UNPAREN type ruviaValue = ::ruvia::detail::makeRequestValue<RUVIA_MODEL_UNPAREN type>( \
                ::ruvia::detail::ResolvedPmrResourceTag{}, \
                ruviaResource); \
            const bool ruviaParsedValue = body_.kind() == ::ruvia::RequestObjectKind::kFormFields \
                ? ::ruvia::detail::parseDecodedFormValue( \
                    ::ruvia::detail::ResolvedPmrResourceTag{}, \
                    value, \
                    ruviaValue, \
                    ruviaResource) \
                : ::ruvia::detail::parseFormValue( \
                    ::ruvia::detail::ResolvedPmrResourceTag{}, \
                    value, \
                    ruviaValue, \
                    ruviaResource); \
            if (ruviaParsedValue) { \
                ruviaField_##field##_.emplace(::std::move(ruviaValue)); \
                ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kParsed; \
            } else { \
                ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kInvalidType; \
            } \
            return; \
        } \
    }

#define RUVIA_MODEL_FIELD_ACCESSORS(T, x) \
    RUVIA_MODEL_FIELD_ACCESSORS_I(T, RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_FIELD_ACCESSORS_I(T, ...) RUVIA_MODEL_FIELD_ACCESSORS_IMPL(T, __VA_ARGS__)
#define RUVIA_MODEL_FIELD_ACCESSORS_IMPL(model_type, type, field, wire, rules) \
    [[nodiscard]] const ::std::optional<RUVIA_MODEL_UNPAREN type>& field() { \
        ruviaEnsureParsed(); \
        return ruviaField_##field##_; \
    } \
    [[nodiscard]] const ::std::optional<RUVIA_MODEL_UNPAREN type>& field() const { \
        ruviaEnsureParsed(); \
        return ruviaField_##field##_; \
    } \
    [[nodiscard]] RUVIA_MODEL_UNPAREN type& field##Ensure() { \
        ruviaEnsureParsed(); \
        auto* const ruviaResource = body_.resource(); \
        if (!ruviaField_##field##_) { \
            ruviaField_##field##_.emplace(::ruvia::detail::makeRequestValue<RUVIA_MODEL_UNPAREN type>( \
                ::ruvia::detail::ResolvedPmrResourceTag{}, \
                ruviaResource)); \
        } \
        ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kParsed; \
        return *ruviaField_##field##_; \
    } \
    void field##Reset() noexcept { \
        ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kMissing; \
        ruviaField_##field##_.reset(); \
    } \
    template <typename RuviaFieldValueT> \
        requires ((::ruvia::detail::isRuviaString<RUVIA_MODEL_UNPAREN type> && \
                      (::std::is_convertible_v<RuviaFieldValueT&&, ::std::string_view> || \
                          ::std::constructible_from<RUVIA_MODEL_UNPAREN type, RuviaFieldValueT&&>)) || \
                  (!::ruvia::detail::isRuviaString<RUVIA_MODEL_UNPAREN type> && \
                      ::std::constructible_from<RUVIA_MODEL_UNPAREN type, RuviaFieldValueT&&>)) \
    model_type& field(RuviaFieldValueT&& value) { \
        ruviaEnsureParsed(); \
        auto* const ruviaResource = body_.resource(); \
        ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kParsed; \
        ::ruvia::detail::model::assignFieldValue( \
            ruviaField_##field##_, \
            ::std::forward<RuviaFieldValueT>(value), \
            ruviaResource); \
        return *this; \
    }

#define RUVIA_MODEL_APPLY_DEFAULT_FIELD(T, x) \
    RUVIA_MODEL_APPLY_DEFAULT_FIELD_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_APPLY_DEFAULT_FIELD_I(...) RUVIA_MODEL_APPLY_DEFAULT_FIELD_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_APPLY_DEFAULT_FIELD_IMPL(type, field, wire, rules) \
    if (ruviaState_##field##_ == ::ruvia::detail::ModelFieldState::kMissing) { \
        rules.applyDefault(ruviaField_##field##_, ruviaResource); \
        if (ruviaField_##field##_) { \
            ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kParsed; \
        } \
    }
