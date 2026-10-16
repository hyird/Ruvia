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
#include "http2/http2_connection_owner_endpoint.h"
#include "http2/http2_request_builder.h"
#include "http2/http2_websocket_handshake.h"
#include "request/http_request_access.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia {

static std::variant<http_request, http_protocol_error> build_http2_server_request(
    detail::http2_connection& connection, std::uint32_t stream_id,
    std::pmr::memory_resource* resource, std::string_view body) {
    auto* stream = connection.stream(stream_id);
    if (stream == nullptr) {
        return http_protocol_error(
            http_status::bad_request, "missing HTTP/2 request stream");
    }
    auto request = detail::http_request_access::make();
    auto result_value = detail::http2_request_builder::build(*stream, request, resource, body);
    if (const auto* failure = result_value.failure()) {
        return failure->protocol_error();
    }
    return request;
}

namespace {

[[nodiscard]] http_client_response_head response_head_from_stream(
    const detail::http2_stream_state& stream, std::pmr::memory_resource* resource) {
    const auto* status = stream.response_status();
    if (status == nullptr) {
        throw std::logic_error("HTTP/2 final response event has no status");
    }
    auto result_value =
        detail::http_client_response_head_access::make(*status, http_protocol_version::http2, resource);
    auto& headers = detail::http_client_response_head_access::headers(result_value);
    headers.reserve(stream.remote_header_count());
    for (std::size_t index = 0; index < stream.remote_header_count(); ++index) {
        const auto field = stream.remote_header_at(index);
        headers.push_back(
            http_header::copy_of(field.name_, field.value_, resource));
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

// The owner endpoint retains this storage after the facade is destroyed while
// escaped events or credits can still refer to its streams.
class http2_connection::impl_type final {
public:
    impl_type(std::pmr::memory_resource* resource, http2_role role, bool enable_push,
        bool receive_origin_advertisements)
        : resource_(resource),
          connection_(resource, role, enable_push, receive_origin_advertisements),
          endpoint_(detail::construct_http_pmr_object<detail::http2_connection_owner_endpoint>(
              resource, &connection_, resource)) {}
    impl_type(const impl_type&) = delete;
    impl_type& operator=(const impl_type&) = delete;

    static void destroy(void* raw) noexcept {
        auto* value = static_cast<impl_type*>(raw);
        detail::destroy_http_pmr_object(value, value->resource_);
    }

    std::pmr::memory_resource* resource_;
    detail::http2_connection connection_;
    detail::http2_connection_owner_endpoint* endpoint_;
};

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

http2_received_data_credit_merge_status http2_received_data_credit::merge(
    http2_received_data_credit&& other) noexcept {
    if (this == &other || !valid() || !other.valid()) {
        return http2_received_data_credit_merge_status::invalid_credit;
    }
    if (endpoint_ != other.endpoint_ || stream_id_ != other.stream_id_) {
        return http2_received_data_credit_merge_status::different_stream;
    }
    if (bytes_ > (std::numeric_limits<std::uint32_t>::max)() - other.bytes_) {
        return http2_received_data_credit_merge_status::overflow;
    }
    bytes_ += other.bytes_;
    auto* endpoint = std::exchange(other.endpoint_, nullptr);
    other.stream_id_ = 0;
    other.bytes_ = 0;
    endpoint->release();
    return http2_received_data_credit_merge_status::merged;
}

http2_request_head_event::http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint,
    std::uint32_t stream_id, http_request request, http_request_expectations expectations,
    http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept
    : stream_id_(stream_id),
      request_(std::move(request)),
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
      request_(std::move(other.request_)),
      snapshot_(other.snapshot_),
      expectations_(other.expectations_),
      content_(other.content_),
      endpoint_(std::exchange(other.endpoint_, nullptr)) {}

void http2_connection::impl_deleter_type::operator()(impl_type* value) const noexcept {
    if (value == nullptr) {
        return;
    }
    auto* endpoint = value->endpoint_;
    endpoint->detach();
    endpoint->retain_storage(value, &impl_type::destroy);
    endpoint->release();
}

http2_connection::http2_connection(std::pmr::memory_resource* resource, http2_role role, bool enable_push, bool receive_origin_advertisements) {
    auto* resolved = detail::http_pmr_resource_or_default(resource);
    impl_.reset(detail::construct_http_pmr_object<impl_type>(resolved, resolved, role, enable_push, receive_origin_advertisements));
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

std::variant<http_request, http_protocol_error> make_http2_server_request(
    http2_connection& connection, std::uint32_t stream_id,
    std::pmr::memory_resource* resource, std::string_view body) {
    return build_http2_server_request(connection.impl_->connection_, stream_id, resource, body);
}

websocket_handshake_validation_result validate_http2_websocket_handshake(
    http2_connection& connection, std::uint32_t stream_id,
    const http_request& request) noexcept {
    const auto* stream = connection.impl_->connection_.stream(stream_id);
    if (stream == nullptr) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return detail::validate_http2_websocket_handshake(*stream, request);
}

http2_websocket_handshake_submit_result http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation) {
    return submit_websocket_handshake(stream_id, request, validation, {});
}

http2_websocket_handshake_submit_result http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation,
    http2_websocket_server_handshake_options options) {
    if (impl_->connection_.find_outbound_stream(stream_id) == nullptr) {
        return http2_websocket_handshake_submit_result(http2_websocket_handshake_submit_failure(
            http2_websocket_handshake_submit_error::closed));
    }
    const auto negotiation = detail::make_websocket_server_negotiation(request, {
                                                                                    .supported_subprotocols_ = options.supported_subprotocols_,
                                                                                    .response_headers_ = options.response_headers_,
                                                                                    .resource_ = impl_->resource_,
                                                                                });
    return impl_->connection_.submit_websocket_handshake(stream_id, validation, negotiation);
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
    const bool client = connection.role() == http2_role::client;
    auto result_value = std::visit(
        [this, &connection, client](auto& value) {
            using value_type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, detail::http2_message_head_record>) {
                auto* stream = connection.stream(value.stream_id_);
                if (client) {
                    if (stream == nullptr) {
                        throw std::logic_error("HTTP/2 response event has no stream");
                    }
                    return http2_event::response_head(value.stream_id_,
                        response_head_from_stream(*stream, impl_->resource_), value.request_content_signal_);
                }
                if (stream == nullptr) {
                    throw std::logic_error("HTTP/2 request event has no stream");
                }
                auto request = detail::http_request_access::make();
                const auto build = detail::http2_request_builder::build(*stream, request, impl_->resource_, {});
                if (build.built() == nullptr) {
                    (void)connection.submit_reset(value.stream_id_, http2_error_code::cancel);
                    connection.consume_event();
                    throw std::logic_error("validated HTTP/2 request cannot be materialized");
                }
                connection.pin_stream(value.stream_id_);
                const auto content = stream->request_content_indication();
                const auto& remote = stream->remote_receive();
                const bool connect_pending = remote.connect_pending() != nullptr ||
                                             remote.connect_pending_end_stream() != nullptr;
                const http2_server_request_snapshot snapshot{
                    .content_ = content,
                    .body_open_ = value.body_open_,
                    .connect_pending_ = connect_pending,
                };
                return http2_event::request_head(impl_->endpoint_, value.stream_id_, std::move(request),
                    stream->request_expectations(), content, snapshot);
            } else if constexpr (std::is_same_v<value_type, detail::http2_message_data_record>) {
                return http2_event::message_body_chunk(
                    impl_->endpoint_, value.stream_id_, value.bytes_, value.flow_control_bytes_);
            } else if constexpr (std::is_same_v<value_type, detail::http2_tunnel_data_record>) {
                return http2_event::tunnel_data(
                    impl_->endpoint_, value.stream_id_, value.bytes_, value.flow_control_bytes_);
            } else if constexpr (std::is_same_v<value_type, detail::http2_message_end_record>) {
                auto* stream = connection.stream(value.stream_id_);
                if (stream == nullptr) {
                    throw std::logic_error("HTTP/2 message end event has no stream");
                }
                auto result = http2_event::message_end(
                    value.stream_id_, stream->take_remote_trailers(), message_content_semantics(*stream));
                if (client) {
                    connection.release_stream_lease(value.stream_id_);
                }
                return result;
            } else {
                if constexpr (std::is_same_v<value_type, http2_tunnel_end_event>) {
                    if (client) {
                        connection.release_stream_lease_after_tunnel_end(value.stream_id());
                    }
                } else if constexpr (std::is_same_v<value_type, http2_stream_closed_event> ||
                                     std::is_same_v<value_type, http2_request_unprocessed_event>) {
                    if (client) {
                        connection.release_stream_lease(value.stream_id());
                    }
                }
                return http2_event(std::move(value));
            }
        },
        event->value());
    connection.consume_event();
    connection.retry_deferred_stream_leases();
    return std::optional<http2_event>(std::move(result_value));
}

std::string_view http2_connection::pending_output() const& noexcept {
    return impl_->connection_.pending_output();
}
http2_output_consume_status http2_connection::consume_output(std::size_t bytes_value) noexcept {
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

http2_request_head_submit_result http2_connection::submit_request_head(
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

http2_request_head_submit_result http2_connection::submit_request_head(
    const http2_connect_request_head_view& request) {
    auto result_value = impl_->connection_.submit_connect_request_head(
        request.authority_.view(), static_cast<std::span<const http_header_view>>(request.headers_));
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

http2_request_head_submit_result http2_connection::submit_request_head(
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

http2_response_head_submit_result http2_connection::submit_response_head(
    std::uint32_t stream_id, const http_response& response,
    http_buffered_response_write_plan write_plan) {
    return impl_->connection_.submit_response_head(stream_id, response, std::move(write_plan));
}

http2_submit_status http2_connection::submit_buffered_response(
    std::uint32_t stream_id, const http_response& response) {
    return impl_->connection_.submit_buffered_response(stream_id, response);
}

http2_streaming_response_head_submit_result http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response response, http_response_stream_kind kind,
    http_response_trailer_intent trailer_intent) {
    return impl_->connection_.submit_streaming_response_head(
        stream_id, std::move(response), kind, trailer_intent);
}

http2_submit_status http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response response) {
    const auto result_value = submit_streaming_response_head(stream_id, std::move(response),
        http_response_stream_kind::generic, http_response_trailer_intent::none);
    if (const auto* failure = result_value.failure()) {
        return detail::http2_response_head_submit_status(failure->error());
    }
    return http2_submit_status::accepted;
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

std::variant<std::uint32_t, http2_push_submit_error> http2_connection::submit_push_promise(
    std::uint32_t associated_stream_id, http_push_request_view request) {
    return impl_->connection_.submit_push_promise(associated_stream_id, request);
}

std::variant<http2_request_head_event, http2_push_submit_error> http2_connection::submit_push_request(
    std::uint32_t associated_stream_id, http_push_request_view request) {
    const auto id = submit_push_promise(associated_stream_id, request);
    if ((id.index() != 0)) {
        return std::get<1>(id);
    }
    try {
        auto built = build_http2_server_request(impl_->connection_, std::get<0>(id), impl_->resource_, {});
        if ((built.index() != 0)) {
            (void)impl_->connection_.submit_reset(std::get<0>(id), http2_error_code::cancel);
            return http2_push_submit_error::invalid_request;
        }
        impl_->connection_.pin_stream(std::get<0>(id));
        return http2_request_head_event(impl_->endpoint_, std::get<0>(id), std::move(std::get<0>(built)), {},
            http_request_content_indication::no_content, {});
    } catch (...) {
        const auto original = std::current_exception();
        try {
            (void)impl_->connection_.submit_reset(std::get<0>(id), http2_error_code::cancel);
            // Preserve the original failure; this reset is rollback only.
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
        std::rethrow_exception(original);
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
    if (!credit.valid() || credit.endpoint_ != impl_->endpoint_) {
        return http2_received_data_acknowledge_status::invalid_credit;
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
std::optional<http2_server_request_view> http2_connection::server_request_view(
    std::uint32_t stream_id) const noexcept {
    return impl_->connection_.server_request_view(stream_id);
}
std::span<const std::uint32_t> http2_connection::take_drained_data_streams() & noexcept {
    return impl_->connection_.take_drained_data_streams();
}
http2_server_request_release_status http2_connection::release(http2_request_head_event&& request) {
    if (request.endpoint_ == nullptr || request.stream_id_ == 0 ||
        request.endpoint_ != impl_->endpoint_) {
        return http2_server_request_release_status::invalid_lease;
    }
    const bool live = impl_->connection_.stream(request.stream_id_) != nullptr;
    if (live) {
        impl_->connection_.release_stream_lease(request.stream_id_);
    }
    auto* endpoint = std::exchange(request.endpoint_, nullptr);
    request.stream_id_ = 0;
    detail::http_request_access::reset(request.request_);
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
