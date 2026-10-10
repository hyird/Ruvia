#pragma once

// websocket sans-I/O connection core shared by server and client runtimes.
//
// The caller owns one persistent PMR input buffer and appends transport bytes to
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

#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_protocol_types.h"

#include "websocket/http_websocket_inbound_assembler.h"
#include "websocket/http_websocket_permessage_deflate.h"

namespace ruvia::detail {

class ws_connection final {
public:
    explicit ws_connection(std::pmr::string& input,
        protocol_byte_limit message_limit = protocol_byte_limit::unlimited(),
        websocket_compression compression = (websocket_compression{}),
        websocket_connection_role role = websocket_connection_role::server,
        websocket_mask_key_generator_type mask_key_generator = nullptr, void* mask_key_context = nullptr,
        int compression_level = 6);

    // Parse buffered transport bytes until one protocol event is available or
    // more input is required (nullopt). Every materialized event contains one
    // typed payload. Application data received after a local Close is validated
    // but not delivered while the peer Close is awaited.
    [[nodiscard]] std::optional<websocket_event> poll() &;
    [[nodiscard]] std::optional<websocket_event> poll() && = delete;

    [[nodiscard]] websocket_output_plan output_plan() const& noexcept;
    [[nodiscard]] websocket_output_plan output_plan() const&& = delete;
    [[nodiscard]] websocket_output_consume_status consume_output(std::size_t n) noexcept;
    void commit_transport_end() noexcept;
    void notify_transport_eof() noexcept;
    [[nodiscard]] websocket_abort_disposition abort() noexcept;
    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept;
    // Drop already-parsed input bytes so they no longer count against an owner's
    // input bound. Invalidates event payload views into the input buffer.
    void release_consumed_input() noexcept;

    // Submit one complete logical message/control payload. Role-correct masking,
    // outbound text UTF-8 validation, optional data-message compression and
    // wire header encoding stay inside the core.
    // Close has a separate typed entry because it owns code/reason validation
    // and close-handshake state rather than accepting a pre-encoded payload.
    [[nodiscard]] websocket_frame_submit_status submit_frame(websocket_opcode opcode, std::string_view payload, bool compress = true);
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

    void append_frame(websocket_opcode opcode, std::string_view payload, bool rsv1 = false);
    void fail(std::uint16_t code, std::string_view reason = {});
    void receive_peer_close() noexcept;
    [[nodiscard]] std::optional<websocket_event> poll_impl() &;

    std::pmr::string* input_;
    protocol_byte_limit message_limit_;
    std::size_t input_offset_{0};
    std::size_t pending_compact_until_{0};

    std::pmr::string out_buffer_;
    std::size_t out_offset_{0};

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
