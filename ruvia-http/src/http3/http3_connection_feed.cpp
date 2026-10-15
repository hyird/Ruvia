#include <algorithm>
#include <utility>
#include <variant>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_header.h"

#include "http3/http3_connection_impl.h"
#include "http3/http3_message_head_error.h"
#include "http3/http3_request_content.h"
#include "http3/http3_trailer_collector.h"

namespace ruvia {
namespace {

http3_connection_error_code map_peer_error(http3_peer_stream_error error) noexcept {
    switch (error) {
        case http3_peer_stream_error::stream_creation_error:
            return http3_connection_error_code::stream_creation_error;
        case http3_peer_stream_error::closed_critical_stream:
            return http3_connection_error_code::closed_critical_stream;
        case http3_peer_stream_error::excessive_load:
            return http3_connection_error_code::excessive_load;
    }
    return http3_connection_error_code::general_protocol_error;
}

http3_connection_error_code map_control_error(http3_control_stream_status status) noexcept {
    switch (status) {
        case http3_control_stream_status::closed_critical_stream:
            return http3_connection_error_code::closed_critical_stream;
        case http3_control_stream_status::missing_settings:
            return http3_connection_error_code::missing_settings;
        case http3_control_stream_status::frame_unexpected:
            return http3_connection_error_code::frame_unexpected;
        case http3_control_stream_status::settings_error:
            return http3_connection_error_code::settings_error;
        case http3_control_stream_status::id_error:
            return http3_connection_error_code::id_error;
        case http3_control_stream_status::frame_error:
            return http3_connection_error_code::frame_error;
        case http3_control_stream_status::limit:
            return http3_connection_error_code::excessive_load;
        case http3_control_stream_status::need_more_data:
            break;
    }
    return http3_connection_error_code::no_error;
}

bool valid_request_trailer_policy(http3_field_section_field_view field) noexcept {
    return is_valid_http_header_value(field.value_) &&
           !detail::is_forbidden_http_request_trailer_name(field.name_);
}

using trailer_collector = detail::http3_trailer_collector<valid_request_trailer_policy>;

}  // namespace

void http3_connection::impl_type::on_request_frame(void* opaque, http3_stream_frame_event frame) {
    auto& request = *static_cast<request_type*>(opaque);
    if (request.terminal_ || request.callback_result_.scope_ != http3_connection_error_scope::none) {
        return;
    }
    if (frame.kind_ == http3_stream_frame_event_kind::data) {
        if (!frame.payload_.empty()) {
            if (request.connect_) {
                const http3_connection_event event{.kind_ = http3_connection_event_kind::tunnel_data,
                    .stream_id_ = request.stream_id_,
                    .body_ = frame.payload_};
                request.callback_(request.callback_context_, event);
            } else {
                const auto body_result = request.body_->feed(frame.payload_.size(), false);
                if (body_result == http3_message_body_result::payload_not_allowed ||
                    body_result == http3_message_body_result::content_length_exceeded ||
                    body_result == http3_message_body_result::length_overflow) {
                    request.callback_result_ = stream_error(http3_connection_error_code::message_error);
                    request.terminal_ = true;
                    return;
                }
                const http3_connection_event event{.kind_ = http3_connection_event_kind::body,
                    .stream_id_ = request.stream_id_,
                    .body_ = frame.payload_};
                request.callback_(request.callback_context_, event);
            }
        }
        if (frame.fin_) {
            finish(request);
        }
        return;
    }
    if (frame.kind_ != http3_stream_frame_event_kind::headers || !frame.end_frame_) {
        return;
    }
    if (!frame.trailers_) {
        const http3_message_head_limits head_limits{.max_field_section_size_ = request.limits_.max_field_section_size_,
            .max_fields_ = request.limits_.max_fields_,
            .max_encoded_bytes_ = request.limits_.max_encoded_field_section_bytes_};
        auto result_value = decode_http3_message_head(request.decoder_, request.stream_id_, frame.payload_,
            http3_message_head_kind::request, request.head_resource_, head_limits);
        if ((result_value.index() == 0) && std::holds_alternative<http3_qpack_blocked>(std::get<0>(result_value))) {
            request.frames_.pause();
            return;
        }
        auto decoded = [&]() -> std::variant<http3_message_head, http3_message_head_error> {
            if ((result_value.index() != 0)) {
                return std::get<1>(result_value);
            }
            return std::move(std::get<http3_message_head>(std::get<0>(result_value)));
        }();
        if ((decoded.index() != 0)) {
            request.callback_result_ = request_error(detail::http3_message_head_error_code(std::get<1>(decoded)));
            request.terminal_ = true;
            return;
        }
        if (!std::get<0>(decoded).protocol_.empty() && !request.limits_.enable_connect_protocol_) {
            request.callback_result_ = stream_error(http3_connection_error_code::message_error);
            request.terminal_ = true;
            return;
        }
        request.connect_ = std::get<0>(decoded).method_ == "CONNECT";
        if (!request.connect_) {
            request.body_.emplace(std::get<0>(decoded).content_length_,
                detail::http3_request_payload_allowed(std::get<0>(decoded)));
        }
        const http3_connection_event event{.kind_ = http3_connection_event_kind::request_head,
            .stream_id_ = request.stream_id_,
            .head_ = &std::get<0>(decoded)};
        request.callback_(request.callback_context_, event);
        return;
    }

    if (request.connect_) {
        request.callback_result_ = stream_error(http3_connection_error_code::message_error);
        request.terminal_ = true;
        return;
    }
    trailer_collector collector_value(request.resource_, {request.limits_.max_encoded_field_section_bytes_,
                                                             request.limits_.max_field_section_size_, request.limits_.max_fields_});
    const auto decoded = request.decoder_.decode(request.stream_id_, frame.payload_, trailer_collector::collect, &collector_value);
    if ((decoded.index() == 0) && std::get<0>(decoded).status_ == http3_qpack_decode_status::blocked) {
        request.frames_.pause();
        return;
    }
    if ((decoded.index() != 0) || !collector_value.valid_) {
        request.callback_result_ = request_error((decoded.index() != 0)
                                                     ? (std::get<1>(decoded) == http3_qpack_connection_error::limit ? http3_connection_error_code::excessive_load
                                                                                                                    : http3_connection_error_code::qpack_decompression_failed)
                                                 : collector_value.limit_exceeded_ ? http3_connection_error_code::excessive_load
                                                                                   : http3_connection_error_code::message_error);
        request.terminal_ = true;
        return;
    }
    for (const auto& field : collector_value.fields_) {
        const http3_connection_event event{.kind_ = http3_connection_event_kind::trailer_field,
            .stream_id_ = request.stream_id_,
            .trailer_ = {field.name_, field.value_, field.never_indexed_}};
        request.callback_(request.callback_context_, event);
    }
}

void http3_connection::impl_type::finish(request_type& request) {
    if (request.terminal_) {
        return;
    }
    if (!request.connect_) {
        const auto result_value = request.body_->feed(0, true);
        if (result_value == http3_message_body_result::content_length_mismatch ||
            result_value == http3_message_body_result::payload_not_allowed) {
            request.callback_result_ = stream_error(http3_connection_error_code::message_error);
            request.terminal_ = true;
            return;
        }
    }
    request.terminal_ = true;
    const http3_connection_event event{.kind_ = http3_connection_event_kind::message_end,
        .stream_id_ = request.stream_id_};
    request.callback_(request.callback_context_, event);
}

void http3_connection::impl_type::on_client_response(void* opaque, const http3_client_response_event& response_event) {
    auto& request = *static_cast<client_request_type*>(opaque);
    if (response_event.kind_ == http3_client_response_event_kind::push_promise) {
        if (!request.owner_.accept_promise(*response_event.push_id_, *response_event.head_)) {
            return;
        }
    }
    if (response_event.kind_ == http3_client_response_event_kind::informational_head ||
        response_event.kind_ == http3_client_response_event_kind::final_head) {
        request.response_observed_ = true;
    }
    const auto kind = [&] {
        switch (response_event.kind_) {
            case http3_client_response_event_kind::push_promise:
                return http3_connection_event_kind::push_promise;
            case http3_client_response_event_kind::informational_head:
                return http3_connection_event_kind::informational_head;
            case http3_client_response_event_kind::final_head:
                return http3_connection_event_kind::final_head;
            case http3_client_response_event_kind::tunnel_data:
                return http3_connection_event_kind::tunnel_data;
            case http3_client_response_event_kind::body:
                return http3_connection_event_kind::body;
            case http3_client_response_event_kind::trailer_field:
                return http3_connection_event_kind::trailer_field;
            case http3_client_response_event_kind::message_end:
                return http3_connection_event_kind::message_end;
            case http3_client_response_event_kind::reset:
                return http3_connection_event_kind::reset;
        }
        return http3_connection_event_kind::body;
    }();
    const http3_connection_event event{.kind_ = kind,
        .stream_id_ = response_event.stream_id_,
        .head_ = response_event.head_,
        .trailer_ = response_event.trailer_,
        .body_ = response_event.body_,
        .response_body_plan_ = response_event.response_body_plan_,
        .push_id_ = response_event.push_id_,
        .request_content_signal_ = response_event.request_content_signal_};
    request.callback_(request.callback_context_, event);
}

bool http3_connection::impl_type::accept_promise(std::uint64_t id, const http3_message_head& head) {
    if (!pushes_.contains(id) && pushes_.size() >= limits_.max_remembered_pushes_) {
        failed_ = connection_error(http3_connection_error_code::excessive_load);
        return false;
    }
    auto& push = pushes_[id];
    if (push.head_) {
        const auto& prior = *push.head_;
        bool same = prior.method_ == head.method_ && prior.scheme_ == head.scheme_ &&
                    prior.authority_ == head.authority_ && prior.path_ == head.path_ && prior.headers_.size() == head.headers_.size();
        for (std::size_t i = 0; same && i < head.headers_.size(); ++i) {
            same = prior.headers_[i].name() == head.headers_[i].name() && prior.headers_[i].value() == head.headers_[i].value();
        }
        if (!same) {
            failed_ = connection_error(http3_connection_error_code::general_protocol_error);
        }
        return same;
    }
    push.head_.emplace(head.copy(resource_));
    return true;
}

void http3_connection::impl_type::on_control(void* opaque, http3_control_stream_event event) {
    auto& target = *static_cast<control_callback_type*>(opaque);
    auto& owner_value = target.owner_;
    if (owner_value.failed_.scope_ != http3_connection_error_scope::none || !owner_value.prepare_peer_settings()) {
        return;
    }
    if (event.kind_ == http3_stream_frame_event_kind::origin) {
        if (owner_value.limits_.receive_origin_advertisements_ && event.origin_advertisement_) {
            target.callback_(target.context_, {.kind_ = http3_connection_event_kind::origin_advertisement,
                                                  .origin_advertisement_ = event.origin_advertisement_});
        }
        return;
    }
    if (event.kind_ == http3_stream_frame_event_kind::request_priority_update || event.kind_ == http3_stream_frame_event_kind::push_priority_update) {
        if (event.kind_ == http3_stream_frame_event_kind::push_priority_update &&
            (!owner_value.pushes_.contains(event.id_) || !owner_value.pushes_.at(event.id_).head_)) {
            owner_value.failed_ = connection_error(http3_connection_error_code::id_error);
            return;
        }
        if (!event.priority_update_) {
            return;
        }
        target.callback_(target.context_, {.kind_ = http3_connection_event_kind::priority_update,
                                              .stream_id_ = event.priority_update_->push_ ? 0 : event.id_,
                                              .push_id_ = event.priority_update_->push_ ? std::optional(event.id_) : std::nullopt,
                                              .priority_update_ = event.priority_update_});
        return;
    }
    if (event.kind_ != http3_stream_frame_event_kind::cancel_push) {
        return;
    }
    auto found = owner_value.pushes_.find(event.id_);
    const auto maximum = owner_value.role_ == http3_peer_role::client ? owner_value.limits_.max_push_id_ : owner_value.control_.max_push_id();
    if (!maximum || event.id_ > *maximum ||
        (owner_value.role_ == http3_peer_role::server && (found == owner_value.pushes_.end() || !found->second.head_))) {
        owner_value.failed_ = connection_error(http3_connection_error_code::id_error);
        return;
    }
    if (found == owner_value.pushes_.end()) {
        if (owner_value.pushes_.size() >= owner_value.limits_.max_remembered_pushes_) {
            owner_value.failed_ = connection_error(http3_connection_error_code::excessive_load);
            return;
        }
        found = owner_value.pushes_.try_emplace(event.id_).first;
    }
    found->second.canceled_ = true;
    const http3_connection_event canceled{.kind_ = http3_connection_event_kind::push_canceled,
        .stream_id_ = found->second.stream_id_.value_or(0),
        .push_id_ = event.id_};
    target.callback_(target.context_, canceled);
}

void http3_connection::impl_type::on_push_response(void* opaque, const http3_client_response_event& source_value) {
    const auto& target = *static_cast<push_callback_type*>(opaque);
    const auto kind = source_value.kind_ == http3_client_response_event_kind::informational_head ? http3_connection_event_kind::informational_head
                      : source_value.kind_ == http3_client_response_event_kind::final_head       ? http3_connection_event_kind::final_head
                      : source_value.kind_ == http3_client_response_event_kind::body             ? http3_connection_event_kind::body
                      : source_value.kind_ == http3_client_response_event_kind::trailer_field    ? http3_connection_event_kind::trailer_field
                      : source_value.kind_ == http3_client_response_event_kind::reset            ? http3_connection_event_kind::reset
                                                                                                 : http3_connection_event_kind::message_end;
    const http3_connection_event event{.kind_ = kind, .stream_id_ = source_value.stream_id_, .head_ = source_value.head_, .trailer_ = source_value.trailer_, .body_ = source_value.body_, .response_body_plan_ = source_value.response_body_plan_, .push_id_ = target.push_id_};
    target.callback_(target.context_, event);
}

http3_connection_result http3_connection::impl_type::feed_push(std::uint64_t stream_id, std::span<const char> bytes_value, bool fin, bool reset,
    http3_connection_callback_type callback_value, void* context_value) {
    if (!limits_.max_push_id_) {
        return failed_ = connection_error(http3_connection_error_code::id_error);
    }
    if (!push_streams_.contains(stream_id) && push_streams_.size() >= limits_.max_active_streams_) {
        return failed_ = connection_error(http3_connection_error_code::excessive_load);
    }
    auto& stream = push_streams_[stream_id];
    std::size_t consumed = 0;
    while (!stream.id_ && consumed < bytes_value.size() && stream.used_ < 8) {
        stream.id_bytes_[stream.used_++] = bytes_value[consumed++];
        const auto id = decode_http3_var_int(std::span(stream.id_bytes_).first(stream.used_));
        if ((id.index() != 0)) {
            continue;
        }
        if (std::get<0>(id).value_ > *limits_.max_push_id_) {
            return failed_ = connection_error(http3_connection_error_code::id_error);
        }
        if (!pushes_.contains(std::get<0>(id).value_) && pushes_.size() >= limits_.max_remembered_pushes_) {
            return failed_ = connection_error(http3_connection_error_code::excessive_load);
        }
        auto& push = pushes_[std::get<0>(id).value_];
        if (push.stream_id_ && *push.stream_id_ != stream_id) {
            return failed_ = connection_error(http3_connection_error_code::id_error);
        }
        push.stream_id_ = stream_id;
        stream.id_ = std::get<0>(id).value_;
        callback_value(context_value, {.kind_ = http3_connection_event_kind::push_stream, .stream_id_ = stream_id, .push_id_ = std::get<0>(id).value_});
    }
    if (!stream.id_) {
        if (fin || reset) {
            (void)peer_streams_.retire_non_critical_stream(stream_id);
            push_streams_.erase(stream_id);
            return fin && !reset ? stream_error(http3_connection_error_code::frame_error) : http3_connection_result{};
        }
        return {};
    }
    auto& push = pushes_.at(*stream.id_);
    if (reset) {
        const auto id = *stream.id_;
        (void)receive_qpack_.cancel(stream_id);
        push.completed_ = true;
        (void)peer_streams_.retire_non_critical_stream(stream_id);
        push_streams_.erase(stream_id);
        callback_value(context_value, {.kind_ = http3_connection_event_kind::reset, .stream_id_ = stream_id, .push_id_ = id});
        return {http3_connection_status::reset};
    }
    if (push.canceled_) {
        (void)receive_qpack_.cancel(stream_id);
        (void)peer_streams_.retire_non_critical_stream(stream_id);
        push_streams_.erase(stream_id);
        return stream_error(http3_connection_error_code::request_cancelled);
    }
    if (!push.head_) {
        return {http3_connection_status::push_promise_pending, http3_connection_error_scope::none,
            http3_connection_error_code::no_error, consumed};
    }
    if (!stream.response_) {
        stream.response_.emplace(stream_id, classify_http_method(push.head_->method_), resource_,
            http3_client_response_limits{.max_field_section_size_ = limits_.max_field_section_size_, .max_fields_ = limits_.max_fields_, .max_encoded_field_section_bytes_ = limits_.max_encoded_field_section_bytes_, .push_stream_ = true}, &receive_qpack_);
    }
    push_callback_type target{callback_value, context_value, *stream.id_};
    const auto result_value = stream.response_->feed(bytes_value.subspan(consumed), fin, false, on_push_response, &target);
    if (result_value.status_ == http3_client_response_status::qpack_blocked) {
        return {http3_connection_status::qpack_blocked,
            http3_connection_error_scope::none, http3_connection_error_code::no_error, consumed + result_value.consumed_bytes_};
    }
    if (result_value.scope_ == http3_connection_error_scope::connection) {
        return failed_ = connection_error(result_value.code_);
    }
    if (result_value.status_ == http3_client_response_status::message_end || result_value.scope_ == http3_connection_error_scope::stream) {
        push.completed_ = true;
        (void)peer_streams_.retire_non_critical_stream(stream_id);
        push_streams_.erase(stream_id);
        return result_value.scope_ == http3_connection_error_scope::stream ? stream_error(result_value.code_)
                                                                           : http3_connection_result{http3_connection_status::message_end};
    }
    return {};
}

http3_connection_result http3_connection::feed(std::uint64_t stream_id, std::span<const char> bytes_value,
    bool fin, bool reset, http3_connection_callback_type callback_value, void* context_value,
    std::pmr::memory_resource* request_head_resource) {
    if (impl_ == nullptr) {
        return impl_type::connection_error(http3_connection_error_code::general_protocol_error);
    }
    auto& impl = *impl_;
    impl_type::feed_guard_type feed_guard(impl);
    try {
        if (impl.failed_.scope_ == http3_connection_error_scope::connection) {
            return impl.failed_;
        }
        if (callback_value == nullptr) {
            return impl_type::connection_error(http3_connection_error_code::general_protocol_error);
        }
        impl.peer_feed_started_ = true;
        if (impl.push_streams_.contains(stream_id)) {
            return impl.feed_push(stream_id, bytes_value, fin, reset, callback_value, context_value);
        }
        if (is_http3_unidirectional_stream_id(stream_id)) {
            const auto peer = impl.peer_streams_.feed(stream_id, bytes_value, fin, reset);
            if ((peer.index() != 0)) {
                impl.failed_ = impl_type::connection_error(map_peer_error(std::get<1>(peer)));
                return impl.failed_;
            }
            if (std::get<0>(peer).kind_ == http3_peer_stream_kind::push && impl.role_ == http3_peer_role::client) {
                auto result_value = impl.feed_push(stream_id, std::get<0>(peer).remaining_, fin, reset, callback_value, context_value);
                if (result_value.status_ == http3_connection_status::qpack_blocked || result_value.status_ == http3_connection_status::push_promise_pending) {
                    result_value.consumed_bytes_ += std::get<0>(peer).consumed_;
                }
                return result_value;
            }
            if (std::get<0>(peer).closed_) {
                return {http3_connection_status::need_more_data};
            }
            if (std::get<0>(peer).kind_ == http3_peer_stream_kind::unclassified || std::get<0>(peer).kind_ == http3_peer_stream_kind::unknown ||
                std::get<0>(peer).kind_ == http3_peer_stream_kind::push) {
                return {http3_connection_status::need_more_data};
            }
            if (std::get<0>(peer).kind_ == http3_peer_stream_kind::control) {
                impl_type::control_callback_type target{impl, callback_value, context_value};
                const auto status = impl.control_.feed(std::get<0>(peer).remaining_, fin || reset, impl_type::on_control, &target);
                if (impl.failed_.scope_ == http3_connection_error_scope::connection) {
                    return impl.failed_;
                }
                if (status != http3_control_stream_status::need_more_data) {
                    impl.failed_ = impl_type::connection_error(map_control_error(status));
                    return impl.failed_;
                }
                if (!impl.prepare_peer_settings()) {
                    return impl.failed_;
                }
            } else if (std::get<0>(peer).kind_ == http3_peer_stream_kind::qpack_encoder) {
                const auto decoded = impl.receive_qpack_.consume_encoder(std::get<0>(peer).remaining_, fin || reset);
                if ((decoded.index() != 0)) {
                    impl.failed_ = impl_type::connection_error(std::get<1>(decoded) == http3_qpack_connection_error::closed_critical_stream
                                                                   ? http3_connection_error_code::closed_critical_stream
                                                                   : http3_connection_error_code::qpack_encoder_stream_error);
                    return impl.failed_;
                }
            } else if (std::get<0>(peer).kind_ == http3_peer_stream_kind::qpack_decoder) {
                if (const auto consumed = impl.transmit_qpack_->consume_decoder(std::get<0>(peer).remaining_, fin || reset); (consumed.index() != 0)) {
                    impl.failed_ = impl_type::connection_error(http3_connection_error_code::qpack_decoder_stream_error);
                    return impl.failed_;
                }
            }
            return {http3_connection_status::need_more_data};
        }

        if (impl.role_ == http3_peer_role::client) {
            // Client response state exists only for explicitly registered local bidi streams.
            if (!is_http3_request_stream_id(stream_id)) {
                impl.failed_ = impl_type::connection_error(http3_connection_error_code::stream_creation_error);
                return impl.failed_;
            }
            auto found = impl.client_requests_.find(stream_id);
            if (found == impl.client_requests_.end()) {
                return impl_type::stream_error(http3_connection_error_code::stream_creation_error);
            }
            auto& request = found->second;
            request.callback_ = callback_value;
            request.callback_context_ = context_value;
            if (reset) {
                if (auto canceled = impl.receive_qpack_.cancel(stream_id); (canceled.index() != 0)) {
                    impl.failed_ = impl_type::connection_error(http3_connection_error_code::qpack_decompression_failed);
                    return impl.failed_;
                }
            }
            const auto response = request.response_.feed(bytes_value, fin, reset, impl_type::on_client_response, &request);
            if (impl.failed_.scope_ == http3_connection_error_scope::connection) {
                return impl.failed_;
            }
            if (response.status_ == http3_client_response_status::qpack_blocked) {
                return {http3_connection_status::qpack_blocked, http3_connection_error_scope::none,
                    http3_connection_error_code::no_error, response.consumed_bytes_};
            }
            if (response.scope_ == http3_connection_error_scope::connection) {
                impl.failed_ = impl_type::connection_error(response.code_);
                impl.client_requests_.clear();
                return impl.failed_;
            }
            if (response.scope_ == http3_connection_error_scope::stream) {
                impl.client_requests_.erase(found);
                return impl_type::stream_error(response.code_);
            }
            if (response.status_ == http3_client_response_status::message_end) {
                impl.client_requests_.erase(found);
                return {http3_connection_status::message_end};
            }
            if (response.status_ == http3_client_response_status::reset) {
                impl.client_requests_.erase(found);
                return {http3_connection_status::reset};
            }
            return {http3_connection_status::need_more_data};
        }
        if (const auto valid = http3_peer_streams::accept_bidirectional(impl.role_, stream_id); (valid.index() != 0)) {
            impl.failed_ = impl_type::connection_error(http3_connection_error_code::stream_creation_error);
            return impl.failed_;
        }

        auto found = impl.requests_.find(stream_id);
        if (found == impl.requests_.end()) {
            if (reset) {
                // A sender can encode dynamic references and publish encoder
                // instructions before any HEADERS bytes reach this stream.
                // Cancellation must release those references even though there
                // is no request/parser node. No stream tombstone is required.
                if (auto canceled = impl.receive_qpack_.cancel(stream_id); (canceled.index() != 0)) {
                    impl.failed_ = impl_type::connection_error(http3_connection_error_code::qpack_decompression_failed);
                    return impl.failed_;
                }
                return {http3_connection_status::need_more_data};
            }
            if (impl.local_goaway_ && stream_id >= *impl.local_goaway_) {
                return impl_type::stream_error(http3_connection_error_code::request_rejected);
            }
            if (impl.requests_.size() >= impl.limits_.max_active_streams_) {
                return impl_type::stream_error(http3_connection_error_code::excessive_load);
            }
            found = impl.requests_.try_emplace(stream_id, impl.resource_, impl.limits_, impl.receive_qpack_).first;
            found->second.stream_id_ = stream_id;
        }
        auto& request = found->second;
        if (reset) {
            if (auto canceled = impl.receive_qpack_.cancel(stream_id); (canceled.index() != 0)) {
                impl.failed_ = impl_type::connection_error(http3_connection_error_code::qpack_decompression_failed);
                return impl.failed_;
            }
            impl.requests_.erase(found);
            const http3_connection_event event{.kind_ = http3_connection_event_kind::reset, .stream_id_ = stream_id};
            callback_value(context_value, event);
            return {http3_connection_status::need_more_data};
        }
        request.callback_ = callback_value;
        request.callback_context_ = context_value;
        request.callback_result_ = {};
        request.head_resource_ = request_head_resource != nullptr ? request_head_resource : impl.resource_;
        const auto status = request.frames_.feed(bytes_value, fin, impl_type::on_request_frame, &request);
        if (status == http3_stream_frame_status::paused) {
            return {http3_connection_status::qpack_blocked, http3_connection_error_scope::none,
                http3_connection_error_code::no_error, request.frames_.consumed_bytes()};
        }
        // Framing continues to consume the same input after a message callback
        // reports a stream error. A later connection-level frame error must not be
        // hidden by that earlier stream failure.
        if (const auto code = http3_connection_error_code_for_stream_frame_status(status)) {
            impl.requests_.erase(found);
            impl.failed_ = impl_type::connection_error(*code);
            return impl.failed_;
        }
        if (request.callback_result_.scope_ != http3_connection_error_scope::none) {
            const auto result_value = request.callback_result_;
            if (result_value.scope_ == http3_connection_error_scope::connection) {
                impl.failed_ = result_value;
            }
            impl.requests_.erase(found);
            return result_value;
        }
        if (status == http3_stream_frame_status::message_end && !request.terminal_) {
            impl_type::finish(request);
            if (request.callback_result_.scope_ != http3_connection_error_scope::none) {
                const auto result_value = request.callback_result_;
                if (result_value.scope_ == http3_connection_error_scope::connection) {
                    impl.failed_ = result_value;
                }
                impl.requests_.erase(found);
                return result_value;
            }
        }
        if (status == http3_stream_frame_status::message_end || request.terminal_) {
            impl.requests_.erase(found);
            return {http3_connection_status::message_end};
        }
        return {http3_connection_status::need_more_data};
    } catch (...) {
        // A callback or allocator may have left an incomplete frame/event in
        // this feed. Replaying the input is unsafe, even on another stream.
        impl.failed_ = impl_type::connection_error(http3_connection_error_code::internal_error);
        throw;
    }
}

}  // namespace ruvia
