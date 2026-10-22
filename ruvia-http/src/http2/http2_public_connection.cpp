#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_header.h"

#include "client/http_client_access.h"
#include "http2/http2_connection.h"
#include "http2/http2_request_builder.h"
#include "http2/http2_websocket_handshake.h"
#include "request/http_request_access.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia {

namespace {

[[nodiscard]] http_client_response_head response_head_from_stream(
    const detail::http2_stream_state& stream, std::pmr::memory_resource* resource) {
    const auto* status = stream.response_status();
    if (status == nullptr) {
        throw std::logic_error("HTTP/2 final response event has no status");
    }
    auto result_value =
        detail::http_client_response_head_access::make(*status, http_protocol_version::http2, resource);
    const auto header_count = stream.remote_header_count();
    if (header_count != 0) {
        auto& headers = detail::http_client_response_head_access::headers(result_value);
        headers.reserve(header_count);
        for (std::size_t index = 0; index < header_count; ++index) {
            const auto field = stream.remote_header_at(index);
            headers.push_back(
                http_header::copy_of(field.name_, field.value_, resource));
        }
    }
    return result_value;
}

[[nodiscard]] http2_message_content_semantics message_content_semantics(
    const detail::http2_stream_state& stream) noexcept {
    const auto& content = stream.remote_content();
    return content.metadata_only_without_length() != nullptr ||
                   content.metadata_only_known_length() != nullptr
               ? http2_message_content_semantics::metadata_only
               : http2_message_content_semantics::content;
}

}  // namespace

// Intrusively reference-counted so escaped events or credits can still inspect
// or abandon stream state after the public http2_connection facade is destroyed.
namespace detail {

class http2_connection_owner_endpoint final {
public:
    http2_connection_owner_endpoint(std::pmr::memory_resource* resource, http2_role role,
        bool enable_push, bool receive_origin_advertisements)
        : resource_(resource),
          connection_(resource, role, enable_push, receive_origin_advertisements) {}
    http2_connection_owner_endpoint(const http2_connection_owner_endpoint&) = delete;
    http2_connection_owner_endpoint& operator=(const http2_connection_owner_endpoint&) = delete;

    void retain() noexcept {
        ++references_;
    }

    void release() noexcept {
        if (references_ == 0) {
            std::terminate();
        }
        if (--references_ == 0) {
            destroy_http_pmr_object(this, resource_);
        }
    }

    void abandon_request(std::uint32_t stream_id) noexcept {
        connection_.abandon_stream_lease(stream_id);
    }

    void abandon_credit(std::uint32_t stream_id, std::uint32_t bytes_value) noexcept {
        connection_.abandon_received_data(stream_id, bytes_value);
    }
    [[nodiscard]] ::ruvia::http2_event materialize_message_head_event(
        const detail::http2_message_head_record& value) {
        auto* stream = connection_.stream(value.stream_id_);
        if (connection_.role() == http2_role::client) {
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 response event has no stream");
            }
            return ::ruvia::http2_event::response_head(
                value.stream_id_, response_head_from_stream(*stream, resource_), value.request_content_signal_);
        }
        if (stream == nullptr) {
            throw std::logic_error("HTTP/2 request event has no stream");
        }
        connection_.pin_stream(value.stream_id_);
        const auto content = stream->request_content_indication();
        const auto& remote = stream->remote_receive();
        const bool connect_pending =
            remote.connect_pending() != nullptr || remote.connect_pending_end_stream() != nullptr;
        const http2_server_request_snapshot snapshot{
            .content_ = content,
            .body_open_ = value.body_open_,
            .connect_pending_ = connect_pending,
        };
        return ::ruvia::http2_event::request_head(
            this, value.stream_id_, stream->request_expectations(), content, snapshot);
    }

    template <typename event_record_type>
    [[nodiscard]] ::ruvia::http2_event materialize_public_http2_event(event_record_type& value) {
        using value_type = std::decay_t<event_record_type>;
        const bool client = connection_.role() == http2_role::client;
        if constexpr (std::is_same_v<value_type, detail::http2_message_head_record>) {
            return materialize_message_head_event(value);
        } else if constexpr (std::is_same_v<value_type, detail::http2_message_data_record>) {
            return ::ruvia::http2_event::message_body_chunk(
                this, value.stream_id_, value.bytes_, value.flow_control_bytes_);
        } else if constexpr (std::is_same_v<value_type, detail::http2_tunnel_data_record>) {
            return ::ruvia::http2_event::tunnel_data(
                this, value.stream_id_, value.bytes_, value.flow_control_bytes_);
        } else if constexpr (std::is_same_v<value_type, detail::http2_message_end_record>) {
            auto* stream = connection_.stream(value.stream_id_);
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 message end event has no stream");
            }
            auto result = ::ruvia::http2_event::message_end(
                value.stream_id_, stream->take_remote_trailers(), message_content_semantics(*stream));
            if (client) {
                connection_.release_stream_lease_after_remote_end(value.stream_id_);
            }
            return result;
        } else {
            if constexpr (std::is_same_v<value_type, http2_tunnel_end_event>) {
                if (client) {
                    connection_.release_stream_lease_after_remote_end(value.stream_id());
                }
            } else if constexpr (std::is_same_v<value_type, http2_stream_closed_event> ||
                                 std::is_same_v<value_type, http2_request_unprocessed_event>) {
                if (client) {
                    connection_.release_stream_lease(value.stream_id());
                }
            }
            return ::ruvia::http2_event(std::move(value));
        }
    }
    std::size_t references_{1};
    std::pmr::memory_resource* resource_;
    detail::http2_connection connection_;
};

}  // namespace detail

http2_received_data_credit::http2_received_data_credit(detail::http2_connection_owner_endpoint* endpoint,
    std::uint32_t stream_id, std::uint32_t bytes_value) noexcept
    : endpoint_(endpoint),
      stream_id_(stream_id),
      bytes_(bytes_value) {
    endpoint_->retain();
}

http2_received_data_credit::~http2_received_data_credit() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandon_credit(stream_id_, bytes_);
    endpoint_->release();
}

http2_received_data_credit::http2_received_data_credit(http2_received_data_credit&& other) noexcept
    : endpoint_(std::exchange(other.endpoint_, nullptr)),
      stream_id_(std::exchange(other.stream_id_, 0)),
      bytes_(std::exchange(other.bytes_, 0)) {}

void http2_received_data_credit::merge(http2_received_data_credit&& other) {
    if (this == &other || !valid() || !other.valid()) {
        throw std::logic_error("invalid HTTP/2 received data credit");
    }
    if (endpoint_ != other.endpoint_ || stream_id_ != other.stream_id_) {
        throw std::logic_error("HTTP/2 received data credits belong to different streams");
    }
    if (bytes_ > (std::numeric_limits<std::uint32_t>::max)() - other.bytes_) {
        throw std::overflow_error("HTTP/2 received data credit overflow");
    }
    bytes_ += other.bytes_;
    auto* endpoint = std::exchange(other.endpoint_, nullptr);
    other.stream_id_ = 0;
    other.bytes_ = 0;
    endpoint->release();
}

http2_request_head_event::http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint,
    std::uint32_t stream_id, http_request_expectations expectations,
    http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept
    : stream_id_(stream_id),
      snapshot_(snapshot),
      expectations_(expectations),
      content_(content),
      endpoint_(endpoint) {
    endpoint_->retain();
}

http2_request_head_event::~http2_request_head_event() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandon_request(stream_id_);
    endpoint_->release();
}

http2_request_head_event::http2_request_head_event(http2_request_head_event&& other) noexcept
    : stream_id_(std::exchange(other.stream_id_, 0)),
      snapshot_(other.snapshot_),
      expectations_(other.expectations_),
      content_(other.content_),
      endpoint_(std::exchange(other.endpoint_, nullptr)) {}

http2_server_request_view http2_request_head_event::request_view() const& noexcept {
    if (endpoint_ == nullptr || stream_id_ == 0) {
        return {};
    }
    return endpoint_->connection_.server_request_view(stream_id_).value_or(http2_server_request_view{});
}

http_request http2_request_head_event::build_request(
    std::pmr::memory_resource* resource, std::string_view body) const {
    if (endpoint_ == nullptr || stream_id_ == 0) {
        throw std::logic_error("HTTP/2 request head event is not active");
    }
    auto* stream = endpoint_->connection_.stream(stream_id_);
    if (stream == nullptr) {
        throw std::logic_error("missing HTTP/2 request stream");
    }
    return detail::http2_request_builder::build(*stream, resource, body);
}
void http2_connection::impl_deleter_type::operator()(detail::http2_connection_owner_endpoint* value) const noexcept {
    if (value != nullptr) {
        value->release();
    }
}

http2_connection::http2_connection(std::pmr::memory_resource* resource, http2_role role, bool enable_push, bool receive_origin_advertisements) {
    auto* resolved = detail::http_pmr_resource_or_default(resource);
    impl_.reset(detail::construct_http_pmr_object<detail::http2_connection_owner_endpoint>(
        resolved, resolved, role, enable_push, receive_origin_advertisements));
    impl_->connection_.begin_connection();
}
http2_connection http2_connection::server(http2_connection_options options) {
    return http2_connection(options.resource_, http2_role::server, false, false);
}
http2_connection http2_connection::client(http2_connection_options options) {
    return http2_connection(options.resource_, http2_role::client, options.enable_push_, options.receive_origin_advertisements_);
}
http2_connection::~http2_connection() = default;
http2_connection::http2_connection(http2_connection&&) noexcept = default;
http2_connection& http2_connection::operator=(http2_connection&&) noexcept = default;

websocket_handshake_validation_result validate_http2_websocket_handshake(
    http2_connection& connection, std::uint32_t stream_id,
    const http_request& request,
    websocket_handshake_validation_options options) noexcept {
    const auto* stream = connection.impl_->connection_.stream(stream_id);
    if (stream == nullptr) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return detail::validate_http2_websocket_handshake(*stream, request, options);
}

std::variant<http2_websocket_negotiation, http2_websocket_handshake_submit_error>
http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation,
    http2_websocket_server_handshake_options options) {
    if (impl_->connection_.find_outbound_stream(stream_id) == nullptr) {
        return http2_websocket_handshake_submit_error::closed;
    }
    auto negotiation = detail::make_websocket_server_negotiation(request,
        {
            .supported_subprotocols_ = options.supported_subprotocols_,
            .response_headers_ = options.response_headers_,
            .resource_ = impl_->resource_,
            .deflate_ = options.deflate_,
        });
    return impl_->connection_.submit_websocket_handshake(
        stream_id, validation, std::move(negotiation));
}

http2_role http2_connection::role() const noexcept {
    return impl_->connection_.role();
}
bool http2_connection::header_block_in_progress() const noexcept {
    return impl_->connection_.header_block_in_progress();
}
bool http2_connection::received_peer_settings() const noexcept {
    return impl_->connection_.received_peer_settings();
}
http2_feed_result http2_connection::feed(std::string_view input) {
    auto& connection = impl_->connection_;
    connection.retry_deferred_stream_leases();
    const auto result_value = connection.feed(input);
    if (result_value == http2_feed_result::connection_not_started) {
        std::terminate();
    }
    connection.retry_deferred_stream_leases();
    return result_value;
}

std::optional<http2_event> http2_connection::next_event() {
    auto& connection = impl_->connection_;
    connection.retry_deferred_stream_leases();
    auto* event = connection.peek_event();
    if (event == nullptr) {
        return std::nullopt;
    }
    auto result_value = std::visit(
        [this](auto& value) {
            return impl_->materialize_public_http2_event(value);
        },
        event->value());
    connection.consume_event();
    connection.retry_deferred_stream_leases();
    return std::optional<http2_event>(std::move(result_value));
}

std::string_view http2_connection::pending_output() const& noexcept {
    return impl_->connection_.pending_output();
}
http2_output_consume_status http2_connection::consume_output(std::size_t bytes_value) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.consume_output(bytes_value);
}
void http2_connection::take_output(std::pmr::string& output) {
    impl_->connection_.retry_deferred_stream_leases();
    impl_->connection_.take_output(output);
}
http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes,
    std::pmr::string& output, http2_data_output_observer_type observer, void* observer_context) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.take_output_batch(max_bytes, output, observer, observer_context);
}
bool http2_connection::wants_write() const noexcept {
    return impl_->connection_.wants_write();
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(
    const http2_regular_request_head_view& request) {
    std::optional<std::string_view> authority;
    if (request.authority_) {
        authority = request.authority_->view();
    }
    auto result_value = impl_->connection_.submit_regular_request_head(request.method_.view(),
        request.scheme_.view(), authority, request.target_.view(),
        static_cast<std::span<const http_header_view>>(request.headers_), request.content_,
        request.expectation_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(
    const http2_connect_request_head_view& request) {
    auto result_value = impl_->connection_.submit_connect_request_head(
        request.authority_.view(), static_cast<std::span<const http_header_view>>(request.headers_));
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(
    const http2_extended_connect_request_head_view& request) {
    auto result_value = impl_->connection_.submit_extended_connect_request_head(request.protocol_.view(),
        request.scheme_.view(), request.authority_.view(), request.target_.view(),
        static_cast<std::span<const http_header_view>>(request.headers_));
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

http2_data_submit_status http2_connection::submit_data(
    std::uint32_t stream_id, std::string_view bytes_value, http2_end_stream end_stream) {
    const auto result_value = impl_->connection_.submit_data(stream_id, bytes_value, end_stream);
    impl_->connection_.retry_deferred_stream_leases();
    return result_value;
}
http2_request_content_release_status http2_connection::release_request_content(
    std::uint32_t stream_id) noexcept {
    return impl_->connection_.release_request_content(stream_id);
}
http2_submit_status http2_connection::submit_connect_response_head(std::uint32_t stream_id, const http_response& response) {
    return impl_->connection_.submit_connect_response_head(stream_id, response);
}

http2_submit_status http2_connection::submit_interim_response_head(
    std::uint32_t stream_id, const http_interim_response_head& response) {
    return impl_->connection_.submit_interim_response_head(stream_id, response);
}

std::variant<http_buffered_response_write_plan, http2_response_head_submit_error>
http2_connection::submit_response_head(
    std::uint32_t stream_id, const http_response& response,
    http_buffered_response_write_plan write_plan) {
    return impl_->connection_.submit_response_head(stream_id, response, std::move(write_plan));
}

std::variant<http_response_stream_commit_plan, http2_response_head_submit_error>
http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response response,
    http_response_trailer_intent trailer_intent) {
    return impl_->connection_.submit_streaming_response_head(
        stream_id, std::move(response), trailer_intent);
}

http2_finish_request_status http2_connection::finish_request(std::uint32_t stream_id,
    std::span<const http_header_view> trailers) {
    const auto status = impl_->connection_.finish_request(stream_id, trailers);
    impl_->connection_.retry_deferred_stream_leases();
    return status;
}

http2_finish_response_status http2_connection::finish_response(
    std::uint32_t stream_id, const http_response_trailer_section& trailers) {
    return impl_->connection_.finish_response(stream_id, trailers);
}

std::variant<http2_request_head_event, http2_push_submit_error> http2_connection::submit_push_request(
    std::uint32_t associated_stream_id, http_push_request_view request) {
    const auto id = impl_->connection_.submit_push_promise(associated_stream_id, request);
    if ((id.index() != 0)) {
        return std::get<1>(id);
    }
    try {
        impl_->connection_.pin_stream(std::get<0>(id));
        return http2_request_head_event(impl_.get(), std::get<0>(id), {},
            http_request_content_indication::no_content, {});
    } catch (...) {
        impl_->connection_.abandon_stream_lease(std::get<0>(id));
        throw;
    }
}

http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    return impl_->connection_.submit_origin_advertisement(origins);
}
http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id,
    std::string_view origin, std::string_view field_value) {
    return impl_->connection_.submit_alternative_service_advertisement(stream_id, origin, field_value);
}
http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    return impl_->connection_.submit_priority_update(stream_id, fields_value);
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    auto& connection = impl_->connection_;
    const auto status = connection.submit_reset(stream_id, error);
    if (status == http2_submit_status::accepted && connection.role() == http2_role::client) {
        // Owner-originated resets deliberately produce no terminal event. The
        // public client facade must therefore release the pin established by
        // submit_request_head() here rather than waiting for next_event().
        connection.release_stream_lease(stream_id);
    }
    connection.retry_deferred_stream_leases();
    return status;
}
http2_received_data_acknowledge_status http2_connection::acknowledge(http2_received_data_credit&& credit) {
    if (!credit.valid() || credit.endpoint_ != impl_.get()) {
        throw std::logic_error("invalid HTTP/2 received data credit");
    }
    const bool acknowledged = impl_->connection_.acknowledge_received_data(credit.stream_id_, credit.bytes_);
    auto* endpoint = std::exchange(credit.endpoint_, nullptr);
    credit.stream_id_ = 0;
    credit.bytes_ = 0;
    endpoint->release();
    return acknowledged ? http2_received_data_acknowledge_status::acknowledged
                        : http2_received_data_acknowledge_status::closed;
}
bool http2_connection::has_queued_data(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.has_queued_data(stream_id);
}

http2_data_queue_state http2_connection::data_queue_state(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.data_queue_state(stream_id);
}
std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.pending_data_output_bytes(stream_id);
}
std::optional<http2_send_window_state> http2_connection::send_window_state(
    std::uint32_t stream_id) const noexcept {
    return impl_->connection_.send_window_state(stream_id);
}
bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_aborted(stream_id);
}
http2_stream_receive_status http2_connection::stream_receive_status(
    std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_receive_status(stream_id);
}
std::span<const std::uint32_t> http2_connection::take_drained_data_streams() & noexcept {
    return impl_->connection_.take_drained_data_streams();
}
http2_server_request_release_status http2_connection::release(http2_request_head_event&& request) {
    if (request.endpoint_ == nullptr || request.stream_id_ == 0 ||
        request.endpoint_ != impl_.get()) {
        throw std::logic_error("invalid HTTP/2 server request lease");
    }
    const bool live = impl_->connection_.stream(request.stream_id_) != nullptr;
    if (live) {
        impl_->connection_.release_stream_lease(request.stream_id_);
    }
    auto* endpoint = std::exchange(request.endpoint_, nullptr);
    request.stream_id_ = 0;
    endpoint->release();
    return live ? http2_server_request_release_status::released : http2_server_request_release_status::closed;
}
void http2_connection::begin_drain() {
    impl_->connection_.begin_drain();
}
bool http2_connection::draining() const noexcept {
    return impl_->connection_.draining();
}
std::optional<http2_error_code> http2_connection::connection_error() const noexcept {
    return impl_->connection_.connection_error();
}

}  // namespace ruvia
