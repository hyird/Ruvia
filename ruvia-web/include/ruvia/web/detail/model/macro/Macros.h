#pragma once

#include "ruvia/web/detail/model/macro/MacroFieldOps.h"

// Unified JSON model body for a normal C++ struct. A model owns one PMR
// resource, parses JSON/form inputs, preserves per-field validation state, and
// serializes through the same schema.

#define RUVIA_MODEL(T, ...)                                                                                                                      \
public:                                                                                                                                          \
    using RuviaModelSchema = void;                                                                                                               \
    explicit T(::std::pmr::memory_resource* resource = nullptr) noexcept                                                                         \
        : ruviaResource_(::ruvia::detail::pmrResourceOrDefault(resource)) {}                                                                      \
    template <typename RuviaResourceOwnerT>                                                                                                      \
        requires requires(RuviaResourceOwnerT& owner) {                                                                                          \
            { owner.resource() } -> ::std::convertible_to<::std::pmr::memory_resource*>;                                                         \
        }                                                                                                                                         \
    explicit T(RuviaResourceOwnerT& owner) noexcept                                                                                              \
        : T(owner.resource()) {}                                                                                                                  \
                                                                                                                                                  \
private:                                                                                                                                         \
    template <typename, typename>                                                                                                                 \
    friend struct ::ruvia::JsonBody;                                                                                                              \
    template <typename, typename>                                                                                                                 \
    friend struct ::ruvia::FormBody;                                                                                                              \
    friend struct ::ruvia::detail::ModelValidationAccess;                                                                                        \
    friend struct ::ruvia::detail::ModelJsonAccess;                                                                                              \
    friend struct ::ruvia::detail::ModelParseAccess;                                                                                             \
    [[nodiscard]] static constexpr auto ruviaSchema() noexcept {                                                                                  \
        return ::ruvia::detail::model::ModelSchema{                                                                                              \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_SCHEMA_FIELD, T, __VA_ARGS__)};                                                                      \
    }                                                                                                                                             \
    template <::ruvia::FixedString Field>                                                                                                        \
    [[nodiscard]] ::ruvia::detail::ModelFieldState ruviaFieldState() const {                                                                      \
        return ::ruvia::detail::model::modelFieldState<Field>(*this, ruviaSchema());                                                              \
    }                                                                                                                                             \
    static ::std::optional<T> ruviaParseJsonBody(                                                                                                \
        ::std::string_view body, ::std::pmr::memory_resource* resource) {                                                                         \
        return ruviaParseJsonBodyDepth(                                                                                                           \
            body, resource, 0, ::ruvia::detail::ModelStringStorage::kBorrowed);                                                                   \
    }                                                                                                                                             \
    static ::std::optional<T> ruviaParseJsonBodyOwned(                                                                                           \
        ::std::string_view body, ::std::pmr::memory_resource* resource) {                                                                         \
        return ruviaParseJsonBodyDepth(                                                                                                           \
            body, resource, 0, ::ruvia::detail::ModelStringStorage::kOwned);                                                                      \
    }                                                                                                                                             \
    static ::std::optional<T> ruviaParseJsonBodyDepth(                                                                                           \
        ::std::string_view body,                                                                                                                  \
        ::std::pmr::memory_resource* resource,                                                                                                   \
        ::std::size_t depth,                                                                                                                      \
        ::ruvia::detail::ModelStringStorage ruviaStringStorage) {                                                                                 \
        auto ruviaInput = body;                                                                                                                   \
        auto ruviaModel = ruviaParseJsonValue(                                                                                                   \
            ruviaInput, resource, depth, ruviaStringStorage);                                                                                    \
        ::ruvia::detail::skipJsonWhitespace(ruviaInput);                                                                                          \
        if (!ruviaModel || !ruviaInput.empty()) return ::std::nullopt;                                                                            \
        return ruviaModel;                                                                                                                        \
    }                                                                                                                                             \
    static ::std::optional<T> ruviaParseJsonValue(                                                                                               \
        ::std::string_view& ruviaInput,                                                                                                           \
        ::std::pmr::memory_resource* resource,                                                                                                   \
        ::std::size_t depth,                                                                                                                      \
        ::ruvia::detail::ModelStringStorage ruviaStringStorage) {                                                                                 \
        if (depth > ::ruvia::detail::kMaxJsonDepth) return ::std::nullopt;                                                                         \
        T model{resource};                                                                                                                        \
        if (!model.ruviaMaterializeJson(ruviaInput, depth, ruviaStringStorage)) return ::std::nullopt;                                             \
        return model;                                                                                                                             \
    }                                                                                                                                             \
    static ::std::optional<T> ruviaParseFormBody(                                                                                                \
        ::std::string_view body, ::std::pmr::memory_resource* resource) {                                                                         \
        auto form = ::ruvia::FormObject::parse(body, resource);                                                                                   \
        if (!form) return ::std::nullopt;                                                                                                         \
        return ruviaMaterializeFormInput(                                                                                                         \
            ::ruvia::detail::makeFormModelInput(form->view(), resource));                                                                         \
    }                                                                                                                                             \
    static ::std::optional<T> ruviaParseFormFields(                                                                                              \
        const ::ruvia::RequestNameValueList& fields,                                                                                              \
        ::std::pmr::memory_resource* resource) {                                                                                                 \
        return ruviaMaterializeFormInput(                                                                                                         \
            ::ruvia::detail::makeFormFieldsModelInput(fields, resource));                                                                         \
    }                                                                                                                                             \
    static ::std::optional<T> ruviaMaterializeFormInput(                                                                                         \
        const ::ruvia::detail::ModelInput& ruviaInput) {                                                                                         \
        T model{ruviaInput.resource()};                                                                                                           \
        if (!model.ruviaMaterializeForm(ruviaInput)) return ::std::nullopt;                                                                        \
        return model;                                                                                                                             \
    }                                                                                                                                             \
    bool ruviaMaterializeJson(                                                                                                                    \
        ::std::string_view& ruviaInput,                                                                                                           \
        ::std::size_t ruviaDepth,                                                                                                                 \
        ::ruvia::detail::ModelStringStorage ruviaStringStorage) {                                                                                 \
        auto* const ruviaResource = ruviaResource_;                                                                                               \
        const bool ruviaValid = ::ruvia::detail::consumeJsonObjectFields(                                                                         \
            ::ruvia::detail::ResolvedPmrResourceTag{},                                                                                            \
            ruviaInput,                                                                                                                           \
            ruviaResource,                                                                                                                        \
            ruviaDepth,                                                                                                                           \
            [this, ruviaResource, ruviaDepth, ruviaStringStorage](                                                                                \
                ::std::string_view key, ::std::string_view& ruviaValueInput) -> bool {                                                            \
                const auto ruviaFieldKeyHash = ::ruvia::detail::model::modelFieldNameHash(key);                                                   \
                bool ruviaFieldResult = true;                                                                                                     \
                const bool ruviaMatched = ::ruvia::detail::model::visitModelFieldByWireName(                                                      \
                    *this,                                                                                                                        \
                    ruviaSchema(),                                                                                                                \
                    ruviaFieldKeyHash,                                                                                                            \
                    key,                                                                                                                          \
                    ruviaFieldResult,                                                                                                             \
                    [&](auto& ruviaSlot) -> bool {                                                                                                \
                        if (ruviaSlot.state() != ::ruvia::detail::ModelFieldState::kMissing) {                                                     \
                            ruviaSlot.markDuplicate();                                                                                            \
                            return ::ruvia::detail::skipJsonValue(                                                                                \
                                ruviaValueInput, ruviaDepth + 1);                                                                                 \
                        }                                                                                                                         \
                        const auto ruviaOriginalValueInput = ruviaValueInput;                                                                     \
                        using RuviaValueT = typename ::std::remove_cvref_t<decltype(ruviaSlot)>::value_type;                                       \
                        if (auto ruviaValue = ::ruvia::detail::parseJsonValue<RuviaValueT>(                                                       \
                                ruviaValueInput,                                                                                                  \
                                ruviaResource,                                                                                                    \
                                ruviaDepth + 1,                                                                                                   \
                                ruviaStringStorage);                                                                                              \
                            ruviaValue) {                                                                                                         \
                            ruviaSlot.emplaceParsed(::std::move(*ruviaValue));                                                                    \
                            return true;                                                                                                          \
                        }                                                                                                                         \
                        ruviaValueInput = ruviaOriginalValueInput;                                                                                \
                        if (!::ruvia::detail::skipJsonValue(ruviaValueInput, ruviaDepth + 1)) {                                                   \
                            return false;                                                                                                         \
                        }                                                                                                                         \
                        ruviaSlot.markInvalidType();                                                                                              \
                        return true;                                                                                                              \
                    });                                                                                                                           \
                if (ruviaMatched) return ruviaFieldResult;                                                                                        \
                return ::ruvia::detail::skipJsonValue(ruviaValueInput, ruviaDepth + 1);                                                          \
            });                                                                                                                                   \
        if (ruviaValid) {                                                                                                                         \
            ::ruvia::detail::model::visitModelFields(                                                                                            \
                *this,                                                                                                                            \
                ruviaSchema(),                                                                                                                    \
                [ruviaResource](const auto&, auto& ruviaSlot) {                                                                                   \
                    ruviaSlot.applyDefault(ruviaResource);                                                                                        \
                });                                                                                                                               \
        }                                                                                                                                         \
        return ruviaValid;                                                                                                                        \
    }                                                                                                                                             \
    bool ruviaMaterializeForm(const ::ruvia::detail::ModelInput& ruviaInput) {                                                                    \
        auto* const ruviaResource = ruviaResource_;                                                                                               \
        const bool ruviaValid = ::ruvia::detail::visitModelInputFormFields(                                                                       \
            ruviaInput,                                                                                                                           \
            [this, &ruviaInput, ruviaResource](::std::string_view key, ::std::string_view value) {                                               \
                bool ruviaMatched = false;                                                                                                        \
                ::ruvia::detail::model::visitModelFields(                                                                                        \
                    *this,                                                                                                                        \
                    ruviaSchema(),                                                                                                                \
                    [&](const auto&, auto& ruviaSlot) {                                                                                           \
                        using RuviaSlotT = ::std::remove_cvref_t<decltype(ruviaSlot)>;                                                             \
                        if constexpr (::ruvia::detail::isFormField<typename RuviaSlotT::value_type>) {                                           \
                            if (ruviaMatched || key != ruviaSlot.wireName()) return;                                                              \
                            ruviaMatched = true;                                                                                                  \
                            if (ruviaSlot.state() != ::ruvia::detail::ModelFieldState::kMissing) {                                                \
                                ruviaSlot.markDuplicate();                                                                                        \
                                return;                                                                                                           \
                            }                                                                                                                     \
                            const auto ruviaEncoding =                                                                                            \
                                ruviaInput.kind() == ::ruvia::detail::ModelInputKind::kFormFields                                                \
                                    ? ::ruvia::detail::FormValueEncoding::kDecoded                                                               \
                                    : ::ruvia::detail::FormValueEncoding::kUrlEncoded;                                                           \
                            using RuviaValueT = typename RuviaSlotT::value_type;                                                                  \
                            auto ruviaValue = ::ruvia::detail::parseFormValue<RuviaValueT>(                                                      \
                                ::ruvia::detail::ResolvedPmrResourceTag{},                                                                        \
                                value,                                                                                                            \
                                ruviaEncoding,                                                                                                    \
                                ruviaResource);                                                                                                   \
                            if (ruviaValue) {                                                                                                     \
                                ruviaSlot.emplaceParsed(::std::move(*ruviaValue));                                                                \
                            } else {                                                                                                              \
                                ruviaSlot.markInvalidType();                                                                                      \
                            }                                                                                                                     \
                        }                                                                                                                         \
                    });                                                                                                                           \
            });                                                                                                                                   \
        if (ruviaValid) {                                                                                                                         \
            ::ruvia::detail::model::visitModelFields(                                                                                            \
                *this,                                                                                                                            \
                ruviaSchema(),                                                                                                                    \
                [ruviaResource](const auto&, auto& ruviaSlot) {                                                                                   \
                    ruviaSlot.applyDefault(ruviaResource);                                                                                        \
                });                                                                                                                               \
        }                                                                                                                                         \
        return ruviaValid;                                                                                                                        \
    }                                                                                                                                             \
    void ruviaAppendJson(::std::pmr::string& output) const {                                                                                      \
        output.push_back('{');                                                                                                                    \
        bool first = true;                                                                                                                        \
        ::ruvia::detail::model::visitModelFields(                                                                                                \
            *this,                                                                                                                                \
            ruviaSchema(),                                                                                                                        \
            [&output, &first](const auto&, const auto& ruviaSlot) {                                                                               \
                const auto& ruviaValue = ruviaSlot.value();                                                                                       \
                if (ruviaValue &&                                                                                                                 \
                    !(ruviaSlot.omitEmpty() && ::ruvia::detail::model::isEmptyValue(*ruviaValue))) {                                             \
                    if (!first) output.push_back(',');                                                                                            \
                    first = false;                                                                                                                \
                    ::ruvia::detail::appendJsonString(output, ruviaSlot.wireName());                                                              \
                    output.push_back(':');                                                                                                        \
                    ::ruvia::detail::appendJsonValue(output, *ruviaValue);                                                                         \
                } else if (!ruviaValue && ruviaSlot.emitNull()) {                                                                                 \
                    if (!first) output.push_back(',');                                                                                            \
                    first = false;                                                                                                                \
                    ::ruvia::detail::appendJsonString(output, ruviaSlot.wireName());                                                              \
                    output.append(":null");                                                                                                      \
                }                                                                                                                                 \
            });                                                                                                                                   \
        output.push_back('}');                                                                                                                    \
    }                                                                                                                                             \
    [[nodiscard]] ::std::size_t ruviaJsonSizeHint() const {                                                                                       \
        ::std::size_t size = 2;                                                                                                                   \
        bool first = true;                                                                                                                        \
        ::ruvia::detail::model::visitModelFields(                                                                                                \
            *this,                                                                                                                                \
            ruviaSchema(),                                                                                                                        \
            [&size, &first](const auto&, const auto& ruviaSlot) {                                                                                 \
                const auto& ruviaValue = ruviaSlot.value();                                                                                       \
                if (ruviaValue &&                                                                                                                 \
                    !(ruviaSlot.omitEmpty() && ::ruvia::detail::model::isEmptyValue(*ruviaValue))) {                                             \
                    if (!first) ++size;                                                                                                           \
                    first = false;                                                                                                                \
                    size += ::ruvia::detail::jsonStringSizeHint(ruviaSlot.wireName()) + 1;                                                       \
                    size += ::ruvia::detail::jsonSizeHintValue(*ruviaValue);                                                                      \
                } else if (!ruviaValue && ruviaSlot.emitNull()) {                                                                                 \
                    if (!first) ++size;                                                                                                           \
                    first = false;                                                                                                                \
                    size += ::ruvia::detail::jsonStringSizeHint(ruviaSlot.wireName()) + 5;                                                       \
                }                                                                                                                                 \
            });                                                                                                                                   \
        return size;                                                                                                                              \
    }                                                                                                                                             \
    template <typename RuviaValidatorT>                                                                                                           \
    void ruviaValidateRequired(::std::string_view prefix, RuviaValidatorT& validator) const {                                                    \
        ::ruvia::detail::model::visitModelFields(                                                                                                \
            *this,                                                                                                                                \
            ruviaSchema(),                                                                                                                        \
            [&prefix, &validator](const auto&, const auto& ruviaSlot) {                                                                           \
                if constexpr (::std::remove_cvref_t<decltype(ruviaSlot)>::required) {                                                            \
                    if (ruviaSlot.state() == ::ruvia::detail::ModelFieldState::kMissing) {                                                       \
                        ::std::pmr::string ruviaPath(validator.resource());                                                                        \
                        ::ruvia::detail::model::appendPath(                                                                                       \
                            ruviaPath, prefix, ruviaSlot.wireName());                                                                              \
                        validator.add(ruviaPath, "required", "is required");                                                                     \
                    }                                                                                                                             \
                }                                                                                                                                 \
            });                                                                                                                                   \
    }                                                                                                                                             \
    ::std::pmr::memory_resource* ruviaResource_;                                                                                                 \
    static_assert(::ruvia::JsonBody<T>::value, "RUVIA_MODEL registered " #T)
