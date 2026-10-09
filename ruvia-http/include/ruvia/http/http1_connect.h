#pragma once

#include <variant>

#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

// The successful head ends HTTP framing and transfers the connection to a
// duplex byte stream. Payload belongs to that stream, not to an HTTP response.
[[nodiscard]] std::variant<http1_response_head_plan, http_protocol_error> prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version) noexcept;

}  // namespace ruvia
