#include "websocket_connection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "borrowed_text.h"
#include "decimal_text.h"
#include "http_field_text.h"
#include "http_io_buffer.h"
#include "http_protocol.h"
#include "utf8.h"
#include "websocket_deflate.h"
#include "websocket_frame.h"

namespace ruvia {

namespace {

enum class websocket_payload_storage : std::uint8_t {
    copy,
    borrow,
};

[[nodiscard]] constexpr websocket_termination websocket_failure_termination(detail::websocket_protocol_failure failure) noexcept {
    switch (failure) {
        case detail::websocket_protocol_failure::protocol_error:
            break;
        case detail::websocket_protocol_failure::invalid_payload_data:
            return websocket_termination::invalid_payload_data;
        case detail::websocket_protocol_failure::message_too_large:
            return websocket_termination::message_too_large;
    }
    return websocket_termination::protocol_error;
}

// Where a data frame sits in its message (RFC 6455 section 5.4): only the
// first frame carries the opcode, only the last one sets FIN.
enum class websocket_fragment_position : std::uint8_t {
    whole,
    first,
    middle,
    last,
};

// UTF-8 of a text message submitted in fragments: a code point may straddle
// fragments, so the bytes of one left incomplete wait for the next fragment.
class websocket_utf8_fragments final {
public:
    [[nodiscard]] bool append(std::string_view fragment) noexcept {
        std::size_t index = 0;
        if (pending_size_ != 0) {
            const auto lead = static_cast<unsigned char>(pending_[0]);
            const std::size_t length = lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : 2);
            while (pending_size_ < length && index < fragment.size()) {
                pending_[pending_size_++] = fragment[index++];
            }
            const auto sequence = scan_utf8_sequence(std::string_view(pending_.data(), pending_size_), 0);
            if (pending_size_ < length) {
                return sequence.length_ == pending_size_;
            }
            if (!sequence.well_formed_) {
                return false;
            }
            pending_size_ = 0;
        }
        while (index < fragment.size()) {
            const auto lead = static_cast<unsigned char>(fragment[index]);
            if (lead < 0x80) {
                ++index;
                continue;
            }
            const auto sequence = scan_utf8_sequence(fragment, index);
            if (sequence.well_formed_) {
                index += sequence.length_;
                continue;
            }
            // A valid lead and continuation bytes cut off by the fragment end.
            if (index + sequence.length_ != fragment.size() || lead < 0xC2 || lead > 0xF4) {
                return false;
            }
            for (; index < fragment.size(); ++index) {
                pending_[pending_size_++] = fragment[index];
            }
        }
        return true;
    }

    [[nodiscard]] bool complete() const noexcept {
        return pending_size_ == 0;
    }

private:
    std::array<char, 4> pending_{};
    std::size_t pending_size_{0};
};

// The data message being submitted as fragments.
struct websocket_outbound_fragments final {
    websocket_opcode opcode_;
    // RSV1 went out on the first frame: every fragment continues one
    // permessage-deflate stream.
    bool compressed_;
    std::size_t size_;
    websocket_utf8_fragments utf8_;
};

}  // namespace

class websocket_connection::impl_type final {
public:
    enum class close_phase_type : std::uint8_t {
        open,
        // A locally initiated Close is at the tail of pending output. Once
        // flushed, the connection keeps receiving until the peer Close.
        local_close_queued,
        awaiting_peer_close,
        // A peer Close was received (or the connection failed) and the final
        // queued output still needs to be flushed before transport end. It may
        // be a Pong queued for an earlier Ping, not another Close.
        final_output_queued,
        // Client only (RFC 6455 section 7.1.1): the close handshake is complete
        // and the final output is still queued; once flushed, the server is
        // expected to close the transport first.
        handshake_output_queued,
        awaiting_peer_eof,
        transport_end_ready,
        closed,
    };

    // A null `input` gives the connection an input buffer of its own.
    impl_type(http_io_buffer* input, websocket_connection_options options) :
        input_(input != nullptr ? input : &owned_input_.emplace()),
        max_buffered_input_bytes_(options.max_buffered_input_bytes_),
        message_limit_(options.message_limit_),
        receive_message_limit_(options.receive_message_limit_.read_ceiling() < options.message_limit_.read_ceiling() ? options.receive_message_limit_ : options.message_limit_),
        receive_storage_(options.receive_storage_),
        role_(options.role_),
        mask_key_generator_(options.mask_key_generator_),
        mask_key_context_(options.mask_key_context_) {
        initialize_codecs(options);
    }

    void initialize_codecs(const websocket_connection_options& options) {
        const auto& compression = options.compression_;
        if (max_buffered_input_bytes_ == 0) {
            throw std::invalid_argument("WebSocket input buffer limit must be greater than zero");
        }
        if (role_ != websocket_connection_role::server && role_ != websocket_connection_role::client) {
            throw std::invalid_argument("invalid WebSocket connection role");
        }
        if (compression.server_max_window_bits_.value_or(15) < 8 || compression.server_max_window_bits_.value_or(15) > 15 || compression.client_max_window_bits_.value_or(15) < 8 || compression.client_max_window_bits_.value_or(15) > 15) {
            throw std::invalid_argument("invalid WebSocket compression window");
        }
        if (role_ == websocket_connection_role::client && mask_key_generator_ == nullptr) {
            throw std::invalid_argument("WebSocket client connection requires a mask key generator");
        }
        validate_websocket_deflater_config(options.deflater_);
        if (compression.enabled_) {
            const bool server = role_ == websocket_connection_role::server;
            const bool send_takeover = !(server ? compression.server_no_context_takeover_ : compression.client_no_context_takeover_);
            const bool receive_takeover = !(server ? compression.client_no_context_takeover_ : compression.server_no_context_takeover_);
            const int send_window = (server ? compression.server_max_window_bits_ : compression.client_max_window_bits_).value_or(15);
            const int receive_window = (server ? compression.client_max_window_bits_ : compression.server_max_window_bits_).value_or(15);
            deflate_.emplace(options.shared_deflate_, options.deflater_, send_takeover, receive_takeover, send_window, receive_window);
        }
    }

    [[nodiscard]] std::size_t pending_input_bytes() const noexcept {
        return input_->size() - pending_consume_;
    }

    void release_consumed_input() noexcept {
        // Keep the storage: the caller may append bytes that alias the released frame.
        input_->consume(std::exchange(pending_consume_, 0));
    }

    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept {
        switch (close_phase_) {
            case close_phase_type::open:
                return websocket_liveness_mode::open;
            case close_phase_type::local_close_queued:
            case close_phase_type::awaiting_peer_close:
                return websocket_liveness_mode::awaiting_peer_close;
            case close_phase_type::handshake_output_queued:
            case close_phase_type::awaiting_peer_eof:
                return websocket_liveness_mode::awaiting_peer_eof;
            case close_phase_type::final_output_queued:
            case close_phase_type::transport_end_ready:
            case close_phase_type::closed:
                return websocket_liveness_mode::inactive;
        }
        return websocket_liveness_mode::inactive;
    }

    void append_frame(websocket_opcode opcode, std::string_view payload_value, bool rsv1 = false, websocket_payload_storage storage = websocket_payload_storage::copy, websocket_fragment_position position = websocket_fragment_position::whole) {
        std::optional<std::string> owned_payload;
        if (detail::http_text_overlaps(payload_value, out_buffer_.view())) {
            // A plan may be its own append source. Snapshot it before prepare()
            // can move storage, and never borrow the temporary snapshot.
            payload_value = owned_payload.emplace(payload_value);
            storage = websocket_payload_storage::copy;
        }
        detail::websocket_frame_header_type header;
        const bool masked = role_ == websocket_connection_role::client;
        websocket_mask_key_type mask{};
        if (masked && !mask_key_generator_(mask_key_context_, mask)) {
            throw std::runtime_error("failed to generate WebSocket client mask key");
        }
        const bool borrow = storage == websocket_payload_storage::borrow && !masked;
        const bool continuation = position == websocket_fragment_position::middle || position == websocket_fragment_position::last;
        const bool final = position == websocket_fragment_position::whole || position == websocket_fragment_position::last;
        const auto header_size = detail::encode_websocket_frame_header(header, continuation ? std::uint8_t{0} : static_cast<std::uint8_t>(opcode), final, payload_value.size(), rsv1, masked);
        const auto mask_size = masked ? mask.size() : std::size_t{0};
        std::size_t additional_size = 0;
        for (const auto size : {borrowed_output_.size(), header_size, mask_size, borrow ? 0 : payload_value.size()}) {
            if (size > std::numeric_limits<std::size_t>::max() - additional_size) {
                throw std::length_error("WebSocket output frame size overflow");
            }
            additional_size += size;
        }
        // Prepare every byte before publishing anything. The appends below
        // then fit without allocation, so a failure cannot orphan a wire header.
        (void)out_buffer_.prepare(additional_size);
        // An earlier borrowed payload precedes this frame on the wire.
        out_buffer_.append(borrowed_output_);
        borrowed_output_ = {};
        out_buffer_.append(std::string_view(header.data(), header_size));
        if (masked) {
            out_buffer_.append(std::string_view(mask.data(), mask.size()));
            const auto payload_start = out_buffer_.size();
            out_buffer_.append(payload_value);
            detail::decode_masked_websocket_payload(out_buffer_.data() + payload_start, payload_value.size(), mask.data());
        } else if (borrow) {
            borrowed_output_ = payload_value;
        } else {
            out_buffer_.append(payload_value);
        }
    }

    void fail(std::uint16_t code) {
        if (close_phase_ == close_phase_type::open) {
            // Every protocol failure code is a valid Close code, so encoding succeeds.
            const auto payload_value = detail::encode_websocket_close_payload(websocket_close_code::from_value(code), {});
            append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
            close_phase_ = close_phase_type::final_output_queued;
            return;
        }
        if (close_phase_ == close_phase_type::local_close_queued) {
            close_phase_ = close_phase_type::final_output_queued;
        } else if (close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = !out_buffer_.empty() || !borrowed_output_.empty() ? close_phase_type::final_output_queued : close_phase_type::transport_end_ready;
        }
    }

    // Both Close frames have been exchanged once the queued output (which may
    // end with a Close echo) is flushed. A server then ends the transport; a
    // client waits for the server to do so.
    [[nodiscard]] close_phase_type handshake_complete_phase() const noexcept {
        const bool output_queued = !out_buffer_.empty() || !borrowed_output_.empty();
        if (role_ == websocket_connection_role::client) {
            return output_queued ? close_phase_type::handshake_output_queued : close_phase_type::awaiting_peer_eof;
        }
        return output_queued ? close_phase_type::final_output_queued : close_phase_type::transport_end_ready;
    }

    void receive_peer_close() noexcept {
        if (close_phase_ == close_phase_type::local_close_queued || close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = handshake_complete_phase();
        }
    }

    void discard_input() noexcept {
        pending_consume_ = 0;
        input_->consume(input_->size());
    }

    void record_termination(websocket_termination cause) noexcept {
        if (termination_ == websocket_termination::none) {
            termination_ = cause;
        }
    }

    // The one submission behind both submit_frame() payload forms.
    [[nodiscard]] websocket_frame_submit_status submit_frame(websocket_opcode opcode, std::string_view payload_value, websocket_frame_options options, websocket_payload_storage storage) {
        if (close_phase_ != close_phase_type::open) {
            return websocket_frame_submit_status::not_open;
        }
        if (opcode == websocket_opcode::ping || opcode == websocket_opcode::pong) {
            // Control frames are never fragmented (RFC 6455 section 5.5).
            if (!options.final_) {
                return websocket_frame_submit_status::invalid_opcode;
            }
            if (payload_value.size() > 125) {
                return websocket_frame_submit_status::control_frame_too_large;
            }
            append_frame(opcode, payload_value, false, storage);
            return websocket_frame_submit_status::accepted;
        }
        if (opcode != websocket_opcode::text && opcode != websocket_opcode::binary) {
            return websocket_frame_submit_status::invalid_opcode;
        }
        if (outbound_fragments_.has_value() && outbound_fragments_->opcode_ != opcode) {
            return websocket_frame_submit_status::message_in_progress;
        }
        const bool first = !outbound_fragments_.has_value();
        const bool final = options.final_;
        const std::size_t uncompressed_size = payload_value.size();
        const std::size_t sent = first ? 0 : outbound_fragments_->size_;
        if (message_limit_.addition_exceeds(sent, uncompressed_size)) {
            return websocket_frame_submit_status::message_too_large;
        }
        auto utf8 = first ? websocket_utf8_fragments{} : outbound_fragments_->utf8_;
        if (opcode == websocket_opcode::text && (!utf8.append(payload_value) || (final && !utf8.complete()))) {
            return websocket_frame_submit_status::invalid_text_payload;
        }
        const bool compression_attempted = first ? options.compress_ && deflate_.has_value() : outbound_fragments_->compressed_;
        bool compressed = compression_attempted;
        const auto position = first ? (final ? websocket_fragment_position::whole : websocket_fragment_position::first) : (final ? websocket_fragment_position::last : websocket_fragment_position::middle);
        try {
            if (compressed) {
                outbound_deflated_.clear();
                if (position == websocket_fragment_position::whole) {
                    // A whole message is sent uncompressed unless deflate shrinks it.
                    if (deflate_->compress(payload_value, outbound_deflated_) && outbound_deflated_.size() < payload_value.size()) {
                        payload_value = outbound_deflated_;
                    } else {
                        deflate_->discard_compression();
                        compressed = false;
                    }
                } else if (deflate_->compress_fragment(payload_value, first, final, outbound_deflated_)) {
                    payload_value = outbound_deflated_;
                } else if (first) {
                    // Nothing went out yet: send this message uncompressed.
                    deflate_->discard_compression();
                    compressed = false;
                } else {
                    throw std::runtime_error("failed to compress a WebSocket message fragment");
                }
            }
            // A compressed payload lives in outbound_deflated_, which the next
            // message reuses, so only the caller's own bytes can be borrowed.
            append_frame(opcode, payload_value, first && compressed, compressed ? websocket_payload_storage::copy : storage, position);
        } catch (...) {
            if (compression_attempted) {
                http_clear_string_retaining_small(outbound_deflated_, websocket_retained_buffer_bytes);
            }
            if (compressed) {
                // The peer never received a first frame's trial, so a later
                // message must not refer to its dictionary. Earlier fragments
                // already advanced the peer's inflater, so a later fragment
                // cannot be retried; nor can the first one once resetting its
                // trial fails.
                bool reusable = first;
                if (first) {
                    try {
                        deflate_->discard_compression();
                    } catch (...) {
                        reusable = false;
                    }
                }
                if (!reusable) {
                    close_phase_ = close_phase_type::transport_end_ready;
                    record_termination(websocket_termination::aborted);
                }
            }
            throw;
        }
        if (compression_attempted) {
            // The frame owns a copy now; keep only a bounded scratch.
            http_clear_string_retaining_small(outbound_deflated_, websocket_retained_buffer_bytes);
        }
        if (final) {
            outbound_fragments_.reset();
        } else if (first) {
            outbound_fragments_.emplace(websocket_outbound_fragments{opcode, compressed, uncompressed_size, utf8});
        } else {
            outbound_fragments_->size_ = sent + uncompressed_size;
            outbound_fragments_->utf8_ = utf8;
        }
        return websocket_frame_submit_status::accepted;
    }

    std::optional<http_io_buffer> owned_input_;
    http_io_buffer* input_;
    const std::size_t max_buffered_input_bytes_;
    protocol_byte_limit message_limit_;
    protocol_byte_limit receive_message_limit_;
    // Bytes of the last returned frame, consumed by the next next_event().
    std::size_t pending_consume_{0};
    std::size_t needed_input_bytes_{0};

    http_io_buffer out_buffer_;
    // An unmasked, uncompressed payload that follows out_buffer_ on the wire
    // without being copied into it. Only the newest frame can borrow; queuing
    // more output first appends it to out_buffer_.
    std::string_view borrowed_output_;

    detail::websocket_inbound_assembler assembler_;
    detail::websocket_receive_storage receive_storage_;

    std::optional<detail::websocket_deflate> deflate_;
    std::string inbound_inflated_;
    std::string outbound_deflated_;
    std::optional<websocket_outbound_fragments> outbound_fragments_;

    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};

    close_phase_type close_phase_{close_phase_type::open};
    websocket_termination termination_{websocket_termination::none};
    std::uint64_t bytes_received_{0};
    std::uint64_t bytes_sent_{0};
};

websocket_connection::websocket_connection(websocket_connection_options options) :
    impl_(std::make_unique<impl_type>(nullptr, options)) {}

websocket_connection::websocket_connection(http_io_buffer& input, websocket_connection_options options) :
    impl_(std::make_unique<impl_type>(&input, options)) {}

websocket_connection::~websocket_connection() = default;
websocket_connection::websocket_connection(websocket_connection&&) noexcept = default;
websocket_connection& websocket_connection::operator=(websocket_connection&&) noexcept = default;

websocket_feed_status websocket_connection::feed(std::string_view input) {
    const auto liveness = impl_->liveness_mode();
    if (liveness == websocket_liveness_mode::inactive) {
        return websocket_feed_status::inactive;
    }
    if (liveness == websocket_liveness_mode::awaiting_peer_eof) {
        // Bytes after the close handshake carry nothing; the client keeps
        // reading only to observe the server's EOF.
        return websocket_feed_status::accepted;
    }
    if (impl_->input_->size() > impl_->max_buffered_input_bytes_ || input.size() > impl_->max_buffered_input_bytes_ - impl_->input_->size()) {
        if (impl_->pending_input_bytes() > impl_->max_buffered_input_bytes_ || input.size() > impl_->max_buffered_input_bytes_ - impl_->pending_input_bytes()) {
            return websocket_feed_status::backpressured;
        }
        // The bound covers bytes awaiting next_event(), not the frame already
        // returned; otherwise a frame that fits could never arrive. append()
        // copies an `input` aliasing the buffer before it moves any byte.
        impl_->release_consumed_input();
    }
    impl_->input_->append(input);
    return websocket_feed_status::accepted;
}

std::optional<websocket_event> websocket_connection::next_event() & {
    using close_phase_type = impl_type::close_phase_type;
    try {
        impl_->assembler_.release_completed(impl_->receive_storage_);
        // The last delivered message expires now. Its size is peer-controlled up
        // to the message limit, so only a bounded buffer stays for the next one.
        http_clear_string_retaining_small(impl_->inbound_inflated_, websocket_retained_buffer_bytes);
        impl_->receive_storage_.settle(detail::websocket_receive_storage::slot::inflate, impl_->inbound_inflated_);
        impl_->needed_input_bytes_ = 0;
        if (impl_->close_phase_ == close_phase_type::final_output_queued || impl_->close_phase_ == close_phase_type::transport_end_ready || impl_->close_phase_ == close_phase_type::closed) {
            return websocket_event::make_transport_end();
        }
        if (impl_->close_phase_ == close_phase_type::handshake_output_queued) {
            // Queued output may still borrow input bytes: drop them once flushed.
            return std::nullopt;
        }
        if (impl_->close_phase_ == close_phase_type::awaiting_peer_eof) {
            impl_->discard_input();
            return std::nullopt;
        }

        const auto protocol_failure_event = [this](detail::websocket_protocol_failure failure) {
            const auto close_code = detail::websocket_protocol_failure_close_code(failure);
            impl_->fail(close_code);
            impl_->record_termination(websocket_failure_termination(failure));
            return websocket_event::protocol_error(close_code);
        };

        for (;;) {
            const auto read = detail::websocket_try_read_frame(*impl_->input_, impl_->pending_consume_, impl_->receive_message_limit_, impl_->deflate_.has_value(), impl_->role_ == websocket_connection_role::server);
            if (const auto* need_input = read.need_input()) {
                impl_->needed_input_bytes_ = need_input->missing_bytes();
                return std::nullopt;
            }
            if (const auto* failure = read.failure()) {
                return protocol_failure_event(failure->error());
            }

            impl_->bytes_received_ = saturating_add<std::uint64_t>(impl_->bytes_received_, impl_->pending_consume_);
            const auto& frame = *read.frame();
            const auto inbound = impl_->assembler_.accept(frame, impl_->receive_message_limit_, impl_->receive_storage_);
            if (const auto* failure = inbound.failure()) {
                return protocol_failure_event(failure->error());
            }
            if (inbound.continue_reading() != nullptr) {
                continue;
            }
            if (const auto* control = inbound.control_frame()) {
                const auto payload_value = control->payload();
                if (control->opcode() == websocket_opcode::ping) {
                    // RFC 6455 requires Pong until a peer Close has arrived. A Pong
                    // is a control frame, so it remains legal while a locally
                    // initiated Close waits for its peer response.
                    impl_->append_frame(websocket_opcode::pong, payload_value);
                    return websocket_event::ping(payload_value);
                }
                if (control->opcode() == websocket_opcode::pong) {
                    return websocket_event::pong(payload_value);
                }
                // Close: the frame reader admits no other control opcode.
                std::uint16_t code = 1005;
                if (payload_value.size() >= 2) {
                    code = detail::read_websocket_uint16(payload_value.data());
                }
                const auto reason = payload_value.size() > 2 ? payload_value.substr(2) : std::string_view{};
                if (impl_->close_phase_ == close_phase_type::open) {
                    impl_->append_frame(websocket_opcode::close, payload_value);
                    impl_->close_phase_ = impl_->handshake_complete_phase();
                    impl_->record_termination(websocket_termination::peer_close);
                } else {
                    impl_->receive_peer_close();
                    impl_->record_termination(websocket_termination::local_close);
                }
                return websocket_event::close(code, reason);
            }

            const auto& inbound_message = *inbound.message();
            const auto& message = inbound_message.message();
            if (inbound_message.content_encoding() == detail::websocket_inbound_content_encoding::identity) {
                if (impl_->close_phase_ != close_phase_type::open) {
                    impl_->assembler_.release_completed(impl_->receive_storage_);
                    continue;
                }
                return websocket_event::message(message.opcode(), message.payload());
            }

            // decompress() only appends, so the buffer must be emptied per MESSAGE, not
            // per next_event(): one next_event() drains several frames, and a message suppressed
            // during the closing handshake (below) returns via `continue` with its bytes
            // still here. Inheriting them would make the next message's UTF-8 check read
            // the concatenation, and would charge its decompression-bomb limit for both.
            impl_->inbound_inflated_.clear();
            // Only a negotiated deflate admits RSV1, so a compressed message implies deflate_.
            const auto inflate_result = impl_->deflate_->decompress(message.payload(), impl_->inbound_inflated_, impl_->receive_message_limit_, impl_->receive_storage_);
            if (inflate_result == websocket_inflate_result::too_large) {
                return protocol_failure_event(detail::websocket_protocol_failure::message_too_large);
            }
            if (inflate_result != websocket_inflate_result::ok) {
                return protocol_failure_event(detail::websocket_protocol_failure::protocol_error);
            }
            const std::string_view view = impl_->inbound_inflated_;
            if (message.opcode() == websocket_opcode::text && !is_valid_utf8(view)) {
                return protocol_failure_event(detail::websocket_protocol_failure::invalid_payload_data);
            }
            if (impl_->close_phase_ != close_phase_type::open) {
                impl_->assembler_.release_completed(impl_->receive_storage_);
                http_clear_string_retaining_small(impl_->inbound_inflated_, websocket_retained_buffer_bytes);
                impl_->receive_storage_.settle(detail::websocket_receive_storage::slot::inflate, impl_->inbound_inflated_);
                continue;
            }
            return websocket_event::message(message.opcode(), view);
        }
    } catch (...) {
        // Frame reading unmasks in place and advances the input cursor. If any
        // later assembler/deflate/output operation fails, retrying that frame is
        // no longer well-defined. The transport has not ended yet, so the plan
        // and abort() must still tell the driver to end it, as after EOF.
        if (impl_->close_phase_ != close_phase_type::closed) {
            impl_->close_phase_ = close_phase_type::transport_end_ready;
        }
        impl_->record_termination(websocket_termination::aborted);
        throw;
    }
}

std::size_t websocket_connection::needed_input_bytes() const noexcept {
    return impl_->needed_input_bytes_;
}

bool websocket_connection::message_pending() const noexcept {
    return impl_->assembler_.fragmented() || impl_->pending_input_bytes() != 0;
}

websocket_output_plan websocket_connection::output_plan() const& noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (impl_->close_phase_ == close_phase_type::transport_end_ready || impl_->close_phase_ == close_phase_type::closed) {
        return websocket_output_plan({}, {}, impl_->close_phase_ == close_phase_type::transport_end_ready ? websocket_transport_disposition::end_transport : websocket_transport_disposition::keep_open);
    }
    const auto bytes_value = impl_->out_buffer_.view();
    const auto disposition = impl_->close_phase_ == close_phase_type::final_output_queued ? websocket_transport_disposition::end_transport : websocket_transport_disposition::keep_open;
    return websocket_output_plan(bytes_value, impl_->borrowed_output_, disposition);
}

websocket_output_consume_status websocket_connection::consume_output(std::size_t bytes_value) noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort makes unsent bytes unreachable without clearing their storage:
    // an async write may still borrow it. A zero-byte transport-end write has
    // no output left to consume, regardless of those discarded backing bytes.
    if (bytes_value == 0 && (impl_->close_phase_ == close_phase_type::transport_end_ready || impl_->close_phase_ == close_phase_type::closed)) {
        return websocket_output_consume_status::drained;
    }
    const auto owned = impl_->out_buffer_.size();
    if (bytes_value > owned && bytes_value - owned > impl_->borrowed_output_.size()) {
        return websocket_output_consume_status::out_of_range;
    }
    impl_->bytes_sent_ = saturating_add<std::uint64_t>(impl_->bytes_sent_, bytes_value);
    if (bytes_value < owned) {
        impl_->out_buffer_.consume(bytes_value);
        return websocket_output_consume_status::pending;
    }
    // The transport has written every owned byte, so a large frame's storage
    // can go back to the resource instead of staying with the connection.
    impl_->out_buffer_.clear_retaining_small(websocket_retained_buffer_bytes);
    if (bytes_value - owned < impl_->borrowed_output_.size()) {
        impl_->borrowed_output_.remove_prefix(bytes_value - owned);
        return websocket_output_consume_status::pending;
    }
    impl_->borrowed_output_ = {};
    if (impl_->close_phase_ == close_phase_type::local_close_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_close;
    } else if (impl_->close_phase_ == close_phase_type::final_output_queued) {
        impl_->close_phase_ = close_phase_type::transport_end_ready;
    } else if (impl_->close_phase_ == close_phase_type::handshake_output_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_eof;
    }
    return websocket_output_consume_status::drained;
}

void websocket_connection::commit_transport_end() noexcept {
    if (impl_->close_phase_ == impl_type::close_phase_type::transport_end_ready) {
        impl_->close_phase_ = impl_type::close_phase_type::closed;
    }
}

void websocket_connection::notify_transport_eof() noexcept {
    if (impl_->close_phase_ != impl_type::close_phase_type::closed) {
        impl_->close_phase_ = impl_type::close_phase_type::transport_end_ready;
    }
    impl_->record_termination(websocket_termination::eof);
}

websocket_abort_disposition websocket_connection::abort(websocket_termination cause) noexcept {
    impl_->record_termination(cause == websocket_termination::none ? websocket_termination::aborted : cause);
    if (impl_->close_phase_ == impl_type::close_phase_type::closed) {
        return websocket_abort_disposition::no_transport_action;
    }
    impl_->close_phase_ = impl_type::close_phase_type::closed;
    return websocket_abort_disposition::abort_transport;
}

websocket_liveness_mode websocket_connection::liveness_mode() const noexcept {
    return impl_->liveness_mode();
}

websocket_termination websocket_connection::termination() const noexcept {
    return impl_->termination_;
}

std::uint64_t websocket_connection::bytes_received() const noexcept {
    return impl_->bytes_received_;
}

std::uint64_t websocket_connection::bytes_sent() const noexcept {
    return impl_->bytes_sent_;
}

websocket_frame_submit_status websocket_connection::submit_frame(websocket_opcode opcode, std::string_view payload_value, websocket_frame_options options) {
    return impl_->submit_frame(opcode, payload_value, options, websocket_payload_storage::copy);
}

websocket_frame_submit_status websocket_connection::submit_frame(websocket_opcode opcode, websocket_borrowed_payload payload_value, websocket_frame_options options) {
    return impl_->submit_frame(opcode, payload_value.bytes(), options, websocket_payload_storage::borrow);
}

websocket_close_submit_status websocket_connection::submit_close(websocket_close_code code, std::string_view reason) {
    using close_phase_type = impl_type::close_phase_type;
    if (impl_->close_phase_ == close_phase_type::closed) {
        return websocket_close_submit_status::closed;
    }
    if (impl_->close_phase_ != close_phase_type::open) {
        return websocket_close_submit_status::already_closing;
    }
    // RFC 6455 §7.4.1 reserves 1010 for a client reporting extensions that
    // were absent from the server handshake. This core emits server frames;
    // a server must reject that mismatch during the opening handshake rather
    // than initiate a Close frame with the client-only status code.
    if (impl_->role_ == websocket_connection_role::server && code.value() == 1010) {
        return websocket_close_submit_status::invalid_code;
    }
    const auto payload_value = detail::encode_websocket_close_payload(code, reason);
    if (const auto* failure = payload_value.failure()) {
        switch (failure->error()) {
            case detail::websocket_close_payload_encode_error::invalid_reason:
                return websocket_close_submit_status::invalid_reason;
            case detail::websocket_close_payload_encode_error::reason_too_large:
                return websocket_close_submit_status::reason_too_large;
        }
    }
    impl_->append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
    impl_->close_phase_ = close_phase_type::local_close_queued;
    return websocket_close_submit_status::accepted;
}

}  // namespace ruvia
