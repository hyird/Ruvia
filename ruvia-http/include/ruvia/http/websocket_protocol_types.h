#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

namespace detail {
class ws_connection;
}  // namespace detail

enum class websocket_transport_disposition : std::uint8_t { keep_open,
    end_transport };
enum class websocket_frame_submit_status : std::uint8_t {
    accepted,
    not_open,
    invalid_opcode,
    message_too_large,
    invalid_text_payload,
    control_frame_too_large
};
enum class websocket_close_submit_status : std::uint8_t {
    accepted,
    already_closing,
    closed,
    invalid_code,
    invalid_reason,
    reason_too_large
};
enum class websocket_abort_disposition : std::uint8_t { abort_transport,
    no_transport_action };
enum class websocket_output_consume_status : std::uint8_t { pending,
    drained,
    out_of_range };
enum class websocket_connection_role : std::uint8_t { server,
    client };
using websocket_mask_key_type = std::array<char, 4>;
// Client transports must supply a fresh cryptographically random key per call.
// The context is borrowed and must outlive the connection. Failure is fatal to
// the transport: operations may throw and the caller must abort the connection.
using websocket_mask_key_generator_type = bool (*)(void*, websocket_mask_key_type&) noexcept;

class websocket_output_plan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr websocket_transport_disposition disposition() const noexcept {
        return disposition_;
    }

private:
    friend class detail::ws_connection;
    constexpr websocket_output_plan(std::string_view bytes_value, websocket_transport_disposition disposition) noexcept
        : bytes_(bytes_value),
          disposition_(disposition) {}
    std::string_view bytes_;
    websocket_transport_disposition disposition_;
};

enum class websocket_event_kind : std::uint8_t { message,
    ping,
    pong,
    close,
    protocol_error,
    transport_end };

class websocket_message_event final {
public:
    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_event;
    constexpr websocket_message_event(websocket_opcode opcode, std::string_view payload_value) noexcept
        : opcode_(opcode),
          payload_(payload_value) {}
    websocket_opcode opcode_;
    std::string_view payload_;
};

class websocket_ping_event final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_event;
    explicit constexpr websocket_ping_event(std::string_view payload_value) noexcept
        : payload_(payload_value) {}
    std::string_view payload_;
};

class websocket_pong_event final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_event;
    explicit constexpr websocket_pong_event(std::string_view payload_value) noexcept
        : payload_(payload_value) {}
    std::string_view payload_;
};

class websocket_close_event final {
public:
    // RFC 6455 section 7.1.5 uses 1005 when the peer supplied no status code;
    // section 7.4.1 reserves it from ever being emitted on wire.
    [[nodiscard]] constexpr std::uint16_t close_code() const noexcept {
        return close_code_;
    }
    [[nodiscard]] constexpr std::string_view reason() const noexcept {
        return reason_;
    }

private:
    friend class websocket_event;
    constexpr websocket_close_event(std::uint16_t close_code, std::string_view reason) noexcept
        : close_code_(close_code),
          reason_(reason) {}
    std::uint16_t close_code_;
    std::string_view reason_;
};

class websocket_protocol_error_event final {
public:
    [[nodiscard]] constexpr std::uint16_t close_code() const noexcept {
        return close_code_;
    }

private:
    friend class websocket_event;
    explicit constexpr websocket_protocol_error_event(std::uint16_t close_code) noexcept
        : close_code_(close_code) {}
    std::uint16_t close_code_;
};

class websocket_transport_end_event final {
private:
    friend class websocket_event;
    constexpr websocket_transport_end_event() noexcept = default;
};

// One protocol value shared by both public drivers and the sans-I/O core.
// Payload/reason views borrow the driver's input or message assembly storage;
// the driver's next poll()/next_event() or feed() may invalidate those views.
class websocket_event final {
public:
    [[nodiscard]] websocket_event_kind kind() const noexcept {
        return static_cast<websocket_event_kind>(value_.index());
    }
    [[nodiscard]] const websocket_message_event* message() const& noexcept {
        return std::get_if<websocket_message_event>(&value_);
    }
    const websocket_message_event* message() const&& = delete;
    [[nodiscard]] const websocket_ping_event* ping() const& noexcept {
        return std::get_if<websocket_ping_event>(&value_);
    }
    const websocket_ping_event* ping() const&& = delete;
    [[nodiscard]] const websocket_pong_event* pong() const& noexcept {
        return std::get_if<websocket_pong_event>(&value_);
    }
    const websocket_pong_event* pong() const&& = delete;
    [[nodiscard]] const websocket_close_event* close() const& noexcept {
        return std::get_if<websocket_close_event>(&value_);
    }
    const websocket_close_event* close() const&& = delete;
    [[nodiscard]] const websocket_protocol_error_event* protocol_error() const& noexcept {
        return std::get_if<websocket_protocol_error_event>(&value_);
    }
    const websocket_protocol_error_event* protocol_error() const&& = delete;
    [[nodiscard]] const websocket_transport_end_event* transport_end() const& noexcept {
        return std::get_if<websocket_transport_end_event>(&value_);
    }
    const websocket_transport_end_event* transport_end() const&& = delete;

private:
    friend class detail::ws_connection;
    using value_type = std::variant<websocket_message_event, websocket_ping_event, websocket_pong_event,
        websocket_close_event, websocket_protocol_error_event, websocket_transport_end_event>;
    static_assert(static_cast<std::uint8_t>(websocket_event_kind::transport_end) + 1 == std::variant_size_v<value_type>);
    template <typename event_type>
    explicit websocket_event(event_type event) noexcept
        : value_(std::move(event)) {}
    [[nodiscard]] static websocket_event message(websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_event(websocket_message_event(opcode, payload_value));
    }
    [[nodiscard]] static websocket_event ping(std::string_view payload_value) noexcept {
        return websocket_event(websocket_ping_event(payload_value));
    }
    [[nodiscard]] static websocket_event pong(std::string_view payload_value) noexcept {
        return websocket_event(websocket_pong_event(payload_value));
    }
    [[nodiscard]] static websocket_event close(std::uint16_t code, std::string_view reason) noexcept {
        return websocket_event(websocket_close_event(code, reason));
    }
    [[nodiscard]] static websocket_event protocol_error(std::uint16_t code) noexcept {
        return websocket_event(websocket_protocol_error_event(code));
    }
    [[nodiscard]] static websocket_event make_transport_end() noexcept {
        return websocket_event(websocket_transport_end_event());
    }
    value_type value_;
};

}  // namespace ruvia
