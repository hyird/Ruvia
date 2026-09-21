#pragma once

#include "ruvia/web/detail/model/ModelBase.h"
#include "ruvia/web/detail/model/macro/MacroFieldOps.h"

// Field descriptors are forwarded directly into a C++ variadic template. No
// preprocessor argument counter or FOR_EACH expansion participates in model
// registration, so the framework does not impose a fixed field-count limit.
// Repeat the schema marker on the generated final type so language services do
// not need to resolve it through the CRTP base.

#define RUVIA_MODEL(T, ...)                                         \
    struct T final : ::ruvia::Model<T __VA_OPT__(, ) __VA_ARGS__> { \
        using RuviaModelBase::RuviaModelBase;                       \
        using RuviaModelSchema = void;                              \
    }
