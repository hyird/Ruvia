#include <charconv>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

#include "ruvia/http/detail/coding/http_response_content_semantics.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

#include "client/http_client_access.h"
#include "coding/http_content_length.h"
#include "field/http_interim_response_validation.h"
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
          interim_headers_(http_field_list_role::recipient),
          informational_fields_(resource) {}

    http2_header_decode_context base_;
    http_interim_response_header_validator interim_headers_;
    response_field_state fields_;
    std::pmr::vector<http_header> informational_fields_;
    std::optional<http_status_code> status_;
    bool saw_regular_{false};
};

// Client-role response head decode: ':status' once and first, then validated regular
// headers into the stream's header table (1xx heads are validated but not stored).
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
        int parsed_status = 0;
        const auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), parsed_status);
        if (value.size() != 3 || ec != std::errc{} || ptr != value.data() + value.size()) {
            return false;
        }
        const auto status = http_status_code::try_from_value(static_cast<std::uint16_t>(parsed_status));
        if (!status || *status == http_status::switching_protocols) {
            return false;
        }
        context_value->status_ = *status;
        return true;
    }
    if (!context_value->status_ || !http2_is_valid_decoded_response_header(name, value)) {
        return false;
    }
    if (!context_value->base_.accept_regular_field()) {
        return false;
    }
    context_value->saw_regular_ = true;
    if (context_value->status_->is_informational()) {
        // Validate interim fields as a recipient before taking owned snapshots.
        if (context_value->interim_headers_.validate(name, value) !=
            http_interim_response_header_validation_status::ok) {
            return false;
        }
        context_value->informational_fields_.push_back(http_header::copy_of(
            name, value, context_value->informational_fields_.get_allocator().resource()));
        return true;
    }
    const auto kind = classify_request_header(name);
    const auto response_known_bit = classify_response_header_name(name);
    const auto response_content_semantics =
        classify_http_response_content_semantics(stream.request_known_method(), *context_value->status_);
    const bool successful_connect =
        response_content_semantics == http_response_content_semantics::connect_tunnel;
    if (response_known_bit == response_header_content_length && successful_connect) {
        // RFC 9110 9.3.6: a client ignores Content-Length on a successful CONNECT
        // response. It describes neither HTTP content nor the following tunnel DATA.
        return true;
    }
    if (validate_response_field(kind, value, http_field_list_role::recipient, context_value->fields_) !=
        response_field_status::ok) {
        return false;
    }
    if (response_known_bit == response_header_content_length) {
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

    // Response trailers have different field semantics from request trailers.
    // Reuse the same response-specific permission table that proves outbound
    // trailer sections, after applying HTTP/2's lowercase and connection-field
    // rules. In particular, Accept-Ranges and ETag are explicitly trailer-safe,
    // while response controls such as Date and Location are not.
    return context_value.accept_regular_field() && http2_is_valid_decoded_response_header(name, value) &&
           !is_forbidden_response_trailer_name(name) &&
           context_value.stream_.append_remote_trailer(name, value);
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
    if (context_value.status_->is_informational()) {
        // 1xx interim head cannot carry END_STREAM. Without it, the remote receive
        // state remains head-pending so the next HEADERS is decoded as another head.
        if (stream.remote_receive().head_end_stream_pending() != nullptr) {
            return header_decode_status::protocol_error;
        }
        stream.mark_informational_response();
        reserve_event_slots(1);
        auto head = http_client_response_head_access::make(
            *context_value.status_, http_protocol_version::http2, resource_);
        if (!context_value.informational_fields_.empty()) {
            auto& headers = http_client_response_head_access::headers(head);
            headers.reserve(context_value.informational_fields_.size());
            for (auto& field : context_value.informational_fields_) {
                headers.push_back(std::move(field));
            }
        }
        std::optional<http_client_request_content_signal> signal;
        if (*context_value.status_ == http_status::continue_value && stream.release_request_continue()) {
            signal = http_client_request_content_signal::continue_value;
        }
        events_.push_back(http2_event(http2_informational_head_event(stream.id(), std::move(head), signal)));
        return header_decode_status::ok;
    }
    (void)stream.cancel_pending_request_continue();
    if (!stream.set_response_status(*context_value.status_)) {
        return header_decode_status::protocol_error;
    }
    const auto content_semantics =
        classify_http_response_content_semantics(stream.request_known_method(), *context_value.status_);
    // Ordinary 205 responses have a zero-length content phase (RFC 9110 15.3.6).
    // HEAD keeps representation metadata, and successful CONNECT carries a tunnel;
    // neither uses the ordinary response-content length constraint.
    if (*context_value.status_ == http_status::reset_content &&
        content_semantics == http_response_content_semantics::with_content &&
        !stream.declare_remote_content_length(0)) {
        return header_decode_status::protocol_error;
    }
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
