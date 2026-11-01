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

[[nodiscard]] const char* common_validation_message(detail::http_interim_response_header_validation_status status) noexcept {
    switch (status) {
        case detail::http_interim_response_header_validation_status::content_length_forbidden:
            return "Content-Length is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::transfer_encoding_forbidden:
            return "Transfer-Encoding is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::trailer_forbidden:
            return "Trailer is forbidden on an interim response";
        case detail::http_interim_response_header_validation_status::repeated_singleton:
            return "repeated singleton interim response header";
        case detail::http_interim_response_header_validation_status::invalid_header:
        case detail::http_interim_response_header_validation_status::ok:
            break;
    }
    return "invalid HTTP/1 interim response header";
}

void analyze_http1_fields(const http_interim_response_head& response, http1_interim_header_facts& facts) {
    detail::http_interim_response_sender_validator validator;
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        const auto value = header.value();
        auto kind = detail::request_header_kind::other;
        const auto status = validator.validate(name, value, &kind);
        if (status != detail::http_interim_response_header_validation_status::ok) {
            throw http1_response_head_error(common_validation_message(status));
        }
        switch (detail::parse_http_response_control_field(
            kind, value, facts.connection_options_, facts.upgrade_protocols_)) {
            case detail::http_response_control_field_status::invalid_connection:
                throw http1_response_head_error("invalid HTTP/1 interim response Connection field");
            case detail::http_response_control_field_status::invalid_upgrade:
                throw http1_response_head_error("invalid HTTP/1 interim response Upgrade field");
            case detail::http_response_control_field_status::te_forbidden:
                throw http1_response_head_error("TE is not a response field");
            case detail::http_response_control_field_status::ok:
                break;
        }

        if (!facts.section_.add_field(name, value)) {
            throw http1_response_head_error("HTTP/1 interim response head is too large");
        }
    }
    if (!detail::http_response_upgrade_pair_valid(facts.connection_options_, facts.upgrade_protocols_)) {
        throw http1_response_head_error("HTTP/1 interim Upgrade requires Connection: Upgrade");
    }
}

[[nodiscard]] char* write_interim_response_head(
    char* output, const http_interim_response_head& response, std::string_view reason_phrase) noexcept {
    detail::http_raw_wire_sink sink(output);
    detail::emit_http1_status_line(sink, response.status(), reason_phrase);
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

std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer) {
    const auto reason_phrase = http_reason_phrase(response.status());
    http1_interim_header_facts facts;
    analyze_http1_fields(response, facts);
    if (!facts.section_.add(detail::http1_status_line_size(reason_phrase)) || !facts.section_.add(crlf.size())) {
        throw http1_response_head_error("HTTP/1 interim response head is too large");
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
