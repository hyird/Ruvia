#pragma once

// websocket sans-I/O connection core shared by server and client runtimes.
//
// The caller owns one persistent http_io_buffer and appends transport bytes to
// it. poll() parses that buffer (unmasking client-to-server frames in place),
// emits at most one event,
// and leaves its payload view valid until the next poll() call. This keeps the
// runtime hot path zero-copy for complete unfragmented messages; fragmented and
// compressed messages use only their protocol-required assembly/decode storage.
//
// Outbound frames, including automatic Pong and Close replies, are serialized
// into the core's pending output. The output plan also owns the orderly transport
// end decision: RFC 6455 close-handshake state and RFC 8441 END_STREAM mapping must
// not be reconstructed by a runtime from a loose `close` boolean. The runtime only
// flushes the plan and owns coroutine I/O, timeout and write-exclusion policy.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_deflate_codecs.h"
#include "ruvia/http/websocket_protocol_types.h"

#include "websocket/http_websocket_inbound_assembler.h"
#include "websocket/http_websocket_permessage_deflate.h"

namespace ruvia::detail {

// How submit_frame() holds an outbound payload that is sent as-is: copied into
// the pending output, or borrowed until that output drains. Masked (client) and
// compressed payloads are always encoded into owned output.
enum class ws_payload_storage : std::uint8_t {
    copy,
    borrow,
};

class ws_connection final {
public:
    // Validates every parameter, deflate ones included even when compression
    // is not negotiated. `shared_deflate` serves the directions negotiated
    // without context takeover (see websocket_deflate); it must outlive this
    // connection. No codec state is allocated here.
    ws_connection(http_io_buffer& input, protocol_byte_limit message_limit, websocket_compression compression,
        websocket_connection_role role, websocket_mask_key_generator_type mask_key_generator, void* mask_key_context,
        int compression_level, int mem_level, websocket_deflate_codecs* shared_deflate);

    // Parse buffered transport bytes until one protocol event is available or
    // more input is required (nullopt). Every materialized event contains one
    // typed payload. Application data received after a local Close is validated
    // but not delivered while the peer Close is awaited.
    [[nodiscard]] std::optional<websocket_event> poll() &;
    [[nodiscard]] std::optional<websocket_event> poll() && = delete;
    // Bytes the frame the last poll() waited for still lacks; 0 when it
    // returned an event or the frame header was incomplete.
    [[nodiscard]] std::size_t needed_input_bytes() const noexcept {
        return needed_input_bytes_;
    }

    [[nodiscard]] websocket_output_plan output_plan() const& noexcept;
    [[nodiscard]] websocket_output_plan output_plan() const&& = delete;
    [[nodiscard]] websocket_output_consume_status consume_output(std::size_t n) noexcept;
    void commit_transport_end() noexcept;
    void notify_transport_eof() noexcept;
    [[nodiscard]] websocket_abort_disposition abort() noexcept;
    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept;
    [[nodiscard]] std::size_t pending_input_bytes() const noexcept {
        return input_->size() - pending_consume_;
    }
    // Drop the last returned frame so it no longer counts against an owner's
    // input bound. Its bytes stay in place until the buffer next needs room.
    void release_consumed_input() noexcept;

    // Submit one complete logical message/control payload. Role-correct masking,
    // outbound text UTF-8 validation, optional data-message compression and
    // wire header encoding stay inside the core. A borrowed payload must stay
    // valid and unchanged until consume_output() drains it or the transport ends.
    // Close has a separate typed entry because it owns code/reason validation
    // and close-handshake state rather than accepting a pre-encoded payload.
    [[nodiscard]] websocket_frame_submit_status submit_frame(websocket_opcode opcode, std::string_view payload,
        bool compress, ws_payload_storage storage = ws_payload_storage::copy);
    [[nodiscard]] websocket_close_submit_status submit_close(std::uint16_t code, std::string_view reason);

private:
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

    void append_frame(websocket_opcode opcode, std::string_view payload, bool rsv1 = false,
        ws_payload_storage storage = ws_payload_storage::copy);
    void fail(std::uint16_t code);
    void receive_peer_close() noexcept;
    [[nodiscard]] std::optional<websocket_event> poll_impl() &;

    http_io_buffer* input_;
    protocol_byte_limit message_limit_;
    // Bytes of the last returned frame, consumed by the next poll().
    std::size_t pending_consume_{0};
    std::size_t needed_input_bytes_{0};

    std::pmr::string out_buffer_;
    std::size_t out_offset_{0};
    // An unmasked, uncompressed payload that follows out_buffer_ on the wire
    // without being copied into it. Only the newest frame can borrow; queuing
    // more output first appends it to out_buffer_.
    std::string_view borrowed_output_;

    websocket_inbound_assembler assembler_;

    std::optional<websocket_deflate> deflate_;
    std::pmr::string inbound_inflated_;
    std::pmr::string outbound_deflated_;

    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};

    close_phase_type close_phase_{close_phase_type::open};
};

}  // namespace ruvia::detail
