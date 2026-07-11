#pragma once

#include <cstdint>

namespace ruvia::detail {

// Which side of the connection this endpoint is. The role affects stream-id
// admission, message-head semantics, connection preface bytes, and directional
// SETTINGS validation.
enum class Http2Role : std::uint8_t {
    kServer,
    kClient,
};

}  // namespace ruvia::detail
