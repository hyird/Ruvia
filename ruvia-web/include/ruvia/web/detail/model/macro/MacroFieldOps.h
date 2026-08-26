#pragma once

#include "ruvia/web/detail/model/ModelSchema.h"
#include "ruvia/web/detail/model/macro/MacroCore.h"

#define RUVIA_REQUIRED_FIELD(field, type, ...)                                 \
    ::ruvia::detail::model::ModelFieldDescriptor<::ruvia::FixedString{#field}, \
        ::ruvia::FixedString{#field}, type, true __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_REQUIRED_FIELD_NAME(wire_name, field, type, ...)                 \
    ::ruvia::detail::model::ModelFieldDescriptor<::ruvia::FixedString{#field}, \
        ::ruvia::FixedString{wire_name}, type, true __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_OPTIONAL_FIELD(field, type, ...)                                 \
    ::ruvia::detail::model::ModelFieldDescriptor<::ruvia::FixedString{#field}, \
        ::ruvia::FixedString{#field}, type, false __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_OPTIONAL_FIELD_NAME(wire_name, field, type, ...)                 \
    ::ruvia::detail::model::ModelFieldDescriptor<::ruvia::FixedString{#field}, \
        ::ruvia::FixedString{wire_name}, type, false __VA_OPT__(, ) __VA_ARGS__>
