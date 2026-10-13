#include "ruvia/http/http1_interim_response_writer.h"

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

#include "field/http_interim_response_validation.h"
#include "util/http_wire_sink.h"

namespace ruvia::detail {

struct http1_interim_response_prepare_result_access final {
    [[nodiscard]] static constexpr http1_interim_response_prepare_result buffer_too_small(
        std::size_t required_head_bytes) noexcept {
        return http1_interim_response_prepare_result(
            http1_interim_response_buffer_too_small(required_head_bytes));
    }

    [[nodiscard]] static constexpr http1_interim_response_prepare_result failure(
        http1_interim_response_prepare_error error) noexcept {
        return http1_interim_response_prepare_result(http1_interim_response_prepare_failure(error));
    }

    [[nodiscard]] static constexpr http1_interim_response_prepare_result prepared(
        std::string_view head, http1_interim_connection_disposition connection_disposition) noexcept {
        return http1_interim_response_prepare_result(
            prepared_http1_interim_response(head, connection_disposition));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr std::string_view http11_status_prefix = "HTTP/1.1 ";
constexpr std::string_view crlf = "\r\n";

struct http1_interim_header_facts final {
    http_field_section_budget section_;
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
};

[[nodiscard]] http1_interim_response_prepare_error common_validation_error(
    detail::http_interim_response_header_validation_status status) noexcept {
    switch (status) {
        case detail::http_interim_response_header_validation_status::invalid_header:
            return http1_interim_response_prepare_error::invalid_header;
        case detail::http_interim_response_header_validation_status::content_length_forbidden:
            return http1_interim_response_prepare_error::content_length_forbidden;
        case detail::http_interim_response_header_validation_status::transfer_encoding_forbidden:
            return http1_interim_response_prepare_error::transfer_encoding_forbidden;
        case detail::http_interim_response_header_validation_status::trailer_forbidden:
            return http1_interim_response_prepare_error::trailer_forbidden;
        case detail::http_interim_response_header_validation_status::repeated_singleton:
            return http1_interim_response_prepare_error::repeated_singleton;
        case detail::http_interim_response_header_validation_status::ok:
            break;
    }
    return http1_interim_response_prepare_error::invalid_header;
}

[[nodiscard]] bool analyze_http1_fields(const http_interim_response_head& response,
    http1_interim_header_facts& facts, http1_interim_response_prepare_error& error) noexcept {
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        if (http_ascii_equals_ignore_case(name, "Connection")) {
            if (facts.connection_options_.parse_field(header.value(), detail::http_field_list_role::sender) !=
                detail::http_field_list_parse_status::ok) {
                error = http1_interim_response_prepare_error::invalid_connection;
                return false;
            }
        } else if (http_ascii_equals_ignore_case(name, "Upgrade")) {
            if (facts.upgrade_protocols_.parse_field(header.value(), detail::http_field_list_role::sender) !=
                detail::http_field_list_parse_status::ok) {
                error = http1_interim_response_prepare_error::invalid_upgrade;
                return false;
            }
        } else if (http_ascii_equals_ignore_case(name, "TE")) {
            error = http1_interim_response_prepare_error::te_field_forbidden;
            return false;
        }

        if (!facts.section_.add_field(name, header.value())) {
            error = http1_interim_response_prepare_error::header_too_large;
            return false;
        }
    }
    if (facts.upgrade_protocols_.has_field() && !facts.connection_options_.upgrade()) {
        error = http1_interim_response_prepare_error::upgrade_connection_option_required;
        return false;
    }
    return true;
}

[[nodiscard]] char* write_interim_response_head(char* output, const http_interim_response_head& response,
    std::string_view status_token, std::string_view reason_phrase) noexcept {
    detail::http_raw_wire_sink sink(output);
    sink.append(http11_status_prefix);
    sink.append(status_token);
    sink.append(' ');
    sink.append(reason_phrase);
    sink.append(crlf);
    for (const auto& header : response.headers()) {
        detail::emit_http1_field_line(sink, header.name(), header.value());
    }
    sink.append(crlf);
    return sink.position();
}

}  // namespace

std::string_view http1_interim_response_prepare_error_message(
    http1_interim_response_prepare_error error) noexcept {
    switch (error) {
        case http1_interim_response_prepare_error::invalid_header:
            return "invalid HTTP/1 interim response header";
        case http1_interim_response_prepare_error::too_many_headers:
            return "too many HTTP/1 interim response headers";
        case http1_interim_response_prepare_error::content_length_forbidden:
            return "Content-Length is forbidden on an interim response";
        case http1_interim_response_prepare_error::transfer_encoding_forbidden:
            return "Transfer-Encoding is forbidden on an interim response";
        case http1_interim_response_prepare_error::trailer_forbidden:
            return "Trailer is forbidden on an interim response";
        case http1_interim_response_prepare_error::te_field_forbidden:
            return "TE is not a response field";
        case http1_interim_response_prepare_error::repeated_singleton:
            return "repeated singleton interim response header";
        case http1_interim_response_prepare_error::invalid_connection:
            return "invalid HTTP/1 interim response Connection field";
        case http1_interim_response_prepare_error::invalid_upgrade:
            return "invalid HTTP/1 interim response Upgrade field";
        case http1_interim_response_prepare_error::upgrade_connection_option_required:
            return "HTTP/1 interim Upgrade requires Connection: Upgrade";
        case http1_interim_response_prepare_error::header_too_large:
            return "HTTP/1 interim response head is too large";
    }
    return "invalid HTTP/1 interim response";
}

http1_interim_response_prepare_result http1_interim_response_writer::prepare(
    const http_interim_response_head& response, std::span<char> head_buffer) const noexcept {
    if (response.headers().size() > max_http_header_fields) {
        return detail::http1_interim_response_prepare_result_access::failure(
            http1_interim_response_prepare_error::too_many_headers);
    }
    const auto common_validation = detail::validate_http_interim_response_headers(response);
    if (common_validation != detail::http_interim_response_header_validation_status::ok) {
        return detail::http1_interim_response_prepare_result_access::failure(
            common_validation_error(common_validation));
    }

    const auto reason_phrase = http_reason_phrase(response.status());
    const auto status_token = detail::http_status_code_token(response.status());
    http1_interim_header_facts facts;
    http1_interim_response_prepare_error error = http1_interim_response_prepare_error::invalid_header;
    if (!analyze_http1_fields(response, facts, error)) {
        return detail::http1_interim_response_prepare_result_access::failure(error);
    }
    if (!facts.section_.add(
            http11_status_prefix.size() + status_token.size() + 1 + reason_phrase.size() + crlf.size()) ||
        !facts.section_.add(crlf.size())) {
        return detail::http1_interim_response_prepare_result_access::failure(
            http1_interim_response_prepare_error::header_too_large);
    }
    const auto head_bytes = facts.section_.bytes();
    if (head_buffer.size() < head_bytes) {
        return detail::http1_interim_response_prepare_result_access::buffer_too_small(head_bytes);
    }

    const auto output = head_buffer.first(head_bytes);
    const auto status_text = detail::http_status_code_token_view(status_token);
    detail::write_http_wire(output,
        detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), response.headers()),
        [&](char* destination) noexcept {
            return write_interim_response_head(destination, response, status_text, reason_phrase);
        });

    return detail::http1_interim_response_prepare_result_access::prepared(
        std::string_view(head_buffer.data(), head_bytes),
        facts.connection_options_.close()
            ? http1_interim_connection_disposition::close_after_interim_response
            : http1_interim_connection_disposition::unchanged);
}

}  // namespace ruvia
