#pragma once

#include "ruvia/web/detail/model/ModelBase.h"
#include "ruvia/web/detail/model/macro/MacroFieldOps.h"

// Field descriptors are forwarded directly into a C++ variadic template. No
// preprocessor argument counter or FOR_EACH expansion participates in model
// registration, so the framework does not impose a fixed field-count limit.

#define RUVIA_REQUEST_MODEL(T, ...)                         \
    struct T final : ::ruvia::RequestModel<T, __VA_ARGS__> { \
        using RuviaModelBase::RuviaModelBase;               \
    }

#define RUVIA_RESPONSE_MODEL(T, ...)                         \
    struct T final : ::ruvia::ResponseModel<T, __VA_ARGS__> { \
        using RuviaModelBase::RuviaModelBase;               \
    }
