#include "client/http1_client_request_headers.h"

#include "ruvia/http/detail/parser/http_parser_syntax.h"

namespace ruvia {

bool analyze_header(http_header_view header, detail::request_header_kind kind,
    request_header_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source) noexcept {
    const auto name = header.name();
    const auto value = header.value();
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        error = http1_client_request_prepare_error::invalid_header;
        return false;
    }
    // The HTTP/1 writer owns Host and refuses message framing it does not
    // generate; the remaining semantics are version-independent.
    switch (kind) {
        case detail::request_header_kind::host:
            error = http1_client_request_prepare_error::host_header_managed_by_writer;
            return false;
        case detail::request_header_kind::transfer_encoding:
            error = http1_client_request_prepare_error::transfer_encoding_unsupported;
            return false;
        default:
            break;
    }
    switch (detail::validate_request_field(kind, value, detail::http_field_list_role::sender, facts.fields_, source)) {
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
        if (facts.connection_options_.parse_field(value, detail::http_connection_field_role::sender) !=
            detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_connection;
            return false;
        }
    } else if (kind == detail::request_header_kind::upgrade) {
        const auto role = source == http_request_field_source::forwarded
                              ? detail::http_field_list_role::recipient
                              : detail::http_field_list_role::sender;
        if (facts.upgrade_protocols_.parse_field(value, role) !=
            detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_upgrade;
            return false;
        }
    } else if (kind == detail::request_header_kind::te) {
        facts.has_te_ = true;
    } else if (kind == detail::request_header_kind::trailer) {
        facts.has_trailer_ = true;
    }

    if (!facts.section_.add_field(name, value)) {
        error = http1_client_request_prepare_error::header_too_large;
        return false;
    }
    return true;
}

bool finalize_headers(
    const request_header_facts& facts, http1_client_request_prepare_error& error) noexcept {
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

bool analyze_headers(std::span<const http_header_view> headers, request_header_facts& facts,
    http1_client_request_prepare_error& error, http_request_field_source source) noexcept {
    for (const auto& header : headers) {
        if (!analyze_header(header, detail::classify_request_header(header.name()), facts, error, source)) {
            return false;
        }
    }
    return finalize_headers(facts, error);
}

}  // namespace ruvia
