#pragma once

#include "ruvia/http/detail/model/MacroFieldOps.h"
#include "ruvia/http/detail/model/MacroJsonOps.h"

// Public model DSL and generated class body.
// Model.h owns runtime field types, parser helpers, model options, and JSON
// serialization. Validation schema macros live in Validation.h.

#define RUVIA_MODEL(T, ...)                                                  \
    class T {                                                               \
    public:                                                                 \
        explicit T(::std::pmr::memory_resource* resource = ::std::pmr::get_default_resource()) noexcept \
            : body_(::ruvia::RequestObject(                                  \
                  ::ruvia::RequestObjectKind::kJson, ::std::string_view{"{}"}, resource)) { \
            ruviaParsed_ = true;                                             \
        }                                                                   \
        template <typename RuviaResourceOwnerT>                               \
            requires requires(RuviaResourceOwnerT& owner) {                   \
                { owner.resource() } -> ::std::convertible_to<::std::pmr::memory_resource*>; \
            }                                                               \
        explicit T(RuviaResourceOwnerT& owner) noexcept                       \
            : T(owner.resource()) {}                                         \
        explicit T(::ruvia::RequestObject body) noexcept : body_(body) {}     \
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
            T request{::ruvia::RequestObject(                                \
                ::ruvia::RequestObjectKind::kJson, json->view(), resource)};  \
            if (!request.ruviaEnsureParsed()) {                              \
                return ::std::nullopt;                                      \
            }                                                               \
            return ::std::move(request);                                     \
        }                                                                   \
        static ::std::optional<T> ruviaParseFormBody(                        \
            ::std::string_view body,                                        \
            ::std::pmr::memory_resource* resource) {                        \
            auto form = ::ruvia::FormObject::parse(body, resource);          \
            if (!form) {                                                    \
                return ::std::nullopt;                                      \
            }                                                               \
            T request{::ruvia::RequestObject(                                \
                ::ruvia::RequestObjectKind::kForm, form->view(), resource)};  \
            if (!request.ruviaEnsureParsed()) {                              \
                return ::std::nullopt;                                      \
            }                                                               \
            return ::std::move(request);                                     \
        }                                                                   \
        [[nodiscard]] const ::ruvia::RequestObject& body() const noexcept {   \
            return body_;                                                    \
        }                                                                   \
        RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_FIELD_ACCESSORS, T, __VA_ARGS__)           \
        [[nodiscard]] ::std::optional<::std::string_view> get(              \
            ::std::string_view field) const {                               \
            if (!ruviaEnsureParsed()) {                                      \
                return ::std::nullopt;                                      \
            }                                                               \
            return body_.get<::std::string_view>(field);                    \
        }                                                                   \
        template <typename FieldT>                                           \
        [[nodiscard]] ::std::optional<FieldT> get(                          \
            ::std::string_view field) const {                               \
            if (!ruviaEnsureParsed()) {                                      \
                return ::std::nullopt;                                      \
            }                                                               \
            return body_.get<FieldT>(field);                                \
        }                                                                   \
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
            ruviaEnsureParsed();                                             \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_FIELD_STATE_BRANCH, T, __VA_ARGS__)    \
            {                                                               \
                static_assert(                                              \
                    ::ruvia::detail::alwaysFalse<decltype(Field)>,           \
                    "unknown RUVIA_MODEL field");                           \
            }                                                               \
        }                                                                   \
        void ruviaAppendJson(::std::pmr::string& output) const {             \
            ruviaEnsureParsed();                                             \
            output.push_back('{');                                          \
            bool first = true;                                              \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_APPEND_JSON_FIELD, T, __VA_ARGS__)     \
            output.push_back('}');                                          \
        }                                                                   \
        [[nodiscard]] ::std::size_t ruviaJsonSizeHint() const {              \
            ruviaEnsureParsed();                                             \
            ::std::size_t size = 2;                                         \
            bool first = true;                                              \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_JSON_SIZE_FIELD, T, __VA_ARGS__)       \
            return size;                                                    \
        }                                                                   \
    private:                                                                \
        bool ruviaEnsureParsed() const {                                     \
            if (ruviaParsed_) {                                              \
                return !ruviaInvalid_;                                       \
            }                                                               \
            ruviaParsed_ = true;                                             \
            ruviaInvalid_ = false;                                           \
            RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_RESET_FIELD, T, __VA_ARGS__)           \
            bool ruviaValid = true;                                          \
            if (body_.kind() == ::ruvia::RequestObjectKind::kJson) {         \
                ruviaValid = ::ruvia::detail::visitRequestJsonFields(body_, [this, &ruviaValid]( \
                    ::std::string_view key,                                 \
                    ::std::string_view value) {                             \
                    if (!ruviaValid) {                                       \
                        return;                                             \
                    }                                                       \
                    RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_PARSE_JSON_FIELD, T, __VA_ARGS__) \
                }) && ruviaValid;                                            \
            } else {                                                        \
                ruviaValid = ::ruvia::detail::visitRequestFormFields(body_, [this, &ruviaValid]( \
                    ::std::string_view key,                                 \
                    ::std::string_view value) {                             \
                    if (!ruviaValid) {                                       \
                        return;                                             \
                    }                                                       \
                    RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_PARSE_FORM_FIELD, T, __VA_ARGS__) \
                }) && ruviaValid;                                            \
            }                                                               \
            if (ruviaValid) {                                                \
                RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_APPLY_DEFAULT_FIELD, T, __VA_ARGS__) \
            }                                                               \
            if (!ruviaValid) {                                               \
                ruviaInvalid_ = true;                                        \
                RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_RESET_FIELD, T, __VA_ARGS__)       \
            }                                                               \
            return ruviaValid;                                               \
        }                                                                   \
        ::ruvia::RequestObject body_;                                        \
        mutable bool ruviaParsed_{false};                                    \
        mutable bool ruviaInvalid_{false};                                   \
        RUVIA_MODEL_FOR_EACH(RUVIA_MODEL_FIELD_STORAGE, T, __VA_ARGS__)               \
    };                                                                      \
    static_assert(::ruvia::JsonBody<T>::value, "RUVIA_MODEL registered " #T)
