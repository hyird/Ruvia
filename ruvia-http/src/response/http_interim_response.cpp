#include "ruvia/http/http_interim_response.h"

#include <stdexcept>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/response/http1_status_line.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

#include "field/http_interim_response_validation.h"
#include "util/http_wire_sink.h"

namespace ruvia {
namespace {

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
    detail::http_interim_response_sender_validator validator;
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        const auto value = header.value();
        auto kind = detail::request_header_kind::other;
        const auto status = validator.validate(name, value, &kind);
        if (status != detail::http_interim_response_header_validation_status::ok) {
            error = common_validation_error(status);
            return false;
        }
        switch (kind) {
            case detail::request_header_kind::connection:
                if (facts.connection_options_.parse_field(value, detail::http_field_list_role::sender) !=
                    detail::http_field_list_parse_status::ok) {
                    error = http1_interim_response_prepare_error::invalid_connection;
                    return false;
                }
                break;
            case detail::request_header_kind::upgrade:
                if (facts.upgrade_protocols_.parse_field(value, detail::http_field_list_role::sender) !=
                    detail::http_field_list_parse_status::ok) {
                    error = http1_interim_response_prepare_error::invalid_upgrade;
                    return false;
                }
                break;
            case detail::request_header_kind::te:
                error = http1_interim_response_prepare_error::te_field_forbidden;
                return false;
            default:
                break;
        }

        if (!facts.section_.add_field(name, value)) {
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

[[nodiscard]] char* write_interim_response_head(
    char* output, const http_interim_response_head& response, std::string_view reason_phrase) noexcept {
    detail::http_raw_wire_sink sink(output);
    detail::emit_http1_status_line(sink, http_protocol_version::http11, response.status(), reason_phrase);
    for (const auto& header : response.headers()) {
        detail::emit_http1_field_line(sink, header.name(), header.value());
    }
    sink.append(crlf);
    return sink.position();
}

}  // namespace

http_interim_response_head::http_interim_response_head(http_status_code status_code, header_init_type headers)
    : status_code_(status_code),
      headers_(headers) {
    if (!detail::http_interim_status_code_valid(status_code)) {
        throw std::invalid_argument(status_code == http_status::switching_protocols
                                        ? "Switching Protocols requires a dedicated protocol driver"
                                        : "invalid interim HTTP status code");
    }
}

std::string_view http1_interim_response_prepare_error_message(
    http1_interim_response_prepare_error error) noexcept {
    switch (error) {
        case http1_interim_response_prepare_error::invalid_header:
            return "invalid HTTP/1 interim response header";
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

std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small,
    http1_interim_response_prepare_error>
prepare_http1_interim_response_head(
    const http_interim_response_head& response, std::span<char> head_buffer) noexcept {
    const auto reason_phrase = http_reason_phrase(response.status());
    http1_interim_header_facts facts;
    http1_interim_response_prepare_error error = http1_interim_response_prepare_error::invalid_header;
    if (!analyze_http1_fields(response, facts, error)) {
        return error;
    }
    if (!facts.section_.add(detail::http1_status_line_size(reason_phrase)) || !facts.section_.add(crlf.size())) {
        return http1_interim_response_prepare_error::header_too_large;
    }
    const auto head_bytes = facts.section_.bytes();
    if (head_buffer.size() < head_bytes) {
        return http1_interim_response_buffer_too_small(head_bytes);
    }

    const auto output = head_buffer.first(head_bytes);
    detail::write_http_wire(output,
        detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), response.headers()),
        [&](char* destination) noexcept {
            return write_interim_response_head(destination, response, reason_phrase);
        });

    return prepared_http1_interim_response(std::string_view(head_buffer.data(), head_bytes),
        facts.connection_options_.close()
            ? http1_interim_connection_disposition::close_after_interim_response
            : http1_interim_connection_disposition::unchanged);
}

}  // namespace ruvia
