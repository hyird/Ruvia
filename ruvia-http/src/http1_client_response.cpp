#include "http1_client_response.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http1_client_request.h"
#include "http1_server_response.h"
#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"

namespace ruvia::detail {

struct http1_client_response_parse_result_access final {
    [[nodiscard]] static http1_client_response_parse_result need_more() noexcept {
        return http1_client_response_parse_result(http1_client_response_need_more());
    }

    [[nodiscard]] static http1_client_response_parse_result failure(http1_client_response_parse_error error) noexcept {
        return http1_client_response_parse_result(error);
    }

    [[nodiscard]] static http1_client_response_parse_result parsed(http_client_response_head head, http1_client_response_plan plan, std::size_t consumed_bytes) noexcept {
        return http1_client_response_parse_result(http1_parsed_client_response_head(std::move(head), std::move(plan), consumed_bytes));
    }
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

[[nodiscard]] constexpr detail::http1_client_request_content_phase receive_continue(detail::http1_client_request_content_phase phase) noexcept {
    switch (phase) {
        case detail::http1_client_request_content_phase::awaiting_continue:
        case detail::http1_client_request_content_phase::content_released_awaiting_continue:
            return detail::http1_client_request_content_phase::continue_received;
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
            return detail::http1_client_request_content_phase::continue_received_content_complete;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_pending:
        case detail::http1_client_request_content_phase::continue_received:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return phase;
    }
    return phase;
}

}  // namespace

std::string_view http1_client_response_parse_error_message(http1_client_response_parse_error error) noexcept {
    switch (error) {
        case http1_client_response_parse_error::header_too_large:
            return "response header is too large";
        case http1_client_response_parse_error::invalid_status_line:
            return "invalid response status line";
        case http1_client_response_parse_error::unsupported_http_version:
            return "unsupported response HTTP version";
        case http1_client_response_parse_error::invalid_status_code:
            return "invalid response status code";
        case http1_client_response_parse_error::invalid_reason_phrase:
            return "invalid response reason phrase";
        case http1_client_response_parse_error::invalid_header:
            return "invalid response header";
        case http1_client_response_parse_error::invalid_connection:
            return "invalid response Connection header";
        case http1_client_response_parse_error::invalid_upgrade:
            return "invalid response Upgrade header";
        case http1_client_response_parse_error::invalid_content_length:
            return "invalid response Content-Length";
        case http1_client_response_parse_error::conflicting_content_length:
            return "conflicting response Content-Length";
        case http1_client_response_parse_error::invalid_transfer_encoding:
            return "invalid response Transfer-Encoding";
        case http1_client_response_parse_error::unsupported_transfer_encoding:
            return "unsupported response transfer coding";
        case http1_client_response_parse_error::content_length_and_transfer_encoding:
            return "response has both Content-Length and Transfer-Encoding";
        case http1_client_response_parse_error::invalid_protocol_switch:
            return "invalid Switching Protocols response";
    }
    return "invalid HTTP/1 response";
}

detail::http1_client_request_content_phase http1_client_response_parser::initial_request_content_phase(const http1_client_exchange_state& state_value) noexcept {
    switch (detail::http1_client_exchange_state_access::content_state(state_value)) {
        case detail::http1_client_initial_content_state::complete:
            return detail::http1_client_request_content_phase::content_complete;
        case detail::http1_client_initial_content_state::pending:
            return detail::http1_client_request_content_phase::content_pending;
        case detail::http1_client_initial_content_state::awaiting_continue:
            return detail::http1_client_request_content_phase::awaiting_continue;
    }
    return detail::http1_client_request_content_phase::content_complete;
}

http1_client_response_parser::http1_client_response_parser(http1_client_exchange_state exchange_state) noexcept :
    exchange_state_(std::move(exchange_state)),
    request_content_phase_(initial_request_content_phase(exchange_state_)) {}

http1_client_response_parse_result http1_client_response_parser::parse(std::string_view buffer) {
    if (phase_ != phase_type::await_response) {
        throw std::logic_error("HTTP/1 client response parser is already terminal");
    }
    const auto fail = [this](http1_client_response_parse_error error) noexcept {
        phase_ = phase_type::failed;
        return detail::http1_client_response_parse_result_access::failure(error);
    };

    // The status line is validated once its line feed arrives, so a non-HTTP
    // peer or a bare LF fails without waiting for the whole head. Bytes before
    // header_scan_offset_ held no line feed in an earlier call.
    if (!status_line_) {
        const auto line_feed = buffer.substr(0, max_http_header_bytes).find('\n', header_scan_offset_);
        if (line_feed != std::string_view::npos) {
            if (line_feed == 0 || buffer[line_feed - 1] != '\r') {
                return fail(http1_client_response_parse_error::invalid_status_line);
            }
            const auto status_line = detail::parse_http1_client_status_line(buffer.substr(0, line_feed - 1));
            if (status_line.index() != 0) {
                return fail(std::get<1>(status_line));
            }
            status_line_ = std::get<0>(status_line);
            status_line_end_ = line_feed - 1;
        }
    }

    const auto header_bytes = detail::find_http_header_end(buffer, header_scan_offset_);
    if (header_bytes == std::string_view::npos) {
        // A field line ending in a bare LF can never complete the
        // CRLF-delimited head (RFC 9112 section 2.2).
        if (status_line_ && detail::has_http_bare_line_feed(buffer, std::max(status_line_end_ + 2, header_scan_offset_))) {
            return fail(http1_client_response_parse_error::invalid_header);
        }
        if (buffer.size() >= max_http_header_bytes) {
            return fail(http1_client_response_parse_error::header_too_large);
        }
        header_scan_offset_ = buffer.size() > 3 ? buffer.size() - 3 : 0;
        return detail::http1_client_response_parse_result_access::need_more();
    }
    // A complete head holds a line feed, so its status line was validated
    // above unless the input broke the same-prefix contract of parse().
    // A retry after an allocation failure rescans from the start.
    if (!status_line_) {
        throw std::logic_error("HTTP/1 client response input does not extend the previous prefix");
    }
    const auto status_line = *status_line_;
    const auto fields_begin = status_line_end_ + 2;
    header_scan_offset_ = 0;
    status_line_.reset();
    status_line_end_ = 0;

    // Exclude only the final empty line: every field keeps its CRLF, matching
    // the request scanner even when the head contains no fields.
    auto field_section = buffer.substr(fields_begin, header_bytes - 2 - fields_begin);
    std::optional<std::string> unfolded_fields;
    // The status line is not a field: its following whitespace cannot be
    // folded into the reason phrase. Only normalize within the field block.
    if (const auto first_fold = detail::find_field_line_fold(field_section); first_fold != std::string_view::npos) {
        unfolded_fields.emplace();
        detail::copy_unfolded_field_lines(field_section, first_fold, detail::field_line_folding_policy::http, *unfolded_fields);
        field_section = *unfolded_fields;
    }
    auto parsed_head = detail::parse_http1_client_response_head_fields(status_line, field_section, exchange_state_);
    if ((parsed_head.index() != 0)) {
        return fail(std::get<1>(parsed_head));
    }
    const auto& parsed_value = std::get<0>(parsed_head);

    auto planning = detail::plan_http1_client_response(exchange_state_, parsed_value, request_content_phase_, interim_faulty_framing_);
    if ((planning.index() != 0)) {
        return fail(std::get<1>(planning));
    }
    auto plan = std::move(std::get<0>(planning));

    // No owning response head is observable until the entire head and framing
    // plan have validated. Field views borrow the input or unfolded_head until
    // they are copied below; neither failure nor allocation commits the exchange.
    auto head = detail::http_client_response_head_access::make(parsed_value.received_status_, parsed_value.protocol_version_);
    detail::http_client_response_head_access::content_length(head, parsed_value.content_length_.value());
    const auto parsed_headers = parsed_value.headers_.view();
    if (!parsed_headers.empty()) {
        auto& headers = detail::http_client_response_head_access::headers(head);
        headers.reserve(parsed_headers.size());
        for (const auto& header_value : parsed_headers) {
            headers.emplace_back(header_value.name(), header_value.value());
        }
    }

    auto result_value = detail::http1_client_response_parse_result_access::parsed(std::move(head), std::move(plan), header_bytes);
    if (parsed_value.status_code_.is_informational() && parsed_value.protocol_version_ == http_protocol_version::http10 && parsed_value.saw_transfer_encoding_) {
        interim_faulty_framing_ = true;
    }
    if (parsed_value.status_code_ == http_status::continue_value) {
        request_content_phase_ = receive_continue(request_content_phase_);
    }
    if (parsed_value.status_code_ == http_status::switching_protocols || parsed_value.status_code_.is_final()) {
        phase_ = phase_type::complete;
    }
    return result_value;
}

void http1_client_response_parser::release_request_content() noexcept {
    if (phase_ == phase_type::await_response && request_content_phase_ == detail::http1_client_request_content_phase::awaiting_continue) {
        request_content_phase_ = detail::http1_client_request_content_phase::content_released_awaiting_continue;
    }
}

http1_client_request_content_completion_status http1_client_response_parser::complete_request_content() noexcept {
    if (phase_ != phase_type::await_response) {
        return http1_client_request_content_completion_status::exchange_terminal;
    }
    switch (request_content_phase_) {
        case detail::http1_client_request_content_phase::content_pending:
            request_content_phase_ = detail::http1_client_request_content_phase::content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::awaiting_continue:
        case detail::http1_client_request_content_phase::content_released_awaiting_continue:
            request_content_phase_ = detail::http1_client_request_content_phase::content_complete_awaiting_continue;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::continue_received:
            request_content_phase_ = detail::http1_client_request_content_phase::continue_received_content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return http1_client_request_content_completion_status::already_complete;
    }
    return http1_client_request_content_completion_status::already_complete;
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_offers_protocol(const http1_client_exchange_state& exchange_state, const http_upgrade_protocol& selected) noexcept {
    const auto offered_protocols = http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state);
    if (offered_protocols.empty()) {
        return false;
    }
    bool offered = false;
    http_upgrade_protocols protocols;
    // The writer already validated this offer. Forwarded Upgrade fields may
    // retain empty members (RFC 9110 sections 2.2 and 5.6.1); selecting a
    // response protocol must not reapply generation grammar to those bytes.
    if (protocols.parse_field(offered_protocols, http_field_list_role::recipient, [&selected, &offered](const http_upgrade_protocol& candidate_value) noexcept {
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

    return http1_client_parsed_status_line{.received_status_ = *parsed_status, .protocol_version_ = version == http1_version_parse_result::http10 ? http_protocol_version::http10 : http_protocol_version::http11};
}

http1_client_parsed_response_head::http1_client_parsed_response_head(const http1_client_parsed_status_line& status_line) noexcept :
    received_status_(status_line.received_status_),
    status_code_(status_line.received_status_.status()),
    protocol_version_(status_line.protocol_version_) {}

http1_client_response_head_parse_result_type parse_http1_client_response_head_fields(const http1_client_parsed_status_line& status_line, std::string_view field_section, const http1_client_exchange_state& exchange_state) {
    http1_client_parsed_response_head output(status_line);

    const auto content_semantics = classify_http_response_content_semantics(http1_client_exchange_state_access::method(exchange_state), output.status_code_);
    // RFC 9112 section 6.3 rule 1 ends 1xx, 204, 304 and HEAD responses at the
    // header section whatever framing fields they carry, and rule 1 precedes
    // the rule 5 Content-Length check. A recipient therefore ignores their
    // framing fields, even invalid ones, rather than rejecting a message it can
    // still delimit exactly; HEAD and 304 Content-Length remains a retained
    // field describing the selected representation.
    const bool framing_fields_apply = content_semantics == http_response_content_semantics::with_content;
    bool parse_length_metadata = content_semantics == http_response_content_semantics::without_content && output.status_code_ != http_status::no_content && (http1_client_exchange_state_access::method(exchange_state) == http_known_method::head || output.status_code_ == http_status::not_modified);

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
                if (output.connection_options_.parse_field(value, http_connection_field_role::response_recipient) != http_field_list_parse_status::ok) {
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
                if (output.upgrade_protocols_.parse_field(value, http_field_list_role::recipient, [&exchange_state, &output, check_offered = content_semantics == http_response_content_semantics::protocol_switch](const http_upgrade_protocol& selected) noexcept {
                    if (check_offered && output.all_upgrade_protocols_offered_ && !request_offers_protocol(exchange_state, selected)) {
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

std::optional<std::string_view> http_client_response_head::header(std::string_view name) const& noexcept {
    return detail::find_last_http_header<http_header>(headers_, name);
}

}  // namespace ruvia

// Whether a parsed response head lets the exchange continue: may this 101 switch
// protocols given what the request offered, does the body have a length, and
// does the connection survive the response.

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_allows_protocol_switch(const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response, http1_client_request_content_phase request_content_phase, bool interim_faulty_framing) noexcept {
    const bool request_content_allows_switch = request_content_phase == http1_client_request_content_phase::content_complete || request_content_phase == http1_client_request_content_phase::content_complete_awaiting_continue || request_content_phase == http1_client_request_content_phase::continue_received_content_complete;
    if (interim_faulty_framing || http1_client_exchange_state_access::close_policy(exchange_state) == http1_close_policy::close_after_response || !request_content_allows_switch) {
        return false;
    }
    if (detail::http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state).empty() || response.connection_options_.close() || !response.connection_options_.upgrade() || !response.upgrade_protocols_.has_protocol()) {
        return false;
    }
    return response.all_upgrade_protocols_offered_;
}

[[nodiscard]] std::optional<http_client_request_content_signal> resolve_request_content_signal(http1_client_request_content_phase phase, http_status_code status_code, bool response_will_close) noexcept {
    if (response_will_close && (phase == http1_client_request_content_phase::awaiting_continue || phase == http1_client_request_content_phase::content_pending || phase == http1_client_request_content_phase::content_released_awaiting_continue || phase == http1_client_request_content_phase::continue_received)) {
        return http_client_request_content_signal::exchange_complete;
    }
    if (status_code == http_status::continue_value) {
        return phase == http1_client_request_content_phase::awaiting_continue ? std::optional<http_client_request_content_signal>(http_client_request_content_signal::continue_value) : std::nullopt;
    }
    if (status_code.is_final() && phase == http1_client_request_content_phase::awaiting_continue) {
        // A final response cancels content only while Expect still gates it.
        // After 100 Continue or a finite wait releases the writer, RFC 9110
        // section 7.5 says to keep sending unless the server explicitly stops
        // it. Closing responses provide that signal and are handled above.
        return http_client_request_content_signal::exchange_complete;
    }
    return std::nullopt;
}

[[nodiscard]] http1_close_policy response_persistence(const http1_client_parsed_response_head& response) noexcept {
    // RFC 9112 section 6.1: an HTTP/1.0 message carrying Transfer-Encoding
    // has faulty framing and the connection closes after it, even when the
    // response ends at its header section and the field is otherwise ignored.
    if (response.connection_options_.close() || (response.protocol_version_ == http_protocol_version::http10 && response.saw_transfer_encoding_)) {
        return http1_close_policy::close_after_response;
    }
    if (response.protocol_version_ == http_protocol_version::http11 || response.connection_options_.keep_alive()) {
        return http1_close_policy::allow_reuse;
    }
    return http1_close_policy::close_after_response;
}

[[nodiscard]] http1_close_policy final_response_persistence(const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response, http1_client_request_content_phase request_content_phase, bool interim_faulty_framing) noexcept {
    // A final response while Expect still gates the content cancels content
    // whose framing the request head already committed. The server may keep
    // reading it (RFC 9110 section 10.1.1), so the next request on this
    // connection would be consumed as that content.
    // A rejected CONNECT is not a reusable HTTP exchange: RFC 9931 section 8
    // requires the proxy to close it before interpreting any following request.
    if (interim_faulty_framing || http1_client_exchange_state_access::method(exchange_state) == http_known_method::connect || http1_client_exchange_state_access::close_policy(exchange_state) == http1_close_policy::close_after_response || request_content_phase == http1_client_request_content_phase::awaiting_continue) {
        return http1_close_policy::close_after_response;
    }
    return response_persistence(response);
}

}  // namespace

struct http1_client_response_plan_access final {
    using request_content_signal_type = std::optional<http_client_request_content_signal>;

    [[nodiscard]] static http1_client_response_plan informational(http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_informational_response(persistence)), request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan without_content(http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_response_without_content(persistence)), request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan known_length(std::size_t content_length, http1_close_policy persistence, request_content_signal_type request_content_signal, bool discards_content) noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_known_length_response(content_length, persistence)), request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan chunked(http_transfer_codings transfer_codings, http1_close_policy persistence, request_content_signal_type request_content_signal, bool discards_content) {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_chunked_response(std::move(transfer_codings), persistence)), request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan close_delimited(http_transfer_codings transfer_codings, request_content_signal_type request_content_signal, bool discards_content) {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_close_delimited_response(std::move(transfer_codings))), request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan connect_tunnel() noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_connect_tunnel()), std::nullopt);
    }

    [[nodiscard]] static http1_client_response_plan protocol_upgrade() noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_protocol_upgrade()), std::nullopt);
    }
};

http1_client_response_planning_result_type plan_http1_client_response(const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response, http1_client_request_content_phase request_content_phase, bool interim_faulty_framing) {
    const auto content_semantics = classify_http_response_content_semantics(http1_client_exchange_state_access::method(exchange_state), response.status_code_);

    if (content_semantics == http_response_content_semantics::protocol_switch) {
        if (response.protocol_version_ != http_protocol_version::http11 || !request_allows_protocol_switch(exchange_state, response, request_content_phase, interim_faulty_framing)) {
            return http1_client_response_parse_error::invalid_protocol_switch;
        }
        return http1_client_response_plan_access::protocol_upgrade();
    }
    if (content_semantics == http_response_content_semantics::informational) {
        // A 1xx never ends the exchange or cancels request content, so it cannot
        // close the connection either; the final response alone decides that
        // (RFC 9110 Section 15.2).
        return http1_client_response_plan_access::informational(http1_close_policy::allow_reuse, resolve_request_content_signal(request_content_phase, response.status_code_, false));
    }
    if (content_semantics == http_response_content_semantics::connect_tunnel) {
        return http1_client_response_plan_access::connect_tunnel();
    }

    // RFC 9110 section 15.3.6 forbids only generating 205 content. Its framing
    // is still authoritative, so delimited content is consumed and discarded
    // and the connection stays reusable.
    const bool discards_content = response.status_code_ == http_status::reset_content && content_semantics == http_response_content_semantics::with_content;
    const auto content_length = response.content_length_.value();

    const auto persistence = final_response_persistence(exchange_state, response, request_content_phase, interim_faulty_framing);
    const auto persistent_content_signal = resolve_request_content_signal(request_content_phase, response.status_code_, persistence == http1_close_policy::close_after_response);
    if (content_semantics == http_response_content_semantics::without_content) {
        return http1_client_response_plan_access::without_content(persistence, persistent_content_signal);
    }

    const auto& transfer_encoding = response.transfer_encoding_.value();
    // RFC 9112 section 6.1: HTTP/1.0 Transfer-Encoding is faulty framing, so the
    // codings are ignored and the message falls back to Content-Length or close
    // delimiting while the connection closes after it.
    const bool http10_faulty_framing = response.protocol_version_ == http_protocol_version::http10 && response.saw_transfer_encoding_;
    if (response.saw_transfer_encoding_ && !http10_faulty_framing) {
        if (content_length.has_value()) {
            return http1_client_response_parse_error::content_length_and_transfer_encoding;
        }
        if (!transfer_encoding.has_value()) {
            return http1_client_response_parse_error::invalid_transfer_encoding;
        }
        if (const auto* final_chunked = transfer_encoding->final_chunked()) {
            return http1_client_response_plan_access::chunked(final_chunked->transfer_codings(), persistence, persistent_content_signal, discards_content);
        }
        return http1_client_response_plan_access::close_delimited(transfer_encoding->non_chunked()->transfer_codings(), resolve_request_content_signal(request_content_phase, response.status_code_, true), discards_content);
    }

    if (content_length.has_value()) {
        if (*content_length > (std::numeric_limits<std::size_t>::max)()) {
            return http1_client_response_parse_error::invalid_content_length;
        }
        return http1_client_response_plan_access::known_length(static_cast<std::size_t>(*content_length), persistence, persistent_content_signal, discards_content);
    }

    // RFC 9112 section 6.3: a body-allowed response with no declared
    // length is delimited by server close and cannot return to a pool.
    return http1_client_response_plan_access::close_delimited(http_transfer_codings{}, resolve_request_content_signal(request_content_phase, response.status_code_, true), discards_content);
}

}  // namespace ruvia::detail
namespace ruvia {

std::string_view http1_client_response_body_error_message(http1_client_response_body_error error) noexcept {
    switch (error) {
        case http1_client_response_body_error::invalid_framing:
            return "invalid HTTP/1 response body framing";
        case http1_client_response_body_error::invalid_transfer_coding:
            return "invalid HTTP/1 response transfer coding";
        case http1_client_response_body_error::incomplete_body:
            return "incomplete HTTP/1 response body";
    }
    return "invalid HTTP/1 response body";
}

http1_client_response_body_decoder::http1_client_response_body_decoder(http1_client_response_plan plan) {
    if (plan.informational() || plan.connect_tunnel() || plan.protocol_upgrade()) {
        throw std::invalid_argument("HTTP/1 response plan has no ordinary message body");
    }
    if (const auto* without = plan.without_content()) {
        framing_ = framing_type::no_body;
        persistence_ = without->persistence();
        return;
    }
    // A discarding (205) plan carries the same framings as an ordinary one,
    // but its content is never emitted, so it is never transfer-decoded.
    discard_content_ = plan.discards_content();
    const auto configure_coding = [this](const http_transfer_codings& codings) {
        if (!codings.empty() && !discard_content_) {
            transfer_.emplace(std::span<const http_transfer_coding>(codings.data(), codings.size()), protocol_byte_limit::unlimited());
        }
    };
    if (const auto* known = plan.known_length()) {
        framing_ = framing_type::fixed;
        remaining_ = known->content_length();
        persistence_ = known->persistence();
    } else if (const auto* chunked = plan.chunked()) {
        framing_ = framing_type::chunked;
        persistence_ = chunked->persistence();
        configure_coding(chunked->transfer_codings());
        chunked_.emplace(http1_chunked_body_decoder_config{.obs_fold_ = http1_trailer_obs_fold::accept, .trailer_message_ = http1_trailer_message::response});
    } else if (const auto* close = plan.close_delimited()) {
        framing_ = framing_type::close_delimited;
        configure_coding(close->transfer_codings());
    } else {
        throw std::invalid_argument("unsupported HTTP/1 response plan");
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decode(std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    return step(available, scratch, eof_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_input(std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    eof_ = true;
    return step(available, scratch, true);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step(std::string_view available, std::span<char> scratch, bool eof) {
    if (!discard_content_) {
        return step_content(available, scratch, eof);
    }
    // RFC 9110 section 15.3.6 forbids only generating 205 content: consume
    // what the framing delimits without emitting it, then report the next
    // non-output result over every byte consumed by this call.
    std::size_t discarded = 0;
    for (;;) {
        const auto result_value = step_content(available.substr(discarded), scratch, eof);
        if (result_value.output() == nullptr) {
            return with_consumed_prefix(result_value, discarded);
        }
        discarded += result_value.consumed_bytes();
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_content(std::string_view available, std::span<char> scratch, bool eof) {
    if (trailers_reported_) {
        return finish_transfer(0, scratch);
    }
    // A full output step can leave inflate output buffered after consuming
    // all wire input. Drain it before asking framing or transport for more.
    if (transfer_ && transfer_phase_ == transfer_phase_type::drain_output) {
        const auto pending = drive_transfer({}, 0, scratch);
        if (pending.need_input() == nullptr) {
            return pending;
        }
    }
    switch (framing_) {
        case framing_type::fixed:
            return step_fixed(available, scratch, eof);
        case framing_type::close_delimited:
            return step_close_delimited(available, scratch, eof);
        case framing_type::chunked:
            return step_chunked(available, scratch, eof);
        case framing_type::no_body:
            break;
    }
    return complete(0);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_fixed(std::string_view available, std::span<char> scratch, bool eof) {
    if (remaining_ == 0) {
        return complete(0);
    }
    if (available.empty()) {
        return need_input_or_incomplete(0, eof);
    }
    const auto output_length = std::min({remaining_, available.size(), scratch.size()});
    remaining_ -= output_length;
    return emit(output_length, available.substr(0, output_length));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_close_delimited(std::string_view available, std::span<char> scratch, bool eof) {
    if (!transfer_) {
        if (available.empty()) {
            return eof ? complete(0) : need_input(0);
        }
        const auto output_length = std::min(available.size(), scratch.size());
        return emit(output_length, available.substr(0, output_length));
    }
    std::size_t consumed = 0;
    for (;;) {
        const auto rest = available.substr(consumed);
        if (transfer_phase_ == transfer_phase_type::ended) {
            // The connection close is the only valid framing end after the
            // transfer coding completed.
            if (!rest.empty()) {
                return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
            }
            return eof ? complete(consumed) : need_input(consumed);
        }
        if (rest.empty()) {
            return eof ? finish_transfer(consumed, scratch) : need_input(consumed);
        }
        const auto driven = drive_transfer(rest, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        consumed += input_consumed;
        if (driven.need_input() == nullptr) {
            return driven;
        }
        if (input_consumed == 0 && transfer_phase_ != transfer_phase_type::ended) {
            return need_input_or_incomplete(consumed, eof);
        }
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_chunked(std::string_view available, std::span<char> scratch, bool eof) {
    std::size_t consumed = 0;
    for (;;) {
        if (auto pending = drain_pending_chunk(available, consumed, scratch, eof)) {
            return *pending;
        }
        const auto rest = available.substr(consumed);
        const auto decoded = chunked_->decode(rest, transfer_ ? (std::numeric_limits<std::size_t>::max)() : scratch.size());
        const auto chunk_consumed = consumed + decoded.consumed_bytes();
        if (decoded.failure() != nullptr) {
            return fail(http1_client_response_body_error::invalid_framing, chunk_consumed);
        }
        if (decoded.need_more() != nullptr) {
            return need_input_or_incomplete(chunk_consumed, eof);
        }
        if (const auto* complete = decoded.complete()) {
            trailers_reported_ = true;
            return validated_trailers(chunk_consumed, complete->trailers());
        }
        const auto body = decoded.body_chunk()->bytes();
        if (!transfer_) {
            return emit(chunk_consumed, body);
        }
        // The chunk decoder already consumed the whole chunk, but the transfer
        // decoder may take only part of its data. Track the rest as wire bytes
        // so the caller keeps the unconsumed suffix without a borrowed view.
        consumed += static_cast<std::size_t>(body.data() - rest.data());
        pending_body_bytes_ = body.size();
        pending_delimiter_bytes_ = chunk_consumed - consumed - body.size();
    }
}

// Feeds pending chunk data to the transfer decoder and then skips the chunk
// delimiter. Returns nothing once both are consumed and framing can continue.
std::optional<http1_client_response_body_decoder::result_type> http1_client_response_body_decoder::drain_pending_chunk(std::string_view available, std::size_t& consumed, std::span<char> scratch, bool eof) {
    if (pending_body_bytes_ != 0) {
        const auto input = available.substr(consumed, std::min(pending_body_bytes_, available.size() - consumed));
        const auto driven = drive_transfer(input, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        pending_body_bytes_ -= input_consumed;
        consumed += input_consumed;
        if (pending_body_bytes_ == 0) {
            consume_pending_delimiter(available, consumed);
        }
        if (driven.need_input() == nullptr) {
            return with_consumed(driven, consumed);
        }
        if (pending_body_bytes_ != 0 || consumed == available.size()) {
            return need_input_or_incomplete(consumed, eof);
        }
    } else {
        consume_pending_delimiter(available, consumed);
    }
    if (pending_delimiter_bytes_ != 0) {
        return need_input_or_incomplete(consumed, eof);
    }
    return std::nullopt;
}

// The chunk decoder already validated these delimiter bytes.
void http1_client_response_body_decoder::consume_pending_delimiter(std::string_view available, std::size_t& consumed) noexcept {
    const auto bytes = std::min(pending_delimiter_bytes_, available.size() - consumed);
    consumed += bytes;
    pending_delimiter_bytes_ -= bytes;
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::drive_transfer(std::string_view input, std::size_t wire_prefix, std::span<char> scratch) {
    const auto decoded = transfer_->decode(input, scratch);
    const auto consumed = wire_prefix + decoded.consumed_bytes();
    if (const auto* output = decoded.output()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.need_input() != nullptr) {
        transfer_phase_ = transfer_phase_type::needs_framed_input;
        return need_input(consumed);
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return need_input(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
    }
    return decoder_failure(consumed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_transfer(std::size_t consumed, std::span<char> scratch) {
    if (!transfer_) {
        return complete(consumed);
    }
    // Buffered output drains before framing EOF is committed to the decoder.
    auto decoded = transfer_->decode({}, scratch);
    if (decoded.need_input() != nullptr) {
        decoded = transfer_->finish_input();
    }
    if (const auto* output = decoded.output()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return complete(consumed);
    }
    if (decoded.decoder_failure() != nullptr) {
        return decoder_failure(consumed);
    }
    return fail(http1_client_response_body_error::incomplete_body, consumed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::replay_terminal() const noexcept {
    return std::visit([](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(0, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(0, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(0));
        }
    }, *terminal_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input(std::size_t consumed) const noexcept {
    return result_type(need_input_type(consumed));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input_or_incomplete(std::size_t consumed, bool eof) {
    if (eof) {
        return fail(http1_client_response_body_error::incomplete_body, consumed);
    }
    return need_input(consumed);
}

// The single emission point.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit(std::size_t consumed, std::string_view bytes_value) {
    return result_type(output_view_type(consumed, bytes_value));
}

// Transfer output borrows scratch and must be drained before more framed input.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit_transfer_output(std::size_t consumed, std::string_view bytes_value) {
    transfer_phase_ = transfer_phase_type::drain_output;
    return emit(consumed, bytes_value);
}

// Re-reports a transfer result with the wire bytes consumed after its input,
// such as the chunk delimiter that follows the final chunk data.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::with_consumed(const result_type& result, std::size_t consumed) noexcept {
    return std::visit([consumed](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, output_view_type>) {
            return result_type(output_view_type(consumed, payload_value.bytes()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(consumed, payload_value.error()));
        } else if constexpr (std::is_same_v<t_type, decoder_failure_type>) {
            return result_type(decoder_failure_type(consumed));
        } else {
            return result_type(payload_value);
        }
    }, result.value_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::with_consumed_prefix(const result_type& result, std::size_t prefix) noexcept {
    return std::visit([prefix](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        const auto consumed = prefix + payload_value.consumed_bytes();
        if constexpr (std::is_same_v<t_type, need_input_type>) {
            return result_type(need_input_type(consumed));
        } else if constexpr (std::is_same_v<t_type, output_view_type>) {
            return result_type(output_view_type(consumed, payload_value.bytes()));
        } else if constexpr (std::is_same_v<t_type, validated_trailers_view_type>) {
            return result_type(validated_trailers_view_type(consumed, payload_value.fields()));
        } else if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(consumed, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(consumed, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(consumed));
        }
    }, result.value_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::validated_trailers(std::size_t consumed, std::span<const http_header> fields) {
    return result_type(validated_trailers_view_type(consumed, fields));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::complete(std::size_t consumed) {
    const auto completed = complete_type(consumed, persistence_);
    terminal_ = completed;
    return result_type(completed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::fail(http1_client_response_body_error error, std::size_t consumed) {
    const auto failure = protocol_failure_type(consumed, error);
    terminal_ = failure;
    return result_type(failure);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decoder_failure(std::size_t consumed) {
    const auto failure = decoder_failure_type(consumed);
    terminal_ = failure;
    return result_type(failure);
}

std::size_t http1_client_response_body_decoder::result_type::consumed_bytes() const noexcept {
    return std::visit([](const auto& payload_value) {
        return payload_value.consumed_bytes();
    }, value_);
}

const http1_client_response_body_decoder::need_input_type* http1_client_response_body_decoder::result_type::need_input() const& noexcept {
    return std::get_if<need_input_type>(&value_);
}

const http1_client_response_body_decoder::output_view_type* http1_client_response_body_decoder::result_type::output() const& noexcept {
    return std::get_if<output_view_type>(&value_);
}

const http1_client_response_body_decoder::validated_trailers_view_type* http1_client_response_body_decoder::result_type::trailers() const& noexcept {
    return std::get_if<validated_trailers_view_type>(&value_);
}

const http1_client_response_body_decoder::complete_type* http1_client_response_body_decoder::result_type::complete() const& noexcept {
    return std::get_if<complete_type>(&value_);
}

const http1_client_response_body_decoder::protocol_failure_type* http1_client_response_body_decoder::result_type::protocol_failure() const& noexcept {
    return std::get_if<protocol_failure_type>(&value_);
}

const http1_client_response_body_decoder::decoder_failure_type* http1_client_response_body_decoder::result_type::decoder_failure() const& noexcept {
    return std::get_if<decoder_failure_type>(&value_);
}

}  // namespace ruvia
