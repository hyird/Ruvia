#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/detail/borrowed_view.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_deflate_codecs.h"
#include "ruvia/http/websocket_protocol.h"

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
