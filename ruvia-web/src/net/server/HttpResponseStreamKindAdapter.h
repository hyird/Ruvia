#pragma once

#include "net/server/HttpResponseStreamHead.h"
#include "ruvia/http/RouteModes.h"

namespace ruvia::detail {

[[nodiscard]] inline ResponseStreamKind responseStreamKindForRouteMode(ResponseBodyMode mode) noexcept {
    return mode == ResponseBodyMode::kSse ? ResponseStreamKind::kSse : ResponseStreamKind::kGeneric;
}

}  // namespace ruvia::detail
