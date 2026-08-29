#pragma once

#include "ruvia/http/HttpKnownMethod.h"

namespace ruvia::detail {

struct HttpConditionalMethodPlan final {
    bool evaluatesPreconditions;
    bool usesNotModifiedResponse;
    bool evaluatesIfModifiedSince;
    bool evaluatesRange;
};

[[nodiscard]] inline constexpr HttpConditionalMethodPlan httpConditionalMethodPlan(
    HttpKnownMethod method) noexcept {
    switch (method) {
        case HttpKnownMethod::kGet:
            return {true, true, true, true};
        case HttpKnownMethod::kHead:
            return {true, true, true, false};
        case HttpKnownMethod::kPost:
        case HttpKnownMethod::kPut:
        case HttpKnownMethod::kDelete:
        case HttpKnownMethod::kPatch:
            return {true, false, false, false};
        case HttpKnownMethod::kOptions:
        case HttpKnownMethod::kConnect:
        case HttpKnownMethod::kUnknown:
            return {false, false, false, false};
    }
    return {false, false, false, false};
}

}  // namespace ruvia::detail
