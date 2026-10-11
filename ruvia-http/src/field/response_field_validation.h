#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/request_header_kind.h"

namespace ruvia::detail {

enum class response_field_status : std::uint8_t {
    ok,
    invalid,
    repeated_singleton,
};

// One state per response header section. Content-Type is a single media type;
// repeated field lines cannot be combined into a list.
class response_field_state final {
private:
    friend response_field_status validate_response_field(request_header_kind kind, std::string_view value,
        http_field_list_role role, response_field_state& state) noexcept;

    bool content_type_seen_{false};
};

// Shared response metadata semantics. Callers validate field-name/value bytes
// and retain protocol-specific framing, connection, and forbidden-field rules.
// A fresh state also validates one value before an http_response setter commits
// its replacement; the setter owns the separate append/replace policy.
[[nodiscard]] response_field_status validate_response_field(request_header_kind kind, std::string_view value,
    http_field_list_role role, response_field_state& state) noexcept;

}  // namespace ruvia::detail
