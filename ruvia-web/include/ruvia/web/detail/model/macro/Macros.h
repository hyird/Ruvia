#pragma once

#include "ruvia/web/detail/model/macro/MacroFieldOps.h"

// Unified JSON model body for a normal C++ struct. A model owns one PMR
// resource, parses JSON/form inputs, preserves per-field validation state, and
// serializes through the same schema.

#define RUVIA_MODEL(T, ...)                                                  \
public:                                                                      \
    using RuviaModelSchema = void;                                           \
    explicit T(::std::pmr::memory_resource* resource = nullptr) noexcept     \
        : ruviaResource_(::ruvia::detail::pmrResourceOrDefault(resource)) {} \
    template <typename RuviaResourceOwnerT>                                  \
        requires requires(RuviaResourceOwnerT& owner) {                      \
            { owner.resource() } -> ::std::convertible_to<::std::pmr::memory_resource*>; \
        }                                                                    \
    explicit T(RuviaResourceOwnerT& owner) noexcept : T(owner.resource()) {} \
private:                                                                     \
    template <typename, typename> friend struct ::ruvia::JsonBody;           \
    template <typename, typename> friend struct ::ruvia::FormBody;           \
    friend struct ::ruvia::detail::ModelValidationAccess;                    \
    friend struct ::ruvia::detail::ModelJsonAccess;                          \
    template <::ruvia::FixedString Field>                                    \
    [[nodiscard]] ::ruvia::detail::ModelFieldState ruviaFieldState() const { \
        RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_FIELD_STATE_BRANCH, T, __VA_ARGS__) \
        {                                                                    \
            static_assert(::ruvia::detail::alwaysFalse<decltype(Field)>,     \
                "unknown RUVIA_MODEL field");                              \
        }                                                                    \
    }                                                                        \
    static ::std::optional<T> ruviaParseJsonBody(                            \
        ::std::string_view body, ::std::pmr::memory_resource* resource) {    \
        return ruviaParseJsonBodyDepth(                                      \
            body, resource, 0,                                               \
            ::ruvia::detail::ModelStringStorage::kBorrowed);                 \
    }                                                                        \
    static ::std::optional<T> ruviaParseJsonBodyOwned(                       \
        ::std::string_view body, ::std::pmr::memory_resource* resource) {    \
        return ruviaParseJsonBodyDepth(                                      \
            body, resource, 0,                                               \
            ::ruvia::detail::ModelStringStorage::kOwned);                    \
    }                                                                        \
    static ::std::optional<T> ruviaParseJsonBodyDepth(                       \
        ::std::string_view body, ::std::pmr::memory_resource* resource,      \
        ::std::size_t depth,                                                 \
        ::ruvia::detail::ModelStringStorage ruviaStringStorage) {            \
        if (depth > ::ruvia::detail::kMaxJsonDepth) return ::std::nullopt;   \
        auto json = ::ruvia::JsonObject::parse(body, resource);              \
        if (!json) return ::std::nullopt;                                    \
        return ruviaMaterializeInput(                                        \
            ::ruvia::detail::makeJsonModelInput(                             \
                json->view(), resource, ruviaStringStorage));                \
    }                                                                        \
    static ::std::optional<T> ruviaParseFormBody(                            \
        ::std::string_view body, ::std::pmr::memory_resource* resource) {    \
        auto form = ::ruvia::FormObject::parse(body, resource);              \
        if (!form) return ::std::nullopt;                                    \
        return ruviaMaterializeInput(                                        \
            ::ruvia::detail::makeFormModelInput(form->view(), resource));    \
    }                                                                        \
    static ::std::optional<T> ruviaParseFormFields(                          \
        const ::ruvia::RequestNameValueList& fields,                         \
        ::std::pmr::memory_resource* resource) {                             \
        return ruviaMaterializeInput(                                        \
            ::ruvia::detail::makeFormFieldsModelInput(fields, resource));    \
    }                                                                        \
    static ::std::optional<T> ruviaMaterializeInput(                         \
        const ::ruvia::detail::ModelInput& ruviaInput) {                     \
        T model{ruviaInput.resource()};                                      \
        if (!model.ruviaMaterialize(ruviaInput)) return ::std::nullopt;      \
        return ::std::move(model);                                           \
    }                                                                        \
    bool ruviaMaterialize(const ::ruvia::detail::ModelInput& ruviaInput) {   \
        auto* const ruviaResource = ruviaResource_;                          \
        const auto ruviaStringStorage = ruviaInput.stringStorage();          \
        bool ruviaValid = false;                                             \
        if (ruviaInput.kind() == ::ruvia::detail::ModelInputKind::kJson) {   \
            ruviaValid = ::ruvia::detail::visitModelInputJsonFields(         \
                ruviaInput, [this, ruviaResource, ruviaStringStorage](       \
                    ::std::string_view key, ::std::string_view value) {      \
                    RUVIA_MODEL_FOR_EACH(                                    \
                        RUVIA_MODEL_PARSE_JSON_FIELD, T, __VA_ARGS__)        \
                });                                                          \
        } else {                                                             \
            ruviaValid = ::ruvia::detail::visitModelInputFormFields(         \
                ruviaInput, [this, &ruviaInput, ruviaResource](              \
                    ::std::string_view key, ::std::string_view value) {      \
                    RUVIA_MODEL_FOR_EACH(                                    \
                        RUVIA_MODEL_PARSE_FORM_FIELD, T, __VA_ARGS__)        \
                });                                                          \
        }                                                                    \
        if (ruviaValid) {                                                    \
            RUVIA_MODEL_FOR_EACH(                                            \
                RUVIA_MODEL_APPLY_DEFAULT_FIELD, T, __VA_ARGS__)            \
        }                                                                    \
        return ruviaValid;                                                   \
    }                                                                        \
    void ruviaAppendJson(::std::pmr::string& output) const {                 \
        output.push_back('{');                                               \
        bool first = true;                                                   \
        RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_APPEND_JSON_FIELD, T, __VA_ARGS__)  \
        output.push_back('}');                                               \
    }                                                                        \
    [[nodiscard]] ::std::size_t ruviaJsonSizeHint() const {                 \
        ::std::size_t size = 2;                                              \
        bool first = true;                                                   \
        RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_JSON_SIZE_FIELD, T, __VA_ARGS__)    \
        return size;                                                         \
    }                                                                        \
    template <typename RuviaValidatorT>                                      \
    void ruviaValidateRequired(                                              \
        ::std::string_view prefix, RuviaValidatorT& validator) const {       \
        RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_REQUIRED_FIELD, T, __VA_ARGS__)     \
    }                                                                        \
    ::std::pmr::memory_resource* ruviaResource_;                             \
    static_assert(::ruvia::JsonBody<T>::value, "RUVIA_MODEL registered " #T)
