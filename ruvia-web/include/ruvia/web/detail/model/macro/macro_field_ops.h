#pragma once

#include "ruvia/web/detail/model/macro/macro_core.h"
#include "ruvia/web/detail/model/model_schema.h"

#define RUVIA_REQUIRED_FIELD(field, type, ...)                                    \
    ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, \
        ::ruvia::fixed_string{#field}, type, true __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_REQUIRED_FIELD_NAME(wire_name, field, type, ...)                    \
    ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, \
        ::ruvia::fixed_string{wire_name}, type, true __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_OPTIONAL_FIELD(field, type, ...)                                    \
    ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, \
        ::ruvia::fixed_string{#field}, type, false __VA_OPT__(, ) __VA_ARGS__>

#define RUVIA_OPTIONAL_FIELD_NAME(wire_name, field, type, ...)                    \
    ::ruvia::detail::model::model_field_descriptor<::ruvia::fixed_string{#field}, \
        ::ruvia::fixed_string{wire_name}, type, false __VA_OPT__(, ) __VA_ARGS__>
