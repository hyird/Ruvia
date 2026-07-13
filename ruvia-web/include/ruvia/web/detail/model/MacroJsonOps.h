#pragma once

#include "ruvia/web/detail/model/MacroCore.h"

// Per-field response JSON size and append fragments.

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
