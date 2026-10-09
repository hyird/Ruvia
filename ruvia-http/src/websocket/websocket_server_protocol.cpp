#include "ruvia/http/websocket_server_protocol.h"

#include "ruvia/http/detail/util/http_pmr_object.h"

#include "websocket/ws_connection.h"

namespace ruvia {

struct websocket_server_protocol::impl_type final {
    explicit impl_type(std::pmr::string& input, protocol_byte_limit message_limit,
        websocket_server_protocol_options options)
        : resource_(input.get_allocator().resource()),
          connection_(input, message_limit, options.compression_,
              websocket_connection_role::server, nullptr, nullptr, options.compression_level_) {}

    std::pmr::memory_resource* resource_;
    detail::ws_connection connection_;
};

void websocket_server_protocol::impl_deleter_type::operator()(impl_type* value) const noexcept {
    if (value != nullptr) {
        auto* resource = value->resource_;
        detail::destroy_http_pmr_object(value, resource);
    }
}

websocket_server_protocol::websocket_server_protocol(std::pmr::string& input,
    protocol_byte_limit message_limit, websocket_compression compression)
    : websocket_server_protocol(input, message_limit, websocket_server_protocol_options{compression, 6}) {}

websocket_server_protocol::websocket_server_protocol(std::pmr::string& input,
    protocol_byte_limit message_limit, websocket_server_protocol_options options)
    : impl_(detail::construct_http_pmr_object<impl_type>(input.get_allocator().resource(), input, message_limit, options)) {}

websocket_server_protocol::~websocket_server_protocol() = default;

std::optional<websocket_event> websocket_server_protocol::poll() & {
    return impl_->connection_.poll();
}

websocket_output_plan websocket_server_protocol::output_plan() const& noexcept {
    return impl_->connection_.output_plan();
}
websocket_output_consume_status websocket_server_protocol::consume_output(std::size_t n) noexcept {
    return impl_->connection_.consume_output(n);
}
void websocket_server_protocol::commit_transport_end() noexcept {
    impl_->connection_.commit_transport_end();
}
void websocket_server_protocol::notify_transport_eof() noexcept {
    impl_->connection_.notify_transport_eof();
}
websocket_abort_disposition websocket_server_protocol::abort() noexcept {
    return impl_->connection_.abort();
}
websocket_liveness_mode websocket_server_protocol::liveness_mode() const noexcept {
    return impl_->connection_.liveness_mode();
}
websocket_frame_submit_status websocket_server_protocol::submit_frame(websocket_opcode opcode, std::string_view payload_value, bool compress) {
    return impl_->connection_.submit_frame(opcode, payload_value, compress);
}
websocket_close_submit_status websocket_server_protocol::submit_close(std::uint16_t code, std::string_view reason) {
    return impl_->connection_.submit_close(code, reason);
}

}  // namespace ruvia
