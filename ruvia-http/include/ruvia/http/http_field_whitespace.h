#pragma once

#include <string_view>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

// Removes only SP and HTAB from the ends of an HTTP field value (OWS).
// The result borrows the caller's input.
[[nodiscard]] std::string_view http_trim_ows(borrowed_text value) noexcept;

}  // namespace ruvia
