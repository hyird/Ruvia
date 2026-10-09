#pragma once

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia::detail {

struct websocket_message_access final {
    [[nodiscard]] static constexpr websocket_message make(
        websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_message(opcode, payload_value);
    }

    template <http_temporary_owning_char_string payload>
    static websocket_message make(websocket_opcode, payload&&) = delete;
};

}  // namespace ruvia::detail
