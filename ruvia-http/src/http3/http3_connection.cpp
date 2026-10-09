#include "ruvia/http/http3_connection.h"

#include <algorithm>
#include <array>
#include <memory_resource>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_client_response.h"
#include "ruvia/http/http3_control_stream.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_qpack_streams.h"
#include "ruvia/http/http3_stream_frames.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response_stream.h"

#include "http3/http3_trailer_collector.h"

namespace ruvia {
namespace {

http3_connection_result connection_error(http3_connection_error_code code) noexcept {
    return {http3_connection_status::connection_error, http3_connection_error_scope::connection, code};
}

http3_connection_result stream_error(http3_connection_error_code code) noexcept {
    return {http3_connection_status::stream_error, http3_connection_error_scope::stream, code};
}

http3_connection_result request_error(http3_connection_error_code code) noexcept {
    if (code == http3_connection_error_code::qpack_decompression_failed ||
        code == http3_connection_error_code::excessive_load) {
        return connection_error(code);
    }
    return stream_error(code);
}

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

}  // namespace

struct http3_connection::impl_type final {
    struct request_type final {
        request_type(std::pmr::memory_resource* resource, const http3_connection_config& limits, http3_qpack_decoder& decoder)
            : frames_(http3_stream_kind::request, resource,
                  {.max_field_section_size_ = limits.max_encoded_field_section_bytes_}),
              resource_(resource),
              limits_(limits),
              decoder_(decoder) {}

        http3_stream_frames frames_;
        std::pmr::memory_resource* resource_;
        http3_connection_config limits_;
        http3_qpack_decoder& decoder_;
        std::optional<http3_message_body> body_;
        bool connect_{false};
        bool terminal_{false};
        http3_connection_result callback_result_{};
        http3_connection_callback_type callback_{nullptr};
        void* callback_context_{nullptr};
        std::uint64_t stream_id_{0};
    };

    struct client_request_type final {
        client_request_type(std::uint64_t stream_id, http_known_method method, std::pmr::memory_resource* resource,
            const http3_connection_config& limits, http3_qpack_decoder& decoder, impl_type& owner_value)
            : response_(stream_id, method, resource,
                  {.max_field_section_size_ = limits.max_field_section_size_,
                      .max_fields_ = limits.max_fields_,
                      .max_encoded_field_section_bytes_ = limits.max_encoded_field_section_bytes_,
                      .max_push_id_ = limits.max_push_id_},
                  &decoder),
              owner_(owner_value) {}
        http3_client_response response_;
        impl_type& owner_;
        bool response_observed_{false};
        http3_connection_callback_type callback_{nullptr};
        void* callback_context_{nullptr};
    };

    using trailer_collector = detail::http3_trailer_collector<valid_request_trailer_policy>;

    struct feed_guard_type final {
        explicit feed_guard_type(impl_type& impl)
            : impl_(impl) {
            if (impl.feeding_) {
                throw std::logic_error("recursive http3_connection::feed()");
            }
            impl.feeding_ = true;
        }
        ~feed_guard_type() {
            impl_.feeding_ = false;
        }
        impl_type& impl_;
    };

    impl_type(http3_peer_role role, std::pmr::memory_resource* resource, http3_connection_config limits)
        : role_(role),
          resource_(resource),
          limits_(limits),
          peer_streams_(role, resource, {.max_active_streams_ = limits.max_peer_unidirectional_streams_}),
          requests_(resource),
          client_requests_(resource),
          pushes_(resource),
          push_streams_(resource),
          control_(role == http3_peer_role::server ? http3_control_role::server : http3_control_role::client,
              resource,
              {.max_field_section_size_ = limits.max_encoded_field_section_bytes_}),
          receive_qpack_({.max_table_capacity_ = limits.qpack_max_table_capacity_,
                             .max_blocked_streams_ = limits.qpack_blocked_streams_,
                             .fields_ = {limits.max_encoded_field_section_bytes_, limits.max_field_section_size_, limits.max_fields_}},
              resource),
          transmit_qpack_(std::in_place, http3_qpack_encoder_config{.max_table_capacity_ = 0, .max_blocked_streams_ = 0}, resource) {}

    [[nodiscard]] http3_field_section_limits local_field_limits(http3_field_section_limits requested) const noexcept {
        requested.max_encoded_bytes_ = std::min(requested.max_encoded_bytes_, limits_.max_encoded_field_section_bytes_);
        requested.max_decoded_bytes_ = std::min(requested.max_decoded_bytes_, limits_.max_field_section_size_);
        requested.max_fields_ = std::min(requested.max_fields_, limits_.max_fields_);
        return requested;
    }

    [[nodiscard]] http3_field_section_limits outbound_field_limits(http3_field_section_limits requested) const noexcept {
        requested = local_field_limits(requested);
        // SETTINGS_MAX_FIELD_SECTION_SIZE limits decoded names, values and the
        // per-field overhead, not compressed QPACK bytes. Before SETTINGS (or
        // when the setting is absent), only the local budget applies.
        const auto& settings = control_.peer_settings();
        if (settings && settings->max_field_section_size_ &&
            std::cmp_less(*settings->max_field_section_size_, requested.max_decoded_bytes_)) {
            requested.max_decoded_bytes_ = static_cast<std::size_t>(*settings->max_field_section_size_);
        }
        return requested;
    }

    template <typename encoded_type>
    [[nodiscard]] std::variant<encoded_type, http3_response_head_failure> response_encoding_result(
        std::variant<encoded_type, http3_response_head_failure> result_value, http3_field_section_limits local_limits) const {
        const auto& settings = control_.peer_settings();
        if ((result_value.index() != 0) && std::get<1>(result_value).kind_ == http3_response_head_error::field_section_error &&
            std::get<1>(result_value).field_section_error_ == http3_field_section_error::field_list_too_large &&
            settings && settings->max_field_section_size_ &&
            std::cmp_less_equal(*settings->max_field_section_size_, local_limits.max_decoded_bytes_)) {
            std::get<1>(result_value).kind_ = http3_response_head_error::peer_field_section_limit;
        }
        return result_value;
    }

    static void on_request_frame(void* opaque, http3_stream_frame_event frame) {
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
                http3_message_head_kind::request, request.resource_, head_limits);
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
                request.callback_result_ = request_error(std::get<1>(decoded) == http3_message_head_error::qpack_decompression_failed
                                                             ? http3_connection_error_code::qpack_decompression_failed
                                                         : std::get<1>(decoded) == http3_message_head_error::field_section_too_large
                                                             ? http3_connection_error_code::excessive_load
                                                             : http3_connection_error_code::message_error);
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
                request.body_.emplace(std::get<0>(decoded).content_length_, true);
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
        trailer_collector collector_value(request.resource_);
        const auto decoded = request.decoder_.decode(request.stream_id_, frame.payload_, trailer_collector::collect, &collector_value);
        if ((decoded.index() == 0) && std::get<0>(decoded).status_ == http3_qpack_decode_status::blocked) {
            request.frames_.pause();
            return;
        }
        if ((decoded.index() != 0) || !collector_value.valid_) {
            request.callback_result_ = request_error((decoded.index() != 0)
                                                         ? (std::get<1>(decoded) == http3_qpack_connection_error::limit ? http3_connection_error_code::excessive_load
                                                                                                                        : http3_connection_error_code::qpack_decompression_failed)
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

    static void finish(request_type& request) {
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

    static void on_client_response(void* opaque, const http3_client_response_event& response_event) {
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

    bool accept_promise(std::uint64_t id, const http3_message_head& head) {
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
                same = prior.headers_[i].name_ == head.headers_[i].name_ && prior.headers_[i].value_ == head.headers_[i].value_;
            }
            if (!same) {
                failed_ = connection_error(http3_connection_error_code::general_protocol_error);
            }
            return same;
        }
        push.head_.emplace(resource_);
        push.head_->method_ = head.method_;
        push.head_->scheme_ = head.scheme_;
        push.head_->authority_ = head.authority_;
        push.head_->path_ = head.path_;
        push.head_->content_length_ = head.content_length_;
        for (const auto& field : head.headers_) {
            push.head_->headers_.emplace_back(field.name_, field.value_, resource_);
        }
        return true;
    }
    struct control_callback_type final {
        impl_type& owner_;
        http3_connection_callback_type callback_;
        void* context_;
    };
    static void on_control(void* opaque, http3_control_stream_event event) {
        auto& target = *static_cast<control_callback_type*>(opaque);
        auto& owner_value = target.owner_;
        if (owner_value.failed_.scope_ != http3_connection_error_scope::none) {
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
    struct push_callback_type final {
        http3_connection_callback_type callback_;
        void* context_;
        std::uint64_t push_id_;
    };
    static void on_push_response(void* opaque, const http3_client_response_event& source_value) {
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
    http3_connection_result feed_push(std::uint64_t stream_id, std::span<const char> bytes_value, bool fin, bool reset,
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

    http3_peer_role role_;
    std::pmr::memory_resource* resource_;
    http3_connection_config limits_;
    http3_peer_streams peer_streams_;
    std::pmr::unordered_map<std::uint64_t, request_type> requests_;
    std::pmr::unordered_map<std::uint64_t, client_request_type> client_requests_;
    struct push_type final {
        std::optional<http3_message_head> head_;
        std::optional<std::uint64_t> stream_id_;
        bool completed_{false};
        bool canceled_{false};
    };
    struct push_stream_type final {
        std::array<char, 8> id_bytes_{};
        std::size_t used_{0};
        std::optional<std::uint64_t> id_;
        std::optional<http3_client_response> response_;
    };
    std::pmr::unordered_map<std::uint64_t, push_type> pushes_;
    std::pmr::unordered_map<std::uint64_t, push_stream_type> push_streams_;
    bool feeding_{false};
    http3_control_stream control_;
    http3_qpack_decoder receive_qpack_;
    std::optional<http3_qpack_encoder> transmit_qpack_;
    bool transmit_qpack_configured_{false};
    http3_connection_result failed_{};
    std::optional<std::uint64_t> local_goaway_;
};

http3_connection::http3_connection(http3_peer_role local_role, std::pmr::memory_resource* resource,
    http3_connection_config limits)
    : resource_(resource),
      impl_(nullptr) {
    if (resource == nullptr || limits.max_active_streams_ == 0 || limits.max_peer_unidirectional_streams_ == 0 || limits.max_remembered_pushes_ == 0 ||
        (limits.max_push_id_ && *limits.max_push_id_ > http3_var_int_max)) {
        throw std::invalid_argument("HTTP/3 connection resource and limits must be valid");
    }
    std::pmr::polymorphic_allocator<impl_type> allocator(resource);
    impl_ = allocator.allocate(1);
    try {
        std::allocator_traits<decltype(allocator)>::construct(allocator, impl_, local_role, resource, limits);
    } catch (...) {
        allocator.deallocate(impl_, 1);
        impl_ = nullptr;
        throw;
    }
}

http3_connection::~http3_connection() {
    if (impl_ != nullptr && !retire()) {
        std::terminate();
    }
}

http3_connection::http3_connection(http3_connection&& other) noexcept
    : resource_(other.resource_),
      impl_(std::exchange(other.impl_, nullptr)) {}

http3_connection& http3_connection::operator=(http3_connection&& other) noexcept {
    if (this != &other) {
        http3_connection temporary(std::move(other));
        std::swap(resource_, temporary.resource_);
        std::swap(impl_, temporary.impl_);
    }
    return *this;
}

http3_connection_result http3_connection::register_client_request(std::uint64_t stream_id, http_known_method method) {
    if (impl_ == nullptr || impl_->role_ != http3_peer_role::client ||
        !is_http3_request_stream_id(stream_id)) {
        return stream_error(http3_connection_error_code::stream_creation_error);
    }
    auto& impl = *impl_;
    if (impl.feeding_) {
        throw std::logic_error("register_client_request() during HTTP/3 feed");
    }
    if (impl.failed_.scope_ == http3_connection_error_scope::connection) {
        return impl.failed_;
    }
    if (impl.control_.goaway_id()) {
        return stream_error(http3_connection_error_code::request_rejected);
    }
    if (impl.client_requests_.contains(stream_id)) {
        return stream_error(http3_connection_error_code::stream_creation_error);
    }
    if (impl.client_requests_.size() >= impl.limits_.max_active_streams_) {
        return stream_error(http3_connection_error_code::excessive_load);
    }
    impl.client_requests_.try_emplace(stream_id, stream_id, method, impl.resource_, impl.limits_, impl.receive_qpack_, impl);
    return {};
}

http3_connection_result http3_connection::cancel_request(std::uint64_t stream_id) {
    if (!impl_ || impl_->feeding_) {
        return stream_error(http3_connection_error_code::general_protocol_error);
    }
    auto& owner_value = *impl_;
    if (owner_value.failed_.scope_ != http3_connection_error_scope::none) {
        return owner_value.failed_;
    }
    const bool live = owner_value.requests_.contains(stream_id) || owner_value.client_requests_.contains(stream_id) || owner_value.push_streams_.contains(stream_id);
    if (!live) {
        return stream_error(http3_connection_error_code::stream_creation_error);
    }
    try {
        if (const auto canceled = owner_value.receive_qpack_.cancel(stream_id); (canceled.index() != 0)) {
            return owner_value.failed_ = connection_error(http3_connection_error_code::qpack_decompression_failed);
        }
        owner_value.requests_.erase(stream_id);
        owner_value.client_requests_.erase(stream_id);
        if (const auto found = owner_value.push_streams_.find(stream_id); found != owner_value.push_streams_.end()) {
            if (found->second.id_) {
                owner_value.pushes_.at(*found->second.id_).completed_ = true;
            }
            (void)owner_value.peer_streams_.retire_non_critical_stream(stream_id);
            owner_value.push_streams_.erase(found);
        }
        return {http3_connection_status::reset};
    } catch (...) {
        owner_value.failed_ = connection_error(http3_connection_error_code::internal_error);
        throw;
    }
}

bool http3_connection::retire_client_request(std::uint64_t stream_id) noexcept {
    if (impl_ == nullptr || impl_->role_ != http3_peer_role::client || impl_->feeding_ ||
        !is_http3_request_stream_id(stream_id)) {
        return false;
    }
    return impl_->client_requests_.erase(stream_id) != 0;
}

bool http3_connection::retire_server_request(std::uint64_t stream_id) noexcept {
    if (impl_ == nullptr || impl_->role_ != http3_peer_role::server || impl_->feeding_ ||
        !is_http3_request_stream_id(stream_id)) {
        return false;
    }
    return impl_->requests_.erase(stream_id) != 0;
}

bool http3_connection::retire() noexcept {
    if (impl_ == nullptr || impl_->feeding_) {
        return false;
    }
    auto* storage = std::exchange(impl_, nullptr);
    std::pmr::polymorphic_allocator<impl_type> allocator(resource_);
    std::allocator_traits<decltype(allocator)>::destroy(allocator, storage);
    allocator.deallocate(storage, 1);
    return true;
}

http3_connection_result http3_connection::feed(std::uint64_t stream_id, std::span<const char> bytes_value,
    bool fin, bool reset, http3_connection_callback_type callback_value, void* context_value) {
    if (impl_ == nullptr) {
        return connection_error(http3_connection_error_code::general_protocol_error);
    }
    auto& impl = *impl_;
    impl_type::feed_guard_type feed_guard(impl);
    try {
        if (impl.failed_.scope_ == http3_connection_error_scope::connection) {
            return impl.failed_;
        }
        if (callback_value == nullptr) {
            return connection_error(http3_connection_error_code::general_protocol_error);
        }
        if (impl.push_streams_.contains(stream_id)) {
            return impl.feed_push(stream_id, bytes_value, fin, reset, callback_value, context_value);
        }
        if (is_http3_unidirectional_stream_id(stream_id)) {
            const auto peer = impl.peer_streams_.feed(stream_id, bytes_value, fin, reset);
            if ((peer.index() != 0)) {
                impl.failed_ = connection_error(map_peer_error(std::get<1>(peer)));
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
                    impl.failed_ = connection_error(map_control_error(status));
                    return impl.failed_;
                }
                if (!impl.transmit_qpack_configured_ && impl.control_.peer_settings()) {
                    const auto& settings = *impl.control_.peer_settings();
                    impl.transmit_qpack_.emplace(http3_qpack_encoder_config{
                                                     .max_table_capacity_ = static_cast<std::size_t>(settings.qpack_max_table_capacity_),
                                                     .max_blocked_streams_ = static_cast<std::size_t>(std::min<std::uint64_t>(settings.qpack_blocked_streams_, impl.limits_.max_active_streams_)),
                                                     .fields_ = {impl.limits_.max_encoded_field_section_bytes_,
                                                         impl.limits_.max_field_section_size_,
                                                         impl.limits_.max_fields_},
                                                     .table_capacity_ = static_cast<std::size_t>(std::min<std::uint64_t>(settings.qpack_max_table_capacity_, 4096))},
                        impl.resource_);
                    impl.transmit_qpack_configured_ = true;
                }
            } else if (std::get<0>(peer).kind_ == http3_peer_stream_kind::qpack_encoder) {
                const auto decoded = impl.receive_qpack_.consume_encoder(std::get<0>(peer).remaining_, fin || reset);
                if ((decoded.index() != 0)) {
                    impl.failed_ = connection_error(std::get<1>(decoded) == http3_qpack_connection_error::closed_critical_stream
                                                        ? http3_connection_error_code::closed_critical_stream
                                                        : http3_connection_error_code::qpack_encoder_stream_error);
                    return impl.failed_;
                }
            } else if (std::get<0>(peer).kind_ == http3_peer_stream_kind::qpack_decoder) {
                if (const auto consumed = impl.transmit_qpack_->consume_decoder(std::get<0>(peer).remaining_, fin || reset); (consumed.index() != 0)) {
                    impl.failed_ = connection_error(http3_connection_error_code::qpack_decoder_stream_error);
                    return impl.failed_;
                }
            }
            return {http3_connection_status::need_more_data};
        }

        if (impl.role_ == http3_peer_role::client) {
            // Client response state exists only for explicitly registered local bidi streams.
            if (!is_http3_request_stream_id(stream_id)) {
                impl.failed_ = connection_error(http3_connection_error_code::stream_creation_error);
                return impl.failed_;
            }
            auto found = impl.client_requests_.find(stream_id);
            if (found == impl.client_requests_.end()) {
                return stream_error(http3_connection_error_code::stream_creation_error);
            }
            auto& request = found->second;
            request.callback_ = callback_value;
            request.callback_context_ = context_value;
            if (reset) {
                if (auto canceled = impl.receive_qpack_.cancel(stream_id); (canceled.index() != 0)) {
                    impl.failed_ = connection_error(http3_connection_error_code::qpack_decompression_failed);
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
                impl.failed_ = connection_error(response.code_);
                impl.client_requests_.clear();
                return impl.failed_;
            }
            if (response.scope_ == http3_connection_error_scope::stream) {
                impl.client_requests_.erase(found);
                return stream_error(response.code_);
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
            impl.failed_ = connection_error(http3_connection_error_code::stream_creation_error);
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
                    impl.failed_ = connection_error(http3_connection_error_code::qpack_decompression_failed);
                    return impl.failed_;
                }
                return {http3_connection_status::need_more_data};
            }
            if (impl.local_goaway_ && stream_id >= *impl.local_goaway_) {
                return stream_error(http3_connection_error_code::request_rejected);
            }
            if (impl.requests_.size() >= impl.limits_.max_active_streams_) {
                return stream_error(http3_connection_error_code::excessive_load);
            }
            found = impl.requests_.try_emplace(stream_id, impl.resource_, impl.limits_, impl.receive_qpack_).first;
            found->second.stream_id_ = stream_id;
        }
        auto& request = found->second;
        if (reset) {
            if (auto canceled = impl.receive_qpack_.cancel(stream_id); (canceled.index() != 0)) {
                impl.failed_ = connection_error(http3_connection_error_code::qpack_decompression_failed);
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
            impl.failed_ = connection_error(*code);
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
        impl.failed_ = connection_error(http3_connection_error_code::internal_error);
        throw;
    }
}

namespace {
std::pmr::vector<char> control_id_frame(std::uint64_t type, std::uint64_t id, std::pmr::memory_resource* resource) {
    std::array<char, 24> buffer{};
    const auto frame = encode_http3_frame_header(buffer, type, http3_var_int_encoded_size(id));
    const auto value = encode_http3_var_int(std::span(buffer).subspan(std::get<0>(frame)), id);
    return {buffer.begin(), buffer.begin() + std::get<0>(frame) + std::get<0>(value), resource};
}
}  // namespace
std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_connection::prepare_max_push_id(std::uint64_t maximum) {
    if (!impl_ || impl_->feeding_ || impl_->role_ != http3_peer_role::client || maximum > http3_var_int_max ||
        impl_->failed_.scope_ != http3_connection_error_scope::none ||
        (impl_->limits_.max_push_id_ && maximum < *impl_->limits_.max_push_id_)) {
        return http3_connection_error_code::id_error;
    }
    auto output = control_id_frame(0xd, maximum, resource_);
    impl_->limits_.max_push_id_ = maximum;
    for (auto& [id, request] : impl_->client_requests_) {
        (void)request.response_.authorize_push(maximum);
    }
    return output;
}
std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_connection::prepare_cancel_push(std::uint64_t push_id) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none ||
        !impl_->pushes_.contains(push_id) || !impl_->pushes_.at(push_id).head_) {
        return http3_connection_error_code::id_error;
    }
    auto output = control_id_frame(3, push_id, resource_);
    impl_->pushes_.at(push_id).canceled_ = true;
    return output;
}
std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_connection::prepare_goaway(std::uint64_t id) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none || id > http3_var_int_max ||
        (impl_->role_ == http3_peer_role::server && !is_http3_request_stream_id(id)) ||
        (impl_->local_goaway_ && id > *impl_->local_goaway_)) {
        return http3_connection_error_code::id_error;
    }
    auto output = control_id_frame(7, id, resource_);
    impl_->local_goaway_ = id;
    return output;
}
std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_connection::prepare_priority_update(http_priority_update update) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none ||
        impl_->role_ != http3_peer_role::client ||
        (update.push_ && (!impl_->pushes_.contains(update.element_id_) || !impl_->pushes_.at(update.element_id_).head_))) {
        return http3_connection_error_code::id_error;
    }
    std::array<char, 32> buffer{};
    const auto size = encode_http3_priority_update(buffer, update);
    if ((size.index() != 0)) {
        return http3_connection_error_code::message_error;
    }
    return std::pmr::vector<char>(buffer.begin(), buffer.begin() + std::get<0>(size), resource_);
}
std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_connection::prepare_push_stream(std::uint64_t stream_id, std::uint64_t push_id) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none ||
        impl_->role_ != http3_peer_role::server || stream_id > http3_var_int_max || (stream_id & 3) != 3 ||
        !impl_->pushes_.contains(push_id) || !impl_->pushes_.at(push_id).head_) {
        return http3_connection_error_code::id_error;
    }
    auto& push = impl_->pushes_.at(push_id);
    if (push.stream_id_ || push.canceled_) {
        return http3_connection_error_code::request_cancelled;
    }
    for (const auto& [id, previous] : impl_->pushes_) {
        if (previous.stream_id_ == stream_id) {
            return http3_connection_error_code::stream_creation_error;
        }
    }
    std::array<char, 9> buffer{1};
    const auto size = encode_http3_var_int(std::span(buffer).subspan(1), push_id);
    std::pmr::vector<char> output(buffer.begin(), buffer.begin() + 1 + std::get<0>(size), resource_);
    push.stream_id_ = stream_id;
    return output;
}

std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_connection::prepare_origin_advertisement(
    std::span<const std::string_view> origins) {
    if (!impl_ || impl_->feeding_ || impl_->role_ != http3_peer_role::server || impl_->failed_.scope_ != http3_connection_error_scope::none) {
        return http3_connection_error_code::general_protocol_error;
    }
    auto bytes_value = encode_http3_origin_frame(origins, impl_->limits_.max_encoded_field_section_bytes_, resource_);
    if ((bytes_value.index() != 0)) {
        return http3_connection_error_code::message_error;
    }
    return std::move(std::get<0>(bytes_value));
}
std::optional<std::uint64_t> http3_connection::peer_max_push_id() const noexcept {
    return impl_ ? impl_->control_.max_push_id() : std::nullopt;
}
const http3_message_head* http3_connection::promised_request(std::uint64_t push_id) const& noexcept {
    if (!impl_) {
        return nullptr;
    }
    const auto found = impl_->pushes_.find(push_id);
    return found == impl_->pushes_.end() || !found->second.head_ ? nullptr : &*found->second.head_;
}
std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_connection::prepare_push_promise(
    std::uint64_t associated_stream_id, std::uint64_t push_id, http_push_request_view request) {
    using error_type = http3_connection_error_code;
    if (!impl_ || impl_->role_ != http3_peer_role::server || impl_->feeding_ ||
        impl_->failed_.scope_ != http3_connection_error_scope::none || !is_http3_request_stream_id(associated_stream_id)) {
        return error_type::general_protocol_error;
    }
    auto& owner_value = *impl_;
    const auto max_id = owner_value.control_.max_push_id();
    if (!max_id || push_id > *max_id) {
        return error_type::id_error;
    }
    if (owner_value.control_.goaway_id()) {
        return error_type::request_rejected;
    }
    if (const auto found = owner_value.pushes_.find(push_id); found != owner_value.pushes_.end() && found->second.canceled_) {
        return error_type::request_cancelled;
    }
    if (request.method_ != "GET" && request.method_ != "HEAD") {
        return error_type::message_error;
    }
    std::pmr::vector<http3_field_section_field_view> fields(resource_);
    for (const auto& field : request.headers_) {
        fields.push_back({field.name(), field.value(), false});
    }
    const auto limits = owner_value.outbound_field_limits({owner_value.limits_.max_encoded_field_section_bytes_,
        owner_value.limits_.max_field_section_size_, owner_value.limits_.max_fields_});
    auto encoded = encode_http3_client_request_head({.method_ = request.method_, .scheme_ = request.scheme_, .authority_ = request.authority_, .path_ = request.path_, .fields_ = fields}, limits, resource_);
    if ((encoded.index() != 0)) {
        return error_type::message_error;
    }
    auto head = decode_http3_message_head(std::get<0>(encoded).field_section_, http3_message_head_kind::request, resource_,
        {owner_value.limits_.max_field_section_size_, owner_value.limits_.max_fields_, owner_value.limits_.max_encoded_field_section_bytes_});
    if ((head.index() != 0) || (std::get<0>(head).content_length_ && *std::get<0>(head).content_length_ != 0)) {
        return error_type::message_error;
    }
    std::array<char, 24> prefix{};
    const auto id_size = http3_var_int_encoded_size(push_id);
    const auto frame_size = encode_http3_frame_header(prefix, 5, id_size + std::get<0>(encoded).field_section_.size());
    const auto id = encode_http3_var_int(std::span(prefix).subspan(std::get<0>(frame_size)), push_id);
    std::pmr::vector<char> output(resource_);
    output.insert(output.end(), prefix.begin(), prefix.begin() + std::get<0>(frame_size) + std::get<0>(id));
    output.insert(output.end(), std::get<0>(encoded).field_section_.begin(), std::get<0>(encoded).field_section_.end());
    if (!owner_value.accept_promise(push_id, std::get<0>(head))) {
        return owner_value.failed_.code_;
    }
    return output;
}

std::variant<std::pmr::vector<char>, http3_qpack_connection_error> http3_connection::encode_field_section(
    std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none) {
        return http3_qpack_connection_error::invalid_stream_id;
    }
    if (!is_http3_request_stream_id(stream_id) &&
        !(impl_->role_ == http3_peer_role::server && (stream_id & 3) == 3 && stream_id <= http3_var_int_max)) {
        return http3_qpack_connection_error::invalid_stream_id;
    }
    return impl_->transmit_qpack_->encode(stream_id, fields_value, impl_->outbound_field_limits({}));
}
std::variant<http3_client_request_head, http3_client_request_head_failure> http3_connection::encode_client_request_head(
    std::uint64_t stream_id, http3_client_request_head_view view, http3_field_section_limits limits) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none ||
        impl_->role_ != http3_peer_role::client || !is_http3_request_stream_id(stream_id)) {
        return http3_client_request_head_failure{http3_client_request_head_error::field_section_error, http3_field_section_error::invalid_prefix};
    }
    view.peer_enable_connect_protocol_ = impl_->control_.peer_settings() && impl_->control_.peer_settings()->enable_connect_protocol_;
    return encode_http3_client_request_head(*impl_->transmit_qpack_, stream_id, view, impl_->outbound_field_limits(limits), resource_);
}
std::variant<http3_response_head, http3_response_head_failure> http3_connection::encode_connect_response_head(
    std::uint64_t stream_id, const http_response& response, http3_field_section_limits limits) {
    if (!is_http3_request_stream_id(stream_id) || !response.status().is_successful() || !response.body_bytes().empty() || response.file_body().has_value()) {
        return http3_response_head_failure{http3_response_head_error::invalid_field};
    }
    return encode_response_head(stream_id, response, plan_buffered_http_response_write(http_known_method::connect, response), limits);
}

std::variant<http3_response_head, http3_response_head_failure> http3_connection::encode_response_head(std::uint64_t stream_id, const http_response& response, http_buffered_response_write_plan plan, http3_field_section_limits limits) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none || impl_->role_ != http3_peer_role::server ||
        (!is_http3_request_stream_id(stream_id) && ((stream_id & 3) != 3 || stream_id > http3_var_int_max))) {
        return http3_response_head_failure{http3_response_head_error::field_section_error, http3_field_section_error::invalid_prefix};
    }
    const auto local_limits = impl_->local_field_limits(limits);
    return impl_->response_encoding_result(
        encode_http3_response_head(*impl_->transmit_qpack_, stream_id, response, plan, impl_->outbound_field_limits(local_limits), resource_), local_limits);
}
std::variant<http3_streaming_response_head, http3_response_head_failure> http3_connection::encode_streaming_response_head(std::uint64_t stream_id, http_response response, http_known_method method, http_response_stream_kind kind, http_response_trailer_intent trailers, http3_field_section_limits limits) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none || impl_->role_ != http3_peer_role::server ||
        (!is_http3_request_stream_id(stream_id) && ((stream_id & 3) != 3 || stream_id > http3_var_int_max))) {
        return http3_response_head_failure{http3_response_head_error::field_section_error, http3_field_section_error::invalid_prefix};
    }
    const auto local_limits = impl_->local_field_limits(limits);
    return impl_->response_encoding_result(
        encode_http3_streaming_response_head(*impl_->transmit_qpack_, stream_id, std::move(response), method, kind, trailers, impl_->outbound_field_limits(local_limits), resource_), local_limits);
}
std::variant<http3_response_head, http3_response_head_failure> http3_connection::encode_interim_response_head(std::uint64_t stream_id, const http_interim_response_head& response, http3_field_section_limits limits) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none || impl_->role_ != http3_peer_role::server ||
        (!is_http3_request_stream_id(stream_id) && ((stream_id & 3) != 3 || stream_id > http3_var_int_max))) {
        return http3_response_head_failure{http3_response_head_error::field_section_error, http3_field_section_error::invalid_prefix};
    }
    const auto local_limits = impl_->local_field_limits(limits);
    return impl_->response_encoding_result(
        encode_http3_interim_response_head(*impl_->transmit_qpack_, stream_id, response, impl_->outbound_field_limits(local_limits), resource_), local_limits);
}
std::variant<http3_response_field_section, http3_response_head_failure> http3_connection::encode_response_trailers(std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits) {
    if (!impl_ || impl_->feeding_ || impl_->failed_.scope_ != http3_connection_error_scope::none || impl_->role_ != http3_peer_role::server ||
        (!is_http3_request_stream_id(stream_id) && ((stream_id & 3) != 3 || stream_id > http3_var_int_max))) {
        return http3_response_head_failure{http3_response_head_error::field_section_error, http3_field_section_error::invalid_prefix};
    }
    const auto local_limits = impl_->local_field_limits(limits);
    return impl_->response_encoding_result(
        encode_http3_response_trailers(*impl_->transmit_qpack_, stream_id, fields_value, impl_->outbound_field_limits(local_limits), resource_), local_limits);
}
std::span<const char> http3_connection::pending_qpack_encoder_output() const& noexcept {
    return impl_ ? impl_->transmit_qpack_->pending_encoder_output() : std::span<const char>{};
}
bool http3_connection::consume_qpack_encoder_output(std::size_t bytes_value) noexcept {
    return impl_ && !impl_->feeding_ && impl_->transmit_qpack_->consume_encoder_output(bytes_value);
}

std::span<const char> http3_connection::pending_qpack_decoder_output() const& noexcept {
    return impl_ ? impl_->receive_qpack_.pending_decoder_output() : std::span<const char>{};
}
bool http3_connection::consume_qpack_decoder_output(std::size_t bytes_value) noexcept {
    return impl_ && !impl_->feeding_ && impl_->receive_qpack_.consume_decoder_output(bytes_value);
}

std::size_t http3_connection::active_request_count() const noexcept {
    return impl_ == nullptr ? 0 : impl_->requests_.size() + impl_->client_requests_.size() + impl_->push_streams_.size();
}

http3_settings http3_connection::local_settings() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    const auto& config = impl_->limits_;
    return {.qpack_max_table_capacity_ = config.qpack_max_table_capacity_,
        .max_field_section_size_ = config.max_field_section_size_,
        .qpack_blocked_streams_ = config.qpack_blocked_streams_,
        .enable_connect_protocol_ = config.enable_connect_protocol_,
        .h3_datagram_ = config.enable_datagrams_};
}

const std::optional<http3_settings>& http3_connection::peer_settings() const noexcept {
    static const std::optional<http3_settings> empty;
    return impl_ == nullptr ? empty : impl_->control_.peer_settings();
}

bool http3_connection::peer_reports_unprocessed(std::uint64_t stream_id,
    std::optional<std::uint64_t> peer_reset_error_code) const noexcept {
    if (impl_ == nullptr || impl_->failed_.scope_ != http3_connection_error_scope::none) {
        return false;
    }
    const auto found = impl_->client_requests_.find(stream_id);
    if (found == impl_->client_requests_.end() || found->second.response_observed_) {
        return false;
    }
    const auto goaway = impl_->control_.goaway_id();
    return (goaway && stream_id >= *goaway) ||
           peer_reset_error_code == static_cast<std::uint64_t>(http3_connection_error_code::request_rejected);
}

std::optional<std::uint64_t> http3_connection::peer_goaway_id() const noexcept {
    return impl_ == nullptr ? std::nullopt : impl_->control_.goaway_id();
}

}  // namespace ruvia
