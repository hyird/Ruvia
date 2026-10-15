#include <array>
#include <span>
#include <variant>

#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_var_int.h"

#include "http3/http3_connection_impl.h"

namespace ruvia {
namespace {

struct control_id_frame final {
    std::array<char, http3_frame_header_max_bytes + http3_var_int_max_bytes> bytes_{};
    std::size_t size_{0};

    control_id_frame(std::uint64_t type, std::uint64_t id) noexcept {
        const auto frame = encode_http3_frame_header(bytes_, type, http3_var_int_encoded_size(id));
        const auto value = encode_http3_var_int(std::span(bytes_).subspan(std::get<0>(frame)), id);
        size_ = std::get<0>(frame) + std::get<0>(value);
    }
    [[nodiscard]] std::span<const char> bytes() const noexcept {
        return std::span<const char>(bytes_).first(size_);
    }
};

}  // namespace

http3_connection_error_code http3_connection::queue_max_push_id(std::uint64_t maximum) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::client);
    if (impl == nullptr || maximum > http3_var_int_max || (impl->limits_.max_push_id_ && maximum < *impl->limits_.max_push_id_)) {
        return http3_connection_error_code::id_error;
    }
    const control_id_frame frame(0xd, maximum);
    impl->reserve_control_output(frame.size_);
    impl->limits_.max_push_id_ = maximum;
    for (auto& [id, request] : impl->client_requests_) {
        (void)request.response_.authorize_push(maximum);
    }
    impl->append_control_output(frame.bytes());
    return http3_connection_error_code::no_error;
}

http3_connection_error_code http3_connection::queue_cancel_push(std::uint64_t push_id) {
    auto* impl = impl_type::require_open(impl_);
    if (impl == nullptr || !impl->pushes_.contains(push_id) || !impl->pushes_.at(push_id).head_) {
        return http3_connection_error_code::id_error;
    }
    const control_id_frame frame(3, push_id);
    impl->reserve_control_output(frame.size_);
    impl->pushes_.at(push_id).canceled_ = true;
    impl->append_control_output(frame.bytes());
    return http3_connection_error_code::no_error;
}

http3_connection_error_code http3_connection::queue_goaway(std::uint64_t id) {
    auto* impl = impl_type::require_open(impl_);
    if (impl == nullptr || id > http3_var_int_max ||
        (impl->role_ == http3_peer_role::server && !is_http3_request_stream_id(id)) ||
        (impl->local_goaway_ && id > *impl->local_goaway_)) {
        return http3_connection_error_code::id_error;
    }
    const control_id_frame frame(7, id);
    impl->reserve_control_output(frame.size_);
    impl->local_goaway_ = id;
    impl->append_control_output(frame.bytes());
    return http3_connection_error_code::no_error;
}

http3_connection_error_code http3_connection::queue_priority_update(http_priority_update update) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::client);
    if (impl == nullptr ||
        (update.push_ && (!impl->pushes_.contains(update.element_id_) || !impl->pushes_.at(update.element_id_).head_))) {
        return http3_connection_error_code::id_error;
    }
    std::array<char, 32> buffer{};
    const auto size = encode_http3_priority_update(buffer, update);
    if ((size.index() != 0)) {
        return http3_connection_error_code::message_error;
    }
    impl->reserve_control_output(std::get<0>(size));
    impl->append_control_output(std::span<const char>(buffer).first(std::get<0>(size)));
    return http3_connection_error_code::no_error;
}

http3_connection_error_code http3_connection::queue_origin_advertisement(std::span<const std::string_view> origins) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::server);
    if (impl == nullptr) {
        return http3_connection_error_code::general_protocol_error;
    }
    const auto bytes_value = encode_http3_origin_frame(origins, impl->limits_.max_encoded_field_section_bytes_, resource_);
    if ((bytes_value.index() != 0)) {
        return http3_connection_error_code::message_error;
    }
    impl->reserve_control_output(std::get<0>(bytes_value).size());
    impl->append_control_output(std::get<0>(bytes_value));
    return http3_connection_error_code::no_error;
}

std::variant<http3_push_stream_prefix, http3_connection_error_code> http3_connection::prepare_push_stream(std::uint64_t stream_id, std::uint64_t push_id) {
    auto* impl = impl_type::require_open(impl_, http3_peer_role::server);
    if (impl == nullptr || http3_stream_id_type(stream_id) != http3_stream_id_type::server_unidirectional ||
        !impl->pushes_.contains(push_id) || !impl->pushes_.at(push_id).head_) {
        return http3_connection_error_code::id_error;
    }
    auto& push = impl->pushes_.at(push_id);
    if (push.stream_id_ || push.canceled_) {
        return http3_connection_error_code::request_cancelled;
    }
    for (const auto& [id, previous] : impl->pushes_) {
        if (previous.stream_id_ == stream_id) {
            return http3_connection_error_code::stream_creation_error;
        }
    }
    http3_push_stream_prefix prefix;
    prefix.bytes_[0] = 1;
    prefix.size_ = 1 + std::get<0>(encode_http3_var_int(std::span(prefix.bytes_).subspan(1), push_id));
    push.stream_id_ = stream_id;
    return prefix;
}

std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_connection::prepare_push_promise(
    std::uint64_t associated_stream_id, std::uint64_t push_id, http_push_request_view request) {
    using error_type = http3_connection_error_code;
    auto* impl = impl_type::require_open(impl_, http3_peer_role::server);
    if (impl == nullptr || !is_http3_request_stream_id(associated_stream_id)) {
        return error_type::general_protocol_error;
    }
    const auto max_id = impl->control_.max_push_id();
    if (!max_id || push_id > *max_id) {
        return error_type::id_error;
    }
    if (impl->control_.goaway_id()) {
        return error_type::request_rejected;
    }
    if (const auto found = impl->pushes_.find(push_id); found != impl->pushes_.end() && found->second.canceled_) {
        return error_type::request_cancelled;
    }
    if (request.method_ != "GET" && request.method_ != "HEAD") {
        return error_type::message_error;
    }
    std::pmr::vector<http3_field_section_field_view> fields(resource_);
    for (const auto& field : request.headers_) {
        fields.push_back({field.name(), field.value(), false});
    }
    const auto limits = impl->outbound_field_limits({impl->limits_.max_encoded_field_section_bytes_,
        impl->limits_.max_field_section_size_, impl->limits_.max_fields_});
    auto encoded = encode_http3_client_request_head({.method_ = request.method_, .scheme_ = request.scheme_, .authority_ = request.authority_, .path_ = request.path_, .fields_ = fields}, limits, resource_);
    if ((encoded.index() != 0)) {
        return error_type::message_error;
    }
    const auto section = std::get<0>(encoded).headers_.field_section();
    auto head = decode_http3_message_head(section, http3_message_head_kind::request, resource_,
        {impl->limits_.max_field_section_size_, impl->limits_.max_fields_, impl->limits_.max_encoded_field_section_bytes_});
    if ((head.index() != 0) || (std::get<0>(head).content_length_ && *std::get<0>(head).content_length_ != 0)) {
        return error_type::message_error;
    }
    std::array<char, http3_frame_header_max_bytes + http3_var_int_max_bytes> prefix{};
    const auto id_size = http3_var_int_encoded_size(push_id);
    const auto frame_size = encode_http3_frame_header(prefix, 5, id_size + section.size());
    const auto id = encode_http3_var_int(std::span(prefix).subspan(std::get<0>(frame_size)), push_id);
    std::pmr::vector<char> output(resource_);
    output.reserve(std::get<0>(frame_size) + std::get<0>(id) + section.size());
    output.insert(output.end(), prefix.begin(), prefix.begin() + std::get<0>(frame_size) + std::get<0>(id));
    output.insert(output.end(), section.begin(), section.end());
    if (!impl->accept_promise(push_id, std::get<0>(head))) {
        return impl->failed_.code_;
    }
    return output;
}

}  // namespace ruvia
