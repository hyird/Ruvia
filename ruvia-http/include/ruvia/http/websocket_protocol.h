#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace ruvia {

// awaiting_peer_eof: a client completed the close handshake and, per RFC 6455
// section 7.1.1, leaves closing the transport to the server. Its driver keeps
// reading (next_event() discards input) until notify_transport_eof(), which
// leads to the transport-end plan; if no EOF arrives within a reasonable time
// the client may close first, through abort().
enum class websocket_liveness_mode : std::uint8_t { open,
    awaiting_peer_close,
    awaiting_peer_eof,
    inactive };

enum class websocket_opcode : std::uint8_t {
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA
};
// Exact RFC 7692 negotiation, including independent sender dictionaries and
// windows. Omitted max-window parameters imply 15; presence also controls the
// negotiated extension's wire metadata.
struct websocket_compression final {
    bool enabled_{false};
    bool server_no_context_takeover_{true};
    bool client_no_context_takeover_{true};
    std::optional<int> server_max_window_bits_{};
    std::optional<int> client_max_window_bits_{};
    bool operator==(const websocket_compression&) const = default;
};

struct websocket_deflate_config final {
    bool enabled_{true};
    int compression_level_{6};
    // Opt-in: never mix secrets and attacker-controlled content in one dictionary.
    // A peer requesting no-context-takeover still receives independent messages.
    bool context_takeover_{false};
    // zlib memLevel (1-9) of a connection's private deflater. Only a direction
    // negotiated with context takeover keeps one, created on its first
    // compressed message: directions without takeover (the default) share the
    // worker's codecs and allocate no codec state per connection. memLevel
    // sizes the hash heads plus pending buffer at 2^(memLevel + 9) bytes on
    // top of the 128 KiB window and history of a 15-bit window: 128 KiB at 8,
    // 8 KiB at 4, so 4 saves 120 KiB per takeover connection at a lower ratio.
    int mem_level_{8};
};

namespace detail {
class websocket_compression_extension final {
public:
    explicit constexpr websocket_compression_extension(websocket_compression compression) noexcept {
        if (!compression.enabled_) {
            return;
        }
        append("permessage-deflate");
        if (compression.server_no_context_takeover_) {
            append("; server_no_context_takeover");
        }
        if (compression.client_no_context_takeover_) {
            append("; client_no_context_takeover");
        }
        if (compression.server_max_window_bits_) {
            append("; server_max_window_bits=");
            number(*compression.server_max_window_bits_);
        }
        if (compression.client_max_window_bits_) {
            append("; client_max_window_bits=");
            number(*compression.client_max_window_bits_);
        }
    }
    [[nodiscard]] constexpr std::string_view view() const& noexcept {
        return {bytes_.data(), size_};
    }
    std::string_view view() const&& = delete;
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

private:
    constexpr void append(std::string_view text) noexcept {
        for (char ch : text) {
            bytes_[size_++] = ch;
        }
    }
    constexpr void number(int value) noexcept {
        if (value >= 10) {
            bytes_[size_++] = '1';
        }
        bytes_[size_++] = static_cast<char>('0' + value % 10);
    }
    std::array<char, 160> bytes_{};
    std::size_t size_{0};
};

}  // namespace detail

class websocket_message final {
public:
    // Construct a message view over caller-owned payload storage. The payload
    // must remain valid until the next read on its connection or until the
    // caller otherwise stops using this view.
    [[nodiscard]] static constexpr websocket_message borrow(
        websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_message(opcode, payload_value);
    }

    websocket_message(const websocket_message&) noexcept = default;
    websocket_message& operator=(const websocket_message&) noexcept = default;
    websocket_message(websocket_message&&) noexcept = default;
    websocket_message& operator=(websocket_message&&) noexcept = default;

    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }

    // The payload view borrows the connection read buffer: it is valid only
    // until the next websocket read() on the same connection (the same rule as
    // body_reader::read). Copy the payload before the next read if it must
    // outlive the current message.
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] bool text() const noexcept {
        return opcode_ == websocket_opcode::text;
    }

    [[nodiscard]] bool binary() const noexcept {
        return opcode_ == websocket_opcode::binary;
    }

private:
    constexpr websocket_message(websocket_opcode opcode, std::string_view payload_value) noexcept
        : opcode_(opcode),
          payload_(payload_value) {}

    websocket_opcode opcode_{websocket_opcode::text};
    std::string_view payload_;
};

class websocket_connection;

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

// The pending transport output: bytes() followed by borrowed_bytes(), written in
// that order. borrowed_bytes() is a payload the driver submitted by reference
// (websocket_connection::submit_borrowed_frame); it is empty otherwise.
class websocket_output_plan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr std::string_view borrowed_bytes() const noexcept {
        return borrowed_bytes_;
    }
    // Total pending bytes: what consume_output() takes once both are written.
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return bytes_.size() + borrowed_bytes_.size();
    }
    [[nodiscard]] constexpr websocket_transport_disposition disposition() const noexcept {
        return disposition_;
    }

private:
    friend class websocket_connection;
    constexpr websocket_output_plan(std::string_view bytes_value, std::string_view borrowed_bytes_value,
        websocket_transport_disposition disposition) noexcept
        : bytes_(bytes_value),
          borrowed_bytes_(borrowed_bytes_value),
          disposition_(disposition) {}
    std::string_view bytes_;
    std::string_view borrowed_bytes_;
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
    friend class websocket_connection;
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
