#include <optional>
#include <utility>
#include <variant>

#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_response_writer.h"

#include "http3/http3_connection_impl.h"

namespace ruvia {
namespace {

// A field list over the peer's advertised limit is reported as the peer's
// limit, not as a local encoder failure.
template <typename encoded_type>
[[nodiscard]] std::variant<encoded_type, http3_response_head_failure> response_encoding_result(
    std::variant<encoded_type, http3_response_head_failure> result_value, http3_field_section_limits local_limits,
    const std::optional<http3_settings>& peer_settings) {
    if ((result_value.index() != 0) && std::get<1>(result_value).kind_ == http3_response_head_error::field_section_error &&
        std::get<1>(result_value).field_section_error_ == http3_field_section_error::field_list_too_large &&
        peer_settings && peer_settings->max_field_section_size_ &&
        std::cmp_less_equal(*peer_settings->max_field_section_size_, local_limits.max_decoded_bytes_)) {
        std::get<1>(result_value).kind_ = http3_response_head_error::peer_field_section_limit;
    }
    return result_value;
}

[[nodiscard]] constexpr http3_response_head_failure invalid_response_stream() noexcept {
    return {http3_response_head_error::field_section_error, http3_field_section_error::invalid_prefix};
}

// Response sections travel on request streams or on server push streams.
[[nodiscard]] constexpr bool is_response_stream_id(std::uint64_t stream_id) noexcept {
    return is_http3_request_stream_id(stream_id) || http3_stream_id_type(stream_id) == http3_stream_id_type::server_unidirectional;
}

}  // namespace

std::variant<std::pmr::vector<char>, http3_qpack_connection_error> http3_connection::encode_field_section(
    std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value) {
    auto* impl = impl_type::require_open(impl_);
    if (impl == nullptr || !(is_http3_request_stream_id(stream_id) ||
                               (impl->role_ == http3_peer_role::server && http3_stream_id_type(stream_id) == http3_stream_id_type::server_unidirectional))) {
        return http3_qpack_connection_error::invalid_stream_id;
    }
    impl->field_encoding_started_ = true;
    return impl->transmit_qpack_->encode(stream_id, fields_value, impl->outbound_field_limits({}));
}

std::variant<http3_client_request_head, http3_client_request_head_failure> http3_connection::encode_client_request_head(
    std::uint64_t stream_id, http3_client_request_head_view view, http3_field_section_limits limits) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::client);
    if (impl == nullptr || !is_http3_request_stream_id(stream_id)) {
        return http3_client_request_head_failure{http3_client_request_head_error::field_section_error, http3_field_section_error::invalid_prefix};
    }
    impl->field_encoding_started_ = true;
    const auto& settings = impl->effective_peer_settings();
    view.peer_enable_connect_protocol_ = settings && settings->enable_connect_protocol_;
    return encode_http3_client_request_head(*impl->transmit_qpack_, stream_id, view, impl->outbound_field_limits(limits), resource_);
}

std::variant<http3_headers_frame, http3_request_trailer_error> http3_connection::encode_request_trailers(std::uint64_t stream_id,
    std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::client);
    if (impl == nullptr || !is_http3_request_stream_id(stream_id)) {
        return http3_request_trailer_error::qpack_encoding_failed;
    }
    impl->field_encoding_started_ = true;
    return encode_http3_request_trailers(*impl->transmit_qpack_, stream_id, fields_value, impl->outbound_field_limits(limits), resource_);
}

std::variant<http3_response_head, http3_response_head_failure> http3_connection::encode_connect_response_head(
    std::uint64_t stream_id, const http_response& response, http3_field_section_limits limits) {
    if (!is_http3_request_stream_id(stream_id) || !response.status().is_successful() || !response.body_bytes().empty() || response.file_body().has_value()) {
        return http3_response_head_failure{http3_response_head_error::invalid_field};
    }
    return encode_response_head(stream_id, response, plan_buffered_http_response_write(http_known_method::connect, response), limits);
}

std::variant<http3_response_head, http3_response_head_failure> http3_connection::encode_response_head(std::uint64_t stream_id, const http_response& response, http_buffered_response_write_plan plan, http3_field_section_limits limits) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::server);
    if (impl == nullptr || !is_response_stream_id(stream_id)) {
        return invalid_response_stream();
    }
    const auto local_limits = impl->local_field_limits(limits);
    return response_encoding_result(
        encode_http3_response_head(*impl->transmit_qpack_, stream_id, response, plan, impl->outbound_field_limits(local_limits), resource_),
        local_limits, impl->control_.peer_settings());
}

std::variant<http3_streaming_response_head, http3_response_head_failure> http3_connection::encode_streaming_response_head(std::uint64_t stream_id, http_response response, http_known_method method, http_response_stream_kind kind, http_response_trailer_intent trailers, http3_field_section_limits limits) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::server);
    if (impl == nullptr || !is_response_stream_id(stream_id)) {
        return invalid_response_stream();
    }
    const auto local_limits = impl->local_field_limits(limits);
    return response_encoding_result(
        encode_http3_streaming_response_head(*impl->transmit_qpack_, stream_id, std::move(response), method, kind, trailers, impl->outbound_field_limits(local_limits), resource_),
        local_limits, impl->control_.peer_settings());
}

std::variant<http3_response_head, http3_response_head_failure> http3_connection::encode_interim_response_head(std::uint64_t stream_id, const http_interim_response_head& response, http3_field_section_limits limits) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::server);
    if (impl == nullptr || !is_response_stream_id(stream_id)) {
        return invalid_response_stream();
    }
    const auto local_limits = impl->local_field_limits(limits);
    return response_encoding_result(
        encode_http3_interim_response_head(*impl->transmit_qpack_, stream_id, response, impl->outbound_field_limits(local_limits), resource_),
        local_limits, impl->control_.peer_settings());
}

std::variant<http3_headers_frame, http3_response_head_failure> http3_connection::encode_response_trailers(std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::server);
    if (impl == nullptr || !is_response_stream_id(stream_id)) {
        return invalid_response_stream();
    }
    const auto local_limits = impl->local_field_limits(limits);
    return response_encoding_result(
        encode_http3_response_trailers(*impl->transmit_qpack_, stream_id, fields_value, impl->outbound_field_limits(local_limits), resource_),
        local_limits, impl->control_.peer_settings());
}

}  // namespace ruvia
