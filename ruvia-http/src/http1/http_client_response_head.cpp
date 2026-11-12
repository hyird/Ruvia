#include "ruvia/http/http_client_response_head.h"

#include <variant>

#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_limits.h"

#include "field/http_field_kind.h"
#include "field/http_header_lookup.h"
#include "http1/http1_field_parsing.h"
#include "http1/http_client_response_head.h"
#include "http1/http_header_block_parser.h"
#include "response/http_response_header_state.h"

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

}  // namespace

http1_client_status_line_parse_result_type parse_http1_client_status_line(std::string_view status_line) noexcept {
    const auto separator = status_line.find(' ');
    if (separator == std::string_view::npos) {
        return http1_client_response_parse_error::invalid_status_line;
    }

    const auto version = parse_http1_version(status_line.substr(0, separator));
    if (version == http1_version_parse_result::invalid || version == http1_version_parse_result::unsupported) {
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
        .protocol_version_ = version == http1_version_parse_result::http10 ? http_protocol_version::http10 : http_protocol_version::http11};
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
    bool parse_length_metadata = content_semantics == http_response_content_semantics::without_content &&
                                 output.status_code_ != http_status::no_content &&
                                 (http1_client_exchange_state_access::method(exchange_state) == http_known_method::head ||
                                     output.status_code_ == http_status::not_modified);

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
        const auto kind = classify_http_field(name);
        output.headers_.append(http_header_view{name, value});

        switch (kind) {
            case http_field_kind::content_length:
                // RFC 9112 section 6.3 rule 1: 1xx (including 101), 204, 304,
                // and HEAD responses end at the first empty line regardless of
                // framing fields; successful CONNECT becomes a tunnel.
                if (framing_fields_apply || parse_length_metadata) {
                    if (const auto error = parse_http1_content_length_field<http1_client_response_parse_error>(value, output.content_length_)) {
                        if (framing_fields_apply) {
                            return *error;
                        }
                        // 无正文优先级不变；不一致的表示元数据只是不再提供已知长度。
                        output.content_length_ = {};
                        parse_length_metadata = false;
                    }
                }
                break;
            case http_field_kind::connection:
                if (output.connection_options_.parse_field(value, http_connection_field_role::response_recipient) !=
                    http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_connection;
                }
                break;
            case http_field_kind::transfer_encoding:
                output.saw_transfer_encoding_ = true;
                // Only a response with content is framed by Transfer-Encoding;
                // successful CONNECT is ignored by client-side rule.
                if (framing_fields_apply && output.protocol_version_ == http_protocol_version::http11) {
                    if (const auto error = parse_http1_transfer_encoding_field<http1_client_response_parse_error>(value, output.transfer_encoding_)) {
                        return *error;
                    }
                }
                break;
            case http_field_kind::upgrade:
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
    return detail::find_last_http_header<http_header>(headers_, name);
}

}  // namespace ruvia
