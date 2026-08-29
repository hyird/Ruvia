#pragma once

#include <cstdint>

#include "ruvia/http/BorrowedText.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

enum class ReadinessState : std::uint8_t {
    kReady,
    kUnavailable,
};

struct ReadinessResponseOptions final {
    ReadinessState state{ReadinessState::kReady};
    BorrowedText unavailableReason{"service is not ready"};
};

[[nodiscard]] HttpResponse makeHealthResponse(Context& context);

[[nodiscard]] HttpResponse makeReadinessResponse(
    Context& context, ReadinessResponseOptions options = {});

}  // namespace ruvia
