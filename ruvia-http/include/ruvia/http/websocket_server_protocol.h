#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/http_limits.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_protocol_types.h"

namespace ruvia {

struct websocket_server_protocol_options final {
    websocket_compression compression_{(websocket_compression{})};
    int compression_level_{6};
};

// Sans-I/O websocket server protocol. The input buffer is borrowed and must
// outlive this object; its memory resource must also outlive this object.
// Event payload views remain valid until the next poll() or input mutation.
class websocket_server_protocol final {
public:
    explicit websocket_server_protocol(std::pmr::string& input,
        protocol_byte_limit message_limit = protocol_byte_limit::limited(default_max_websocket_message_bytes),
        websocket_compression compression = (websocket_compression{}));
    websocket_server_protocol(std::pmr::string& input, protocol_byte_limit message_limit,
        websocket_server_protocol_options options);
    ~websocket_server_protocol();

    websocket_server_protocol(const websocket_server_protocol&) = delete;
    websocket_server_protocol& operator=(const websocket_server_protocol&) = delete;
    websocket_server_protocol(websocket_server_protocol&&) = delete;
    websocket_server_protocol& operator=(websocket_server_protocol&&) = delete;

    [[nodiscard]] std::optional<websocket_event> poll() &;
    std::optional<websocket_event> poll() && = delete;
    [[nodiscard]] websocket_output_plan output_plan() const& noexcept;
    websocket_output_plan output_plan() const&& = delete;
    [[nodiscard]] websocket_output_consume_status consume_output(std::size_t n) noexcept;
    void commit_transport_end() noexcept;
    void notify_transport_eof() noexcept;
    [[nodiscard]] websocket_abort_disposition abort() noexcept;
    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept;
    [[nodiscard]] websocket_frame_submit_status submit_frame(
        websocket_opcode opcode, std::string_view payload, bool compress = true);
    [[nodiscard]] websocket_close_submit_status submit_close(
        std::uint16_t code, std::string_view reason);

private:
    struct impl_type;
    struct impl_deleter_type {
        void operator()(impl_type* value) const noexcept;
    };
    std::unique_ptr<impl_type, impl_deleter_type> impl_;
};

}  // namespace ruvia
