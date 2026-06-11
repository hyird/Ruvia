#pragma once

// Public model DSL and generated class body.
// Model.h owns runtime field types, parser helpers, model options, and JSON
// serialization. Validation schema macros live in Validation.h.

#define RUVIA_MODEL_EXPAND(x) x

#define RUVIA_MODEL_NARG(...) RUVIA_MODEL_NARG_(__VA_ARGS__, RUVIA_MODEL_RSEQ())
#define RUVIA_MODEL_NARG_(...) RUVIA_MODEL_EXPAND(RUVIA_MODEL_ARG_N(__VA_ARGS__))
#define RUVIA_MODEL_ARG_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) N
#define RUVIA_MODEL_RSEQ() 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0

#define RUVIA_MODEL_CAT(a, b) RUVIA_MODEL_CAT_(a, b)
#define RUVIA_MODEL_CAT_(a, b) a##b

#define RUVIA_DEFAULT(value) ::ruvia::detail::model::Default{value}
#define RUVIA_OMIT_EMPTY ::ruvia::detail::model::OmitEmpty{}
#define RUVIA_EMIT_NULL ::ruvia::detail::model::EmitNull{}
#define RUVIA_FIELD(field, type, ...) \
    ((type), field, (#field), (::ruvia::detail::model::ModelOptions{__VA_ARGS__}))
#define RUVIA_FIELD_NAME(wire_name, field, type, ...) \
    ((type), field, (wire_name), (::ruvia::detail::model::ModelOptions{__VA_ARGS__}))

#define RUVIA_MODEL_FE_1(m, T, x)       m(T, x)
#define RUVIA_MODEL_FE_2(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_1(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_3(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_2(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_4(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_3(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_5(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_4(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_6(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_5(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_7(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_6(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_8(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_7(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_9(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_8(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_10(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_9(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_11(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_10(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_12(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_11(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_13(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_12(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_14(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_13(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_15(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_14(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_16(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_15(m, T, __VA_ARGS__))

#define RUVIA_MODEL_FOR_EACH(m, T, ...) \
    RUVIA_MODEL_EXPAND(RUVIA_MODEL_CAT(RUVIA_MODEL_FE_, RUVIA_MODEL_NARG(__VA_ARGS__))(m, T, __VA_ARGS__))

#define RUVIA_MODEL_UNPAREN(...) __VA_ARGS__

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
        if (auto ruviaValue = ::ruvia::detail::parseJsonValue<RUVIA_MODEL_UNPAREN type>(ruviaValueInput, body_.resource()); ruviaValue) { \
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
            RUVIA_MODEL_UNPAREN type ruviaValue = ::ruvia::detail::makeRequestValue<RUVIA_MODEL_UNPAREN type>(body_.resource()); \
            if (::ruvia::detail::parseFormValue(value, ruviaValue, body_.resource())) { \
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
    [[nodiscard]] ::ruvia::ModelFieldRef<RUVIA_MODEL_UNPAREN type> field() { \
        ruviaEnsureParsed(); \
        return ::ruvia::ModelFieldRef<RUVIA_MODEL_UNPAREN type>( \
            ruviaField_##field##_, \
            ruviaState_##field##_, \
            body_.resource()); \
    } \
    [[nodiscard]] ::ruvia::ModelFieldConstRef<RUVIA_MODEL_UNPAREN type> field() const { \
        ruviaEnsureParsed(); \
        return ::ruvia::ModelFieldConstRef<RUVIA_MODEL_UNPAREN type>(ruviaField_##field##_); \
    } \
    template <typename RuviaFieldValueT> \
        requires ((::ruvia::detail::isRuviaString<RUVIA_MODEL_UNPAREN type> && \
                      (::std::is_convertible_v<RuviaFieldValueT&&, ::std::string_view> || \
                          ::std::constructible_from<RUVIA_MODEL_UNPAREN type, RuviaFieldValueT&&>)) || \
                  (!::ruvia::detail::isRuviaString<RUVIA_MODEL_UNPAREN type> && \
                      ::std::constructible_from<RUVIA_MODEL_UNPAREN type, RuviaFieldValueT&&>)) \
    model_type& field(RuviaFieldValueT&& value) { \
        ruviaEnsureParsed(); \
        ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kParsed; \
        ::ruvia::detail::model::assignFieldValue( \
            ruviaField_##field##_, \
            ::std::forward<RuviaFieldValueT>(value), \
            body_.resource()); \
        return *this; \
    }

#define RUVIA_MODEL_APPLY_DEFAULT_FIELD(T, x) \
    RUVIA_MODEL_APPLY_DEFAULT_FIELD_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_APPLY_DEFAULT_FIELD_I(...) RUVIA_MODEL_APPLY_DEFAULT_FIELD_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_APPLY_DEFAULT_FIELD_IMPL(type, field, wire, rules) \
    if (ruviaState_##field##_ == ::ruvia::detail::ModelFieldState::kMissing) { \
        rules.applyDefault(ruviaField_##field##_, body_.resource()); \
        if (ruviaField_##field##_) { \
            ruviaState_##field##_ = ::ruvia::detail::ModelFieldState::kParsed; \
        } \
    }

#define RUVIA_MODEL_JSON_SIZE_FIELD(T, x) \
    RUVIA_MODEL_JSON_SIZE_FIELD_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_JSON_SIZE_FIELD_I(...) RUVIA_MODEL_JSON_SIZE_FIELD_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_JSON_SIZE_FIELD_IMPL(type, field, wire, rules) \
    if (ruviaField_##field##_ && !(rules.omitEmpty() && ::ruvia::detail::model::isEmptyValue(*ruviaField_##field##_))) { \
        if (!first) { \
            ++size; \
        } \
        first = false; \
        size += ::ruvia::detail::jsonStringSizeHint(::std::string_view{wire}) + 1; \
        size += ::ruvia::detail::jsonSizeHintValue(*ruviaField_##field##_); \
    } else if (!ruviaField_##field##_ && rules.emitNull()) { \
        if (!first) { \
            ++size; \
        } \
        first = false; \
        size += ::ruvia::detail::jsonStringSizeHint(::std::string_view{wire}) + 5; \
    }

#define RUVIA_MODEL_APPEND_JSON_FIELD(T, x) \
    RUVIA_MODEL_APPEND_JSON_FIELD_I(RUVIA_MODEL_UNPAREN x)
#define RUVIA_MODEL_APPEND_JSON_FIELD_I(...) RUVIA_MODEL_APPEND_JSON_FIELD_IMPL(__VA_ARGS__)
#define RUVIA_MODEL_APPEND_JSON_FIELD_IMPL(type, field, wire, rules) \
    if (ruviaField_##field##_ && !(rules.omitEmpty() && ::ruvia::detail::model::isEmptyValue(*ruviaField_##field##_))) { \
        if (!first) { \
            output.push_back(','); \
        } \
        first = false; \
        ::ruvia::detail::appendJsonString(output, ::std::string_view{wire}); \
        output.push_back(':'); \
        ::ruvia::detail::appendJsonValue(output, *ruviaField_##field##_); \
    } else if (!ruviaField_##field##_ && rules.emitNull()) { \
        if (!first) { \
            output.push_back(','); \
        } \
        first = false; \
        ::ruvia::detail::appendJsonString(output, ::std::string_view{wire}); \
        output.append(":null"); \
    }

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
