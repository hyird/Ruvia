#include <optional>
#include <utility>
#include <variant>

#include "ruvia/http/detail/coding/http_response_content_semantics.h"
#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http_client_response_head.h"

#include "client/http_client_response_head.h"

// Whether a parsed response head lets the exchange continue: may this 101 switch
// protocols given what the request offered, does the body have a length, and
// does the connection survive the response.

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
    if (protocols.parse_field(offered_protocols, http_field_list_role::sender,
            [&selected, &offered](const http_upgrade_protocol& candidate_value) noexcept {
                offered = offered || http_upgrade_protocol_equals(candidate_value, selected);
                return true;
            }) != http_field_list_parse_status::ok) {
        return false;
    }
    return protocols.has_protocol() && offered;
}

[[nodiscard]] bool request_allows_protocol_switch(const http1_client_exchange_state& exchange_state,
    const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase) noexcept {
    const bool request_content_allows_switch =
        request_content_phase == http1_client_request_content_phase::content_complete ||
        request_content_phase == http1_client_request_content_phase::continue_received_content_complete;
    if (http1_client_exchange_state_access::close_policy(exchange_state) ==
            http1_close_policy::close_after_response ||
        !request_content_allows_switch) {
        return false;
    }
    if (!http1_client_exchange_state_access::connection_options(exchange_state).upgrade() ||
        response.connection_options_.close() || !response.connection_options_.upgrade() ||
        !response.upgrade_protocols_.has_protocol()) {
        return false;
    }

    http_upgrade_protocols selected_protocols;
    for (std::size_t i = 0; i < response.header_count_; ++i) {
        const auto& header_value = response.headers_[i];
        if (!http_ascii_equals_ignore_case(header_value.name(), "Upgrade")) {
            continue;
        }
        if (selected_protocols.parse_field(header_value.value(), http_field_list_role::recipient,
                [&exchange_state](const http_upgrade_protocol& selected) noexcept {
                    return request_offers_protocol(exchange_state, selected);
                }) != http_field_list_parse_status::ok) {
            return false;
        }
    }
    return selected_protocols.has_protocol();
}

[[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal(
    http1_client_request_content_phase phase, http_status_code status_code,
    bool response_will_close) noexcept {
    if (response_will_close && (phase == http1_client_request_content_phase::awaiting_continue ||
                                   phase == http1_client_request_content_phase::content_pending ||
                                   phase == http1_client_request_content_phase::continue_received)) {
        return http_client_request_content_signal::exchange_complete;
    }
    if (status_code == http_status::continue_value) {
        return phase == http1_client_request_content_phase::awaiting_continue
                   ? std::optional<http_client_request_content_signal>(
                         http_client_request_content_signal::continue_value)
                   : std::nullopt;
    }
    if (status_code.is_final()) {
        // A final response cancels content only while Expect still gates it.
        // Once 100 Continue releases the writer, RFC 9110 section 7.5 says the
        // client should keep sending the request unless the server explicitly
        // indicates otherwise. RFC 9112 section 9.5 makes a closing final
        // response that explicit signal, including after Continue released the
        // body writer.
        if (phase == http1_client_request_content_phase::awaiting_continue ||
            (response_will_close &&
                (phase == http1_client_request_content_phase::content_pending ||
                    phase == http1_client_request_content_phase::continue_received))) {
            return http_client_request_content_signal::exchange_complete;
        }
    }
    return std::nullopt;
}

[[nodiscard]] http1_close_policy response_persistence(
    const http1_client_parsed_response_head& response) noexcept {
    if (response.connection_options_.close()) {
        return http1_close_policy::close_after_response;
    }
    if (response.protocol_version_ == http_protocol_version::http11 ||
        response.connection_options_.keep_alive()) {
        return http1_close_policy::allow_reuse;
    }
    return http1_close_policy::close_after_response;
}

[[nodiscard]] http1_close_policy final_response_persistence(
    const http1_client_exchange_state& exchange_state,
    const http1_client_parsed_response_head& response) noexcept {
    if (http1_client_exchange_state_access::close_policy(exchange_state) ==
        http1_close_policy::close_after_response) {
        return http1_close_policy::close_after_response;
    }
    return response_persistence(response);
}

}  // namespace

struct http1_client_response_plan_access final {
    using request_content_signal_type = std::optional<http_client_request_content_signal>;

    [[nodiscard]] static http1_client_response_plan informational(
        http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_informational_response(persistence)),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan without_content(
        http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_response_without_content(persistence)),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan zero_content_known_length(
        http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(
                http1_client_response_with_zero_content(http1_client_response_with_zero_content::framing_type(
                    http1_client_known_length_response(0, persistence)))),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan zero_content_chunked(
        http_transfer_codings transfer_codings, http1_close_policy persistence,
        request_content_signal_type request_content_signal) {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(
                http1_client_response_with_zero_content(http1_client_response_with_zero_content::framing_type(
                    http1_client_chunked_response(std::move(transfer_codings), persistence)))),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan zero_content_close_delimited(
        http_transfer_codings transfer_codings, request_content_signal_type request_content_signal) {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(
                http1_client_response_with_zero_content(http1_client_response_with_zero_content::framing_type(
                    http1_client_close_delimited_response(std::move(transfer_codings))))),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan known_length(std::size_t content_length,
        http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(
                http1_client_known_length_response(content_length, persistence)),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan chunked(http_transfer_codings transfer_codings,
        http1_close_policy persistence, request_content_signal_type request_content_signal) {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_chunked_response(
                                              std::move(transfer_codings), persistence)),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan close_delimited(
        http_transfer_codings transfer_codings, request_content_signal_type request_content_signal) {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_close_delimited_response(
                std::move(transfer_codings))),
            request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan connect_tunnel(
        request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_connect_tunnel()), request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan protocol_upgrade(
        request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(
            http1_client_response_plan::state_type(http1_client_protocol_upgrade()), request_content_signal);
    }
};

http1_client_response_planning_result_type plan_http1_client_response(
    const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase) {
    const auto content_semantics = classify_http_response_content_semantics(
        http1_client_exchange_state_access::method(exchange_state), response.status_code_);

    if (content_semantics == http_response_content_semantics_type::protocol_switch) {
        if (response.protocol_version_ != http_protocol_version::http11 ||
            response.content_length_field_present_ || response.saw_transfer_encoding_ ||
            !request_allows_protocol_switch(exchange_state, response, request_content_phase)) {
            return http1_client_response_parse_error::invalid_protocol_switch;
        }
        return http1_client_response_plan_access::protocol_upgrade(std::nullopt);
    }
    if (content_semantics == http_response_content_semantics_type::informational) {
        const auto persistence = response_persistence(response);
        return http1_client_response_plan_access::informational(
            persistence, request_content_signal(request_content_phase, response.status_code_,
                             persistence == http1_close_policy::close_after_response));
    }
    if (content_semantics == http_response_content_semantics_type::connect_tunnel) {
        return http1_client_response_plan_access::connect_tunnel(std::nullopt);
    }

    const bool reset_content_requires_empty = response.status_code_ == http_status::reset_content;
    const auto content_length = response.content_length_.value();
    if (reset_content_requires_empty && content_length.has_value() && *content_length != 0) {
        return http1_client_response_parse_error::invalid_content_length;
    }

    const auto persistence = final_response_persistence(exchange_state, response);
    const auto persistent_content_signal = request_content_signal(request_content_phase,
        response.status_code_, persistence == http1_close_policy::close_after_response);
    if (content_semantics == http_response_content_semantics_type::without_content) {
        return http1_client_response_plan_access::without_content(persistence, persistent_content_signal);
    }

    const auto& transfer_encoding = response.transfer_encoding_.value();
    if (response.saw_transfer_encoding_) {
        if (content_length.has_value()) {
            return http1_client_response_parse_error::content_length_and_transfer_encoding;
        }
        if (!transfer_encoding.has_value()) {
            return http1_client_response_parse_error::invalid_transfer_encoding;
        }
        if (const auto* final_chunked = transfer_encoding->final_chunked()) {
            if (reset_content_requires_empty) {
                return http1_client_response_plan_access::zero_content_chunked(
                    final_chunked->transfer_codings(), persistence, persistent_content_signal);
            }
            return http1_client_response_plan_access::chunked(
                final_chunked->transfer_codings(), persistence, persistent_content_signal);
        }
        if (reset_content_requires_empty) {
            return http1_client_response_plan_access::zero_content_close_delimited(
                transfer_encoding->non_chunked()->transfer_codings(),
                request_content_signal(request_content_phase, response.status_code_, true));
        }
        return http1_client_response_plan_access::close_delimited(
            transfer_encoding->non_chunked()->transfer_codings(),
            request_content_signal(request_content_phase, response.status_code_, true));
    }

    if (content_length.has_value()) {
        if (reset_content_requires_empty) {
            return http1_client_response_plan_access::zero_content_known_length(
                persistence, persistent_content_signal);
        }
        return http1_client_response_plan_access::known_length(
            *content_length, persistence, persistent_content_signal);
    }

    // RFC 9112 section 6.3: a body-allowed response with no declared
    // length is delimited by server close and cannot return to a pool.
    if (reset_content_requires_empty) {
        return http1_client_response_plan_access::zero_content_close_delimited(
            http_transfer_codings(response.transfer_encoding_.resource()),
            request_content_signal(request_content_phase, response.status_code_, true));
    }
    return http1_client_response_plan_access::close_delimited(
        http_transfer_codings(response.transfer_encoding_.resource()),
        request_content_signal(request_content_phase, response.status_code_, true));
}

}  // namespace ruvia::detail
