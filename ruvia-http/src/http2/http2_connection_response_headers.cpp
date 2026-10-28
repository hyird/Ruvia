#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

#include "ruvia/http/detail/coding/http_response_content_semantics.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

#include "client/http_client_access.h"
#include "coding/http_content_length.h"
#include "field/response_field_validation.h"
#include "http2/http2_connection.h"
#include "http2/http2_header_block.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_request_headers.h"
#include "http2/http2_response_headers.h"

// Decoding a client response head: ':status' first and once, informational
// response events, and regular headers allowed in the stream's final table.

namespace ruvia::detail {

namespace {

struct http2_response_decode_context final {
    http2_response_decode_context(http2_stream_state& stream, std::pmr::memory_resource* resource) noexcept
        : base_(stream),
          informational_fields_(resource) {}

    http2_header_decode_context base_;
    response_field_state fields_;
    std::pmr::vector<http_header> informational_fields_;
    std::optional<http_received_status> status_;
    http_response_content_semantics content_semantics_{http_response_content_semantics::with_content};
    bool saw_regular_{false};
};

// ':status' is first and unique. Generic binary field checks precede dispatch;
// interim metadata is retained separately without applying sender prohibitions.
bool http2_on_decoded_response_header(void* target, std::string_view name, std::string_view value) {
    auto* context_value = static_cast<http2_response_decode_context*>(target);
    if (!http2_accumulate_header_list_bytes(context_value->base_, name, value)) {
        return false;
    }
    auto& stream = context_value->base_.stream_;
    if (name.empty()) {
        return false;
    }
    if (name.front() == ':') {
        if (name != ":status" || context_value->status_ || context_value->saw_regular_) {
            return false;
        }
        // RFC 9110 section 15: a received 000..099 or 600..999 is processed as
        // a 5xx response.
        const auto status = http_received_status::try_parse(value);
        if (!status || status->status() == http_status::switching_protocols) {
            return false;
        }
        context_value->status_ = *status;
        context_value->content_semantics_ =
            classify_http_response_content_semantics(stream.request_known_method(), status->status());
        return true;
    }
    if (!context_value->status_ || !http2_is_valid_decoded_response_header(name, value)) {
        return false;
    }
    context_value->saw_regular_ = true;
    if (context_value->status_->status().is_informational()) {
        // These fields cannot frame a 1xx or carry over to the final response.
        context_value->informational_fields_.emplace_back(
            name, value, context_value->informational_fields_.get_allocator().resource());
        return true;
    }
    const auto kind = classify_request_header(name);
    const bool successful_connect =
        context_value->content_semantics_ == http_response_content_semantics::connect_tunnel;
    if (validate_response_field(kind, value, http_field_list_role::recipient, context_value->fields_) !=
        response_field_status::ok) {
        return false;
    }
    // RFC 9110 9.3.6: successful CONNECT ignores Content-Length semantics,
    // including invalid or unrepresentable numerals. Retain the generic-byte-valid
    // field so extension validators (RFC 9297 section 3.2) can still inspect it.
    if (kind == request_header_kind::content_length && !successful_connect) {
        http_content_length_state content_length;
        if (content_length.parse_field(value) != http_content_length_parse_status::ok) {
            return false;
        }
        if (!stream.declare_remote_content_length(*content_length.value())) {
            return false;
        }
    }
    return stream.append_remote_header(name, value, kind);
}

}  // namespace

bool http2_on_decoded_response_trailer(void* target, std::string_view name, std::string_view value) {
    auto& context_value = *static_cast<http2_header_decode_context*>(target);
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }

    // Pseudo-header, connection-specific and TE fields are malformed (RFC 9113
    // sections 8.1 and 8.2.2). Response trailer permissions follow the same
    // response-specific table that proves outbound trailer sections, but that
    // table binds only the sender (RFC 9110 section 6.5.1): a received field it
    // does not permit is discarded, never merged into the header section.
    if (!http2_is_valid_decoded_response_header(name, value)) {
        return false;
    }
    return is_forbidden_response_trailer_name(name) || context_value.stream_.append_remote_trailer(name, value);
}

header_decode_status http2_connection::decode_response_header_block(http2_stream_state& stream,
    std::string_view block, hpack_decoder::decode_transaction_type& hpack_transaction) {
    http2_response_decode_context context_value{stream, resource_};
    const auto result_value = decoder_.decode(
        block, &context_value,
        [](void* target, std::string_view name, std::string_view value) {
            return http2_on_decoded_response_header(target, name, value);
        },
        hpack_transaction);
    if (const auto status = http2_classify_header_decode_result(result_value);
        status != header_decode_status::ok) {
        return status;
    }
    if (!context_value.status_) {
        return header_decode_status::protocol_error;
    }
    if (context_value.status_->status().is_informational()) {
        // 1xx interim head cannot carry END_STREAM. Without it, the remote receive
        // state remains head-pending so the next HEADERS is decoded as another head.
        if (stream.remote_receive().head_end_stream_pending() != nullptr) {
            return header_decode_status::protocol_error;
        }
        stream.mark_informational_response();
        if (discarded_header_stream_ && &stream == &*discarded_header_stream_) {
            return header_decode_status::ok;
        }
        reserve_event_slots(1);
        auto head = http_client_response_head_access::make(
            *context_value.status_, http_protocol_version::http2, resource_);
        if (!context_value.informational_fields_.empty()) {
            http_client_response_head_access::headers(head) =
                std::move(context_value.informational_fields_);
        }
        std::optional<http_client_request_content_signal> signal;
        if (context_value.status_->status() == http_status::continue_value && stream.release_request_continue()) {
            signal = http_client_request_content_signal::continue_value;
        }
        events_.push_back(http2_event(http2_informational_head_event(stream.id(), std::move(head), signal)));
        return header_decode_status::ok;
    }
    (void)stream.cancel_pending_request_continue();
    if (!stream.set_response_status(*context_value.status_)) {
        return header_decode_status::protocol_error;
    }
    const auto content_semantics = context_value.content_semantics_;
    // A 205 sender MUST NOT generate content (RFC 9110 15.3.6), but DATA framing
    // stays unambiguous: Content-Length still binds the DATA sum (RFC 9113 8.1.1;
    // 205 is not a no-content response under RFC 9110 6.4.1), and any content is
    // consumed and discarded by the DATA path instead of failing the response.
    if (content_semantics == http_response_content_semantics::without_content &&
        !stream.select_remote_content_metadata_only()) {
        return header_decode_status::protocol_error;
    }
    if (stream.tunnel().pending() != nullptr) {
        if (content_semantics == http_response_content_semantics::connect_tunnel) {
            if (!stream.accept_connect()) {
                return header_decode_status::protocol_error;
            }
            stream.begin_local_content_unbounded();
            (void)stream.open_local_connect_tunnel();
        } else {
            if (!stream.reject_connect()) {
                return header_decode_status::protocol_error;
            }
            if (http2_remote_peer_half_closed(stream) && !stream.remote_content().terminal_length_valid()) {
                return header_decode_status::protocol_error;
            }
            output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream.id(), {});
            (void)stream.reject_local_connect();
        }
    } else if (!stream.finalize_remote_content_head()) {
        return header_decode_status::protocol_error;
    }
    if (http2_remote_peer_half_closed(stream) && !stream.remote_content().terminal_length_valid()) {
        return header_decode_status::protocol_error;
    }
    return header_decode_status::ok;
}

}  // namespace ruvia::detail
