#pragma once

#include <optional>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"

#include "websocket/http_websocket_frame_codec.h"
#include "websocket/http_websocket_payload_validation.h"

namespace ruvia::detail {

class websocket_frame_read_result;

// One borrowed frame with validated metadata combinations. Payload storage must
// outlive the view, so named factories reject owning-string rvalues. They also
// keep continuation and control frames from acquiring an impossible data opcode
// or compression bit; the wire reader additionally owns masking, length, and
// Close payload validation before publishing the same type.
class websocket_frame_view final {
public:
    [[nodiscard]] static constexpr websocket_frame_view text(
        std::string_view payload_value, bool final, bool compressed = false) noexcept {
        return websocket_frame_view(websocket_frame_kind::text, payload_value, final, compressed);
    }

    template <http_temporary_owning_char_string string>
    static websocket_frame_view text(string&&, bool, bool = false) = delete;

    [[nodiscard]] static constexpr websocket_frame_view binary(
        std::string_view payload_value, bool final, bool compressed = false) noexcept {
        return websocket_frame_view(websocket_frame_kind::binary, payload_value, final, compressed);
    }

    template <http_temporary_owning_char_string string>
    static websocket_frame_view binary(string&&, bool, bool = false) = delete;

    [[nodiscard]] static constexpr websocket_frame_view continuation(
        std::string_view payload_value, bool final) noexcept {
        return websocket_frame_view(websocket_frame_kind::continuation, payload_value, final, false);
    }

    template <http_temporary_owning_char_string string>
    static websocket_frame_view continuation(string&&, bool) = delete;

    [[nodiscard]] static std::optional<websocket_frame_view> close(
        std::string_view payload_value) noexcept {
        if (payload_value.size() > 125 || websocket_close_payload_failure(payload_value).has_value()) {
            return std::nullopt;
        }
        return websocket_frame_view(websocket_frame_kind::close, payload_value, true, false);
    }

    template <http_temporary_owning_char_string string>
    static std::optional<websocket_frame_view> close(string&&) = delete;

    [[nodiscard]] static constexpr std::optional<websocket_frame_view> ping(
        std::string_view payload_value) noexcept {
        if (payload_value.size() > 125) {
            return std::nullopt;
        }
        return websocket_frame_view(websocket_frame_kind::ping, payload_value, true, false);
    }

    template <http_temporary_owning_char_string string>
    static std::optional<websocket_frame_view> ping(string&&) = delete;

    [[nodiscard]] static constexpr std::optional<websocket_frame_view> pong(
        std::string_view payload_value) noexcept {
        if (payload_value.size() > 125) {
            return std::nullopt;
        }
        return websocket_frame_view(websocket_frame_kind::pong, payload_value, true, false);
    }

    template <http_temporary_owning_char_string string>
    static std::optional<websocket_frame_view> pong(string&&) = delete;

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
        : websocket_frame_view(start.kind(), payload_value, start.final(), start.compressed()) {}

    constexpr websocket_frame_view(
        websocket_frame_kind kind, std::string_view payload_value, bool final, bool compressed) noexcept
        : kind_(kind),
          payload_(payload_value),
          final_(final),
          compressed_(compressed) {}

    websocket_frame_kind kind_;
    std::string_view payload_;
    bool final_;
    bool compressed_;
};
}  // namespace ruvia::detail
