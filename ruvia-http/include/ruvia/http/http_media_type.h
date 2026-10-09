#pragma once

#include <string_view>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

// Returns the media type before any parameters. The view borrows the input;
// owning string temporaries cannot be passed through borrowed_text.
[[nodiscard]] std::string_view http_media_type_only(borrowed_text value) noexcept;

// Validates the syntax of one Content-Type field value.
[[nodiscard]] bool is_valid_http_content_type_field_value(std::string_view value) noexcept;

}  // namespace ruvia
