#pragma once

#include <string_view>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

// Returns the media type before any parameters. The view borrows the input;
// owning string temporaries cannot be passed through borrowed_text.
[[nodiscard]] std::string_view http_media_type_only(borrowed_text value) noexcept;

// Validates one Content-Type field value, including RFC 9110's optional empty
// semicolon-delimited parameter slots. Nonempty parameters require name=value
// without whitespace around '='.
[[nodiscard]] bool is_valid_http_content_type_field_value(std::string_view value) noexcept;

}  // namespace ruvia
