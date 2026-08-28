#pragma once

#include <string_view>

namespace ruvia {

// Validates an outbound HTTP origin-form request target without allocating.
// The same protocol primitive is shared by ordinary HTTP clients and
// WebSocket opening handshakes before either runtime performs I/O.
[[nodiscard]] bool isValidHttpClientOriginTarget(std::string_view target) noexcept;

}  // namespace ruvia
