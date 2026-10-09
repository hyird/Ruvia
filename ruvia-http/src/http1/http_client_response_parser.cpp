#include <variant>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_status.h"

#include "client/http_client_access.h"
#include "client/http_client_response_head.h"
#include "client/http_client_response_limits.h"
#include "http_header_access.h"
#include "parser/http_header_block_parser.h"

namespace ruvia::detail {

struct http1_client_response_parse_result_access final {
    [[nodiscard]] static http1_client_response_parse_result need_more() noexcept {
        return http1_client_response_parse_result(http1_client_response_need_more());
    }

    [[nodiscard]] static http1_client_response_parse_result failure(
        http1_client_response_parse_error error) noexcept {
        return http1_client_response_parse_result(http1_client_response_parse_failure(error));
    }

    [[nodiscard]] static http1_client_response_parse_result terminal(bool completed) noexcept {
        return http1_client_response_parse_result(http1_client_response_parse_terminal(completed));
    }

    [[nodiscard]] static http1_client_response_parse_result parsed(http_client_response_head head,
        http1_client_response_plan plan, std::size_t consumed_bytes) noexcept {
        return http1_client_response_parse_result(
            http1_parsed_client_response_head(std::move(head), std::move(plan), consumed_bytes));
    }
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

[[nodiscard]] constexpr detail::http1_client_request_content_phase receive_continue(
    detail::http1_client_request_content_phase phase) noexcept {
    switch (phase) {
        case detail::http1_client_request_content_phase::awaiting_continue:
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

std::string_view http1_client_response_parse_error_message(
    http1_client_response_parse_error error) noexcept {
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
        case http1_client_response_parse_error::too_many_headers:
            return "too many response headers";
        case http1_client_response_parse_error::invalid_content_length:
            return "invalid response Content-Length";
        case http1_client_response_parse_error::conflicting_content_length:
            return "conflicting response Content-Length";
        case http1_client_response_parse_error::invalid_transfer_encoding:
            return "invalid response Transfer-Encoding";
        case http1_client_response_parse_error::unsupported_transfer_encoding:
            return "unsupported response transfer coding";
        case http1_client_response_parse_error::transfer_encoding_in_http10:
            return "Transfer-Encoding in HTTP/1.0 response";
        case http1_client_response_parse_error::content_length_and_transfer_encoding:
            return "response has both Content-Length and Transfer-Encoding";
        case http1_client_response_parse_error::invalid_protocol_switch:
            return "invalid Switching Protocols response";
        case http1_client_response_parse_error::too_many_informational_responses:
            return "too many informational responses";
    }
    return "invalid HTTP/1 response";
}

http1_client_response_parse_result http1_client_response_parser::parse(std::string_view buffer) {
    if (phase_ == phase_type::complete) {
        return detail::http1_client_response_parse_result_access::terminal(true);
    }
    if (phase_ == phase_type::failed) {
        return detail::http1_client_response_parse_result_access::terminal(false);
    }
    const auto fail = [this](http1_client_response_parse_error error) noexcept {
        phase_ = phase_type::failed;
        return detail::http1_client_response_parse_result_access::failure(error);
    };

    const auto header_bytes = detail::find_http_header_end(buffer, header_scan_offset_);
    if (header_bytes == std::string_view::npos) {
        if (buffer.size() >= max_http_header_bytes) {
            return fail(http1_client_response_parse_error::header_too_large);
        }
        header_scan_offset_ = buffer.size() > 3 ? buffer.size() - 3 : 0;
        return detail::http1_client_response_parse_result_access::need_more();
    }
    header_scan_offset_ = 0;
    if (header_bytes > max_http_header_bytes) {
        return fail(http1_client_response_parse_error::header_too_large);
    }

    // Remove the terminal CRLF CRLF. The last field line then has the same
    // shape as every preceding line except that it has no trailing delimiter.
    const auto head_section = buffer.substr(0, header_bytes - 4);
    auto parsed_head =
        detail::parse_http1_client_response_head_fields(head_section, exchange_state_, resource_);
    if ((parsed_head.index() != 0)) {
        return fail(std::get<1>(parsed_head));
    }
    const auto& parsed_value = std::get<0>(parsed_head);

    auto planning = detail::plan_http1_client_response(exchange_state_, parsed_value, request_content_phase_);
    if ((planning.index() != 0)) {
        return fail(std::get<1>(planning));
    }
    auto plan = std::move(std::get<0>(planning));
    const auto* const informational_plan = plan.informational();
    const bool informational = informational_plan != nullptr;
    const bool closing_informational =
        informational && informational_plan->persistence() == http1_close_policy::close_after_response;
    if (informational && informational_response_count_ >= detail::max_http_client_interim_responses) {
        return fail(http1_client_response_parse_error::too_many_informational_responses);
    }

    // No owning response head is observable until the entire head and framing
    // plan have validated. Protocol failure therefore has no partially mutated
    // out-parameter and performs no PMR allocation.
    auto head = detail::http_client_response_head_access::make(
        parsed_value.status_code_, parsed_value.protocol_version_, resource_);
    auto& headers = detail::http_client_response_head_access::headers(head);
    if (parsed_value.header_count_ != 0) {
        headers.reserve(parsed_value.header_count_);
    }
    for (std::size_t i = 0; i < parsed_value.header_count_; ++i) {
        const auto& header_value = parsed_value.headers_[i];
        headers.emplace_back(
            detail::http_header_access::make(header_value.name(), header_value.value(), resource_));
    }

    auto result_value = detail::http1_client_response_parse_result_access::parsed(
        std::move(head), std::move(plan), header_bytes);
    if (informational) {
        ++informational_response_count_;
    }
    if (parsed_value.status_code_ == http_status::continue_value && !closing_informational) {
        request_content_phase_ = receive_continue(request_content_phase_);
    }
    if (closing_informational || parsed_value.status_code_ == http_status::switching_protocols ||
        parsed_value.status_code_.is_final()) {
        phase_ = phase_type::complete;
    }
    return result_value;
}

http1_client_request_content_completion_status
http1_client_response_parser::complete_request_content() noexcept {
    if (phase_ != phase_type::await_response) {
        return http1_client_request_content_completion_status::exchange_terminal;
    }
    switch (request_content_phase_) {
        case detail::http1_client_request_content_phase::content_pending:
            request_content_phase_ = detail::http1_client_request_content_phase::content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::awaiting_continue:
            request_content_phase_ =
                detail::http1_client_request_content_phase::content_complete_awaiting_continue;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::continue_received:
            request_content_phase_ =
                detail::http1_client_request_content_phase::continue_received_content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return http1_client_request_content_completion_status::already_complete;
    }
    return http1_client_request_content_completion_status::already_complete;
}

}  // namespace ruvia
