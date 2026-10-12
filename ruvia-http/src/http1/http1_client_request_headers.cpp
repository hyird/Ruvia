#include "client/http1_client_request_headers.h"

#include "ruvia/http/detail/parser/http_parser_syntax.h"

namespace ruvia {

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
        // The HTTP/1 writer owns Host and refuses message framing it does not
        // generate; the remaining semantics are version-independent.
        switch (kind) {
            case detail::request_header_kind::host:
                error = http1_client_request_prepare_error::host_header_managed_by_writer;
                return false;
            case detail::request_header_kind::transfer_encoding:
                error = http1_client_request_prepare_error::transfer_encoding_unsupported;
                return false;
            case detail::request_header_kind::trailer:
                error = http1_client_request_prepare_error::trailer_section_unsupported;
                return false;
            default:
                break;
        }
        switch (detail::validate_request_field(kind, value, detail::http_field_list_role::sender, facts.fields_)) {
            case detail::request_field_status::ok:
                break;
            case detail::request_field_status::invalid:
                error = http1_client_request_prepare_error::invalid_header;
                return false;
            case detail::request_field_status::managed_by_sender:
                error = kind == detail::request_header_kind::expect
                            ? http1_client_request_prepare_error::expect_header_managed_by_writer
                            : http1_client_request_prepare_error::content_length_managed_by_writer;
                return false;
        }
        if (kind == detail::request_header_kind::connection) {
            if (facts.connection_options_.parse_field(value, detail::http_field_list_role::sender,
                    [](std::string_view option) noexcept {
                        return !detail::http_connection_option_conflicts_with_managed_field(option);
                    }) != detail::http_field_list_parse_status::ok) {
                error = http1_client_request_prepare_error::invalid_connection;
                return false;
            }
        } else if (kind == detail::request_header_kind::upgrade) {
            if (facts.upgrade_protocols_.parse_field(value, detail::http_field_list_role::sender,
                    [](const detail::http_upgrade_protocol&) noexcept { return true; }) !=
                detail::http_field_list_parse_status::ok) {
                error = http1_client_request_prepare_error::invalid_upgrade;
                return false;
            }
        } else if (kind == detail::request_header_kind::te) {
            facts.has_te_ = true;
        }

        if (!facts.section_.add_field(name, value)) {
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
