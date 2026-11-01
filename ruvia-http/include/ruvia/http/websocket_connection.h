#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_deflate_codecs.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

struct websocket_connection_options final {
    protocol_byte_limit message_limit_{protocol_byte_limit::limited(default_max_websocket_message_bytes)};
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
    // compressed message from the input buffer's resource.
    websocket_deflate_codecs* shared_deflate_{nullptr};
};

// Sans-I/O RFC 6455 driver for an already upgraded connection. It reads frames
// from the caller's input buffer: append received bytes, then call
// next_event(). The buffer and its resource, which also holds the
// address-stable implementation and every protocol buffer, must outlive the
// connection. The role fixes inbound mask validation and outbound masking,
// including automatic Pong/Close. After the close handshake a server plans
// end_transport once its output is flushed; a client keeps the transport open
// (liveness awaiting_peer_eof, in which next_event() discards input) until
// notify_transport_eof(), because the server closes first (RFC 6455 section
// 7.1.1). Event views remain valid until the next next_event() or an append to
// the input buffer. Output bytes remain valid until next_event(),
// submit_frame(), submit_close(), consume_output(), or destruction. Input, EOF
// and abort do not invalidate an in-flight write.
class websocket_connection final {
public:
    explicit websocket_connection(http_io_buffer& input, websocket_connection_options options = {});
    ~websocket_connection();
    websocket_connection(const websocket_connection&) = delete;
    websocket_connection& operator=(const websocket_connection&) = delete;
    websocket_connection(websocket_connection&&) noexcept;
    websocket_connection& operator=(websocket_connection&&) noexcept;

    [[nodiscard]] std::optional<websocket_event> next_event() &;
    std::optional<websocket_event> next_event() && = delete;
    [[nodiscard]] std::size_t needed_input_bytes() const noexcept;
    [[nodiscard]] websocket_output_plan output_plan() const& noexcept;
    websocket_output_plan output_plan() const&& = delete;
    [[nodiscard]] websocket_output_consume_status consume_output(std::size_t bytes) noexcept;
    void commit_transport_end() noexcept;
    void notify_transport_eof() noexcept;
    [[nodiscard]] websocket_abort_disposition abort() noexcept;
    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept;
    // A failed submission preserves queued output and discards any unsent
    // compression history. It can be retried unless resetting the codec also
    // failed, in which case liveness_mode() reports inactive.
    [[nodiscard]] websocket_frame_submit_status submit_frame(
        websocket_opcode opcode, std::string_view payload, bool compress = true);
    // submit_frame() without copying an uncompressed, unmasked payload:
    // output_plan() exposes it as borrowed_bytes() after the frame header.
    [[nodiscard]] websocket_frame_submit_status submit_borrowed_frame(
        websocket_opcode opcode, std::string_view payload, bool compress = true);
    [[nodiscard]] websocket_close_submit_status submit_close(
        std::uint16_t code, std::string_view reason);

private:
    class impl_type;
    struct impl_deleter_type {
        void operator()(impl_type* value) const noexcept;
    };
    std::unique_ptr<impl_type, impl_deleter_type> impl_;
};

}  // namespace ruvia
