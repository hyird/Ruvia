#include "ruvia/http/websocket_connection.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_pmr_string.h"

#include "util/http_text_overlap.h"
#include "websocket/http_websocket_close_payload.h"
#include "websocket/http_websocket_frame_reader.h"
#include "websocket/http_websocket_inbound_assembler.h"
#include "websocket/http_websocket_permessage_deflate.h"

namespace ruvia {

namespace {

enum class ws_payload_storage : std::uint8_t {
    copy,
    borrow,
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
        transport_end_ready,
        closed,
    };

    impl_type(std::pmr::memory_resource* memory, websocket_connection_options options)
        : resource_(memory),
          owned_input_(std::in_place, std::pmr::polymorphic_allocator<char>(memory)),
          input_(&*owned_input_),
          max_buffered_input_bytes_(options.max_buffered_input_bytes_),
          message_limit_(options.message_limit_),
          out_buffer_(memory),
          assembler_(memory),
          inbound_inflated_(memory),
          outbound_deflated_(memory),
          role_(options.role_),
          mask_key_generator_(options.mask_key_generator_),
          mask_key_context_(options.mask_key_context_) {
        initialize_codecs(options);
    }

    impl_type(std::pmr::memory_resource* memory, http_io_buffer& input,
        websocket_connection_options options)
        : resource_(memory),
          input_(&input),
          max_buffered_input_bytes_(options.max_buffered_input_bytes_),
          message_limit_(options.message_limit_),
          out_buffer_(input.get_allocator().resource()),
          assembler_(input.get_allocator().resource()),
          inbound_inflated_(input.get_allocator().resource()),
          outbound_deflated_(input.get_allocator().resource()),
          role_(options.role_),
          mask_key_generator_(options.mask_key_generator_),
          mask_key_context_(options.mask_key_context_) {
        initialize_codecs(options);
    }

    void initialize_codecs(const websocket_connection_options& options) {
        const auto& compression = options.compression_;
        if (role_ != websocket_connection_role::server && role_ != websocket_connection_role::client) {
            throw std::invalid_argument("invalid WebSocket connection role");
        }
        if (compression.server_max_window_bits_.value_or(15) < 8 ||
            compression.server_max_window_bits_.value_or(15) > 15 ||
            compression.client_max_window_bits_.value_or(15) < 8 ||
            compression.client_max_window_bits_.value_or(15) > 15) {
            throw std::invalid_argument("invalid WebSocket compression window");
        }
        if (role_ == websocket_connection_role::client && mask_key_generator_ == nullptr) {
            throw std::invalid_argument("WebSocket client connection requires a mask key generator");
        }
        if (options.compression_level_ < 0 || options.compression_level_ > 9) {
            throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
        }
        if (options.mem_level_ < 1 || options.mem_level_ > 9) {
            throw std::invalid_argument("WebSocket deflate memory level must be between 1 and 9");
        }
        if (detail::websocket_deflate_negotiated(compression)) {
            const bool server = role_ == websocket_connection_role::server;
            const bool send_takeover =
                !(server ? compression.server_no_context_takeover_ : compression.client_no_context_takeover_);
            const bool receive_takeover =
                !(server ? compression.client_no_context_takeover_ : compression.server_no_context_takeover_);
            const int send_window =
                (server ? compression.server_max_window_bits_ : compression.client_max_window_bits_).value_or(15);
            const int receive_window =
                (server ? compression.client_max_window_bits_ : compression.server_max_window_bits_).value_or(15);
            deflate_.emplace(input_->get_allocator().resource(), options.shared_deflate_,
                options.compression_level_, options.mem_level_, send_takeover, receive_takeover,
                send_window, receive_window);
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
            case close_phase_type::final_output_queued:
            case close_phase_type::transport_end_ready:
            case close_phase_type::closed:
                return websocket_liveness_mode::inactive;
        }
        return websocket_liveness_mode::inactive;
    }

    void append_frame(websocket_opcode opcode, std::string_view payload_value, bool rsv1 = false,
        ws_payload_storage storage = ws_payload_storage::copy) {
        std::pmr::string owned_payload(out_buffer_.get_allocator().resource());
        if (detail::http_text_overlaps(payload_value, {out_buffer_.data(), out_buffer_.size() + 1})) {
            // output_plan() exposes a borrowed view. Copy only for this aliasing
            // case so reserve() below cannot invalidate its own append source; the
            // copy dies here, so it cannot be borrowed either.
            owned_payload.assign(payload_value);
            payload_value = owned_payload;
            storage = ws_payload_storage::copy;
        }
        detail::websocket_frame_header_type header;
        const bool masked = role_ == websocket_connection_role::client;
        websocket_mask_key_type mask{};
        if (masked && !mask_key_generator_(mask_key_context_, mask)) {
            throw std::runtime_error("failed to generate WebSocket client mask key");
        }
        const bool borrow = storage == ws_payload_storage::borrow && !masked;
        const auto header_size =
            detail::encode_websocket_frame_header(header, opcode, payload_value.size(), rsv1, masked);
        const auto mask_size = masked ? mask.size() : std::size_t{0};
        auto total_size = out_buffer_.size();
        for (const auto size : {borrowed_output_.size(), header_size, mask_size, borrow ? 0 : payload_value.size()}) {
            if (size > out_buffer_.max_size() - total_size) {
                throw std::length_error("WebSocket output frame size overflow");
            }
            total_size += size;
        }
        // One reserve is the transaction boundary. Once it succeeds, no append
        // can allocate, so an exception can never publish an orphan wire header.
        out_buffer_.reserve(total_size);
        // An earlier borrowed payload precedes this frame on the wire.
        out_buffer_.append(borrowed_output_);
        borrowed_output_ = {};
        out_buffer_.append(header.data(), header_size);
        if (masked) {
            out_buffer_.append(mask.data(), mask.size());
            const auto payload_start = out_buffer_.size();
            out_buffer_.append(payload_value.data(), payload_value.size());
            detail::decode_masked_websocket_payload(
                out_buffer_.data() + payload_start, payload_value.size(), mask.data());
        } else if (borrow) {
            borrowed_output_ = payload_value;
        } else {
            out_buffer_.append(payload_value.data(), payload_value.size());
        }
    }

    void fail(std::uint16_t code) {
        if (close_phase_ == close_phase_type::open) {
            // Every protocol failure code is a valid Close code, so encoding succeeds.
            const auto payload_value = detail::encode_websocket_close_payload(code, {});
            append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
            close_phase_ = close_phase_type::final_output_queued;
            return;
        }
        if (close_phase_ == close_phase_type::local_close_queued) {
            close_phase_ = close_phase_type::final_output_queued;
        } else if (close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = out_offset_ < out_buffer_.size() || !borrowed_output_.empty()
                               ? close_phase_type::final_output_queued
                               : close_phase_type::transport_end_ready;
        }
    }

    void receive_peer_close() noexcept {
        if (close_phase_ == close_phase_type::local_close_queued) {
            close_phase_ = close_phase_type::final_output_queued;
        } else if (close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = out_offset_ < out_buffer_.size() || !borrowed_output_.empty()
                               ? close_phase_type::final_output_queued
                               : close_phase_type::transport_end_ready;
        }
    }

    [[nodiscard]] websocket_frame_submit_status submit_frame(
        websocket_opcode opcode, std::string_view payload_value, bool compress, ws_payload_storage storage) {
        if (close_phase_ != close_phase_type::open) {
            return websocket_frame_submit_status::not_open;
        }

        const bool data_frame = opcode == websocket_opcode::text || opcode == websocket_opcode::binary;
        const bool control_frame = opcode == websocket_opcode::ping || opcode == websocket_opcode::pong;
        if (!data_frame && !control_frame) {
            return websocket_frame_submit_status::invalid_opcode;
        }
        if (data_frame && detail::websocket_message_exceeds_limit(payload_value.size(), message_limit_)) {
            return websocket_frame_submit_status::message_too_large;
        }
        if (opcode == websocket_opcode::text && !detail::is_valid_utf8(payload_value)) {
            return websocket_frame_submit_status::invalid_text_payload;
        }
        if (control_frame && payload_value.size() > 125) {
            return websocket_frame_submit_status::control_frame_too_large;
        }
        const bool compression_attempted = data_frame && compress && deflate_.has_value();
        try {
            bool rsv1 = false;
            if (compression_attempted) {
                outbound_deflated_.clear();
                if (deflate_->compress(payload_value, outbound_deflated_) &&
                    outbound_deflated_.size() < payload_value.size()) {
                    payload_value = outbound_deflated_;
                    rsv1 = true;
                } else {
                    deflate_->discard_compression();
                }
            }
            // A compressed payload lives in outbound_deflated_, which the next
            // message reuses, so only the caller's own bytes can be borrowed.
            append_frame(opcode, payload_value, rsv1, rsv1 ? ws_payload_storage::copy : storage);
        } catch (...) {
            if (compression_attempted) {
                http_clear_pmr_string_retaining_small(
                    outbound_deflated_, websocket_retained_buffer_bytes);
                // The peer never received this trial. Do not let a later message
                // refer to its dictionary, including after output allocation or
                // mask generation fails. A reset failure makes reuse unsafe.
                try {
                    deflate_->discard_compression();
                } catch (...) {
                    close_phase_ = close_phase_type::transport_end_ready;
                    throw;
                }
            }
            throw;
        }
        if (compression_attempted) {
            // The frame owns a copy now; keep only a bounded scratch.
            http_clear_pmr_string_retaining_small(
                outbound_deflated_, websocket_retained_buffer_bytes);
        }
        return websocket_frame_submit_status::accepted;
    }

    std::pmr::memory_resource* resource_;
    std::optional<http_io_buffer> owned_input_;
    http_io_buffer* input_;
    const std::size_t max_buffered_input_bytes_;
    protocol_byte_limit message_limit_;
    // Bytes of the last returned frame, consumed by the next next_event().
    std::size_t pending_consume_{0};
    std::size_t needed_input_bytes_{0};

    std::pmr::string out_buffer_;
    std::size_t out_offset_{0};
    // An unmasked, uncompressed payload that follows out_buffer_ on the wire
    // without being copied into it. Only the newest frame can borrow; queuing
    // more output first appends it to out_buffer_.
    std::string_view borrowed_output_;

    detail::websocket_inbound_assembler assembler_;

    std::optional<detail::websocket_deflate> deflate_;
    std::pmr::string inbound_inflated_;
    std::pmr::string outbound_deflated_;

    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};

    close_phase_type close_phase_{close_phase_type::open};
};

void websocket_connection::impl_deleter_type::operator()(impl_type* value) const noexcept {
    if (value != nullptr) {
        auto* resource = value->resource_;
        detail::destroy_http_pmr_object(value, resource);
    }
}

websocket_connection::websocket_connection(websocket_connection_options options) {
    if (options.max_buffered_input_bytes_ == 0) {
        throw std::invalid_argument("WebSocket input buffer limit must be greater than zero");
    }
    auto* resource = detail::http_pmr_resource_or_default(options.resource_);
    impl_.reset(detail::construct_http_pmr_object<impl_type>(resource, resource, options));
}

websocket_connection::websocket_connection(
    http_io_buffer& input, websocket_connection_options options) {
    if (options.max_buffered_input_bytes_ == 0) {
        throw std::invalid_argument("WebSocket input buffer limit must be greater than zero");
    }
    auto* resource = options.resource_ != nullptr
                         ? options.resource_
                         : detail::http_pmr_resource_or_default(input.get_allocator().resource());
    impl_.reset(detail::construct_http_pmr_object<impl_type>(resource, resource, input, options));
}

websocket_connection::~websocket_connection() = default;
websocket_connection::websocket_connection(websocket_connection&&) noexcept = default;
websocket_connection& websocket_connection::operator=(websocket_connection&&) noexcept = default;

websocket_feed_status websocket_connection::feed(std::string_view input) {
    if (impl_->liveness_mode() == websocket_liveness_mode::inactive) {
        return websocket_feed_status::inactive;
    }
    if (input.size() > impl_->max_buffered_input_bytes_ - impl_->input_->size()) {
        if (input.size() > impl_->max_buffered_input_bytes_ - impl_->pending_input_bytes()) {
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
        impl_->assembler_.release_completed();
        // The last delivered message expires now. Its size is peer-controlled up
        // to the message limit, so only a bounded buffer stays for the next one.
        http_clear_pmr_string_retaining_small(
            impl_->inbound_inflated_, websocket_retained_buffer_bytes);
        impl_->needed_input_bytes_ = 0;
        if (impl_->close_phase_ == close_phase_type::final_output_queued ||
            impl_->close_phase_ == close_phase_type::transport_end_ready ||
            impl_->close_phase_ == close_phase_type::closed) {
            return websocket_event::make_transport_end();
        }

        const auto protocol_failure_event = [this](detail::websocket_protocol_failure failure) {
            const auto close_code = detail::websocket_protocol_failure_close_code(failure);
            impl_->fail(close_code);
            return websocket_event::protocol_error(close_code);
        };

        for (;;) {
            const auto read = detail::websocket_try_read_frame(*impl_->input_, impl_->pending_consume_,
                impl_->message_limit_, impl_->deflate_.has_value(),
                impl_->role_ == websocket_connection_role::server);
            if (const auto* need_input = read.need_input()) {
                impl_->needed_input_bytes_ = need_input->missing_bytes();
                return std::nullopt;
            }
            if (const auto* failure = read.failure()) {
                return protocol_failure_event(failure->error());
            }

            const auto& frame = *read.frame();
            const auto inbound = impl_->assembler_.accept(frame, impl_->message_limit_);
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
                    impl_->close_phase_ = close_phase_type::final_output_queued;
                } else {
                    impl_->receive_peer_close();
                }
                return websocket_event::close(code, reason);
            }

            const auto& inbound_message = *inbound.message();
            const auto& message = inbound_message.message();
            if (inbound_message.content_encoding() == detail::websocket_inbound_content_encoding::identity) {
                if (impl_->close_phase_ != close_phase_type::open) {
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
            const auto inflate_result = impl_->deflate_->decompress(
                message.payload(), impl_->inbound_inflated_, impl_->message_limit_);
            if (inflate_result == websocket_inflate_result::too_large) {
                return protocol_failure_event(detail::websocket_protocol_failure::message_too_large);
            }
            if (inflate_result != websocket_inflate_result::ok) {
                return protocol_failure_event(detail::websocket_protocol_failure::protocol_error);
            }
            const std::string_view view = impl_->inbound_inflated_;
            if (message.opcode() == websocket_opcode::text && !detail::is_valid_utf8(view)) {
                return protocol_failure_event(detail::websocket_protocol_failure::invalid_payload_data);
            }
            if (impl_->close_phase_ != close_phase_type::open) {
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
        throw;
    }
}

std::size_t websocket_connection::needed_input_bytes() const noexcept {
    return impl_->needed_input_bytes_;
}

websocket_output_plan websocket_connection::output_plan() const& noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (impl_->close_phase_ == close_phase_type::transport_end_ready ||
        impl_->close_phase_ == close_phase_type::closed) {
        return websocket_output_plan({}, {},
            impl_->close_phase_ == close_phase_type::transport_end_ready
                ? websocket_transport_disposition::end_transport
                : websocket_transport_disposition::keep_open);
    }
    const auto bytes_value = std::string_view(
        impl_->out_buffer_.data() + impl_->out_offset_, impl_->out_buffer_.size() - impl_->out_offset_);
    const auto disposition = impl_->close_phase_ == close_phase_type::final_output_queued
                                 ? websocket_transport_disposition::end_transport
                                 : websocket_transport_disposition::keep_open;
    return websocket_output_plan(bytes_value, impl_->borrowed_output_, disposition);
}

websocket_output_consume_status websocket_connection::consume_output(std::size_t bytes_value) noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort makes unsent bytes unreachable without clearing their storage:
    // an async write may still borrow it. A zero-byte transport-end write has
    // no output left to consume, regardless of those discarded backing bytes.
    if (bytes_value == 0 && (impl_->close_phase_ == close_phase_type::transport_end_ready ||
                                impl_->close_phase_ == close_phase_type::closed)) {
        return websocket_output_consume_status::drained;
    }
    const auto owned = impl_->out_buffer_.size() - impl_->out_offset_;
    if (bytes_value > owned && bytes_value - owned > impl_->borrowed_output_.size()) {
        return websocket_output_consume_status::out_of_range;
    }
    if (bytes_value < owned) {
        impl_->out_offset_ += bytes_value;
        return websocket_output_consume_status::pending;
    }
    // The transport has written every owned byte, so a large frame's storage
    // can go back to the resource instead of staying with the connection.
    http_clear_pmr_string_retaining_small(
        impl_->out_buffer_, websocket_retained_buffer_bytes);
    impl_->out_offset_ = 0;
    if (bytes_value - owned < impl_->borrowed_output_.size()) {
        impl_->borrowed_output_.remove_prefix(bytes_value - owned);
        return websocket_output_consume_status::pending;
    }
    impl_->borrowed_output_ = {};
    if (impl_->close_phase_ == close_phase_type::local_close_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_close;
    } else if (impl_->close_phase_ == close_phase_type::final_output_queued) {
        impl_->close_phase_ = close_phase_type::transport_end_ready;
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
}

websocket_abort_disposition websocket_connection::abort() noexcept {
    if (impl_->close_phase_ == impl_type::close_phase_type::closed) {
        return websocket_abort_disposition::no_transport_action;
    }
    impl_->close_phase_ = impl_type::close_phase_type::closed;
    return websocket_abort_disposition::abort_transport;
}

websocket_liveness_mode websocket_connection::liveness_mode() const noexcept {
    return impl_->liveness_mode();
}

websocket_frame_submit_status websocket_connection::submit_frame(
    websocket_opcode opcode, std::string_view payload_value, bool compress) {
    return impl_->submit_frame(opcode, payload_value, compress, ws_payload_storage::copy);
}

websocket_frame_submit_status websocket_connection::submit_borrowed_frame(
    websocket_opcode opcode, std::string_view payload_value, bool compress) {
    return impl_->submit_frame(opcode, payload_value, compress, ws_payload_storage::borrow);
}

websocket_close_submit_status websocket_connection::submit_close(
    std::uint16_t code, std::string_view reason) {
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
    if (impl_->role_ == websocket_connection_role::server && code == 1010) {
        return websocket_close_submit_status::invalid_code;
    }
    const auto payload_value = detail::encode_websocket_close_payload(code, reason);
    if (const auto* failure = payload_value.failure()) {
        switch (failure->error()) {
            case detail::websocket_close_payload_encode_error::invalid_code:
                return websocket_close_submit_status::invalid_code;
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
