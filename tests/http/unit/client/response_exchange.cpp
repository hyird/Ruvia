#include <optional>

#include "http_client_response_fixture.h"

// HTTP/1 client responses: the exchange around it: interim responses, Expect, upgrades and tunnels.

RUVIA_TEST(http_client_informational_response_enforces_shared_field_contract) {
    for (const auto status :
        {std::string_view("HTTP/1.1 100 Continue"), std::string_view("HTTP/1.1 103 Early Hints")}) {
        const auto head = parse_head("GET", status);
        RUVIA_CHECK(head.plan().informational() != nullptr);
    }

    const auto valid = parse_head("GET",
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </style.css>; rel=preload\r\n"
        "Content-Type: text/html; charset=utf-8");
    RUVIA_CHECK(valid.plan().informational() != nullptr);

    constexpr std::array invalid_fields{
        std::string_view("Content-Length: 0"),
        std::string_view("Transfer-Encoding: chunked"),
        std::string_view("Trailer: X-Checksum"),
        std::string_view("Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                         "date: Thu, 01 Jan 1970 00:00:01 GMT"),
    };
    for (const auto fields : invalid_fields) {
        std::string head("HTTP/1.1 103 Early Hints\r\n");
        head.append(fields);
        const auto result_value = parse_result("GET", head);
        RUVIA_CHECK(result_value.parsed() == nullptr);
        RUVIA_CHECK(result_value.failure() != nullptr);
        if (result_value.failure() != nullptr) {
            RUVIA_CHECK(result_value.failure()->error() == http1_client_response_parse_error::invalid_header);
        }
    }
}

RUVIA_TEST(http_client_limits_informational_responses_per_exchange) {
    ruvia::http_client_request_view request;
    request.method_ = "GET";
    std::array<char, 512> request_head;
    const auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    http1_client_response_parser parser(prepared.prepared()->exchange_state());
    constexpr std::string_view early_hints = "HTTP/1.1 103 Early Hints\r\n\r\n";
    bool reached_limit = false;
    for (std::size_t i = 0; i < 1024; ++i) {
        const auto interim = parser.parse(early_hints);
        if (const auto* failure = interim.failure()) {
            RUVIA_CHECK(failure->error() ==
                        http1_client_response_parse_error::too_many_informational_responses);
            reached_limit = true;
            break;
        }
        RUVIA_CHECK(interim.parsed() != nullptr);
        if (interim.parsed() != nullptr) {
            RUVIA_CHECK(interim.parsed()->plan().informational() != nullptr);
        }
    }
    RUVIA_CHECK(reached_limit);
    const auto after_failure = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(after_failure.terminal() != nullptr);
    if (after_failure.terminal() != nullptr) {
        RUVIA_CHECK(after_failure.terminal()->failed());
    }
}

RUVIA_TEST(http_client_expect_continue_is_one_stateful_exchange_contract) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    std::array<char, 512> request_head;
    const auto prepared_result = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared = prepared_result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    RUVIA_CHECK(prepared->content_plan().continue_gated() != nullptr);

    http1_client_response_parser parser(prepared->exchange_state());
    auto early_hints = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(early_hints.parsed() != nullptr);
    if (early_hints.parsed() != nullptr) {
        RUVIA_CHECK(!early_hints.parsed()->plan().request_content_signal());
        RUVIA_CHECK(early_hints.parsed()->plan().informational() != nullptr);
    }

    auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        RUVIA_CHECK(continue_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }
    const auto duplicate_continue = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(duplicate_continue.parsed() != nullptr);
    if (duplicate_continue.parsed() != nullptr) {
        RUVIA_CHECK(!duplicate_continue.parsed()->plan().request_content_signal());
    }
    RUVIA_CHECK(
        parser.complete_request_content() == http1_client_request_content_completion_status::completed);
    RUVIA_CHECK(parser.complete_request_content() ==
                http1_client_request_content_completion_status::already_complete);

    auto final_response = parser.parse("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        RUVIA_CHECK(!final_response.parsed()->plan().request_content_signal());
    }

    const auto after_final = parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    RUVIA_CHECK(after_final.terminal() != nullptr);
    if (after_final.terminal() != nullptr) {
        RUVIA_CHECK(after_final.terminal()->completed());
    }
    RUVIA_CHECK(parser.complete_request_content() ==
                http1_client_request_content_completion_status::exchange_terminal);
}

RUVIA_TEST(http_client_closing_informational_response_ends_exchange) {
    ruvia::http_client_request_view get_request;
    get_request.method_ = "GET";
    std::array<char, 512> get_head;
    const auto get_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), get_request, get_head);
    RUVIA_CHECK(get_prepared.prepared() != nullptr);
    if (get_prepared.prepared() == nullptr) {
        return;
    }

    http1_client_response_parser get_parser(get_prepared.prepared()->exchange_state());
    const auto early_hints =
        get_parser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(early_hints.parsed() != nullptr);
    if (early_hints.parsed() != nullptr) {
        const auto* const informational = early_hints.parsed()->plan().informational();
        RUVIA_CHECK(informational != nullptr);
        if (informational != nullptr) {
            RUVIA_CHECK(informational->persistence() == http1_close_policy::close_after_response);
        }
    }
    const auto after_close = get_parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(after_close.terminal() != nullptr);
    if (after_close.terminal() != nullptr) {
        RUVIA_CHECK(after_close.terminal()->completed());
    }

    ruvia::http_client_request_view post_request;
    post_request.method_ = "POST";
    post_request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    std::array<char, 512> post_head;
    const auto post_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), post_request, post_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(post_prepared.prepared() != nullptr);
    if (post_prepared.prepared() == nullptr) {
        return;
    }

    http1_client_response_parser post_parser(post_prepared.prepared()->exchange_state());
    const auto closing_continue =
        post_parser.parse("HTTP/1.1 100 Continue\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(closing_continue.parsed() != nullptr);
    if (closing_continue.parsed() != nullptr) {
        RUVIA_CHECK(closing_continue.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
    }

    http1_client_response_parser closing_hints_parser(post_prepared.prepared()->exchange_state());
    const auto closing_hints =
        closing_hints_parser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(closing_hints.parsed() != nullptr);
    if (closing_hints.parsed() != nullptr) {
        RUVIA_CHECK(closing_hints.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
    }

    std::array<char, 512> request_close_head;
    const auto request_close_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), get_request, request_close_head,
        http1_client_request_wire_policy{.close_policy_ = http1_close_policy::close_after_response});
    RUVIA_CHECK(request_close_prepared.prepared() != nullptr);
    if (request_close_prepared.prepared() == nullptr) {
        return;
    }

    http1_client_response_parser request_close_parser(request_close_prepared.prepared()->exchange_state());
    const auto non_closing_hints = request_close_parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(non_closing_hints.parsed() != nullptr);
    if (non_closing_hints.parsed() != nullptr) {
        const auto* const informational = non_closing_hints.parsed()->plan().informational();
        RUVIA_CHECK(informational != nullptr);
        if (informational != nullptr) {
            RUVIA_CHECK(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
    const auto request_close_final = request_close_parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    RUVIA_CHECK(request_close_final.parsed() != nullptr);
    if (request_close_final.parsed() != nullptr) {
        RUVIA_CHECK(require_without_content(request_close_final.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }
}

RUVIA_TEST(http_client_expect_final_cancels_only_pending_request_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    std::array<char, 512> request_head;
    const auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    http1_client_response_parser parser(prepared.prepared()->exchange_state());
    const auto final_response =
        parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        RUVIA_CHECK(final_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
    }

    http1_client_response_parser completed_parser(prepared.prepared()->exchange_state());
    RUVIA_CHECK(completed_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto completed_final =
        completed_parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(completed_final.parsed() != nullptr);
    if (completed_final.parsed() != nullptr) {
        RUVIA_CHECK(!completed_final.parsed()->plan().request_content_signal());
    }
}

RUVIA_TEST(http_client_upgrade_after_expect_requires_prior_continue) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.headers_ = upgrade_headers;
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    std::array<char, 512> rejected_head;
    const auto rejected_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, rejected_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(rejected_prepared.prepared() != nullptr);
    if (rejected_prepared.prepared() == nullptr) {
        return;
    }
    http1_client_response_parser rejected_parser(rejected_prepared.prepared()->exchange_state());
    const auto rejected = rejected_parser.parse(switching);
    RUVIA_CHECK(rejected.failure() != nullptr);
    if (rejected.failure() != nullptr) {
        RUVIA_CHECK(
            rejected.failure()->error() == http1_client_response_parse_error::invalid_protocol_switch);
    }
    const auto after_failure = rejected_parser.parse(switching);
    RUVIA_CHECK(after_failure.terminal() != nullptr);
    if (after_failure.terminal() != nullptr) {
        RUVIA_CHECK(after_failure.terminal()->failed());
    }

    // RFC 9110 section 7.8 still requires the server to acknowledge Expect
    // with 100 before 101, even when the client released and completed content
    // after its own finite wait expired.
    std::array<char, 512> completed_without_continue_head;
    const auto completed_without_continue_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        completed_without_continue_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(completed_without_continue_prepared.prepared() != nullptr);
    if (completed_without_continue_prepared.prepared() == nullptr) {
        return;
    }
    http1_client_response_parser completed_without_continue_parser(
        completed_without_continue_prepared.prepared()->exchange_state());
    RUVIA_CHECK(completed_without_continue_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto completed_without_continue = completed_without_continue_parser.parse(switching);
    RUVIA_CHECK(completed_without_continue.failure() != nullptr);
    if (completed_without_continue.failure() != nullptr) {
        RUVIA_CHECK(completed_without_continue.failure()->error() ==
                    http1_client_response_parse_error::invalid_protocol_switch);
    }

    std::array<char, 512> late_continue_head;
    const auto late_continue_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, late_continue_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(late_continue_prepared.prepared() != nullptr);
    if (late_continue_prepared.prepared() == nullptr) {
        return;
    }
    http1_client_response_parser late_continue_parser(late_continue_prepared.prepared()->exchange_state());
    RUVIA_CHECK(late_continue_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto late_continue = late_continue_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(late_continue.parsed() != nullptr);
    if (late_continue.parsed() != nullptr) {
        RUVIA_CHECK(!late_continue.parsed()->plan().request_content_signal());
    }
    const auto accepted_after_late_continue = late_continue_parser.parse(switching);
    RUVIA_CHECK(accepted_after_late_continue.parsed() != nullptr);
    if (accepted_after_late_continue.parsed() != nullptr) {
        RUVIA_CHECK(accepted_after_late_continue.parsed()->plan().protocol_upgrade() != nullptr);
    }

    std::array<char, 512> pending_head;
    const auto pending_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, pending_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(pending_prepared.prepared() != nullptr);
    if (pending_prepared.prepared() == nullptr) {
        return;
    }
    http1_client_response_parser pending_parser(pending_prepared.prepared()->exchange_state());
    const auto pending_continue = pending_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(pending_continue.parsed() != nullptr);
    const auto pending_upgrade = pending_parser.parse(switching);
    RUVIA_CHECK(pending_upgrade.failure() != nullptr);
    if (pending_upgrade.failure() != nullptr) {
        RUVIA_CHECK(pending_upgrade.failure()->error() ==
                    http1_client_response_parse_error::invalid_protocol_switch);
    }

    std::array<char, 512> accepted_head;
    const auto accepted_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, accepted_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(accepted_prepared.prepared() != nullptr);
    if (accepted_prepared.prepared() == nullptr) {
        return;
    }
    http1_client_response_parser accepted_parser(accepted_prepared.prepared()->exchange_state());
    const auto continue_response = accepted_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continue_response.parsed() != nullptr);
    RUVIA_CHECK(accepted_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto accepted = accepted_parser.parse(switching);
    RUVIA_CHECK(accepted.parsed() != nullptr);
    if (accepted.parsed() != nullptr) {
        RUVIA_CHECK(accepted.parsed()->plan().protocol_upgrade() != nullptr);
    }
}

RUVIA_TEST(http_client_upgrade_requires_complete_request_content) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.headers_ = upgrade_headers;
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    std::array<char, 512> incomplete_head;
    const auto incomplete_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, incomplete_head);
    RUVIA_CHECK(incomplete_prepared.prepared() != nullptr);
    if (incomplete_prepared.prepared() == nullptr) {
        return;
    }
    http1_client_response_parser incomplete_parser(incomplete_prepared.prepared()->exchange_state());
    const auto incomplete = incomplete_parser.parse(switching);
    RUVIA_CHECK(incomplete.failure() != nullptr);
    if (incomplete.failure() != nullptr) {
        RUVIA_CHECK(
            incomplete.failure()->error() == http1_client_response_parse_error::invalid_protocol_switch);
    }

    std::array<char, 512> complete_head;
    const auto complete_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, complete_head);
    RUVIA_CHECK(complete_prepared.prepared() != nullptr);
    if (complete_prepared.prepared() == nullptr) {
        return;
    }
    http1_client_response_parser complete_parser(complete_prepared.prepared()->exchange_state());
    RUVIA_CHECK(complete_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto complete_value = complete_parser.parse(switching);
    RUVIA_CHECK(complete_value.parsed() != nullptr);
    if (complete_value.parsed() != nullptr) {
        RUVIA_CHECK(complete_value.parsed()->plan().protocol_upgrade() != nullptr);
    }
}

RUVIA_TEST(http_client_switching_protocols_is_an_exclusive_upgrade_transition) {
    const ruvia::http_header_view request_headers[] = {
        {"Connection", "keep-alive, Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto upgraded = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket",
        http1_close_policy::allow_reuse, request_headers);
    RUVIA_CHECK(upgraded.head().status() == ruvia::http_status::switching_protocols);
    RUVIA_CHECK(upgraded.plan().protocol_upgrade() != nullptr);
    RUVIA_CHECK(upgraded.plan().connect_tunnel() == nullptr);

    // Protocol names compare case-insensitively; versions remain exact tokens.
    const auto versioned = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: irc/6.9",
        http1_close_policy::allow_reuse, request_headers);
    RUVIA_CHECK(versioned.plan().protocol_upgrade() != nullptr);
    RUVIA_CHECK(parse_fails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: IRC/6.10",
        http1_close_policy::allow_reuse, request_headers));
}

RUVIA_TEST(http_client_connection_fields_use_recipient_list_semantics) {
    const auto reusable = parse_head("GET",
        "HTTP/1.0 200 OK\r\n"
        "Connection: , keep-alive,\r\nContent-Length: 0");
    RUVIA_CHECK(require_known_length(reusable.plan()).persistence() == http1_close_policy::allow_reuse);
    RUVIA_CHECK(reusable.head().protocol_version() == http_protocol_version::http10);

    RUVIA_CHECK(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nConnection: close;invalid\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_connection);
    RUVIA_CHECK(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nUpgrade: websocket/\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_upgrade);
    RUVIA_CHECK(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nUpgrade: websocket\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_connection);

    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto upgraded = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , websocket,",
        http1_close_policy::allow_reuse, offered);
    RUVIA_CHECK(upgraded.plan().protocol_upgrade() != nullptr);
}

RUVIA_TEST(http_client_rejects_end_to_end_connection_options) {
    for (const std::string_view option :
        {"Content-Length", "DATE", "Set-Cookie", "Authorization", "Cookie", "Range"}) {
        std::string response = "HTTP/1.1 200 OK\r\nConnection: ";
        response.append(option);
        response.append("\r\nContent-Length: 0");
        RUVIA_CHECK(parse_failure_error("GET", response) ==
                    http1_client_response_parse_error::invalid_connection);
    }

    const auto extension = parse_head("GET",
        "HTTP/1.1 200 OK\r\nConnection: X-Hop\r\n"
        "X-Hop: local\r\nContent-Length: 0");
    RUVIA_CHECK(extension.plan().known_length() != nullptr);
}

RUVIA_TEST(http_client_switching_protocols_requires_wire_agreement) {
    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const ruvia::http_header_view closing_offer[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "websocket"},
    };
    constexpr std::string_view valid_response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket";

    RUVIA_CHECK(parse_fails("GET", valid_response));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, offered));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade",
        http1_close_policy::allow_reuse, offered));
    RUVIA_CHECK(parse_fails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: IRC",
        http1_close_policy::allow_reuse, offered));
    RUVIA_CHECK(parse_fails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\nContent-Length: 0",
        http1_close_policy::allow_reuse, offered));
    RUVIA_CHECK(parse_fails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Transfer-Encoding: chunked",
        http1_close_policy::allow_reuse, offered));
    RUVIA_CHECK(parse_fails("GET",
        "HTTP/1.0 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, offered));
    RUVIA_CHECK(parse_fails("GET", valid_response, http1_close_policy::allow_reuse, closing_offer));
}

RUVIA_TEST(http_client_response_plan_owns_version_and_connection_persistence) {
    const auto http10 = parse_head("GET", "HTTP/1.0 200 OK\r\nContent-Length: 3");
    const auto& http10_body = require_known_length(http10.plan());
    RUVIA_CHECK(http10_body.persistence() == http1_close_policy::close_after_response);

    const auto http10_keep_alive =
        parse_head("GET", "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 3");
    RUVIA_CHECK(
        require_known_length(http10_keep_alive.plan()).persistence() == http1_close_policy::allow_reuse);

    const auto response_close =
        parse_head("GET", "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3");
    RUVIA_CHECK(require_known_length(response_close.plan()).persistence() ==
                http1_close_policy::close_after_response);

    const auto request_close = parse_head(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 3", http1_close_policy::close_after_response);
    RUVIA_CHECK(require_known_length(request_close.plan()).persistence() ==
                http1_close_policy::close_after_response);
}

RUVIA_TEST(http_client_successful_connect_transitions_to_tunnel) {
    const auto tunnel = parse_head("CONNECT",
        "HTTP/1.1 200 Connection Established\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: chunked;invalid=parameter");
    RUVIA_CHECK(tunnel.plan().connect_tunnel() != nullptr);

    const auto rejected =
        parse_head("CONNECT", "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 3");
    RUVIA_CHECK(rejected.plan().known_length() != nullptr);

    // Methods are case-sensitive. A custom lowercase token is not CONNECT.
    const auto lowercase = parse_head("connect", "HTTP/1.1 200 OK");
    RUVIA_CHECK(lowercase.plan().close_delimited() != nullptr);
}

RUVIA_TEST(http1_client_exchange_state_owns_every_long_lived_response_fact) {
    std::optional<ruvia::http1_client_exchange_state> head_exchange;
    std::array<char, 512> head_wire{};
    std::string method = "HEAD";
    {
        ruvia::http_client_request_view request{.method_ = method, .target_ = "/resource"};
        const auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, head_wire);
        if (prepared.prepared() == nullptr) {
            throw std::runtime_error("HEAD request preparation failed");
        }
        head_exchange.emplace(prepared.prepared()->exchange_state());
    }
    method.assign("GET");
    ruvia::http1_client_response_parser head_parser(std::move(*head_exchange));
    auto head_result = head_parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n");
    if (head_result.parsed() == nullptr) {
        throw std::runtime_error("HEAD response parsing failed");
    }
    RUVIA_CHECK(head_result.parsed()->plan().without_content() != nullptr);

    std::optional<ruvia::http1_client_exchange_state> upgrade_exchange;
    std::array<char, 512> upgrade_wire{};
    std::string connection = "Upgrade";
    std::string protocol = "websocket";
    {
        const std::array headers{
            ruvia::http_header_view{"Connection", connection},
            ruvia::http_header_view{"Upgrade", protocol},
        };
        ruvia::http_client_request_view request{
            .method_ = "GET", .target_ = "/socket", .headers_ = headers};
        const auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, upgrade_wire);
        if (prepared.prepared() == nullptr) {
            throw std::runtime_error("Upgrade request preparation failed");
        }
        upgrade_exchange.emplace(prepared.prepared()->exchange_state());
    }
    connection.assign("close");
    protocol.assign("not-websocket");
    ruvia::http1_client_response_parser upgrade_parser(std::move(*upgrade_exchange));
    auto upgrade_result = upgrade_parser.parse(
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    if (upgrade_result.parsed() == nullptr) {
        throw std::runtime_error("Upgrade response parsing failed");
    }
    RUVIA_CHECK(upgrade_result.parsed()->plan().protocol_upgrade() != nullptr);
}
