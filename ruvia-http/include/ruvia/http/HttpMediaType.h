#pragma once

#include <string_view>

#include "ruvia/http/BorrowedText.h"

namespace ruvia {

// Returns the media type before any parameters. The view borrows the input;
// owning string temporaries cannot be passed through BorrowedText.
[[nodiscard]] std::string_view httpMediaTypeOnly(BorrowedText value) noexcept;

// Validates the syntax of one Content-Type field value.
[[nodiscard]] bool isValidHttpContentTypeFieldValue(std::string_view value) noexcept;

}  // namespace ruvia
