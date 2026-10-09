#include "ruvia/http/http3_client_response.h"

#include <limits>
#include <stdexcept>
#include <variant>

#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_stream_frames.h"
#include "ruvia/http/http3_var_int.h"

#include "http3/http3_trailer_collector.h"

namespace ruvia {
namespace {

http3_client_response_result stream_error(http3_connection_error_code code) noexcept {
    return {http3_client_response_status::stream_error, http3_connection_error_scope::stream, code};
}

http3_client_response_result connection_error(http3_connection_error_code code) noexcept {
    return {http3_client_response_status::connection_error, http3_connection_error_scope::connection, code};
}

bool valid_response_trailer_policy(http3_field_section_field_view field) noexcept {
    return detail::response_trailer_content_valid(field.name_, field.value_);
}

}  // namespace

struct http3_client_response::impl_type final {
    using trailer_collector = detail::http3_trailer_collector<valid_response_trailer_policy>;

    impl_type(std::uint64_t stream, http_known_method method, std::pmr::memory_resource* resource,
        http3_client_response_limits configured, http3_qpack_decoder* shared_decoder)
        : stream_id_(stream),
          request_method_(method),
          memory_(resource),
          frames_(http3_stream_kind::response, resource,
              {.max_field_section_size_ = configured.max_encoded_field_section_bytes_,
                  .max_settings_payload_bytes_ = 64 * 1024,
                  .allow_push_ = configured.max_push_id_.has_value() && !configured.push_stream_}),
          limits_(configured),
          decoder_(shared_decoder) {}

    std::uint64_t stream_id_;
    http_known_method request_method_;
    std::pmr::memory_resource* memory_;
    http3_stream_frames frames_;
    http3_client_response_limits limits_;
    http3_qpack_decoder* decoder_;
    http3_client_response_callback_type callback_{};
    void* callback_context_{};
    http3_client_response_result result_{};
    http3_message_body body_{std::nullopt, true};
    std::optional<http_response_body_plan> body_plan_;
    std::uint64_t tunnel_data_length_{0};
    bool final_headers_{false};
    bool trailers_{false};
    bool tunnel_{false};
    bool terminal_{false};
    bool feeding_{false};

    static void on_frame(void* opaque, http3_stream_frame_event frame) {
        auto& self = *static_cast<impl_type*>(opaque);
        if (self.terminal_ || self.result_.scope_ != http3_connection_error_scope::none) {
            return;
        }
        if (frame.kind_ == http3_stream_frame_event_kind::push_promise) {
            const auto id = decode_http3_var_int(frame.payload_);
            if ((id.index() != 0)) {
                self.result_ = connection_error(http3_connection_error_code::frame_error);
                return;
            }
            if (!self.limits_.max_push_id_ || std::get<0>(id).value_ > *self.limits_.max_push_id_ || self.limits_.push_stream_) {
                self.result_ = connection_error(http3_connection_error_code::id_error);
                return;
            }
            const auto section = frame.payload_.subspan(std::get<0>(id).encoded_bytes_);
            const http3_message_head_limits head_limits{self.limits_.max_field_section_size_,
                self.limits_.max_fields_, self.limits_.max_encoded_field_section_bytes_};
            auto head = [&]() -> std::variant<http3_message_head, http3_message_head_error> {
                if (!self.decoder_) {
                    return decode_http3_message_head(section, http3_message_head_kind::request, self.memory_, head_limits);
                }
                auto decoded = decode_http3_message_head(*self.decoder_, self.stream_id_, section,
                    http3_message_head_kind::request, self.memory_, head_limits);
                if ((decoded.index() != 0)) {
                    return std::get<1>(decoded);
                }
                if (auto* result = std::get_if<http3_message_head>(&std::get<0>(decoded))) {
                    return std::move(*result);
                }
                self.frames_.pause();
                return http3_message_head_error::message_error;
            }();
            if (self.frames_.paused()) {
                return;
            }
            if ((head.index() != 0) || (std::get<0>(head).method_ != "GET" && std::get<0>(head).method_ != "HEAD") ||
                (std::get<0>(head).content_length_ && *std::get<0>(head).content_length_ != 0)) {
                self.result_ = connection_error((head.index() != 0) && std::get<1>(head) == http3_message_head_error::qpack_decompression_failed
                                                    ? http3_connection_error_code::qpack_decompression_failed
                                                    : http3_connection_error_code::message_error);
                return;
            }
            const http3_client_response_event event{.kind_ = http3_client_response_event_kind::push_promise,
                .stream_id_ = self.stream_id_,
                .head_ = &std::get<0>(head),
                .push_id_ = std::get<0>(id).value_};
            self.callback_(self.callback_context_, event);
            return;
        }
        if (frame.kind_ == http3_stream_frame_event_kind::data) {
            if (!self.final_headers_ || self.trailers_) {
                self.result_ = stream_error(http3_connection_error_code::message_error);
                return;
            }
            if (self.tunnel_) {
                if (frame.payload_.size() > std::numeric_limits<std::uint64_t>::max() - self.tunnel_data_length_) {
                    self.result_ = stream_error(http3_connection_error_code::message_error);
                    return;
                }
                self.tunnel_data_length_ += frame.payload_.size();
                if (!frame.payload_.empty()) {
                    const http3_client_response_event event{.kind_ = http3_client_response_event_kind::tunnel_data,
                        .stream_id_ = self.stream_id_,
                        .body_ = frame.payload_};
                    self.callback_(self.callback_context_, event);
                }
                return;
            }
            const auto counted = self.body_.feed(frame.payload_.size(), false);
            if (counted != http3_message_body_result::accepted) {
                self.result_ = stream_error(http3_connection_error_code::message_error);
                return;
            }
            if (!frame.payload_.empty()) {
                const http3_client_response_event event{.kind_ = http3_client_response_event_kind::body,
                    .stream_id_ = self.stream_id_,
                    .body_ = frame.payload_};
                self.callback_(self.callback_context_, event);
            }
            return;
        }
        if (frame.kind_ != http3_stream_frame_event_kind::headers || !frame.end_frame_) {
            return;
        }

        if (self.final_headers_) {
            if (self.trailers_ || self.tunnel_) {
                self.result_ = stream_error(http3_connection_error_code::message_error);
                return;
            }
            trailer_collector collector_value(self.memory_);
            const http3_field_section_limits field_limits{self.limits_.max_encoded_field_section_bytes_,
                self.limits_.max_field_section_size_, self.limits_.max_fields_};
            if (self.decoder_) {
                const auto decoded = self.decoder_->decode(self.stream_id_, frame.payload_, trailer_collector::collect, &collector_value);
                if ((decoded.index() == 0) && std::get<0>(decoded).status_ == http3_qpack_decode_status::blocked) {
                    self.frames_.pause();
                    return;
                }
                if ((decoded.index() != 0) || !collector_value.valid_) {
                    self.result_ = (decoded.index() != 0) ? connection_error(std::get<1>(decoded) == http3_qpack_connection_error::limit
                                                                                 ? http3_connection_error_code::excessive_load
                                                                                 : http3_connection_error_code::qpack_decompression_failed)
                                                          : stream_error(http3_connection_error_code::message_error);
                    return;
                }
            } else {
                const auto decoded = decode_http3_field_section(frame.payload_, trailer_collector::collect, &collector_value, field_limits, self.memory_);
                if ((decoded.index() != 0) || !collector_value.valid_) {
                    self.result_ = collector_value.valid_ ? connection_error(std::get<1>(decoded) == http3_field_section_error::field_list_too_large ||
                                                                                     std::get<1>(decoded) == http3_field_section_error::field_section_too_large || std::get<1>(decoded) == http3_field_section_error::too_many_fields
                                                                                 ? http3_connection_error_code::excessive_load
                                                                                 : http3_connection_error_code::qpack_decompression_failed)
                                                          : stream_error(http3_connection_error_code::message_error);
                    return;
                }
            }
            self.trailers_ = true;
            for (const auto& field : collector_value.fields_) {
                const http3_client_response_event event{.kind_ = http3_client_response_event_kind::trailer_field,
                    .stream_id_ = self.stream_id_,
                    .trailer_ = {field.name_, field.value_, field.never_indexed_}};
                self.callback_(self.callback_context_, event);
            }
            return;
        }

        const http3_message_head_limits head_limits{self.limits_.max_field_section_size_,
            self.limits_.max_fields_, self.limits_.max_encoded_field_section_bytes_};
        auto decoded = [&]() -> std::variant<http3_message_head, http3_message_head_error> {
            if (!self.decoder_) {
                return decode_http3_message_head(frame.payload_, http3_message_head_kind::response, self.memory_, head_limits);
            }
            auto result_value = decode_http3_message_head(*self.decoder_, self.stream_id_, frame.payload_,
                http3_message_head_kind::response, self.memory_, head_limits);
            if ((result_value.index() != 0)) {
                return std::get<1>(result_value);
            }
            if (auto* head = std::get_if<http3_message_head>(&std::get<0>(result_value))) {
                return std::move(*head);
            }
            self.frames_.pause();
            return http3_message_head_error::message_error;
        }();
        if (self.frames_.paused()) {
            return;
        }
        if ((decoded.index() != 0)) {
            self.result_ = std::get<1>(decoded) == http3_message_head_error::qpack_decompression_failed
                               ? connection_error(http3_connection_error_code::qpack_decompression_failed)
                           : std::get<1>(decoded) == http3_message_head_error::field_section_too_large
                               ? connection_error(http3_connection_error_code::excessive_load)
                               : stream_error(http3_connection_error_code::message_error);
            return;
        }
        if (std::get<0>(decoded).status_ < 200) {
            if (std::get<0>(decoded).status_ == 101) {
                self.result_ = stream_error(http3_connection_error_code::message_error);
                return;
            }
            const http3_client_response_event event{.kind_ = http3_client_response_event_kind::informational_head,
                .stream_id_ = self.stream_id_,
                .head_ = &std::get<0>(decoded),
                .request_content_signal_ = std::get<0>(decoded).status_ == 100 ? std::optional{http_client_request_content_signal::continue_value} : std::nullopt};
            self.callback_(self.callback_context_, event);
            return;
        }
        self.final_headers_ = true;
        self.tunnel_ = self.request_method_ == http_known_method::connect && std::get<0>(decoded).status_ >= 200 && std::get<0>(decoded).status_ < 300;
        self.body_plan_ = plan_http_response_body(self.request_method_, http_status_code::from_value(std::get<0>(decoded).status_));
        const bool payload_allowed = !self.tunnel_ && self.body_plan_->status_allows_body() && !self.body_plan_->body_suppressed();
        // For HEAD and bodyless statuses Content-Length is representation metadata,
        // not a DATA accounting expectation.
        self.body_ = http3_message_body(payload_allowed ? std::get<0>(decoded).content_length_ : std::nullopt, payload_allowed);
        const http3_client_response_event event{.kind_ = http3_client_response_event_kind::final_head,
            .stream_id_ = self.stream_id_,
            .head_ = &std::get<0>(decoded),
            .response_body_plan_ = self.body_plan_,
            .request_content_signal_ = http_client_request_content_signal::exchange_complete};
        self.callback_(self.callback_context_, event);
    }
};

http3_client_response::http3_client_response(std::uint64_t stream_id, http_known_method request_method,
    std::pmr::memory_resource* resource, http3_client_response_limits limits, http3_qpack_decoder* decoder)
    : resource_(resource),
      impl_(nullptr) {
    if (!resource || limits.max_fields_ == 0 || limits.max_encoded_field_section_bytes_ == 0 || limits.max_field_section_size_ == 0) {
        throw std::invalid_argument("HTTP/3 client response requires a resource and positive limits");
    }
    if ((!limits.push_stream_ && !is_http3_request_stream_id(stream_id)) ||
        (limits.push_stream_ && http3_stream_id_type(stream_id) != http3_stream_id_type::server_unidirectional)) {
        throw std::invalid_argument("HTTP/3 response stream must be a valid locally initiated bidirectional stream");
    }
    std::pmr::polymorphic_allocator<impl_type> allocator(resource);
    impl_ = allocator.allocate(1);
    try {
        std::allocator_traits<decltype(allocator)>::construct(allocator, impl_, stream_id, request_method, resource, limits, decoder);
    } catch (...) {
        allocator.deallocate(impl_, 1);
        impl_ = nullptr;
        throw;
    }
}

http3_client_response::~http3_client_response() {
    if (impl_) {
        std::pmr::polymorphic_allocator<impl_type> allocator(resource_);
        std::allocator_traits<decltype(allocator)>::destroy(allocator, impl_);
        allocator.deallocate(impl_, 1);
    }
}

http3_client_response::http3_client_response(http3_client_response&& other) noexcept
    : resource_(other.resource_),
      impl_(other.impl_) {
    other.impl_ = nullptr;
}

http3_client_response& http3_client_response::operator=(http3_client_response&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            std::pmr::polymorphic_allocator<impl_type> allocator(resource_);
            std::allocator_traits<decltype(allocator)>::destroy(allocator, impl_);
            allocator.deallocate(impl_, 1);
        }
        resource_ = other.resource_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

http3_client_response_result http3_client_response::feed(std::span<const char> bytes_value, bool fin, bool reset,
    http3_client_response_callback_type callback_value, void* context_value) {
    if (!impl_) {
        return stream_error(http3_connection_error_code::message_error);
    }
    auto& self = *impl_;
    if (self.feeding_) {
        throw std::logic_error("recursive HTTP/3 client response feed");
    }
    if (self.terminal_) {
        return self.result_;
    }
    if (!callback_value) {
        return stream_error(http3_connection_error_code::message_error);
    }
    self.feeding_ = true;
    struct guard {
        bool& active_;
        ~guard() {
            active_ = false;
        }
    } guard_value{self.feeding_};
    self.callback_ = callback_value;
    self.callback_context_ = context_value;
    self.result_ = {};
    try {
        if (reset) {
            self.terminal_ = true;
            self.result_ = {http3_client_response_status::reset, http3_connection_error_scope::none,
                http3_connection_error_code::no_error};
            const http3_client_response_event event{.kind_ = http3_client_response_event_kind::reset, .stream_id_ = self.stream_id_};
            callback_value(context_value, event);
            return self.result_;
        }
        const auto frame_status = self.frames_.feed(bytes_value, fin, impl_type::on_frame, &self);
        self.result_.consumed_bytes_ = self.frames_.consumed_bytes();
        if (frame_status == http3_stream_frame_status::paused) {
            self.result_.status_ = http3_client_response_status::qpack_blocked;
            return self.result_;
        }
        // A framing failure later in this input can have connection scope even
        // when an earlier message callback found a stream-scoped error.
        if (frame_status != http3_stream_frame_status::need_more_data && frame_status != http3_stream_frame_status::message_end) {
            self.terminal_ = true;
            // A response PUSH_PROMISE is connection-fatal because this client
            // has not authorized any push IDs; other framing failures use the
            // shared protocol status mapping.
            const auto code = frame_status == http3_stream_frame_status::push_promise && !self.limits_.push_stream_
                                  ? http3_connection_error_code::id_error
                                  : http3_connection_error_code_for_stream_frame_status(frame_status)
                                        .value_or(http3_connection_error_code::frame_error);
            self.result_ = connection_error(code);
            return self.result_;
        }
        if (self.result_.scope_ != http3_connection_error_scope::none) {
            self.terminal_ = true;
            return self.result_;
        }
        if (frame_status == http3_stream_frame_status::message_end) {
            if (!self.final_headers_) {
                self.terminal_ = true;
                self.result_ = stream_error(http3_connection_error_code::message_error);
                return self.result_;
            }
            if (!self.tunnel_) {
                const auto end = self.body_.feed(0, true);
                if (end != http3_message_body_result::complete) {
                    self.terminal_ = true;
                    self.result_ = stream_error(http3_connection_error_code::message_error);
                    return self.result_;
                }
            }
            self.terminal_ = true;
            self.result_.status_ = http3_client_response_status::message_end;
            const http3_client_response_event event{.kind_ = http3_client_response_event_kind::message_end,
                .stream_id_ = self.stream_id_,
                .response_body_plan_ = self.body_plan_};
            callback_value(context_value, event);
        }
        return self.result_;
    } catch (...) {
        self.terminal_ = true;
        self.result_ = stream_error(http3_connection_error_code::message_error);
        throw;
    }
}

bool http3_client_response::authorize_push(std::uint64_t maximum) noexcept {
    if (!impl_ || impl_->feeding_ || impl_->limits_.push_stream_ || maximum > http3_var_int_max ||
        (impl_->limits_.max_push_id_ && maximum < *impl_->limits_.max_push_id_)) {
        return false;
    }
    impl_->limits_.max_push_id_ = maximum;
    impl_->frames_.allow_push();
    return true;
}

std::uint64_t http3_client_response::stream_id() const noexcept {
    return impl_ ? impl_->stream_id_ : 0;
}

}  // namespace ruvia
