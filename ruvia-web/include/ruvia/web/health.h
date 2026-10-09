#pragma once

#include <cstdint>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"

namespace ruvia {

enum class readiness_state : std::uint8_t {
    ready,
    unavailable,
};

struct readiness_response_options final {
    readiness_state state_{readiness_state::ready};
    borrowed_text unavailable_reason_{"service is not ready"};
};

[[nodiscard]] http_response make_health_response(context& context_value);

[[nodiscard]] http_response make_readiness_response(
    context& context_value, readiness_response_options options = {});

}  // namespace ruvia
