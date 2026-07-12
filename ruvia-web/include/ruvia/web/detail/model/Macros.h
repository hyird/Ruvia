#pragma once

#include "ruvia/web/detail/model/MacroFieldOps.h"
#include "ruvia/web/detail/model/MacroJsonOps.h"

// Public model DSL and generated class body.
// Model.h owns runtime field types, parser helpers, model options, and JSON
// serialization. Validation schema macros live in Validation.h.

#define RUVIA_MODEL(T, ...)                                                  \
    class T {                                                               \
    public:                                                                 \
        RUVIA_MODEL_FIELD_COUNT_GUARD(__VA_ARGS__)                           \
        explicit T(::std::pmr::memory_resource* resource = nullptr) noexcept \
            : ruviaResource_(::ruvia::detail::pmrResourceOrDefault(resource)) {} \
        template <typename RuviaResourceOwnerT>                               \
            requires requires(RuviaResourceOwnerT& owner) {                   \
                { owner.resource() } -> ::std::convertible_to<::std::pmr::memory_resource*>; \
            }                                                               \
        explicit T(RuviaResourceOwnerT& owner) noexcept                       \
            : T(owner.resource()) {}                                         \
        static ::std::optional<T> ruviaParseJsonBody(                        \
            ::std::string_view body,                                        \
            ::std::pmr::memory_resource* resource) {                        \
            return ruviaParseJsonBodyDepth(body, resource, 0);               \
        }                                                                   \
        static ::std::optional<T> ruviaParseJsonBodyDepth(                   \
            ::std::string_view body,                                        \
            ::std::pmr::memory_resource* resource,                          \
            ::std::size_t depth) {                                          \
            if (depth > ::ruvia::detail::kMaxJsonDepth) {                    \
                return ::std::nullopt;                                      \
            }                                                               \
            auto json = ::ruvia::JsonObject::parse(body, resource);          \
            if (!json) {                                                    \
                return ::std::nullopt;                                      \
            }                                                               \
            return ruviaMaterializeInput(                                    \
                ::ruvia::detail::makeJsonModelInput(json->view(), resource)); \
        }                                                                   \
        static ::std::optional<T> ruviaParseFormBody(                        \
            ::std::string_view body,                                        \
            ::std::pmr::memory_resource* resource) {                        \
            auto form = ::ruvia::FormObject::parse(body, resource);          \
            if (!form) {                                                    \
                return ::std::nullopt;                                      \
            }                                                               \
            return ruviaMaterializeInput(                                    \
                ::ruvia::detail::makeFormModelInput(form->view(), resource)); \
        }                                                                   \
        static ::std::optional<T> ruviaParseFormFields(                      \
            const ::ruvia::RequestNameValueList& fields,                    \
            ::std::pmr::memory_resource* resource) {                        \
            return ruviaMaterializeInput(                                    \
                ::ruvia::detail::makeFormFieldsModelInput(fields, resource)); \
        }                                                                   \
        RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_FIELD_ACCESSORS, T, __VA_ARGS__)           \
        template <::ruvia::FixedString Field>                                \
        [[nodiscard]] auto get() const {                                    \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_TYPED_GET_BRANCH, T, __VA_ARGS__)      \
            {                                                               \
                static_assert(                                              \
                    ::ruvia::detail::alwaysFalse<decltype(Field)>,           \
                    "unknown RUVIA_MODEL JSON field");                      \
            }                                                               \
        }                                                                   \
        template <::ruvia::FixedString Field>                                \
        [[nodiscard]] ::ruvia::detail::ModelFieldState ruviaFieldState() const { \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_FIELD_STATE_BRANCH, T, __VA_ARGS__)    \
            {                                                               \
                static_assert(                                              \
                    ::ruvia::detail::alwaysFalse<decltype(Field)>,           \
                    "unknown RUVIA_MODEL field");                           \
            }                                                               \
        }                                                                   \
        void ruviaAppendJson(::std::pmr::string& output) const {             \
            output.push_back('{');                                          \
            bool first = true;                                              \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_APPEND_JSON_FIELD, T, __VA_ARGS__)     \
            output.push_back('}');                                          \
        }                                                                   \
        [[nodiscard]] ::std::size_t ruviaJsonSizeHint() const {              \
            ::std::size_t size = 2;                                         \
            bool first = true;                                              \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_JSON_SIZE_FIELD, T, __VA_ARGS__)       \
            return size;                                                    \
        }                                                                   \
    private:                                                                \
        static ::std::optional<T> ruviaMaterializeInput(                    \
            const ::ruvia::detail::ModelInput& ruviaInput) {                \
            T request{ruviaInput.resource()};                               \
            if (!request.ruviaMaterialize(ruviaInput)) {                    \
                return ::std::nullopt;                                      \
            }                                                               \
            return ::std::move(request);                                     \
        }                                                                   \
        bool ruviaMaterialize(const ::ruvia::detail::ModelInput& ruviaInput) { \
            auto* const ruviaResource = ruviaResource_;                     \
            bool ruviaValid = false;                                         \
            if (ruviaInput.kind() == ::ruvia::detail::ModelInputKind::kJson) { \
                ruviaValid = ::ruvia::detail::visitModelInputJsonFields(ruviaInput, [this, ruviaResource]( \
                    ::std::string_view key,                                 \
                    ::std::string_view value) {                             \
                    RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_PARSE_JSON_FIELD, T, __VA_ARGS__) \
                });                                                         \
            } else {                                                        \
                ruviaValid = ::ruvia::detail::visitModelInputFormFields(ruviaInput, [this, &ruviaInput, ruviaResource]( \
                    ::std::string_view key,                                 \
                    ::std::string_view value) {                             \
                    RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_PARSE_FORM_FIELD, T, __VA_ARGS__) \
                });                                                         \
            }                                                               \
            if (ruviaValid) {                                                \
                RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_APPLY_DEFAULT_FIELD, T, __VA_ARGS__) \
            }                                                               \
            return ruviaValid;                                               \
        }                                                                   \
        ::std::pmr::memory_resource* ruviaResource_;                         \
        RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_FIELD_STORAGE, T, __VA_ARGS__)               \
    };                                                                      \
    static_assert(::ruvia::JsonBody<T>::value, "RUVIA_MODEL registered " #T)
