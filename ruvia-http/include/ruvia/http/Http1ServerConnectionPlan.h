#pragma once

#include <cstdint>

#include "ruvia/http/Http1RequestConnectionPlan.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"

namespace ruvia {

using Http1ServerConnectionPlan = ::ruvia::Http1RequestConnectionPlan;

[[nodiscard]] inline constexpr Http1ServerConnectionPlan http1PlanHttp10RequestConnection(
    bool close, bool keepAlive) noexcept {
    return ::ruvia::planHttp10RequestConnection(close, keepAlive);
}

[[nodiscard]] inline constexpr Http1ServerConnectionPlan http1PlanHttp11RequestConnection(
    bool close) noexcept {
    return ::ruvia::planHttp11RequestConnection(close);
}

}  // namespace ruvia
