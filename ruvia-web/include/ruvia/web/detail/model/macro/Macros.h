#pragma once

#include "ruvia/web/detail/model/ModelBase.h"
#include "ruvia/web/detail/model/macro/MacroFieldOps.h"

// Field descriptors are forwarded directly into a C++ variadic template. No
// preprocessor argument counter or FOR_EACH expansion participates in model
// registration, so the framework does not impose a fixed field-count limit.
// Repeat each role marker on the generated final type so language services do
// not need to resolve it through the CRTP base.

#define RUVIA_REQUEST_MODEL(T, ...)                         \
    struct T final : ::ruvia::RequestModel<T, __VA_ARGS__> { \
        using RuviaModelBase::RuviaModelBase;               \
        using RuviaRequestModelSchema = void;               \
    }

#define RUVIA_RESPONSE_MODEL(T, ...)                         \
    struct T final : ::ruvia::ResponseModel<T, __VA_ARGS__> { \
        using RuviaModelBase::RuviaModelBase;               \
        using RuviaResponseModelSchema = void;              \
    }
