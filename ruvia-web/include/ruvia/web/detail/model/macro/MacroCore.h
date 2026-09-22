#pragma once

#include "ruvia/web/detail/model/ModelOptions.h"

// The closure is a structural NTTP even when invoking its factory needs runtime
// state. Let the compiler infer constexpr only for expressions that support it.
#define RUVIA_DEFAULT(value) ::ruvia::detail::model::Default<[]() { return value; }>
#define RUVIA_INITIAL(value) ::ruvia::detail::model::Initial<[]() { return value; }>
#define RUVIA_NULLABLE ::ruvia::detail::model::Nullable
#define RUVIA_OMIT_EMPTY ::ruvia::detail::model::OmitEmpty
#define RUVIA_EMIT_NULL ::ruvia::detail::model::EmitNull
