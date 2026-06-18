#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ruvia/http/HttpTypes.h"

// Lightweight route-resolution result types. Kept in their own header (rather
// than RouterInternal.h) so the request hot path — including the connection
// work set, which embeds a RouteResolution — can name them without pulling in
// the full router/controller/model machinery. RouteEntry is only referenced
// through a pointer here, so a forward declaration suffices.

namespace ruvia::detail {

struct RouteEntry;

struct RouteMatch final {
    const RouteEntry* route{nullptr};
    std::array<RouteParamView, kMaxRouteParams> params{};
    std::size_t paramCount{0};
};

enum class RouteResolveStatus {
    kFound,
    kNotFound,
    kMethodNotAllowed
};

struct RouteResolution final {
    RouteResolveStatus status{RouteResolveStatus::kNotFound};
    const RouteEntry* route{nullptr};
    RouteMatch match{};
    RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
    std::uint32_t allowedMethods{0};
    bool dynamic{false};

    [[nodiscard]] bool found() const noexcept {
        return status == RouteResolveStatus::kFound && route != nullptr;
    }
};

}  // namespace ruvia::detail
