#include "client/http1_client_request_headers.h"

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_expectations.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_content_semantics.h"

#include "coding/http_content_coding.h"
#include "field/http_cors_fields.h"
#include "field/http_media_type.h"
#include "field/http_origin_fields.h"
#include "field/http_te_fields.h"
namespace ruvia {

bool add_head_bytes(std::size_t& total, std::size_t bytes_value) noexcept {
    if (bytes_value > max_http_header_bytes - total) {
        return false;
    }
    total += bytes_value;
    return true;
}

bool is_valid_client_te_field(std::string_view value) noexcept {
    return detail::is_valid_client_http_te_field_value(value);
}

bool analyze_headers(std::span<const http_header_view> headers, request_header_facts& facts,
    http1_client_request_prepare_error& error) noexcept {
    for (const auto& header : headers) {
        const auto name = header.name();
        const auto value = header.value();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
            error = http1_client_request_prepare_error::invalid_header;
            return false;
        }
        const auto kind = detail::classify_request_header(name);
        if (detail::http_ascii_equals_ignore_case(name, "Host")) {
            error = http1_client_request_prepare_error::host_header_managed_by_writer;
            return false;
        }
        if (detail::http_ascii_equals_ignore_case(name, "Content-Length")) {
            error = http1_client_request_prepare_error::content_length_managed_by_writer;
            return false;
        }
        if (detail::http_ascii_equals_ignore_case(name, "Transfer-Encoding")) {
            error = http1_client_request_prepare_error::transfer_encoding_unsupported;
            return false;
        }
        if (detail::http_ascii_equals_ignore_case(name, "Trailer")) {
            error = http1_client_request_prepare_error::trailer_section_unsupported;
            return false;
        }
        if (detail::http_ascii_equals_ignore_case(name, "Expect")) {
            error = http1_client_request_prepare_error::expect_header_managed_by_writer;
            return false;
        }
        if ((kind == detail::request_header_kind::origin &&
                !detail::is_valid_http_origin_field_value(value)) ||
            (kind == detail::request_header_kind::access_control_request_method &&
                !detail::is_valid_http_cors_request_method(value)) ||
            (kind == detail::request_header_kind::access_control_request_headers &&
                !detail::is_valid_http_cors_request_header_names(value))) {
            error = http1_client_request_prepare_error::invalid_header;
            return false;
        }
        if (const auto bit = detail::singleton_request_header_bit(kind); bit != 0) {
            if ((facts.singleton_headers_ & bit) != 0) {
                error = http1_client_request_prepare_error::invalid_header;
                return false;
            }
            facts.singleton_headers_ |= bit;
        }
        if (detail::http_ascii_equals_ignore_case(name, "Connection")) {
            if (facts.connection_options_.parse_field(value, detail::http_field_list_role::sender,
                    [](std::string_view option) noexcept {
                        return !detail::http_connection_option_conflicts_with_managed_field(option);
                    }) != detail::http_field_list_parse_status::ok) {
                error = http1_client_request_prepare_error::invalid_connection;
                return false;
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "Upgrade")) {
            if (facts.upgrade_protocols_.parse_field(value, detail::http_field_list_role::sender,
                    [](const detail::http_upgrade_protocol&) noexcept { return true; }) !=
                detail::http_field_list_parse_status::ok) {
                error = http1_client_request_prepare_error::invalid_upgrade;
                return false;
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "TE")) {
            if (!is_valid_client_te_field(value)) {
                error = http1_client_request_prepare_error::invalid_header;
                return false;
            }
            facts.has_te_ = true;
        } else if (detail::http_ascii_equals_ignore_case(name, "Content-Type")) {
            if (!detail::is_valid_http_content_type_field_value(value)) {
                error = http1_client_request_prepare_error::invalid_header;
                return false;
            }
            facts.has_content_type_ = true;
        } else if (detail::http_ascii_equals_ignore_case(name, "Content-Encoding")) {
            if (!detail::is_valid_http_content_encoding_field_value(
                    value, detail::http_field_list_role::sender)) {
                error = http1_client_request_prepare_error::invalid_header;
                return false;
            }
        }

        if (!add_head_bytes(facts.wire_bytes_, name.size()) || !add_head_bytes(facts.wire_bytes_, 2) ||
            !add_head_bytes(facts.wire_bytes_, value.size()) ||
            !add_head_bytes(facts.wire_bytes_, crlf.size())) {
            error = http1_client_request_prepare_error::header_too_large;
            return false;
        }
    }
    if (facts.upgrade_protocols_.has_field() && !facts.connection_options_.upgrade()) {
        error = http1_client_request_prepare_error::upgrade_connection_option_required;
        return false;
    }
    if (facts.has_te_ && !facts.connection_options_.te()) {
        error = http1_client_request_prepare_error::te_connection_option_required;
        return false;
    }
    return true;
}

}  // namespace ruvia
