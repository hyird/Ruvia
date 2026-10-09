#include "ruvia/http/websocket_connection.h"

#include <stdexcept>

#include "ruvia/http/detail/util/http_pmr_object.h"

#include "websocket/ws_connection.h"

namespace ruvia {
class websocket_connection::impl_type final {
public:
    impl_type(std::pmr::memory_resource* memory, websocket_connection_options options)
        : resource_(memory),
          input_(memory),
          max_buffered_input_bytes_(options.max_buffered_input_bytes_),
          connection_(input_, options.message_limit_, options.compression_,
              options.role_, options.mask_key_generator_, options.mask_key_context_, options.compression_level_) {}
    std::pmr::memory_resource* resource_;
    std::pmr::string input_;
    const std::size_t max_buffered_input_bytes_;
    detail::ws_connection connection_;
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

websocket_connection::~websocket_connection() = default;
websocket_connection::websocket_connection(websocket_connection&&) noexcept = default;
websocket_connection& websocket_connection::operator=(websocket_connection&&) noexcept = default;

websocket_feed_status websocket_connection::feed(std::string_view input) {
    if (impl_->connection_.liveness_mode() == websocket_liveness_mode::inactive) {
        return websocket_feed_status::inactive;
    }
    if (input.size() > impl_->max_buffered_input_bytes_ - impl_->input_.size()) {
        return websocket_feed_status::backpressured;
    }
    impl_->input_.append(input);
    return websocket_feed_status::accepted;
}

std::optional<websocket_event> websocket_connection::next_event() & {
    return impl_->connection_.poll();
}

websocket_output_plan websocket_connection::output_plan() const& noexcept {
    return impl_->connection_.output_plan();
}

websocket_output_consume_status websocket_connection::consume_output(std::size_t bytes_value) noexcept {
    return impl_->connection_.consume_output(bytes_value);
}

void websocket_connection::commit_transport_end() noexcept {
    impl_->connection_.commit_transport_end();
}
void websocket_connection::notify_transport_eof() noexcept {
    impl_->connection_.notify_transport_eof();
}
websocket_abort_disposition websocket_connection::abort() noexcept {
    return impl_->connection_.abort();
}
websocket_liveness_mode websocket_connection::liveness_mode() const noexcept {
    return impl_->connection_.liveness_mode();
}
websocket_frame_submit_status websocket_connection::submit_frame(websocket_opcode opcode, std::string_view payload_value, bool compress) {
    return impl_->connection_.submit_frame(opcode, payload_value, compress);
}
websocket_close_submit_status websocket_connection::submit_close(std::uint16_t code, std::string_view reason) {
    return impl_->connection_.submit_close(code, reason);
}

}  // namespace ruvia
