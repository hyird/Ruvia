#pragma once

#include "ruvia/web/detail/model/ModelOptions.h"

#define RUVIA_DEFAULT(value) ::ruvia::detail::model::Default<[]() constexpr { return value; }>
#define RUVIA_NULLABLE ::ruvia::detail::model::Nullable
#define RUVIA_OMIT_EMPTY ::ruvia::detail::model::OmitEmpty
#define RUVIA_EMIT_NULL ::ruvia::detail::model::EmitNull
