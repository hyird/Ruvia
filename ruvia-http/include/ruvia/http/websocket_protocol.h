#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"

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

// The supported Close status space from RFC 6455 section 7.4 and the IANA
// WebSocket Close Code registry. Role-specific restrictions remain with the
// connection. Local-only sentinel codes such as 1005/1006 are not wire values.
class websocket_close_code final {
public:
    template <std::integral integer_type>
    [[nodiscard]] static constexpr websocket_close_code from_value(integer_type value) {
        if (!is_valid_value(value)) {
            throw std::invalid_argument("invalid WebSocket Close status code");
        }
        return websocket_close_code(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<websocket_close_code> try_from_value(
        integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return websocket_close_code(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        const auto integer_value = +value;
        return (integer_value >= 1000 && integer_value <= 1003) ||
               (integer_value >= 1007 && integer_value <= 1014) ||
               (integer_value >= 3000 && integer_value <= 4999);
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(websocket_close_code, websocket_close_code) = default;

private:
    struct validated_tag_type {};

    constexpr websocket_close_code(std::uint16_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint16_t value_;
};

static_assert(std::is_trivially_copyable_v<websocket_close_code>);
static_assert(sizeof(websocket_close_code) == sizeof(std::uint16_t));

namespace websocket_close_status {
inline constexpr auto normal_closure = websocket_close_code::from_value(1000);
inline constexpr auto going_away = websocket_close_code::from_value(1001);
inline constexpr auto internal_error = websocket_close_code::from_value(1011);
}  // namespace websocket_close_status

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

// zlib parameters of an endpoint's permessage-deflate deflater, which RFC 7692
// leaves to the sender: compression level (0-9) and memLevel (1-9). Only a
// direction negotiated with context takeover keeps a private deflater, created
// on its first compressed message: directions without takeover (the default)
// share the worker's codecs, which use zlib's default memLevel, and allocate no
// codec state per connection. memLevel sizes the hash heads plus pending
// buffer at 2^(memLevel + 9) bytes on top of the 128 KiB window and history of
// a 15-bit window: 128 KiB at 8, 8 KiB at 4, so 4 saves 120 KiB per takeover
// connection at a lower ratio.
struct websocket_deflater_config final {
    int compression_level_{6};
    int mem_level_{8};
};

// The one range check of websocket_deflater_config; throws
// std::invalid_argument outside zlib's ranges.
inline void validate_websocket_deflater_config(websocket_deflater_config config) {
    if (config.compression_level_ < 0 || config.compression_level_ > 9) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    if (config.mem_level_ < 1 || config.mem_level_ > 9) {
        throw std::invalid_argument("WebSocket deflate memory level must be between 1 and 9");
    }
}

// Server permessage-deflate policy.
struct websocket_deflate_config final {
    bool enabled_{true};
    // Opt-in: never mix secrets and attacker-controlled content in one dictionary.
    // A peer requesting no-context-takeover still receives independent messages.
    bool context_takeover_{false};
    websocket_deflater_config deflater_{};
};

// Client permessage-deflate offer. compression_ holds the offered extension
// parameters (RFC 7692 section 7.1); offer_client_max_window_bits_ adds the
// valueless client_max_window_bits that lets the server pick the client window
// (section 7.1.2.2) and is required when compression_.client_max_window_bits_
// is set.
struct websocket_client_deflate_offer final {
    websocket_compression compression_{};
    bool offer_client_max_window_bits_{true};
    websocket_deflater_config deflater_{};
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
    // Construct a zero-copy message over caller-owned payload storage, rejecting
    // direct owning-string temporaries. Keep that storage alive and unchanged
    // until the view, or a send operation borrowing it, is no longer in use.
    [[nodiscard]] static constexpr websocket_message borrow(
        websocket_opcode opcode, borrowed_text payload_value) noexcept {
        return websocket_message(opcode, payload_value.view());
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
// the next next_event(), feed() or an append to the driver's input buffer may
// invalidate those views.
class websocket_event final {
public:
    [[nodiscard]] websocket_event_kind kind() const noexcept {
        return static_cast<websocket_event_kind>(value_.index());
    }
    [[nodiscard]] const websocket_message* message() const& noexcept {
        return std::get_if<websocket_message>(&value_);
    }
    const websocket_message* message() const&& = delete;
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
    using value_type = std::variant<websocket_message, websocket_ping_event, websocket_pong_event,
        websocket_close_event, websocket_protocol_error_event, websocket_transport_end_event>;
    static_assert(static_cast<std::uint8_t>(websocket_event_kind::transport_end) + 1 == std::variant_size_v<value_type>);
    template <typename event_type>
    explicit websocket_event(event_type event) noexcept
        : value_(std::move(event)) {}
    [[nodiscard]] static websocket_event message(websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_event(websocket_message::borrow(opcode, payload_value));
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
