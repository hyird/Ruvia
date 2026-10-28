#include "ruvia/http/http_client_response_head.h"

#include <variant>

#include "ruvia/http/detail/coding/http_response_content_semantics.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_field_syntax.h"

#include "client/http_client_response_head.h"
#include "field/field_value_scan.h"
#include "field/response_field_validation.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] http1_client_status_line_parse_result_type parse_status_line(
    std::string_view status_line) noexcept {
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

[[nodiscard]] bool request_offers_protocol(
    const http1_client_exchange_state& exchange_state, const http_upgrade_protocol& selected) noexcept {
    const auto offered_protocols =
        http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state);
    if (offered_protocols.empty()) {
        return false;
    }
    bool offered = false;
    http_upgrade_protocols protocols;
    if (protocols.parse_field(offered_protocols, http_field_list_role::sender,
            [&selected, &offered](const http_upgrade_protocol& candidate_value) noexcept {
                offered = offered || http_upgrade_protocol_equals(candidate_value, selected);
                return true;
            }) != http_field_list_parse_status::ok) {
        return false;
    }
    return protocols.has_protocol() && offered;
}

}  // namespace

http1_client_parsed_response_head::http1_client_parsed_response_head(
    const http1_client_parsed_status_line& status_line, std::pmr::memory_resource* resource) noexcept
    : received_status_(status_line.received_status_),
      status_code_(status_line.received_status_.status()),
      protocol_version_(status_line.protocol_version_),
      headers_(resource) {}

http1_client_response_head_parse_result_type parse_http1_client_response_head_fields(
    std::string_view head_section, const http1_client_exchange_state& exchange_state,
    std::pmr::memory_resource* resource) {
    const auto first_line_end = head_section.find("\r\n");
    const auto first_line =
        first_line_end == std::string_view::npos ? head_section : head_section.substr(0, first_line_end);
    const auto status_line = parse_status_line(first_line);
    if ((status_line.index() != 0)) {
        return std::get<1>(status_line);
    }
    http1_client_parsed_response_head output(std::get<0>(status_line), resource);

    const auto content_semantics = classify_http_response_content_semantics(
        http1_client_exchange_state_access::method(exchange_state), output.status_code_);
    response_field_state response_fields;
    // RFC 9112 section 6.3 rule 1 ends 1xx, 204, 304 and HEAD responses at the
    // header section whatever framing fields they carry, and rule 1 precedes
    // the rule 5 Content-Length check. A recipient therefore ignores their
    // framing fields, even invalid ones, rather than rejecting a message it can
    // still delimit exactly; HEAD and 304 Content-Length remains a retained
    // field describing the selected representation.
    const bool framing_fields_apply = content_semantics == http_response_content_semantics::with_content;

    auto remaining = first_line_end == std::string_view::npos ? std::string_view{}
                                                              : head_section.substr(first_line_end + 2);
    std::size_t cursor_value = 0;
    while (cursor_value < remaining.size()) {
        const auto name_start = cursor_value;
        cursor_value += http_token_prefix_size(
            {remaining.data() + cursor_value, remaining.size() - cursor_value});
        if (cursor_value == name_start || cursor_value >= remaining.size() ||
            remaining[cursor_value] != ':') {
            return http1_client_response_parse_error::invalid_header;
        }
        const auto name = remaining.substr(name_start, cursor_value - name_start);
        ++cursor_value;

        while (cursor_value < remaining.size() && is_http_ows_char(remaining[cursor_value])) {
            ++cursor_value;
        }
        const auto value_start = cursor_value;
        cursor_value += http_field_value_prefix_size(
            {remaining.data() + cursor_value, remaining.size() - cursor_value});
        if (cursor_value < remaining.size() &&
            (cursor_value + 1 >= remaining.size() || remaining[cursor_value] != '\r' ||
                remaining[cursor_value + 1] != '\n')) {
            return http1_client_response_parse_error::invalid_header;
        }
        // Leading OWS is already consumed, so trimming only drops trailing OWS.
        const auto value = http_trim_ows(remaining.substr(value_start, cursor_value - value_start));
        const auto kind = classify_request_header(name);
        if (validate_response_field(kind, value, http_field_list_role::recipient,
                response_fields) != response_field_status::ok) {
            return http1_client_response_parse_error::invalid_header;
        }
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

        if (cursor_value == remaining.size()) {
            break;
        }
        cursor_value += 2;
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
