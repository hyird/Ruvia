#include "ruvia/http/http_client_response_head.h"

#include <variant>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_limits.h"

#include "client/http_client_response_head.h"
#include "parser/http_header_block_parser.h"
#include "response/http_response_content_semantics.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_offers_protocol(
    const http1_client_exchange_state& exchange_state, const http_upgrade_protocol& selected) noexcept {
    const auto offered_protocols =
        http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state);
    if (offered_protocols.empty()) {
        return false;
    }
    bool offered = false;
    http_upgrade_protocols protocols;
    // The writer already validated this offer. Forwarded Upgrade fields may
    // retain empty members (RFC 9110 sections 2.2 and 5.6.1); selecting a
    // response protocol must not reapply generation grammar to those bytes.
    if (protocols.parse_field(offered_protocols, http_field_list_role::recipient,
            [&selected, &offered](const http_upgrade_protocol& candidate_value) noexcept {
                offered = offered || http_upgrade_protocol_equals(candidate_value, selected);
                return true;
            }) != http_field_list_parse_status::ok) {
        return false;
    }
    return protocols.has_protocol() && offered;
}

[[nodiscard]] request_header_kind classify_client_response_control_header(
    std::string_view name) noexcept {
    switch (name.size()) {
        case 7:
            return http_ascii_equals_ignore_case(name, "Upgrade")
                       ? request_header_kind::upgrade
                       : request_header_kind::other;
        case 10:
            return http_ascii_equals_ignore_case(name, "Connection")
                       ? request_header_kind::connection
                       : request_header_kind::other;
        case 14:
            return http_ascii_equals_ignore_case(name, "Content-Length")
                       ? request_header_kind::content_length
                       : request_header_kind::other;
        case 17:
            return http_ascii_equals_ignore_case(name, "Transfer-Encoding")
                       ? request_header_kind::transfer_encoding
                       : request_header_kind::other;
        default:
            return request_header_kind::other;
    }
}

}  // namespace

http1_client_status_line_parse_result_type parse_http1_client_status_line(std::string_view status_line) noexcept {
    const auto separator = status_line.find(' ');
    if (separator == std::string_view::npos) {
        return http1_client_response_parse_error::invalid_status_line;
    }

    // HTTP-version = "HTTP/" DIGIT "." DIGIT (RFC 9112 section 2.3). A higher
    // HTTP/1 minor version is processed as HTTP/1.1 (RFC 9110 section 6.2).
    const auto version = status_line.substr(0, separator);
    if (version.size() != 8 || !version.starts_with("HTTP/1.") || version[7] < '0' || version[7] > '9') {
        return http1_client_response_parse_error::unsupported_http_version;
    }

    // status-line = HTTP-version SP status-code SP [ reason-phrase ]. The
    // separator after the three digits is mandatory even for an empty phrase.
    if (status_line.size() < separator + 5 || status_line[separator + 4] != ' ') {
        return http1_client_response_parse_error::invalid_status_code;
    }

    // RFC 9110 section 15: a received 000..099 or 600..999 is processed as a
    // 5xx response.
    const auto parsed_status = http_received_status::try_parse(status_line.substr(separator + 1, 3));
    if (!parsed_status) {
        return http1_client_response_parse_error::invalid_status_code;
    }

    // Unlike a field value, reason-phrase can legitimately begin or end with
    // SP/HTAB. Validate bytes directly instead of applying OWS trimming rules.
    if (!is_valid_http_field_value_bytes(status_line.substr(separator + 5))) {
        return http1_client_response_parse_error::invalid_reason_phrase;
    }

    return http1_client_parsed_status_line{.received_status_ = *parsed_status,
        .protocol_version_ = version[7] == '0' ? http_protocol_version::http10 : http_protocol_version::http11};
}

http1_client_parsed_response_head::http1_client_parsed_response_head(
    const http1_client_parsed_status_line& status_line, std::pmr::memory_resource* resource) noexcept
    : received_status_(status_line.received_status_),
      status_code_(status_line.received_status_.status()),
      protocol_version_(status_line.protocol_version_),
      headers_(resource) {}

http1_client_response_head_parse_result_type parse_http1_client_response_head_fields(
    const http1_client_parsed_status_line& status_line, std::string_view field_section,
    const http1_client_exchange_state& exchange_state, std::pmr::memory_resource* resource) {
    http1_client_parsed_response_head output(status_line, resource);

    const auto content_semantics = classify_http_response_content_semantics(
        http1_client_exchange_state_access::method(exchange_state), output.status_code_);
    // RFC 9112 section 6.3 rule 1 ends 1xx, 204, 304 and HEAD responses at the
    // header section whatever framing fields they carry, and rule 1 precedes
    // the rule 5 Content-Length check. A recipient therefore ignores their
    // framing fields, even invalid ones, rather than rejecting a message it can
    // still delimit exactly; HEAD and 304 Content-Length remains a retained
    // field describing the selected representation.
    const bool framing_fields_apply = content_semantics == http_response_content_semantics::with_content;

    std::size_t cursor_value = 0;
    while (cursor_value < field_section.size()) {
        // The field-count rule of http_field_section_budget.
        if (output.headers_.size() == max_http_header_fields) {
            return http1_client_response_parse_error::header_too_large;
        }
        const auto field = scan_http1_field_line(field_section, cursor_value);
        if (!field) {
            return http1_client_response_parse_error::invalid_header;
        }
        const auto name = field->name();
        const auto value = field->value();
        const auto kind = classify_client_response_control_header(name);
        output.headers_.append(http_header_view{name, value});

        switch (kind) {
            case request_header_kind::content_length:
                // RFC 9112 section 6.3 rule 1: 1xx (including 101), 204, 304,
                // and HEAD responses end at the first empty line regardless of
                // framing fields; successful CONNECT becomes a tunnel.
                if (framing_fields_apply) {
                    switch (output.content_length_.parse_field(value)) {
                        case http_content_length_parse_status::ok:
                            break;
                        case http_content_length_parse_status::invalid:
                            return http1_client_response_parse_error::invalid_content_length;
                        case http_content_length_parse_status::conflicting:
                            return http1_client_response_parse_error::conflicting_content_length;
                    }
                }
                break;
            case request_header_kind::connection:
                if (output.connection_options_.parse_field(value, http_connection_field_role::response_recipient) !=
                    http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_connection;
                }
                break;
            case request_header_kind::transfer_encoding:
                output.saw_transfer_encoding_ = true;
                // Only a response with content is framed by Transfer-Encoding;
                // successful CONNECT is ignored by client-side rule.
                if (framing_fields_apply && output.protocol_version_ == http_protocol_version::http11) {
                    switch (output.transfer_encoding_.parse_field(value)) {
                        case http_transfer_encoding_parse_status::ok:
                            break;
                        case http_transfer_encoding_parse_status::malformed:
                            return http1_client_response_parse_error::invalid_transfer_encoding;
                        case http_transfer_encoding_parse_status::unsupported:
                            break;
                    }
                }
                break;
            case request_header_kind::upgrade:
                if (output.upgrade_protocols_.parse_field(value, http_field_list_role::recipient,
                        [&exchange_state, &output,
                            check_offered = content_semantics ==
                                            http_response_content_semantics::protocol_switch](
                            const http_upgrade_protocol& selected) noexcept {
                            if (check_offered && output.all_upgrade_protocols_offered_ &&
                                !request_offers_protocol(exchange_state, selected)) {
                                output.all_upgrade_protocols_offered_ = false;
                            }
                            return true;
                        }) != http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_upgrade;
                }
                break;
            default:
                break;
        }
    }

    if (output.transfer_encoding_.unsupported()) {
        return http1_client_response_parse_error::unsupported_transfer_encoding;
    }
    // RFC 9112 section 6.1: an HTTP/1.0 message carrying Transfer-Encoding is
    // treated as if its framing were faulty and the connection closes after it.
    // The transfer codings stay unparsed; the plan ignores Transfer-Encoding and
    // falls back to Content-Length or close delimiting.
    return output;
}

}  // namespace ruvia::detail

namespace ruvia {

std::optional<std::string_view> http_client_response_head::header(
    std::string_view name) const& noexcept {
    for (auto field = headers_.rbegin(); field != headers_.rend(); ++field) {
        if (http_ascii_equals_ignore_case(field->name(), name)) {
            return field->value();
        }
    }
    return std::nullopt;
}

}  // namespace ruvia
