#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
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
    control_frame_too_large,
    // A fragmented data message is still open; only control frames and frames
    // of its own opcode may be submitted until its final fragment.
    message_in_progress
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
// Why a WebSocket session ended, as this endpoint observed it; none while it is
// still active. The first cause wins: a later Close, EOF or abort never
// replaces it. The peer Close status itself is the close event's.
enum class websocket_termination : std::uint8_t {
    none,
    // The peer started the close handshake.
    peer_close,
    // The peer answered a locally started close handshake.
    local_close,
    // The transport ended without a Close frame (RFC 6455 section 7.1.5, 1006).
    eof,
    // An invalid frame or message (sent Close 1002).
    protocol_error,
    // A text message or Close reason that is not UTF-8 (sent Close 1007).
    invalid_payload_data,
    // A frame, message or inflated message above the receive limit (sent Close 1009).
    message_too_large,
    // A driver deadline: heartbeat Pong, close handshake or operation timeout.
    timeout,
    // Ended locally: abort(), a local failure or the session's teardown.
    aborted,
    // The transport failed to read or write.
    io_error,
};
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
// (websocket_connection::submit_frame with a websocket_borrowed_payload); it is
// empty otherwise.
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
    [[nodiscard]] const websocket_message* message() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_message>(&value_);
    }
    const websocket_message* message() const&& = delete;
    [[nodiscard]] const websocket_ping_event* ping() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_ping_event>(&value_);
    }
    const websocket_ping_event* ping() const&& = delete;
    [[nodiscard]] const websocket_pong_event* pong() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_pong_event>(&value_);
    }
    const websocket_pong_event* pong() const&& = delete;
    [[nodiscard]] const websocket_close_event* close() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_close_event>(&value_);
    }
    const websocket_close_event* close() const&& = delete;
    [[nodiscard]] const websocket_protocol_error_event* protocol_error() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_protocol_error_event>(&value_);
    }
    const websocket_protocol_error_event* protocol_error() const&& = delete;
    [[nodiscard]] const websocket_transport_end_event* transport_end() const& noexcept RUVIA_LIFETIMEBOUND {
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

namespace ruvia {

namespace detail {
class zlib_deflate_stream;
class zlib_inflate_stream;
}  // namespace detail

enum class websocket_inflate_result : std::uint8_t {
    ok,
    error,
    too_large,
};

// RFC 7692 permessage-deflate codecs for directions negotiated without context
// takeover. Such a direction starts every message from an empty dictionary,
// so one owner (for example a worker) can serve any number of connections
// with one codec set instead of a dictionary pair per connection.
//
// Each call handles one whole message and resets the codec it used before
// returning; a codec left by an exception is discarded and recreated by the
// next use: no message, and no failed message, ever influences another. Raw
// deflaters are created on first use for each (compression level, window
// bits) pair; one raw inflater with a 15-bit window serves every receive
// window. Not thread-safe; the address is stable for the borrowers.
class websocket_deflate_codecs final {
public:
    websocket_deflate_codecs() noexcept = default;
    ~websocket_deflate_codecs();
    websocket_deflate_codecs(const websocket_deflate_codecs&) = delete;
    websocket_deflate_codecs& operator=(const websocket_deflate_codecs&) = delete;
    websocket_deflate_codecs(websocket_deflate_codecs&&) = delete;
    websocket_deflate_codecs& operator=(websocket_deflate_codecs&&) = delete;

    // Appends the compressed message payload to `out` without the trailing
    // 0x00 0x00 0xFF 0xFF of the final flush (RFC 7692 section 7.2.1). The
    // level is 0 through 9 and the negotiated window 8 through 15; anything
    // else throws std::invalid_argument. false reports a codec failure;
    // allocation exceptions propagate with their original type.
    [[nodiscard]] bool deflate_message(
        std::string_view payload, std::string& out, int compression_level, int window_bits);

    // Appends the decompressed message to `out`, bounded by `message_limit`
    // across everything this call appends (RFC 7692 section 7.2.2).
    [[nodiscard]] websocket_inflate_result inflate_message(
        std::string_view payload, std::string& out, protocol_byte_limit message_limit);

private:
    static constexpr int min_window_bits = 8;
    static constexpr int max_window_bits = 15;
    static constexpr int compression_levels = 10;

    std::array<detail::zlib_deflate_stream*,
        static_cast<std::size_t>((max_window_bits - min_window_bits + 1) * compression_levels)>
        deflaters_{};
    detail::zlib_inflate_stream* inflater_{nullptr};
};

}  // namespace ruvia

namespace ruvia {

enum class websocket_feed_status : std::uint8_t { accepted,
    inactive,
    backpressured };

struct websocket_frame_options final {
    // False sends a non-final fragment of a text or binary message.
    bool final_{true};
    // Selects permessage-deflate, when negotiated, for a message's first frame.
    bool compress_{true};
};

// A frame payload websocket_connection::submit_frame() queues by reference
// instead of copying. Construction rejects temporary owning strings, whose
// bytes would expire before the transport writes them.
class websocket_borrowed_payload final {
public:
    explicit constexpr websocket_borrowed_payload(borrowed_text bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_.view();
    }

private:
    borrowed_text bytes_;
};

struct websocket_connection_options final {
    protocol_byte_limit message_limit_{protocol_byte_limit::limited(default_max_websocket_message_bytes)};
    // Additional receive-only ceiling, including message aggregation and inflation.
    // The smaller of this limit and message_limit_ applies to input; output is unchanged.
    protocol_byte_limit receive_message_limit_{};
    websocket_compression compression_{(websocket_compression{})};
    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};
    // The private deflater of a direction negotiated with context takeover.
    websocket_deflater_config deflater_{};
    // Borrowed codecs for every permessage-deflate direction negotiated without
    // context takeover; they must outlive the connection and are used only
    // synchronously inside its calls. nullptr gives such a direction a private
    // codec too. Private codecs are created on the direction's first
    // compressed message.
    websocket_deflate_codecs* shared_deflate_{nullptr};
    // Independent bound for feed() bytes awaiting next_event(). A rejected feed
    // consumes nothing; drain events or provide smaller chunks before retrying.
    std::size_t max_buffered_input_bytes_{default_max_websocket_message_bytes + 14};
};

// Sans-I/O RFC 6455 driver for an already upgraded connection. Input arrives
// through feed(), into a buffer the connection owns unless it was constructed
// with the caller's input buffer; with a borrowed buffer the caller may also
// append received bytes directly before calling next_event(). A borrowed
// buffer must outlive the
// connection. The role fixes inbound mask validation and outbound masking,
// including automatic Pong/Close. After the close handshake a server plans
// end_transport once its output is flushed; a client keeps the transport open
// (liveness awaiting_peer_eof, which feed() and next_event() answer by
// discarding input) until notify_transport_eof(), because the server closes
// first (RFC 6455 section 7.1.1). Event views remain valid until the next
// feed(), next_event() or an append to a borrowed input buffer. Output bytes
// remain valid until next_event(), submit_frame(), submit_close(),
// consume_output(), or destruction. Input, EOF and abort do not invalidate an
// in-flight write.
class websocket_connection final {
public:
    explicit websocket_connection(websocket_connection_options options = {});
    explicit websocket_connection(http_io_buffer& input, websocket_connection_options options = {});
    ~websocket_connection();
    websocket_connection(const websocket_connection&) = delete;
    websocket_connection& operator=(const websocket_connection&) = delete;
    websocket_connection(websocket_connection&&) noexcept;
    websocket_connection& operator=(websocket_connection&&) noexcept;

    // Input may borrow the previous event payload; any required snapshot is
    // taken before reclaiming consumed input storage.
    [[nodiscard]] websocket_feed_status feed(std::string_view input);
    template <detail::http_temporary_owning_char_string input_type>
    websocket_feed_status feed(input_type&&) = delete;
    [[nodiscard]] std::optional<websocket_event> next_event() & RUVIA_LIFETIMEBOUND;
    std::optional<websocket_event> next_event() && = delete;
    [[nodiscard]] std::size_t needed_input_bytes() const noexcept;
    [[nodiscard]] websocket_output_plan output_plan() const& noexcept RUVIA_LIFETIMEBOUND;
    websocket_output_plan output_plan() const&& = delete;
    [[nodiscard]] websocket_output_consume_status consume_output(std::size_t bytes) noexcept;
    void commit_transport_end() noexcept;
    // Records eof as the termination unless an earlier cause was recorded.
    void notify_transport_eof() noexcept;
    // `cause` tells why the driver aborts (none records aborted); an earlier
    // termination cause is kept.
    [[nodiscard]] websocket_abort_disposition abort(
        websocket_termination cause = websocket_termination::aborted) noexcept;
    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept;
    [[nodiscard]] websocket_termination termination() const noexcept;
    // RFC 6455 wire bytes of whole frames: header, mask key and payload as
    // framed, so after permessage-deflate, data and control frames alike.
    // Received counts every frame next_event() parsed; sent counts the output
    // consume_output() acknowledged. Both saturate.
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;
    // One frame of a text or binary message, or a Ping or Pong. A data frame is
    // a fragment of its message (RFC 6455 section 5.4): a final frame that
    // continues no open message is a whole message; with final_ false the
    // first frame opens the message with `opcode`, and with RSV1 when
    // compress_ selects permessage-deflate; later frames of the same opcode
    // are its continuation frames, and the final one ends it. Ping, Pong and
    // Close may be submitted between fragments; a data frame of the other
    // opcode reports message_in_progress until the final fragment, and a
    // non-final control frame reports invalid_opcode (section 5.5). A whole
    // message stays uncompressed when deflate does not shrink it; a
    // fragmented compressed message continues one DEFLATE stream across its
    // fragments, each ending at a sync flush (RFC 7692 section 7.2.1), and its
    // first frame fixes compression for all of them. Text is validated across
    // fragment boundaries and the message limit covers the whole message. A
    // rejected frame leaves an open message open. A failed submission
    // preserves queued output and discards any unsent compression history. It
    // can be retried unless resetting the codec also failed, in which case
    // liveness_mode() reports inactive.
    // This form copies the payload into the connection's output before returning.
    [[nodiscard]] websocket_frame_submit_status submit_frame(
        websocket_opcode opcode, std::string_view payload, websocket_frame_options options = {});
    // The same submission queuing an uncompressed, unmasked payload by
    // reference: output_plan() exposes it as borrowed_bytes() after the frame
    // header. Its bytes must stay alive and unchanged until consume_output()
    // has acknowledged them, abort(), or destruction. A masked (client) or
    // compressed frame, or a payload overlapping the pending output, is copied.
    [[nodiscard]] websocket_frame_submit_status submit_frame(
        websocket_opcode opcode, websocket_borrowed_payload payload, websocket_frame_options options = {});
    [[nodiscard]] websocket_close_submit_status submit_close(
        websocket_close_code code, std::string_view reason);

private:
    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia

#pragma once

namespace ruvia {

inline constexpr std::size_t max_websocket_subprotocols = 64;

// Allocation-free validated RFC 6455 subprotocol list, suitable for server
// route configuration and parsing offered protocol lists. Capacity is a product
// limit of max_websocket_subprotocols distinct protocols across all list elements;
// a handshake offering more is rejected as invalid rather than truncated.
class websocket_subprotocol_set final {
public:
    // Strict: every protocol must be a new token (configuration and the
    // offers a client sends, which RFC 6455 section 4.1 requires to be unique).
    [[nodiscard]] bool append(borrowed_text protocol) noexcept {
        const auto protocol_view = protocol.view();
        if (!is_valid_http_header_name(protocol_view) || size_ == protocols_.size() || contains(protocol_view)) {
            return false;
        }
        protocols_[size_++] = protocol_view;
        return true;
    }

    // Strict list form of append(); empty list elements are ignored.
    [[nodiscard]] bool append_list(borrowed_text value) noexcept {
        return append_elements(value.view(), false);
    }

    // A received Sec-WebSocket-Protocol offer. Uniqueness binds the sending
    // client only (RFC 6455 section 4.1); a server receiving a repeated
    // protocol skips it (section 4.2.1 imposes no recipient check).
    [[nodiscard]] bool append_offered_list(borrowed_text value) noexcept {
        return append_elements(value.view(), true);
    }

    [[nodiscard]] bool contains(std::string_view protocol) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (protocols_[i] == protocol) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

private:
    [[nodiscard]] bool append_elements(std::string_view value, bool skip_duplicates) noexcept {
        return detail::http_visit_token_list(value, detail::http_field_list_role::recipient,
                   [this, skip_duplicates](std::string_view protocol) noexcept {
                       return (skip_duplicates && contains(protocol)) || append(protocol);
                   }) == detail::http_field_list_parse_status::ok;
    }

    std::array<std::string_view, max_websocket_subprotocols> protocols_{};
    std::size_t size_{0};
};

// Server handshake policy shared by HTTP/1.1 Upgrade and RFC 8441 Extended
// CONNECT.
struct websocket_server_handshake_options final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    borrowed_span<std::string_view> supported_subprotocols_{};
    // Additional response fields, copied before the response plan is returned.
    // Framing, connection, and Sec-websocket fields belong to the handshake
    // itself. Names/values must be valid fields; values cannot start or end
    // with SP/HTAB.
    borrowed_http_headers response_headers_{};
    websocket_deflate_config deflate_{};
};

struct websocket_handshake_validation_options final {
    // RFC 6455 4.2.2 lets a server that only speaks particular subprotocols
    // refuse a client offering none of them, instead of accepting without a
    // Sec-WebSocket-Protocol response field. When set, a request whose offer
    // shares no entry with supported_subprotocols_ fails validation.
    borrowed_span<std::string_view> supported_subprotocols_{};
    bool require_subprotocol_{false};
};

class websocket_handshake_accepted final {
private:
    friend class websocket_handshake_validation_result;

    constexpr websocket_handshake_accepted() noexcept = default;
};

class websocket_handshake_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept;

    // RFC 6455 requires a server rejecting an unsupported version to advertise
    // every supported version. Apply this after any application error handler.
    void apply_required_response_headers(http_response& response) const;

private:
    friend class websocket_handshake_validation_result;

    enum class kind_type : std::uint8_t { invalid_request,
        unsupported_version,
        unsupported_subprotocol };

    explicit constexpr websocket_handshake_failure(kind_type kind) noexcept
        : kind_(kind) {}

    kind_type kind_;
};

class websocket_handshake_validation_result;
class http2_connection;
class http3_connection;
class http3_request_head;
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
class http2_connection;
class http2_stream_state;
class websocket_request_header_validator;
inline constexpr std::string_view websocket_switching_protocols_head_prefix =
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
[[nodiscard]] websocket_handshake_validation_result validate_http2_websocket_handshake(
    const http2_stream_state&, const http_request&,
    websocket_handshake_validation_options) noexcept;
}  // namespace detail

class websocket_handshake_validation_result final {
public:
    [[nodiscard]] constexpr const websocket_handshake_accepted* accepted() const& noexcept RUVIA_LIFETIMEBOUND {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const websocket_handshake_accepted* accepted() const&& = delete;

    [[nodiscard]] constexpr const websocket_handshake_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const websocket_handshake_failure* failure() const&& = delete;

private:
    friend class detail::websocket_request_header_validator;
    friend websocket_handshake_validation_result validate_http2_websocket_handshake(
        http2_connection&, std::uint32_t, const http_request&,
        websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result detail::validate_http2_websocket_handshake(
        const detail::http2_stream_state&, const http_request&,
        websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result validate_http3_websocket_handshake(
        const http3_request_head&, const http_request&, websocket_handshake_validation_options) noexcept;
    friend websocket_handshake_validation_result validate_websocket_handshake(
        const http_request&, const http1_request_body_plan&, websocket_handshake_validation_options) noexcept;

    using value_type = std::variant<websocket_handshake_accepted, websocket_handshake_failure>;

    explicit constexpr websocket_handshake_validation_result(
        websocket_handshake_accepted accepted) noexcept
        : value_(accepted) {}

    explicit constexpr websocket_handshake_validation_result(
        websocket_handshake_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr websocket_handshake_validation_result make_accepted() noexcept {
        return websocket_handshake_validation_result(websocket_handshake_accepted());
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_invalid_request() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::invalid_request));
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_unsupported_version() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::unsupported_version));
    }

    [[nodiscard]] static constexpr websocket_handshake_validation_result
    make_unsupported_subprotocol() noexcept {
        return websocket_handshake_validation_result(
            websocket_handshake_failure(websocket_handshake_failure::kind_type::unsupported_subprotocol));
    }

    value_type value_;
};

// The negotiation an RFC 8441 (HTTP/2) or RFC 9220 (HTTP/3) extended CONNECT
// 200 response committed on the wire. The runtime configures its websocket
// connection from this value, never from a separately recomputed
// compression/subprotocol tuple.
class websocket_extended_connect_negotiation final {
public:
    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    friend class http3_connection;
    websocket_extended_connect_negotiation(std::string subprotocol, websocket_compression compression) noexcept
        : subprotocol_(std::move(subprotocol)),
          compression_(compression) {}
    std::string subprotocol_;
    websocket_compression compression_;
};

// Validates an RFC 6455 HTTP/1.1 opening handshake, including the parser-owned
// request-body framing plan. It does not create or write a response.
[[nodiscard]] websocket_handshake_validation_result validate_websocket_handshake(
    const http_request& request, const http1_request_body_plan& body_plan,
    websocket_handshake_validation_options options = {}) noexcept;

// Owned HTTP/1.1 101 response plan. for_each_response_part emits stable views for
// scatter-gather I/O; compression() configures the subsequent websocket driver
// from the exact negotiation encoded in this response.
class websocket_server_handshake final {
public:
    websocket_server_handshake(const websocket_server_handshake&) = delete;
    websocket_server_handshake& operator=(const websocket_server_handshake&) = delete;
    websocket_server_handshake(websocket_server_handshake&&) noexcept = default;
    websocket_server_handshake& operator=(websocket_server_handshake&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] constexpr websocket_compression compression() const noexcept {
        return compression_;
    }

    template <typename visitor_type>
    void for_each_response_part(visitor_type&& visitor) const& {
        visitor(detail::websocket_switching_protocols_head_prefix);
        visitor(std::string_view(accept_.data(), accept_.size()));
        visitor(crlf);
        if (!subprotocol_.empty()) {
            visitor(subprotocol_header_prefix);
            visitor(std::string_view(subprotocol_));
            visitor(crlf);
        }
        if (!compression_extension_.empty()) {
            visitor(extensions_header_prefix);
            visitor(compression_extension_.view());
            visitor(crlf);
        }
        for (const auto& header : response_headers_) {
            visitor(header.name());
            visitor(std::string_view(": "));
            visitor(header.value());
            visitor(crlf);
        }
        visitor(crlf);
    }

    template <typename visitor_type>
    void for_each_response_part(visitor_type&&) const&& = delete;

private:
    friend websocket_server_handshake make_websocket_server_handshake(
        const http_request&, websocket_server_handshake_options);

    inline static constexpr std::string_view subprotocol_header_prefix = "Sec-WebSocket-Protocol: ";
    inline static constexpr std::string_view extensions_header_prefix = "Sec-WebSocket-Extensions: ";
    inline static constexpr std::string_view crlf = "\r\n";

    websocket_server_handshake(std::array<char, 28> accept, std::string subprotocol,
        websocket_compression compression, std::vector<http_header> response_headers_value) noexcept
        : accept_(accept),
          subprotocol_(std::move(subprotocol)),
          compression_(compression),
          compression_extension_(compression),
          response_headers_(std::move(response_headers_value)) {}

    std::array<char, 28> accept_;
    std::string subprotocol_;
    websocket_compression compression_;
    detail::websocket_compression_extension compression_extension_;
    std::vector<http_header> response_headers_;
};

// Call after validate_websocket_handshake() succeeds. The selected subprotocol
// and response fields are copied; the response plan borrows no
// request or server-preference storage.
[[nodiscard]] websocket_server_handshake make_websocket_server_handshake(
    const http_request& request, websocket_server_handshake_options options = {});

}  // namespace ruvia

namespace ruvia {
struct websocket_client_negotiation_config_view final {
    borrowed_span<std::string_view> subprotocols_{};
    websocket_client_deflate_offer deflate_{};
};
// The one failure shape of a client opening handshake over HTTP/1.1 Upgrade
// (RFC 6455 section 4.1) or Extended CONNECT (RFC 8441 over HTTP/2, RFC 9220
// over HTTP/3).
enum class websocket_client_handshake_error : std::uint8_t {
    // Not 101 over HTTP/1.1, not 2xx over Extended CONNECT.
    response_status,
    // Extended CONNECT: the 2xx HEADERS ended the stream.
    stream_closed,
    // HTTP/1.1: Sec-WebSocket-Accept is missing, repeated or wrong.
    accept,
    // HTTP/1.1: no Upgrade: websocket.
    upgrade,
    // HTTP/1.1: no Connection: Upgrade.
    connection,
    subprotocol,
    extensions,
    // Extended CONNECT: a connection-specific response field (RFC 9113
    // section 8.2.2, RFC 9114 section 4.2).
    forbidden_field,
};
struct websocket_client_negotiation_result_view final {
    // Borrows the supplied response headers.
    std::string_view selected_subprotocol_{};
    websocket_compression compression_{};
};

class websocket_client_negotiation;
namespace detail {
// The server's Sec-WebSocket-Protocol and Sec-WebSocket-Extensions selection
// against the offer; `extended` (RFC 8441) also rejects connection-specific
// fields. The result view borrows `fields`.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_websocket_client_selection(
    const websocket_client_negotiation& negotiation, std::span<const http_header> fields, bool extended);
}  // namespace detail

// HTTP-version-independent RFC 6455/7692 client negotiation owner. Configuration
// is copied once. It supplies request fields to HTTP/1 Upgrade or
// RFC 8441 Extended CONNECT, and validates the server's exact selection; the
// version-specific response validators check the same selection after their
// own status checks.
// All offer views expire with this owner; response views expire with the head.
class websocket_client_negotiation final {
public:
    // Validates the subprotocols and the deflate offer, including its deflater.
    static void validate_configuration(websocket_client_negotiation_config_view config);
    explicit websocket_client_negotiation(websocket_client_negotiation_config_view config);
    websocket_client_negotiation(websocket_client_negotiation&&) noexcept = default;
    websocket_client_negotiation& operator=(websocket_client_negotiation&&) = delete;
    websocket_client_negotiation(const websocket_client_negotiation&) = delete;
    websocket_client_negotiation& operator=(const websocket_client_negotiation&) = delete;
    [[nodiscard]] std::span<const http_header> request_fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span<const http_header>(fields_.data(), field_count_);
    }
    std::span<const http_header> request_fields() const&& = delete;
    // The server's Sec-WebSocket-Protocol and Sec-WebSocket-Extensions selection
    // in `fields` alone, without status or connection-specific field checks.
    [[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    validate_fields(std::span<const http_header> fields) const;
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    validate_fields(fields_type&&) const = delete;

private:
    friend std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
    detail::validate_websocket_client_selection(
        const websocket_client_negotiation&, std::span<const http_header>, bool);

    websocket_client_deflate_offer deflate_;
    std::array<http_header, 3> fields_{};
    std::size_t field_count_{0};
    // The owned Sec-WebSocket-Protocol request value; empty when none is offered.
    std::string_view offered_subprotocols_{};
};

class http1_parsed_client_response_head;

// RFC 6455 section 4.1 opening handshake over HTTP/1.1 Upgrade. The caller
// owns the base64-encoded 16-byte nonce (`encoded_key`), the negotiation and the
// application headers for the whole exchange; subprotocols and the deflate offer
// are validated by websocket_client_negotiation.

// Rejects application headers the handshake or request writer generates itself
// (Host, framing, Expect, Upgrade/Connection and every Sec-WebSocket-* field)
// and any header the request writer would refuse.
void validate_http1_websocket_client_handshake_configuration(std::span<const http_header_view> headers);

[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header> headers, std::span<char> head_buffer,
    http_request_field_source source = http_request_field_source::generated);
[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header_view> headers, std::span<char> head_buffer,
    http_request_field_source source = http_request_field_source::generated);

// Validates the 101 response against the key, then the server's selection
// against the negotiation. The result view borrows the response head.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response RUVIA_LIFETIMEBOUND);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24>,
    const websocket_client_negotiation&, const http1_parsed_client_response_head&&) = delete;

// Extended CONNECT over HTTP/2 or HTTP/3: validates the server's 2xx final
// head on a still-open stream, then its selection against the negotiation.
// The result view borrows the response head.
[[nodiscard]] std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_extended_connect_websocket_client_response(
    const websocket_client_negotiation& negotiation, const http_client_response_head& head, bool stream_open);
std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_extended_connect_websocket_client_response(
    const websocket_client_negotiation&, const http_client_response_head&&, bool) = delete;
}  // namespace ruvia
