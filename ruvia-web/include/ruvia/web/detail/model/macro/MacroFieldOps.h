#pragma once
#include "ruvia/web/detail/model/ModelField.h"
#include "ruvia/web/detail/model/ModelSchema.h"
#include "ruvia/web/detail/model/macro/MacroCore.h"

// Field macros produce descriptors consumed once by RUVIA_MODEL. The model
// macro expands each descriptor into both the member/accessor declaration and
// the schema entry, so a field cannot be omitted from parsing or serialization.

#define RUVIA_MODEL_DECLARE_CONST_ACCESSOR_true(field)                  \
    [[nodiscard]] const RuviaFieldType_##field& field() const& {        \
        return ruviaField_##field##_.requiredValue();                   \
    }                                                                   \
    [[nodiscard]] const RuviaFieldType_##field& field() const&& = delete;

#define RUVIA_MODEL_DECLARE_CONST_ACCESSOR_false(field)                                \
    [[nodiscard]] const ::std::optional<RuviaFieldType_##field>& field() const& noexcept { \
        return ruviaField_##field##_.value();                                           \
    }                                                                                   \
    [[nodiscard]] const ::std::optional<RuviaFieldType_##field>& field() const&& = delete;

#define RUVIA_MODEL_DECLARE_RESET_true(field)

#define RUVIA_MODEL_DECLARE_RESET_false(field) \
    void field##Reset() noexcept {               \
        ruviaField_##field##_.reset();           \
    }

#define RUVIA_MODEL_DECLARE_FIELD_IMPL(required, wire, field, type, ...)                                                                                                                                                                                                                                                                                          \
private:                                                                                                                                                                                                                                                                                                                                                          \
    using RuviaFieldType_##field = RUVIA_MODEL_UNPAREN type;                                                                                                                                                                                                                                                                                                      \
    using RuviaFieldOptions_##field = decltype(::ruvia::detail::model::ModelOptions{__VA_ARGS__});                                                                                                                                                                                                                                                                \
    static constexpr auto ruviaFieldWireHash_##field = ::ruvia::detail::model::modelFieldNameHash(::std::string_view{wire});                                                                                                                                                                                                                                      \
    ::ruvia::detail::model::ModelField<RuviaFieldType_##field, required, RuviaFieldOptions_##field> ruviaField_##field##_{wire, ::ruvia::detail::model::ModelOptions{__VA_ARGS__}};                                                                                                                                                                               \
                                                                                                                                                                                                                                                                                                                                                                  \
public:                                                                                                                                                                                                                                                                                                                                                           \
    RUVIA_MODEL_CAT(RUVIA_MODEL_DECLARE_CONST_ACCESSOR_, required)(field)                                                                                                                                                                                                                                                                                           \
    [[nodiscard]] RuviaFieldType_##field& field##Ensure() & {                                                                                                                                                                                                                                                                                                     \
        return ruviaField_##field##_.ensure(ruviaResource_);                                                                                                                                                                                                                                                                                                      \
    }                                                                                                                                                                                                                                                                                                                                                             \
    [[nodiscard]] RuviaFieldType_##field& field##Ensure() && = delete;                                                                                                                                                                                                                                                                                            \
    RUVIA_MODEL_CAT(RUVIA_MODEL_DECLARE_RESET_, required)(field)                                                                                                                                                                                                                                                                                                   \
    template <typename RuviaFieldValueT>                                                                                                                                                                                                                                                                                                                          \
        requires((::ruvia::detail::isRuviaString<RuviaFieldType_##field> && (::std::is_convertible_v<RuviaFieldValueT &&, ::std::string_view> || ::std::constructible_from<RuviaFieldType_##field, RuviaFieldValueT &&>)) || (!::ruvia::detail::isRuviaString<RuviaFieldType_##field> && ::std::constructible_from<RuviaFieldType_##field, RuviaFieldValueT &&>)) \
    auto& field(RuviaFieldValueT&& value) & {                                                                                                                                                                                                                                                                                                                     \
        ruviaField_##field##_.assign(::std::forward<RuviaFieldValueT>(value), ruviaResource_);                                                                                                                                                                                                                                                                    \
        return *this;                                                                                                                                                                                                                                                                                                                                             \
    }                                                                                                                                                                                                                                                                                                                                                             \
    template <typename RuviaFieldValueT>                                                                                                                                                                                                                                                                                                                          \
    auto& field(RuviaFieldValueT&&) && = delete;

#define RUVIA_FIELD(field, type, ...) (true, #field, field, (type) __VA_OPT__(,) __VA_ARGS__)

#define RUVIA_FIELD_NAME(wire_name, field, type, ...) (true, wire_name, field, (type) __VA_OPT__(,) __VA_ARGS__)

#define RUVIA_OPTIONAL_FIELD(field, type, ...) (false, #field, field, (type) __VA_OPT__(,) __VA_ARGS__)

#define RUVIA_OPTIONAL_FIELD_NAME(wire_name, field, type, ...) (false, wire_name, field, (type) __VA_OPT__(,) __VA_ARGS__)

#define RUVIA_MODEL_DECLARE_FIELD(T, descriptor) RUVIA_MODEL_DECLARE_FIELD_EXPAND descriptor
#define RUVIA_MODEL_DECLARE_FIELD_EXPAND(...) RUVIA_MODEL_DECLARE_FIELD_IMPL(__VA_ARGS__)

#define RUVIA_MODEL_SCHEMA_FIELD(T, descriptor) RUVIA_MODEL_SCHEMA_FIELD_EXPAND(T, descriptor)
#define RUVIA_MODEL_SCHEMA_FIELD_EXPAND(T, descriptor) RUVIA_MODEL_SCHEMA_FIELD_EXPAND_ARGS(T, RUVIA_MODEL_UNPAREN descriptor)
#define RUVIA_MODEL_SCHEMA_FIELD_EXPAND_ARGS(T, ...) RUVIA_MODEL_SCHEMA_FIELD_UNPACK(T, __VA_ARGS__)
#define RUVIA_MODEL_SCHEMA_FIELD_UNPACK(T, required, wire, field, type, ...) \
    ::ruvia::detail::model::ModelFieldDescriptor<&T::ruviaField_##field##_, T::ruviaFieldWireHash_##field, ::ruvia::FixedString{#field}>{},
