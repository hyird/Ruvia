#pragma once

#include <string_view>

#include "ruvia/http/BorrowedText.h"

namespace ruvia {

// Removes only SP and HTAB from the ends of an HTTP field value (OWS).
// The result borrows the caller's input.
[[nodiscard]] std::string_view httpTrimOws(BorrowedText value) noexcept;

}  // namespace ruvia
