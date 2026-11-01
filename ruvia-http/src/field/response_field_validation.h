#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/field/request_header_kind.h"

namespace ruvia::detail {

enum class response_field_status : std::uint8_t {
    ok,
    invalid,
    repeated_singleton,
};

// One state per generated response header section. Content-Type's single media
// type cannot be combined into a list (RFC 9110 section 5.3).
class response_field_state final {
private:
    friend response_field_status validate_response_field(request_header_kind kind, std::string_view value,
        response_field_state& state) noexcept;

    bool content_type_seen_{false};
};

// Sender-only response metadata semantics. Callers validate field-name/value bytes
// and retain protocol-specific framing, connection, and forbidden-field rules.
// A fresh state also validates one value before an http_response setter commits
// its replacement; the setter owns the separate append/replace policy.
[[nodiscard]] response_field_status validate_response_field(request_header_kind kind, std::string_view value,
    response_field_state& state) noexcept;

}  // namespace ruvia::detail
