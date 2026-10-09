#pragma once

#include "ruvia/web/detail/model/model_options.h"

// The closure is a structural NTTP even when invoking its factory needs runtime
// state. Let the compiler infer constexpr only for expressions that support it.
#define RUVIA_DEFAULT(value) ::ruvia::detail::model::default_value<[]() { return value; }>
#define RUVIA_INITIAL(value) ::ruvia::detail::model::initial<[]() { return value; }>
#define RUVIA_NULLABLE ::ruvia::detail::model::nullable
#define RUVIA_OMIT_EMPTY ::ruvia::detail::model::omit_empty
#define RUVIA_EMIT_NULL ::ruvia::detail::model::emit_null
