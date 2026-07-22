#pragma once

#include <string_view>

namespace ruvia::detail {

// WHATWG Fetch `serialized-origin` syntax, the wire form an Origin field carries.
// Unlike an RFC 3986 uri-host it requires lowercase scheme and domain bytes and
// canonical IPv6 groups, a canonical 16-bit decimal port with known defaults
// omitted, and never contains a path. The opaque-origin literal `null` is
// deliberately not a serialized origin.
[[nodiscard]] bool isValidHttpSerializedOrigin(std::string_view value) noexcept;

}  // namespace ruvia::detail
