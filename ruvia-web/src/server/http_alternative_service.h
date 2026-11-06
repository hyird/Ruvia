#pragma once

#include <string_view>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

// Adds the listener's Alt-Svc advertisement (RFC 7838 §3) to a final
// response. An application that set its own Alt-Svc field keeps it: the
// advertisement is a default, never a second, conflicting field. `value`
// borrows listener storage that outlives every response of the connection.
inline void apply_alternative_service(http_response& response, std::string_view value) {
    if (!value.empty() && !response.header("Alt-Svc").has_value()) {
        response.header_stable_view("Alt-Svc", value);
    }
}

}  // namespace ruvia::detail
