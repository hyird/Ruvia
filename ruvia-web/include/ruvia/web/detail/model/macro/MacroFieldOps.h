#pragma once

#include "ruvia/web/detail/model/macro/MacroCore.h"
#include "ruvia/web/detail/model/ModelField.h"

// A field declaration lives directly inside a normal struct. The generated
// accessors deliberately preserve the existing Hono-style model call surface.

#define RUVIA_MODEL_DECLARE_FIELD(required, wire, field, type, ...) \
private: \
    using RuviaFieldType_##field = RUVIA_MODEL_UNPAREN type; \
    using RuviaFieldOptions_##field = decltype(::ruvia::detail::model::ModelOptions{__VA_ARGS__}); \
    ::ruvia::detail::model::ModelField< \
        RuviaFieldType_##field, \
        required, \
        RuviaFieldOptions_##field> ruviaField_##field##_ { \
            wire, ::ruvia::detail::model::ModelOptions{__VA_ARGS__}}; \
public: \
    [[nodiscard]] const ::std::optional<RuviaFieldType_##field>& field() const & noexcept { \
        return ruviaField_##field##_.value(); \
    } \
    [[nodiscard]] const ::std::optional<RuviaFieldType_##field>& field() const && = delete; \
    [[nodiscard]] RuviaFieldType_##field& field##Ensure() & { \
        return ruviaField_##field##_.ensure(ruviaResource_); \
    } \
    [[nodiscard]] RuviaFieldType_##field& field##Ensure() && = delete; \
    void field##Reset() noexcept { \
        ruviaField_##field##_.reset(); \
    } \
    template <typename RuviaFieldValueT> \
        requires ((::ruvia::detail::isRuviaString<RuviaFieldType_##field> && \
                      (::std::is_convertible_v<RuviaFieldValueT&&, ::std::string_view> || \
                          ::std::constructible_from<RuviaFieldType_##field, RuviaFieldValueT&&>)) || \
                  (!::ruvia::detail::isRuviaString<RuviaFieldType_##field> && \
                      ::std::constructible_from<RuviaFieldType_##field, RuviaFieldValueT&&>)) \
    auto& field(RuviaFieldValueT&& value) & { \
        ruviaField_##field##_.assign( \
            ::std::forward<RuviaFieldValueT>(value), ruviaResource_); \
        return *this; \
    } \
    template <typename RuviaFieldValueT> \
    auto& field(RuviaFieldValueT&&) && = delete;

#define RUVIA_FIELD(field, type, ...) \
    RUVIA_MODEL_DECLARE_FIELD(true, #field, field, (type), __VA_ARGS__)

#define RUVIA_FIELD_NAME(wire_name, field, type, ...) \
    RUVIA_MODEL_DECLARE_FIELD(true, wire_name, field, (type), __VA_ARGS__)

#define RUVIA_OPTIONAL_FIELD(field, type, ...) \
    RUVIA_MODEL_DECLARE_FIELD(false, #field, field, (type), __VA_ARGS__)

#define RUVIA_OPTIONAL_FIELD_NAME(wire_name, field, type, ...) \
    RUVIA_MODEL_DECLARE_FIELD(false, wire_name, field, (type), __VA_ARGS__)

#define RUVIA_MODEL_FIELD_STATE_BRANCH(T, field) \
    if constexpr (Field == ::ruvia::FixedString{#field}) { \
        return ruviaField_##field##_.state(); \
    } else

#define RUVIA_MODEL_PARSE_JSON_FIELD(T, field) \
    if (key == ruviaField_##field##_.wireName()) { \
        if (ruviaField_##field##_.state() != ::ruvia::detail::ModelFieldState::kMissing) { \
            ruviaField_##field##_.markDuplicate(); \
            return; \
        } \
        auto ruviaValueInput = value; \
        using RuviaValueT = typename decltype(ruviaField_##field##_)::value_type; \
        if (auto ruviaValue = ::ruvia::detail::parseJsonValue<RuviaValueT>( \
                ruviaValueInput, ruviaResource, 0, \
                ruviaStringStorage); ruviaValue) { \
            ::ruvia::detail::skipJsonWhitespace(ruviaValueInput); \
            if (ruviaValueInput.empty()) { \
                ruviaField_##field##_.emplaceParsed(::std::move(*ruviaValue)); \
            } else { \
                ruviaField_##field##_.markInvalidType(); \
            } \
        } else { \
            ruviaField_##field##_.markInvalidType(); \
        } \
        return; \
    }

#define RUVIA_MODEL_PARSE_FORM_FIELD(T, field) \
    if constexpr (::ruvia::detail::isFormField< \
            typename decltype(ruviaField_##field##_)::value_type>) { \
        if (key == ruviaField_##field##_.wireName()) { \
            if (ruviaField_##field##_.state() != ::ruvia::detail::ModelFieldState::kMissing) { \
                ruviaField_##field##_.markDuplicate(); \
                return; \
            } \
            const auto ruviaEncoding = ruviaInput.kind() == \
                    ::ruvia::detail::ModelInputKind::kFormFields \
                ? ::ruvia::detail::FormValueEncoding::kDecoded \
                : ::ruvia::detail::FormValueEncoding::kUrlEncoded; \
            using RuviaValueT = typename decltype(ruviaField_##field##_)::value_type; \
            auto ruviaValue = ::ruvia::detail::parseFormValue<RuviaValueT>( \
                ::ruvia::detail::ResolvedPmrResourceTag{}, value, \
                ruviaEncoding, ruviaResource); \
            if (ruviaValue) { \
                ruviaField_##field##_.emplaceParsed(::std::move(*ruviaValue)); \
            } else { \
                ruviaField_##field##_.markInvalidType(); \
            } \
            return; \
        } \
    }

#define RUVIA_MODEL_APPLY_DEFAULT_FIELD(T, field) \
    ruviaField_##field##_.applyDefault(ruviaResource);

#define RUVIA_MODEL_APPEND_JSON_FIELD(T, field) \
    { \
        const auto& ruviaSlot = ruviaField_##field##_; \
        const auto& ruviaValue = ruviaSlot.value(); \
        if (ruviaValue && !(ruviaSlot.omitEmpty() && \
                ::ruvia::detail::model::isEmptyValue(*ruviaValue))) { \
            if (!first) output.push_back(','); \
            first = false; \
            ::ruvia::detail::appendJsonString(output, ruviaSlot.wireName()); \
            output.push_back(':'); \
            ::ruvia::detail::appendJsonValue(output, *ruviaValue); \
        } else if (!ruviaValue && ruviaSlot.emitNull()) { \
            if (!first) output.push_back(','); \
            first = false; \
            ::ruvia::detail::appendJsonString(output, ruviaSlot.wireName()); \
            output.append(":null"); \
        } \
    }

#define RUVIA_MODEL_JSON_SIZE_FIELD(T, field) \
    { \
        const auto& ruviaSlot = ruviaField_##field##_; \
        const auto& ruviaValue = ruviaSlot.value(); \
        if (ruviaValue && !(ruviaSlot.omitEmpty() && \
                ::ruvia::detail::model::isEmptyValue(*ruviaValue))) { \
            if (!first) ++size; \
            first = false; \
            size += ::ruvia::detail::jsonStringSizeHint(ruviaSlot.wireName()) + 1; \
            size += ::ruvia::detail::jsonSizeHintValue(*ruviaValue); \
        } else if (!ruviaValue && ruviaSlot.emitNull()) { \
            if (!first) ++size; \
            first = false; \
            size += ::ruvia::detail::jsonStringSizeHint(ruviaSlot.wireName()) + 5; \
        } \
    }

#define RUVIA_MODEL_REQUIRED_FIELD(T, field) \
    { \
        const auto& ruviaSlot = ruviaField_##field##_; \
        if constexpr (::std::remove_cvref_t<decltype(ruviaSlot)>::required) { \
            if (ruviaSlot.state() == ::ruvia::detail::ModelFieldState::kMissing) { \
                ::std::pmr::string ruviaPath(validator.resource()); \
                ::ruvia::detail::model::appendPath( \
                    ruviaPath, prefix, ruviaSlot.wireName()); \
                validator.add(ruviaPath, "required", "is required"); \
            } \
        } \
    }
