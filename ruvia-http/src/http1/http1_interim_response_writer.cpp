#include "ruvia/http/http1_interim_response_writer.h"

#include <cstring>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_status.h"

#include "field/http_interim_response_validation.h"

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
    std::size_t wire_bytes_{0};
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
};

[[nodiscard]] bool add_head_bytes(std::size_t& total, std::size_t bytes_value) noexcept {
    if (bytes_value > max_http_header_bytes - total) {
        return false;
    }
    total += bytes_value;
    return true;
}

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
        if (detail::http_ascii_equals_ignore_case(name, "Connection")) {
            if (facts.connection_options_.parse_field(header.value(),
                    detail::http_field_list_role::sender, [](std::string_view option) noexcept {
                        return !detail::http_connection_option_conflicts_with_managed_field(option);
                    }) != detail::http_field_list_parse_status::ok) {
                error = http1_interim_response_prepare_error::invalid_connection;
                return false;
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "Upgrade")) {
            if (facts.upgrade_protocols_.parse_field(header.value(),
                    detail::http_field_list_role::sender,
                    [](const detail::http_upgrade_protocol&) noexcept { return true; }) !=
                detail::http_field_list_parse_status::ok) {
                error = http1_interim_response_prepare_error::invalid_upgrade;
                return false;
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "TE")) {
            error = http1_interim_response_prepare_error::te_field_forbidden;
            return false;
        }

        if (!add_head_bytes(facts.wire_bytes_, name.size()) || !add_head_bytes(facts.wire_bytes_, 2) ||
            !add_head_bytes(facts.wire_bytes_, header.value().size()) ||
            !add_head_bytes(facts.wire_bytes_, crlf.size())) {
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

void append_view(char*& cursor_value, std::string_view value) noexcept {
    if (!value.empty()) {
        std::memcpy(cursor_value, value.data(), value.size());
        cursor_value += value.size();
    }
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
    facts.wire_bytes_ =
        http11_status_prefix.size() + status_token.size() + 1 + reason_phrase.size() + crlf.size();
    http1_interim_response_prepare_error error = http1_interim_response_prepare_error::invalid_header;
    if (!analyze_http1_fields(response, facts, error)) {
        return detail::http1_interim_response_prepare_result_access::failure(error);
    }
    if (!add_head_bytes(facts.wire_bytes_, crlf.size())) {
        return detail::http1_interim_response_prepare_result_access::failure(
            http1_interim_response_prepare_error::header_too_large);
    }
    if (head_buffer.size() < facts.wire_bytes_) {
        return detail::http1_interim_response_prepare_result_access::buffer_too_small(facts.wire_bytes_);
    }

    char* cursor_value = head_buffer.data();
    append_view(cursor_value, http11_status_prefix);
    append_view(cursor_value, detail::http_status_code_token_view(status_token));
    *cursor_value++ = ' ';
    append_view(cursor_value, reason_phrase);
    append_view(cursor_value, crlf);
    for (const auto& header : response.headers()) {
        append_view(cursor_value, header.name());
        append_view(cursor_value, ": ");
        append_view(cursor_value, header.value());
        append_view(cursor_value, crlf);
    }
    append_view(cursor_value, crlf);

    return detail::http1_interim_response_prepare_result_access::prepared(
        std::string_view(head_buffer.data(), facts.wire_bytes_),
        facts.connection_options_.close()
            ? http1_interim_connection_disposition::close_after_interim_response
            : http1_interim_connection_disposition::unchanged);
}

}  // namespace ruvia
