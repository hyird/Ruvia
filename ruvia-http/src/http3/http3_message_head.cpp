#include "ruvia/http/http3_message_head.h"

#include <memory_resource>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_expectations.h"
#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/detail/util/http_ows.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http_media_type.h"
#include "ruvia/http/http_request_target.h"

#include "coding/http_content_coding.h"
#include "coding/http_content_length.h"
#include "field/binary_field_name.h"
#include "field/http_cors_fields.h"
#include "field/http_origin_fields.h"
#include "parser/http_request_target.h"

namespace ruvia {
namespace {

bool valid_path(std::string_view value, std::string_view method) noexcept {
    return (value == "*" && method == "OPTIONS") || is_valid_http_origin_form_target(value);
}

bool valid_authority(std::string_view value, std::string_view scheme) noexcept {
    if (http_ascii_equals_ignore_case(scheme, "http") ||
        http_ascii_equals_ignore_case(scheme, "https")) {
        const auto host = parse_http_authority_host(value);
        return host.has_value() && !host->empty();
    }
    for (const unsigned char ch : value) {
        if (ch <= 0x20 || ch == 0x7f || ch == '/' || ch == '?' || ch == '#' ||
            (ch == '@' && (scheme.empty() || http_ascii_equals_ignore_case(scheme, "http") ||
                              http_ascii_equals_ignore_case(scheme, "https")))) {
            return false;
        }
    }
    return !value.empty();
}

struct decode_state final {
    decode_state(http3_message_head& message_head, http3_message_head_kind message_kind,
        std::pmr::memory_resource* resource, std::size_t maximum_size)
        : head_(message_head),
          kind_(message_kind),
          section_size_(maximum_size),
          host_(resource) {}

    http3_message_head& head_;
    http3_message_head_kind kind_;
    http3_message_head_error error_{http3_message_head_error::message_error};
    detail::http_header_section_size section_size_;
    bool ordinary_seen_{false};
    bool method_seen_{false};
    bool protocol_seen_{false};
    bool scheme_seen_{false};
    bool authority_seen_{false};
    bool path_seen_{false};
    bool status_seen_{false};
    bool host_seen_{false};
    bool callback_rejected_{false};
    std::pmr::string host_;
    bool content_type_seen_{false};
    detail::http_content_length_state<std::uint64_t> content_length_;
};

bool fail(decode_state& state_value) noexcept {
    state_value.error_ = http3_message_head_error::message_error;
    state_value.callback_rejected_ = true;
    return false;
}

bool receive_field(void* opaque, http3_field_section_field_view field) {
    auto& state_value = *static_cast<decode_state*>(opaque);
    if (field.name_.empty() || !detail::is_valid_http_field_value_bytes(field.value_)) {
        return fail(state_value);
    }
    if (!state_value.section_size_.add(field.name_, field.value_)) {
        state_value.error_ = http3_message_head_error::field_section_too_large;
        state_value.callback_rejected_ = true;
        return false;
    }

    if (field.name_.front() == ':') {
        if (state_value.ordinary_seen_) {
            return fail(state_value);
        }
        const auto set_text = [&](std::pmr::string& target, bool& seen) {
            if (seen) {
                return false;
            }
            target.assign(field.value_);
            seen = true;
            return true;
        };
        if (state_value.kind_ == http3_message_head_kind::request) {
            if (field.name_ == ":method") {
                if (!detail::is_valid_http_header_name(field.value_) || !set_text(state_value.head_.method_, state_value.method_seen_)) {
                    return fail(state_value);
                }
            } else if (field.name_ == ":protocol") {
                if (!detail::is_valid_http_header_name(field.value_) ||
                    !set_text(state_value.head_.protocol_, state_value.protocol_seen_)) {
                    return fail(state_value);
                }
            } else if (field.name_ == ":scheme") {
                if (!detail::is_valid_uri_scheme(field.value_) || !set_text(state_value.head_.scheme_, state_value.scheme_seen_)) {
                    return fail(state_value);
                }
            } else if (field.name_ == ":authority") {
                if (field.value_.empty() || !set_text(state_value.head_.authority_, state_value.authority_seen_)) {
                    return fail(state_value);
                }
            } else if (field.name_ == ":path") {
                if (field.value_.empty() || !set_text(state_value.head_.path_, state_value.path_seen_)) {
                    return fail(state_value);
                }
            } else {
                return fail(state_value);
            }
        } else {
            if (field.name_ != ":status" || state_value.status_seen_ || field.value_.size() != 3 ||
                field.value_[0] < '1' || field.value_[0] > '5' ||
                field.value_[1] < '0' || field.value_[1] > '9' ||
                field.value_[2] < '0' || field.value_[2] > '9') {
                return fail(state_value);
            }
            state_value.head_.status_ = static_cast<std::uint16_t>((field.value_[0] - '0') * 100 +
                                                                   (field.value_[1] - '0') * 10 + (field.value_[2] - '0'));
            state_value.status_seen_ = true;
        }
        return true;
    }

    state_value.ordinary_seen_ = true;
    if (!detail::is_valid_binary_field_name(field.name_)) {
        return fail(state_value);
    }
    if (detail::is_forbidden_http_binary_connection_field(field.name_)) {
        return fail(state_value);
    }
    if (field.name_ == "te" &&
        (state_value.kind_ != http3_message_head_kind::request ||
            !http_ascii_equals_ignore_case(detail::http_trim_ows(field.value_), "trailers"))) {
        return fail(state_value);
    }
    if (field.name_ == "host") {
        if (state_value.kind_ != http3_message_head_kind::request || field.value_.empty() ||
            state_value.host_seen_ ||
            (state_value.authority_seen_ && state_value.head_.authority_ != field.value_)) {
            return fail(state_value);
        }
        state_value.host_.assign(field.value_);
        state_value.host_seen_ = true;
    }
    if (state_value.kind_ == http3_message_head_kind::request) {
        if ((field.name_ == "origin" && !detail::is_valid_http_origin_field_value(field.value_)) ||
            (field.name_ == "access-control-request-method" &&
                !detail::is_valid_http_cors_request_method(field.value_)) ||
            (field.name_ == "access-control-request-headers" &&
                !detail::is_valid_http_cors_request_header_names(field.value_)) ||
            (field.name_ == "expect" &&
                !detail::is_valid_received_http_expect_field_value(field.value_))) {
            return fail(state_value);
        }
    }
    if (field.name_ == "content-type") {
        if (state_value.content_type_seen_ || !is_valid_http_content_type_field_value(field.value_)) {
            return fail(state_value);
        }
        state_value.content_type_seen_ = true;
    } else if (field.name_ == "content-encoding" &&
               !detail::is_valid_http_content_encoding_field_value(
                   field.value_, detail::http_field_list_role::recipient)) {
        return fail(state_value);
    }
    if (field.name_ == "trailer" &&
        !(state_value.kind_ == http3_message_head_kind::request
                ? detail::is_valid_http_request_trailer_field_value(
                      field.value_, detail::http_field_list_role::recipient)
                : detail::is_valid_http_response_trailer_field_value(
                      field.value_, detail::http_field_list_role::recipient))) {
        return fail(state_value);
    }
    if (field.name_ == "content-length") {
        if (state_value.content_length_.parse_field(field.value_) != detail::http_content_length_parse_status::ok) {
            return fail(state_value);
        }
    }
    state_value.head_.headers_.emplace_back(field.name_, field.value_, state_value.head_.headers_.get_allocator().resource());
    return true;
}

std::optional<http3_message_head_error> finish_head(decode_state& state_value) {
    auto& head = state_value.head_;
    if (state_value.kind_ == http3_message_head_kind::request) {
        if (!state_value.method_seen_) {
            return http3_message_head_error::message_error;
        }
        if (head.method_ == "CONNECT") {
            if (state_value.protocol_seen_) {
                if (!state_value.scheme_seen_ || !state_value.authority_seen_ || !state_value.path_seen_ ||
                    !valid_path(head.path_, head.method_) || !valid_authority(head.authority_, head.scheme_)) {
                    return http3_message_head_error::message_error;
                }
            } else {
                const auto tunnel = detail::parse_http_authority(head.authority_);
                if (state_value.scheme_seen_ || state_value.path_seen_ || !state_value.authority_seen_ || !tunnel ||
                    tunnel->port_kind() != detail::http_authority_port_kind::value ||
                    *tunnel->port() == 0) {
                    return http3_message_head_error::message_error;
                }
            }
        } else {
            if (state_value.protocol_seen_ || !state_value.scheme_seen_ || !state_value.path_seen_ || !valid_path(head.path_, head.method_)) {
                return http3_message_head_error::message_error;
            }
            if (state_value.authority_seen_ && state_value.host_seen_ && head.authority_ != state_value.host_) {
                return http3_message_head_error::message_error;
            }
            const bool authority_required = http_ascii_equals_ignore_case(head.scheme_, "http") ||
                                            http_ascii_equals_ignore_case(head.scheme_, "https");
            if ((state_value.authority_seen_ && !valid_authority(head.authority_, head.scheme_)) ||
                (state_value.host_seen_ && !valid_authority(state_value.host_, head.scheme_))) {
                return http3_message_head_error::message_error;
            }
            if (authority_required && !state_value.authority_seen_ && !state_value.host_seen_) {
                return http3_message_head_error::message_error;
            }
            if (!state_value.authority_seen_ && state_value.host_seen_) {
                head.authority_.assign(state_value.host_);
            }
        }
    } else if (!state_value.status_seen_) {
        return http3_message_head_error::message_error;
    }
    head.content_length_ = state_value.content_length_.value();
    return {};
}

}  // namespace

http3_message_header::http3_message_header(std::pmr::memory_resource* resource)
    : name_(resource),
      value_(resource) {}

http3_message_header::http3_message_header(
    std::string_view header_name, std::string_view header_value, std::pmr::memory_resource* resource)
    : name_(header_name, resource),
      value_(header_value, resource) {}

http3_message_head::http3_message_head(std::pmr::memory_resource* resource)
    : method_(resource),
      protocol_(resource),
      scheme_(resource),
      authority_(resource),
      path_(resource),
      headers_(resource) {}

std::variant<http3_message_head, http3_message_head_error> decode_http3_message_head(
    std::span<const char> field_section, http3_message_head_kind kind, std::pmr::memory_resource* resource,
    http3_message_head_limits limits) {
    auto* owner_value = resource != nullptr ? resource : std::pmr::get_default_resource();
    http3_message_head head(owner_value);
    decode_state state_value{head, kind, owner_value, limits.max_field_section_size_};
    const http3_field_section_limits decoder_limits{limits.max_encoded_bytes_, limits.max_field_section_size_,
        limits.max_fields_};
    const auto decoded = decode_http3_field_section(field_section, receive_field, &state_value, decoder_limits, owner_value);
    if ((decoded.index() != 0)) {
        if (std::get<1>(decoded) == http3_field_section_error::field_list_too_large ||
            std::get<1>(decoded) == http3_field_section_error::field_section_too_large ||
            std::get<1>(decoded) == http3_field_section_error::too_many_fields) {
            return http3_message_head_error::field_section_too_large;
        }
        if (state_value.callback_rejected_) {
            return state_value.error_;
        }
        return http3_message_head_error::qpack_decompression_failed;
    }
    if (auto error = finish_head(state_value)) {
        return *error;
    }
    return head;
}

std::variant<http3_decoded_message_head_type, http3_message_head_error> decode_http3_message_head(
    http3_qpack_decoder& decoder, std::uint64_t stream_id, std::span<const char> field_section,
    http3_message_head_kind kind, std::pmr::memory_resource* resource, http3_message_head_limits limits) {
    auto* owner_value = resource ? resource : std::pmr::get_default_resource();
    http3_message_head head(owner_value);
    decode_state state_value{head, kind, owner_value, limits.max_field_section_size_};
    if (field_section.size() > limits.max_encoded_bytes_) {
        return http3_message_head_error::field_section_too_large;
    }
    const auto result_value = decoder.decode(stream_id, field_section, receive_field, &state_value);
    if ((result_value.index() != 0)) {
        return std::get<1>(result_value) == http3_qpack_connection_error::limit
                   ? http3_message_head_error::field_section_too_large
                   : http3_message_head_error::qpack_decompression_failed;
    }
    if (std::get<0>(result_value).status_ == http3_qpack_decode_status::blocked) {
        return http3_decoded_message_head_type{http3_qpack_blocked{}};
    }
    if (state_value.callback_rejected_) {
        return state_value.error_;
    }
    if (std::get<0>(result_value).fields_ > limits.max_fields_) {
        return http3_message_head_error::field_section_too_large;
    }
    if (auto error = finish_head(state_value)) {
        return *error;
    }
    return http3_decoded_message_head_type{std::move(head)};
}

}  // namespace ruvia
