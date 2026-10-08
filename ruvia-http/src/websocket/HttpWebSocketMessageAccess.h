#pragma once

#include "ruvia/http/WebSocketProtocol.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {

struct WebSocketMessageAccess final {
    [[nodiscard]] static constexpr WebSocketMessage make(
        WebSocketOpcode opcode, std::string_view payload) noexcept {
        return WebSocketMessage(opcode, payload);
    }

    template <HttpTemporaryOwningCharString Payload>
    static WebSocketMessage make(WebSocketOpcode, Payload&&) = delete;
};

}  // namespace ruvia::detail
