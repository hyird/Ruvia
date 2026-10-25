#include <optional>

#include "http_client_response_fixture.h"

// HTTP/1 client responses: the exchange around it: interim responses, Expect, upgrades and tunnels.

RUVIA_TEST(http_client_informational_responses_ignore_framing_fields) {
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

    // RFC 9112 section 6.3 rule 1: a 1xx ends at its header section, so the
    // framing fields a sender must not emit are ignored rather than fatal.
    for (const auto fields : {std::string_view("Content-Length: 5"),
             std::string_view("Transfer-Encoding: chunked"),
             std::string_view("Content-Length: invalid\r\nTransfer-Encoding: gzip")}) {
        std::string head("HTTP/1.1 103 Early Hints\r\n");
        head.append(fields);
        const auto ignored = parse_head("GET", head);
        RUVIA_CHECK(ignored.plan().informational() != nullptr);
        RUVIA_CHECK_EQ(ignored.consumed_bytes(), head.size() + 4);
    }
}

RUVIA_TEST(http_client_long_informational_sequence_preserves_continue_and_final_boundary) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    std::array<char, 512> request_head;
    const auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    if (prepared_req == nullptr) {
        return;
    }

    http1_client_response_parser parser(prepared_req->exchange_state());
    constexpr std::string_view early_hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </asset>; rel=preload\r\n\r\n";
    for (std::size_t i = 0; i < 300; ++i) {
        const auto interim = parser.parse(early_hints);
        const auto* parsed = interim.parsed();
        RUVIA_CHECK(parsed != nullptr);
        if (parsed == nullptr) {
            return;
        }
        RUVIA_CHECK(parsed->plan().informational() != nullptr);
        RUVIA_CHECK(!parsed->plan().request_content_signal());
        RUVIA_CHECK_EQ(parsed->consumed_bytes(), early_hints.size());
    }
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continue_response.parsed() != nullptr);
    if (continue_response.parsed() == nullptr) {
        return;
    }
    RUVIA_CHECK(continue_response.parsed()->plan().request_content_signal() ==
                http_client_request_content_signal::continue_value);
    RUVIA_CHECK(
        parser.complete_request_content() == http1_client_request_content_completion_status::completed);

    constexpr std::string_view final_wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcNEXT";
    const auto final_response = parser.parse(final_wire);
    RUVIA_CHECK(final_response.parsed() != nullptr);
    if (const auto* parsed = final_response.parsed()) {
        RUVIA_CHECK_EQ(parsed->head().status().value(), std::uint16_t{200});
        RUVIA_CHECK_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{3});
        RUVIA_CHECK_EQ(final_wire.substr(parsed->consumed_bytes()), "abcNEXT");
        RUVIA_CHECK(!parsed->plan().request_content_signal());
    }
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)parser.parse({}); }));
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
    const auto* prepared = std::get_if<ruvia::prepared_http1_client_request>(&prepared_result);
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

    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)parser.parse("HTTP/1.1 204 No Content\r\n\r\n"); }));
    RUVIA_CHECK(parser.complete_request_content() ==
                http1_client_request_content_completion_status::exchange_terminal);
}

RUVIA_TEST(http_client_closing_informational_response_keeps_exchange_open) {
    ruvia::http_client_request_view get_request;
    get_request.method_ = "GET";
    std::array<char, 512> get_head;
    const auto get_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), get_request, get_head);
    const auto* get_prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&get_prepared);
    RUVIA_CHECK(get_prepared_req != nullptr);
    if (get_prepared_req == nullptr) {
        return;
    }

    http1_client_response_parser get_parser(get_prepared_req->exchange_state());
    const auto early_hints =
        get_parser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(early_hints.parsed() != nullptr);
    if (early_hints.parsed() != nullptr) {
        const auto* const informational = early_hints.parsed()->plan().informational();
        RUVIA_CHECK(informational != nullptr);
        if (informational != nullptr) {
            RUVIA_CHECK(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
    // RFC 9110 Section 15.2: any number of 1xx responses precede the final
    // response, whose own persistence decides the connection. A close option on
    // the 1xx is metadata only.
    const auto final_response = get_parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        RUVIA_CHECK(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }

    ruvia::http_client_request_view post_request;
    post_request.method_ = "POST";
    post_request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    std::array<char, 512> post_head;
    const auto post_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), post_request, post_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* post_prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&post_prepared);
    RUVIA_CHECK(post_prepared_req != nullptr);
    if (post_prepared_req == nullptr) {
        return;
    }

    http1_client_response_parser post_parser(post_prepared_req->exchange_state());
    const auto closing_continue =
        post_parser.parse("HTTP/1.1 100 Continue\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(closing_continue.parsed() != nullptr);
    if (closing_continue.parsed() != nullptr) {
        RUVIA_CHECK(closing_continue.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }

    http1_client_response_parser closing_hints_parser(post_prepared_req->exchange_state());
    const auto closing_hints =
        closing_hints_parser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(closing_hints.parsed() != nullptr);
    if (closing_hints.parsed() != nullptr) {
        RUVIA_CHECK(!closing_hints.parsed()->plan().request_content_signal());
    }

    std::array<char, 512> request_close_head;
    const auto request_close_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), get_request, request_close_head,
        http1_client_request_wire_policy{.close_policy_ = http1_close_policy::close_after_response});
    const auto* request_close_prepared_req =
        std::get_if<ruvia::prepared_http1_client_request>(&request_close_prepared);
    RUVIA_CHECK(request_close_prepared_req != nullptr);
    if (request_close_prepared_req == nullptr) {
        return;
    }

    http1_client_response_parser request_close_parser(request_close_prepared_req->exchange_state());
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
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    if (prepared_req == nullptr) {
        return;
    }

    http1_client_response_parser parser(prepared_req->exchange_state());
    const auto final_response =
        parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        RUVIA_CHECK(final_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
        // The head committed Content-Length framing for content never sent.
        RUVIA_CHECK(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }

    http1_client_response_parser completed_parser(prepared_req->exchange_state());
    RUVIA_CHECK(completed_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto completed_final =
        completed_parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(completed_final.parsed() != nullptr);
    if (completed_final.parsed() != nullptr) {
        RUVIA_CHECK(!completed_final.parsed()->plan().request_content_signal());
        RUVIA_CHECK(require_known_length(completed_final.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }
}

RUVIA_TEST(http_client_timeout_released_content_follows_final_response_persistence) {
    std::array<char, 512> request_head;
    const auto prepared = ruvia::http1_client_request_writer().prepare_streaming(
        ruvia::http_origin_view::https({.host_ = "example.test"}),
        {.method_ = "POST", .target_ = "/upload", .content_length_ = 6}, request_head,
        {.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    if (prepared_req == nullptr) {
        return;
    }
    for (const bool closing : {false, true}) {
        http1_client_response_parser parser(prepared_req->exchange_state());
        parser.release_request_content();
        const auto response = parser.parse(closing
                                               ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                                               : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        RUVIA_CHECK(response.parsed() != nullptr);
        if (response.parsed() != nullptr) {
            const auto& plan = response.parsed()->plan();
            RUVIA_CHECK(require_known_length(plan).persistence() ==
                        (closing ? http1_close_policy::close_after_response : http1_close_policy::allow_reuse));
            if (closing) {
                RUVIA_CHECK(plan.request_content_signal() == http_client_request_content_signal::exchange_complete);
            } else {
                RUVIA_CHECK(!plan.request_content_signal());
            }
        }
    }
}

RUVIA_TEST(http_client_upgrade_accepts_omitted_continue_after_content_complete) {
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
    const auto* rejected_prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&rejected_prepared);
    RUVIA_CHECK(rejected_prepared_req != nullptr);
    if (rejected_prepared_req == nullptr) {
        return;
    }
    http1_client_response_parser rejected_parser(rejected_prepared_req->exchange_state());
    const auto rejected = rejected_parser.parse(switching);
    RUVIA_CHECK(rejected.failure() != nullptr);
    if (rejected.failure() != nullptr) {
        RUVIA_CHECK(
            *rejected.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)rejected_parser.parse(switching); }));

    // RFC 9110 Section 10.1.1 lets the server omit 100 once it has received
    // some or all of the content, so a 101 after the client finished writing
    // the body is a valid switch.
    std::array<char, 512> completed_without_continue_head;
    const auto completed_without_continue_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        completed_without_continue_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* completed_without_continue_prepared_req =
        std::get_if<ruvia::prepared_http1_client_request>(&completed_without_continue_prepared);
    RUVIA_CHECK(completed_without_continue_prepared_req != nullptr);
    if (completed_without_continue_prepared_req == nullptr) {
        return;
    }
    http1_client_response_parser completed_without_continue_parser(
        completed_without_continue_prepared_req->exchange_state());
    completed_without_continue_parser.release_request_content();
    RUVIA_CHECK(completed_without_continue_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto completed_without_continue = completed_without_continue_parser.parse(switching);
    RUVIA_CHECK(completed_without_continue.parsed() != nullptr);
    if (completed_without_continue.parsed() != nullptr) {
        RUVIA_CHECK(completed_without_continue.parsed()->plan().protocol_upgrade() != nullptr);
    }

    std::array<char, 512> late_continue_head;
    const auto late_continue_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, late_continue_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* late_continue_prepared_req =
        std::get_if<ruvia::prepared_http1_client_request>(&late_continue_prepared);
    RUVIA_CHECK(late_continue_prepared_req != nullptr);
    if (late_continue_prepared_req == nullptr) {
        return;
    }
    http1_client_response_parser late_continue_parser(late_continue_prepared_req->exchange_state());
    late_continue_parser.release_request_content();
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
    const auto* pending_prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&pending_prepared);
    RUVIA_CHECK(pending_prepared_req != nullptr);
    if (pending_prepared_req == nullptr) {
        return;
    }
    http1_client_response_parser pending_parser(pending_prepared_req->exchange_state());
    const auto pending_continue = pending_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(pending_continue.parsed() != nullptr);
    const auto pending_upgrade = pending_parser.parse(switching);
    RUVIA_CHECK(pending_upgrade.failure() != nullptr);
    if (pending_upgrade.failure() != nullptr) {
        RUVIA_CHECK(*pending_upgrade.failure() ==
                    http1_client_response_parse_error::invalid_protocol_switch);
    }

    std::array<char, 512> accepted_head;
    const auto accepted_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, accepted_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* accepted_prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&accepted_prepared);
    RUVIA_CHECK(accepted_prepared_req != nullptr);
    if (accepted_prepared_req == nullptr) {
        return;
    }
    http1_client_response_parser accepted_parser(accepted_prepared_req->exchange_state());
    accepted_parser.release_request_content();
    const auto continue_response = accepted_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        RUVIA_CHECK(!continue_response.parsed()->plan().request_content_signal());
    }
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
    const auto* incomplete_prepared_req =
        std::get_if<ruvia::prepared_http1_client_request>(&incomplete_prepared);
    RUVIA_CHECK(incomplete_prepared_req != nullptr);
    if (incomplete_prepared_req == nullptr) {
        return;
    }
    http1_client_response_parser incomplete_parser(incomplete_prepared_req->exchange_state());
    const auto incomplete = incomplete_parser.parse(switching);
    RUVIA_CHECK(incomplete.failure() != nullptr);
    if (incomplete.failure() != nullptr) {
        RUVIA_CHECK(
            *incomplete.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }

    std::array<char, 512> complete_head;
    const auto complete_prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, complete_head);
    const auto* complete_prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&complete_prepared);
    RUVIA_CHECK(complete_prepared_req != nullptr);
    if (complete_prepared_req == nullptr) {
        return;
    }
    http1_client_response_parser complete_parser(complete_prepared_req->exchange_state());
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
    const auto advertised = parse_head("GET",
        "HTTP/1.1 200 OK\r\nUpgrade: websocket\r\nContent-Length: 0");
    RUVIA_CHECK_EQ(require_known_length(advertised.plan()).content_length(), std::size_t{0});
    RUVIA_CHECK(require_known_length(advertised.plan()).persistence() == http1_close_policy::allow_reuse);

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

RUVIA_TEST(http_client_rejects_only_protected_connection_options) {
    // Only fields that change how a response is processed are protected.
    for (const std::string_view option : {"Content-Length", "Transfer-Encoding", "Set-Cookie"}) {
        std::string response = "HTTP/1.1 200 OK\r\nConnection: ";
        response.append(option);
        response.append("\r\nContent-Length: 0");
        RUVIA_CHECK(parse_failure_error("GET", response) ==
                    http1_client_response_parse_error::invalid_connection);
    }

    // RFC 9110 section 7.6.1 forbids only sending these options; the named
    // end-to-end fields neither frame nor change the response, and the
    // request-only fields cannot affect how a response is processed.
    for (const std::string_view option :
        {"X-Hop", "DATE", "Range", "Cache-Control", "Host", "Authorization", "Origin", "Cookie"}) {
        std::string response = "HTTP/1.1 200 OK\r\nConnection: ";
        response.append(option);
        response.append("\r\nX-Hop: local\r\nContent-Length: 0");
        const auto accepted = parse_head("GET", response);
        RUVIA_CHECK(accepted.plan().known_length() != nullptr);
    }
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

RUVIA_TEST(http_client_rejected_connect_never_reuses_the_proxy_connection) {
    for (const std::string_view response : {
             "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0",
             "HTTP/1.1 403 Forbidden\r\nContent-Length: 3",
             "HTTP/1.0 407 Proxy Authentication Required\r\nConnection: keep-alive\r\nContent-Length: 0",
         }) {
        const auto rejected = parse_head("CONNECT", response);
        RUVIA_CHECK(require_known_length(rejected.plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }

    const auto chunked = parse_head("CONNECT",
        "HTTP/1.1 502 Bad Gateway\r\nTransfer-Encoding: chunked");
    RUVIA_CHECK(require_chunked(chunked.plan()).persistence() ==
                http1_close_policy::close_after_response);
    const auto without_content = parse_head("CONNECT", "HTTP/1.1 304 Not Modified");
    RUVIA_CHECK(require_without_content(without_content.plan()).persistence() ==
                http1_close_policy::close_after_response);

    // Persistence belongs to the rejected CONNECT, not to these otherwise
    // reusable response fields or the normal non-CONNECT framing alternative.
    const auto ordinary = parse_head("GET",
        "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0");
    RUVIA_CHECK(require_known_length(ordinary.plan()).persistence() ==
                http1_close_policy::allow_reuse);
    for (const std::string_view response : {
             "HTTP/1.1 200 Connection Established",
             "HTTP/1.1 205 Reset Content",
             "HTTP/1.1 299 Tunnel Established",
         }) {
        const auto tunnel = parse_head("CONNECT", response);
        RUVIA_CHECK(tunnel.plan().connect_tunnel() != nullptr);
    }
}

RUVIA_TEST(http1_client_exchange_state_owns_every_long_lived_response_fact) {
    std::optional<ruvia::http1_client_exchange_state> head_exchange;
    std::array<char, 512> head_wire{};
    std::string method = "HEAD";
    {
        ruvia::http_client_request_view request{.method_ = method, .target_ = "/resource"};
        const auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, head_wire);
        const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
        if (prepared_req == nullptr) {
            throw std::runtime_error("HEAD request preparation failed");
        }
        head_exchange.emplace(prepared_req->exchange_state());
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
        const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
        if (prepared_req == nullptr) {
            throw std::runtime_error("Upgrade request preparation failed");
        }
        upgrade_exchange.emplace(prepared_req->exchange_state());
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
