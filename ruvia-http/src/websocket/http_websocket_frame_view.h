#pragma once

#include <string_view>

#include "websocket/http_websocket_frame_codec.h"

namespace ruvia::detail {

class websocket_frame_read_result;

// One borrowed frame published by the wire reader, which owns masking, length,
// opcode/compression combinations, and Close payload validation. Payload
// storage must outlive the view.
class websocket_frame_view final {
public:
    [[nodiscard]] constexpr websocket_frame_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend class websocket_frame_read_result;

    constexpr websocket_frame_view(
        const websocket_frame_start& start, std::string_view payload_value) noexcept
        : kind_(start.kind()),
          payload_(payload_value),
          final_(start.final()),
          compressed_(start.compressed()) {}

    websocket_frame_kind kind_;
    std::string_view payload_;
    bool final_;
    bool compressed_;
};
}  // namespace ruvia::detail
