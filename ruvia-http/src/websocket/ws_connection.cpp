#include "websocket/ws_connection.h"

#include <cstdint>
#include <stdexcept>

#include "websocket/http_websocket_close_payload.h"
#include "websocket/http_websocket_frame_reader.h"

namespace ruvia::detail {

ws_connection::ws_connection(std::pmr::string& input, protocol_byte_limit message_limit,
    websocket_compression compression, websocket_connection_role role, websocket_mask_key_generator_type mask_key_generator,
    void* mask_key_context, int compression_level)
    : input_(&input),
      message_limit_(message_limit),
      out_buffer_(input.get_allocator().resource()),
      assembler_(input.get_allocator().resource()),
      inbound_inflated_(input.get_allocator().resource()),
      outbound_deflated_(input.get_allocator().resource()),
      role_(role),
      mask_key_generator_(mask_key_generator),
      mask_key_context_(mask_key_context) {
    if (role_ != websocket_connection_role::server && role_ != websocket_connection_role::client) {
        throw std::invalid_argument("invalid WebSocket connection role");
    }
    if (compression.server_max_window_bits_.value_or(15) < 8 || compression.server_max_window_bits_.value_or(15) > 15 ||
        compression.client_max_window_bits_.value_or(15) < 8 || compression.client_max_window_bits_.value_or(15) > 15) {
        throw std::invalid_argument("invalid WebSocket compression window");
    }
    if (role_ == websocket_connection_role::client && mask_key_generator_ == nullptr) {
        throw std::invalid_argument("WebSocket client connection requires a mask key generator");
    }
    if (compression_level < 0 || compression_level > 9) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    if (websocket_deflate_negotiated(compression)) {
        const bool server = role_ == websocket_connection_role::server;
        const bool send_takeover = !(server ? compression.server_no_context_takeover_ : compression.client_no_context_takeover_);
        const bool receive_takeover = !(server ? compression.client_no_context_takeover_ : compression.server_no_context_takeover_);
        const int send_window = (server ? compression.server_max_window_bits_ : compression.client_max_window_bits_).value_or(15);
        const int receive_window = (server ? compression.client_max_window_bits_ : compression.server_max_window_bits_).value_or(15);
        deflate_.emplace(compression_level, send_takeover, receive_takeover, send_window, receive_window);
    }
}

websocket_output_plan ws_connection::output_plan() const& noexcept {
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (close_phase_ == close_phase_type::transport_end_ready || close_phase_ == close_phase_type::closed) {
        return websocket_output_plan({}, close_phase_ == close_phase_type::transport_end_ready
                                             ? websocket_transport_disposition::end_transport
                                             : websocket_transport_disposition::keep_open);
    }
    const auto bytes_value =
        std::string_view(out_buffer_.data() + out_offset_, out_buffer_.size() - out_offset_);
    const auto disposition = close_phase_ == close_phase_type::final_output_queued
                                 ? websocket_transport_disposition::end_transport
                                 : websocket_transport_disposition::keep_open;
    return websocket_output_plan(bytes_value, disposition);
}

websocket_output_consume_status ws_connection::consume_output(std::size_t n) noexcept {
    // EOF/abort makes unsent bytes unreachable without clearing their storage:
    // an async write may still borrow it. A zero-byte transport-end write has
    // no output left to consume, regardless of those discarded backing bytes.
    if (n == 0 &&
        (close_phase_ == close_phase_type::transport_end_ready || close_phase_ == close_phase_type::closed)) {
        return websocket_output_consume_status::drained;
    }
    const auto remaining = out_buffer_.size() - out_offset_;
    if (n > remaining) {
        return websocket_output_consume_status::out_of_range;
    }
    if (n < remaining) {
        out_offset_ += n;
        return websocket_output_consume_status::pending;
    }

    out_buffer_.clear();
    out_offset_ = 0;
    if (close_phase_ == close_phase_type::local_close_queued) {
        close_phase_ = close_phase_type::awaiting_peer_close;
    } else if (close_phase_ == close_phase_type::final_output_queued) {
        close_phase_ = close_phase_type::transport_end_ready;
    }
    return websocket_output_consume_status::drained;
}

void ws_connection::commit_transport_end() noexcept {
    if (close_phase_ == close_phase_type::transport_end_ready) {
        close_phase_ = close_phase_type::closed;
    }
}

void ws_connection::notify_transport_eof() noexcept {
    if (close_phase_ == close_phase_type::closed) {
        return;
    }
    close_phase_ = close_phase_type::transport_end_ready;
}

websocket_abort_disposition ws_connection::abort() noexcept {
    if (close_phase_ == close_phase_type::closed) {
        return websocket_abort_disposition::no_transport_action;
    }
    close_phase_ = close_phase_type::closed;
    return websocket_abort_disposition::abort_transport;
}

websocket_liveness_mode ws_connection::liveness_mode() const noexcept {
    switch (close_phase_) {
        case close_phase_type::open:
            return websocket_liveness_mode::open;
        case close_phase_type::local_close_queued:
        case close_phase_type::awaiting_peer_close:
            return websocket_liveness_mode::awaiting_peer_close;
        case close_phase_type::final_output_queued:
        case close_phase_type::transport_end_ready:
        case close_phase_type::closed:
            return websocket_liveness_mode::inactive;
    }
    return websocket_liveness_mode::inactive;
}

void ws_connection::append_frame(websocket_opcode opcode, std::string_view payload_value, bool rsv1) {
    std::pmr::string owned_payload(out_buffer_.get_allocator().resource());
    if (!payload_value.empty() && !out_buffer_.empty()) {
        const auto payload_address = reinterpret_cast<std::uintptr_t>(payload_value.data());
        const auto buffer_address = reinterpret_cast<std::uintptr_t>(out_buffer_.data());
        if (payload_address >= buffer_address && payload_address - buffer_address < out_buffer_.size()) {
            // output_plan() exposes a borrowed view. Copy only for this aliasing
            // case so reserve() below cannot invalidate its own append source.
            owned_payload.assign(payload_value);
            payload_value = owned_payload;
        }
    }
    websocket_frame_header_type header;
    const bool masked = role_ == websocket_connection_role::client;
    websocket_mask_key_type mask{};
    if (masked && !mask_key_generator_(mask_key_context_, mask)) {
        throw std::runtime_error("failed to generate WebSocket client mask key");
    }
    const auto header_size =
        encode_websocket_frame_header(header, opcode, payload_value.size(), rsv1, masked);
    const auto mask_size = masked ? mask.size() : std::size_t{0};
    if (header_size > out_buffer_.max_size() - out_buffer_.size() ||
        mask_size > out_buffer_.max_size() - out_buffer_.size() - header_size ||
        payload_value.size() > out_buffer_.max_size() - out_buffer_.size() - header_size - mask_size) {
        throw std::length_error("WebSocket output frame size overflow");
    }
    // One reserve is the transaction boundary. Once it succeeds, neither append
    // can allocate, so an exception can never publish an orphan wire header.
    out_buffer_.reserve(out_buffer_.size() + header_size + mask_size + payload_value.size());
    out_buffer_.append(header.data(), header_size);
    if (masked) {
        out_buffer_.append(mask.data(), mask.size());
        const auto payload_start = out_buffer_.size();
        out_buffer_.append(payload_value.data(), payload_value.size());
        decode_masked_websocket_payload(out_buffer_.data() + payload_start, payload_value.size(), mask.data());
    } else {
        out_buffer_.append(payload_value.data(), payload_value.size());
    }
}

void ws_connection::fail(std::uint16_t code, std::string_view reason) {
    if (close_phase_ == close_phase_type::open) {
        const auto payload_value = encode_websocket_close_payload(code, reason);
        const auto* encoded = payload_value.encoded();
        if (encoded == nullptr) {
            return;
        }
        append_frame(websocket_opcode::close, encoded->bytes());
        close_phase_ = close_phase_type::final_output_queued;
        return;
    }
    if (close_phase_ == close_phase_type::local_close_queued) {
        close_phase_ = close_phase_type::final_output_queued;
    } else if (close_phase_ == close_phase_type::awaiting_peer_close) {
        close_phase_ = out_offset_ < out_buffer_.size() ? close_phase_type::final_output_queued
                                                        : close_phase_type::transport_end_ready;
    }
}

void ws_connection::receive_peer_close() noexcept {
    if (close_phase_ == close_phase_type::local_close_queued) {
        close_phase_ = close_phase_type::final_output_queued;
    } else if (close_phase_ == close_phase_type::awaiting_peer_close) {
        close_phase_ = out_offset_ < out_buffer_.size() ? close_phase_type::final_output_queued
                                                        : close_phase_type::transport_end_ready;
    }
}

websocket_frame_submit_status ws_connection::submit_frame(websocket_opcode opcode, std::string_view payload_value, bool compress) {
    if (close_phase_ != close_phase_type::open) {
        return websocket_frame_submit_status::not_open;
    }

    const bool data_frame = opcode == websocket_opcode::text || opcode == websocket_opcode::binary;
    const bool control_frame = opcode == websocket_opcode::ping || opcode == websocket_opcode::pong;
    if (!data_frame && !control_frame) {
        return websocket_frame_submit_status::invalid_opcode;
    }
    if (data_frame && websocket_message_exceeds_limit(payload_value.size(), message_limit_)) {
        return websocket_frame_submit_status::message_too_large;
    }
    if (opcode == websocket_opcode::text && !is_valid_utf8(payload_value)) {
        return websocket_frame_submit_status::invalid_text_payload;
    }
    if (control_frame && payload_value.size() > 125) {
        return websocket_frame_submit_status::control_frame_too_large;
    }
    bool rsv1 = false;
    if (data_frame && compress && deflate_.has_value()) {
        outbound_deflated_.clear();
        if (deflate_->compress(payload_value, outbound_deflated_) &&
            outbound_deflated_.size() < payload_value.size()) {
            payload_value = outbound_deflated_;
            rsv1 = true;
        } else {
            deflate_->discard_compression();
        }
    }
    append_frame(opcode, payload_value, rsv1);
    return websocket_frame_submit_status::accepted;
}

websocket_close_submit_status ws_connection::submit_close(std::uint16_t code, std::string_view reason) {
    if (close_phase_ == close_phase_type::closed) {
        return websocket_close_submit_status::closed;
    }
    if (close_phase_ != close_phase_type::open) {
        return websocket_close_submit_status::already_closing;
    }
    // RFC 6455 §7.4.1 reserves 1010 for a client reporting extensions that
    // were absent from the server handshake. This core emits server frames;
    // a server must reject that mismatch during the opening handshake rather
    // than initiate a Close frame with the client-only status code.
    if (role_ == websocket_connection_role::server && code == 1010) {
        return websocket_close_submit_status::invalid_code;
    }
    const auto payload_value = encode_websocket_close_payload(code, reason);
    if (const auto* failure = payload_value.failure()) {
        switch (failure->error()) {
            case websocket_close_payload_encode_error::invalid_code:
                return websocket_close_submit_status::invalid_code;
            case websocket_close_payload_encode_error::invalid_reason:
                return websocket_close_submit_status::invalid_reason;
            case websocket_close_payload_encode_error::reason_too_large:
                return websocket_close_submit_status::reason_too_large;
        }
    }
    append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
    close_phase_ = close_phase_type::local_close_queued;
    return websocket_close_submit_status::accepted;
}

std::optional<websocket_event> ws_connection::poll() & {
    try {
        return poll_impl();
    } catch (...) {
        // Frame reading unmasks in place and advances the input cursor. If any
        // later assembler/deflate/output operation fails, retrying that frame is
        // no longer well-defined; make the terminal transport decision explicit.
        close_phase_ = close_phase_type::closed;
        throw;
    }
}

std::optional<websocket_event> ws_connection::poll_impl() & {
    assembler_.release_completed();
    std::pmr::string(inbound_inflated_.get_allocator()).swap(inbound_inflated_);
    if (close_phase_ == close_phase_type::final_output_queued ||
        close_phase_ == close_phase_type::transport_end_ready || close_phase_ == close_phase_type::closed) {
        return websocket_event::make_transport_end();
    }

    const auto protocol_failure_event = [this](websocket_protocol_failure failure) {
        const auto close_code = websocket_protocol_failure_close_code(failure);
        fail(close_code);
        return websocket_event::protocol_error(close_code);
    };

    for (;;) {
        const auto read = websocket_try_read_frame(*input_, input_offset_, pending_compact_until_,
            message_limit_, deflate_.has_value(), role_ == websocket_connection_role::server);
        if (read.need_input() != nullptr) {
            return std::nullopt;
        }
        if (const auto* failure = read.failure()) {
            return protocol_failure_event(failure->error());
        }

        const auto& frame = *read.frame();
        const auto inbound = assembler_.accept(frame, message_limit_);
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
                append_frame(websocket_opcode::pong, payload_value);
                return websocket_event::ping(payload_value);
            }
            if (control->opcode() == websocket_opcode::pong) {
                return websocket_event::pong(payload_value);
            }
            if (control->opcode() == websocket_opcode::close) {
                std::uint16_t code = 1005;
                if (payload_value.size() >= 2) {
                    code = read_websocket_uint16(payload_value.data());
                }
                const auto reason = payload_value.size() > 2 ? payload_value.substr(2) : std::string_view{};
                if (close_phase_ == close_phase_type::open) {
                    append_frame(websocket_opcode::close, payload_value);
                    close_phase_ = close_phase_type::final_output_queued;
                } else {
                    receive_peer_close();
                }
                return websocket_event::close(code, reason);
            }
            return protocol_failure_event(websocket_protocol_failure::protocol_error);
        }

        const auto& inbound_message = *inbound.message();
        const auto& message = inbound_message.message();
        if (inbound_message.content_encoding() == websocket_inbound_content_encoding::identity) {
            if (close_phase_ != close_phase_type::open) {
                continue;
            }
            return websocket_event::message(message.opcode(), message.payload());
        }

        // decompress() only appends, so the buffer must be emptied per MESSAGE, not
        // per poll(): one poll() drains several frames, and a message suppressed
        // during the closing handshake (below) returns via `continue` with its bytes
        // still here. Inheriting them would make the next message's UTF-8 check read
        // the concatenation, and would charge its decompression-bomb limit for both.
        inbound_inflated_.clear();
        const auto inflate_result = deflate_.has_value() ? deflate_->decompress(message.payload(),
                                                               inbound_inflated_, message_limit_)
                                                         : websocket_inflate_result::error;
        if (inflate_result == websocket_inflate_result::too_large) {
            return protocol_failure_event(websocket_protocol_failure::message_too_large);
        }
        if (inflate_result != websocket_inflate_result::ok) {
            return protocol_failure_event(websocket_protocol_failure::protocol_error);
        }
        const std::string_view view = inbound_inflated_;
        if (message.opcode() == websocket_opcode::text && !is_valid_utf8(view)) {
            return protocol_failure_event(websocket_protocol_failure::invalid_payload_data);
        }
        if (close_phase_ != close_phase_type::open) {
            continue;
        }
        return websocket_event::message(message.opcode(), view);
    }
}

}  // namespace ruvia::detail
