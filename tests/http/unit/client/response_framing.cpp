#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/http/http1_client_response_body_decoder.h"

#include "failing_memory_resource.h"
#include "http_client_response_fixture.h"

// HTTP/1 client responses: what the head says about the body.

RUVIA_TEST(http_owned_header_assignment_preserves_resources_and_allows_retry) {
    failing_memory_resource first_resource;
    failing_memory_resource second_resource;
    {
        auto first = ruvia::http_header("X-First", "retained", &first_resource);
        auto second = ruvia::http_header("X-Second", "replacement", &second_resource);
        const auto* first_bytes = first.name().data();
        first_resource.fail_after(0);
        bool failed = false;
        try {
            first = std::move(second);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK(first.name().data() == first_bytes);
        RUVIA_CHECK_EQ(first.value(), "retained");
        RUVIA_CHECK_EQ(second.value(), "replacement");
        first_resource.allow_allocations();
        first = std::move(second);
        RUVIA_CHECK_EQ(first.name(), "X-Second");
        RUVIA_CHECK_EQ(first.value(), "replacement");
        RUVIA_CHECK_EQ(second_resource.live_allocations(), std::size_t{0});
        auto copy = first;
        RUVIA_CHECK_EQ(copy.name(), first.name());
        RUVIA_CHECK(copy.name().data() != first.name().data());
        auto moved = std::move(first);
        RUVIA_CHECK_EQ(moved.value(), "replacement");
        copy = moved;
        RUVIA_CHECK_EQ(copy.value(), "replacement");
    }
    RUVIA_CHECK_EQ(first_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(second_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http_client_response_header_extraction_preserves_owned_fields_on_failure) {
    failing_memory_resource resource;
    {
        auto parsed = parse_wire("GET",
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n",
            http1_close_policy::allow_reuse, {}, &resource);
        RUVIA_CHECK(parsed.parsed() != nullptr);
        if (parsed.parsed() == nullptr) {
            return;
        }
        auto head = std::move(*parsed.parsed()).take_head();
        resource.fail_after(0);
        bool failed = false;
        try {
            const auto extracted = std::move(head).take_headers();
            RUVIA_CHECK_EQ(extracted.size(), std::size_t{1});
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        resource.allow_allocations();
        if (failed) {
            RUVIA_CHECK_EQ(head.headers().size(), std::size_t{1});
            RUVIA_CHECK_EQ(head.headers().front().value(), "text/plain");
            const auto extracted = std::move(head).take_headers();
            RUVIA_CHECK_EQ(extracted.size(), std::size_t{1});
            RUVIA_CHECK_EQ(extracted.front().name(), "Content-Type");
        }
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http_client_response_plan_alternatives_are_exclusive) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto informational = parse_head("GET", "HTTP/1.1 103 Early Hints");
    const auto without_content = parse_head("GET", "HTTP/1.1 204 No Content");
    const auto known_length = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 1");
    const auto chunked = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    const auto close_delimited = parse_head("GET", "HTTP/1.1 200 OK");
    const auto tunnel = parse_head("CONNECT", "HTTP/1.1 200 Connection Established");
    const auto upgrade = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, upgrade_headers);

    for (const auto* plan : {&informational.plan(), &without_content.plan(), &known_length.plan(),
             &chunked.plan(), &close_delimited.plan(), &tunnel.plan(), &upgrade.plan()}) {
        RUVIA_CHECK_EQ(active_plan_alternative_count(*plan), std::size_t{1});
    }
}

RUVIA_TEST(http_client_response_plan_owns_content_length_framing) {
    constexpr std::string_view header_value = "HTTP/1.1 200 OK\r\nContent-Length: 5";
    const auto head = parse_head("GET", header_value);
    const auto& known_length = require_known_length(head.plan());
    RUVIA_CHECK_EQ(known_length.content_length(), std::size_t{5});
    RUVIA_CHECK(known_length.requires_body_consumption());
    RUVIA_CHECK(known_length.persistence() == http1_close_policy::allow_reuse);
    RUVIA_CHECK_EQ(head.consumed_bytes(), header_value.size() + 4);

    const auto empty = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    RUVIA_CHECK(!require_known_length(empty.plan()).requires_body_consumption());
}

RUVIA_TEST(http_client_content_length_combined_and_repeated_equal_values) {
    const auto combined = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 5");
    RUVIA_CHECK_EQ(require_known_length(combined.plan()).content_length(), std::size_t{5});

    const auto repeated =
        parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5");
    RUVIA_CHECK_EQ(require_known_length(repeated.plan()).content_length(), std::size_t{5});

    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 6"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
}

RUVIA_TEST(http_client_response_unfolds_fields_before_framing_and_owns_values) {
    ruvia::http_client_request_view request;
    request.method_ = "GET";
    std::array<char, 512> request_head;
    const auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    if (prepared_req == nullptr) {
        return;
    }

    http1_client_response_parser parser(prepared_req->exchange_state());
    std::string wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0,\r\n \t0\r\n"
        "X-Trace: one \t\r\n\t two\r\n\r\n";
    for (std::size_t size = 1; size < wire.size(); ++size) {
        const auto partial = parser.parse(std::string_view(wire).substr(0, size));
        RUVIA_CHECK(partial.need_more() != nullptr);
    }

    const auto result_value = parser.parse(wire);
    const auto* parsed = result_value.parsed();
    RUVIA_CHECK(parsed != nullptr);
    if (parsed == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->consumed_bytes(), wire.size());
    RUVIA_CHECK_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{0});
    wire.assign(wire.size(), 'x');
    const auto headers = parsed->head().headers();
    RUVIA_CHECK_EQ(headers.size(), std::size_t{2});
    if (headers.size() == 2) {
        RUVIA_CHECK_EQ(headers[0].name(), "Content-Length");
        RUVIA_CHECK_EQ(headers[0].value(), "0, 0");
        RUVIA_CHECK_EQ(headers[1].name(), "X-Trace");
        RUVIA_CHECK_EQ(headers[1].value(), "one two");
    }
}

RUVIA_TEST(http_client_response_unfolding_preserves_framing_validation) {
    const auto transfer = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip,\r\n\tchunked");
    const auto& chunked = require_chunked(transfer.plan());
    RUVIA_CHECK_EQ(chunked.transfer_codings().size(), std::size_t{1});
    RUVIA_CHECK(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    RUVIA_CHECK_EQ(transfer.head().headers().front().value(), "gzip, chunked");

    const std::pair<std::string_view, http1_client_response_parse_error> invalid[] = {
        {"Content-Length: 4\r\n 2", http1_client_response_parse_error::invalid_content_length},
        {"Content-Length: 4,\r\n 2", http1_client_response_parse_error::conflicting_content_length},
        {"Transfer-Encoding: chunked,\r\n gzip", http1_client_response_parse_error::invalid_transfer_encoding},
        {"Content-Length: 0\r\nTransfer-Encoding: gzip,\r\n chunked",
            http1_client_response_parse_error::content_length_and_transfer_encoding},
        {" Content-Length: 0", http1_client_response_parse_error::invalid_header},
        {"\tContent-Length: 0\r\nX-Trace: one\r\n two", http1_client_response_parse_error::invalid_header},
    };
    for (const auto& [fields, expected_error] : invalid) {
        const std::string head = "HTTP/1.1 200 OK\r\n" + std::string(fields);
        RUVIA_CHECK(parse_failure_error("GET", head) == expected_error);
    }
}

RUVIA_TEST(http_client_folded_continue_allocation_failure_allows_retry) {
    constexpr std::string_view wire =
        "HTTP/1.1 100 Continue\r\nX-Trace: interim\r\n continuation\r\n\r\n";
    bool saw_failure = false;
    bool saw_success = false;
    for (std::size_t fail_after = 0; fail_after < 16; ++fail_after) {
        failing_memory_resource resource;
        bool allocation_failed = false;
        {
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
            http1_client_response_parser parser(prepared_req->exchange_state(), &resource);
            resource.fail_after(fail_after);
            for (const auto size : {wire.find("\r\n continuation") + 1, wire.size() - 1}) {
                const auto partial = parser.parse(wire.substr(0, size));
                RUVIA_CHECK(partial.need_more() != nullptr);
                RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
            }
            try {
                const auto accepted = parser.parse(wire);
                RUVIA_CHECK(accepted.parsed() != nullptr);
                if (const auto* parsed = accepted.parsed()) {
                    RUVIA_CHECK(parsed->plan().request_content_signal() ==
                                http_client_request_content_signal::continue_value);
                }
            } catch (const std::bad_alloc&) {
                allocation_failed = true;
            }
            resource.allow_allocations();
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
            if (allocation_failed) {
                const auto retried = parser.parse(wire);
                RUVIA_CHECK(retried.parsed() != nullptr);
                if (const auto* parsed = retried.parsed()) {
                    RUVIA_CHECK(parsed->plan().request_content_signal() ==
                                http_client_request_content_signal::continue_value);
                    RUVIA_CHECK_EQ(parsed->head().headers().front().value(), "interim continuation");
                }
            }
            const auto repeated = parser.parse(wire);
            RUVIA_CHECK(repeated.parsed() != nullptr);
            if (const auto* parsed = repeated.parsed()) {
                RUVIA_CHECK(!parsed->plan().request_content_signal());
            }
            const auto final_response = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
            RUVIA_CHECK(final_response.parsed() != nullptr);
            if (const auto* parsed = final_response.parsed()) {
                RUVIA_CHECK(!parsed->plan().request_content_signal());
            }
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        if (allocation_failed) {
            saw_failure = true;
        } else {
            saw_success = true;
            break;
        }
    }
    RUVIA_CHECK(saw_failure);
    RUVIA_CHECK(saw_success);
}

RUVIA_TEST(http_client_response_plan_owns_chunked_framing_and_reuse) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked");
    const auto& chunked = require_chunked(head.plan());
    RUVIA_CHECK(chunked.transfer_codings().empty());
    RUVIA_CHECK(chunked.persistence() == http1_close_policy::allow_reuse);
}

RUVIA_TEST(http_client_response_parser_propagates_transfer_plan_allocation_failures) {
    constexpr std::string_view header_value =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked";
    bool saw_failure = false;
    bool saw_success = false;
    for (std::size_t fail_after = 0; fail_after < 32; ++fail_after) {
        failing_memory_resource resource;
        resource.fail_after(fail_after);
        bool allocation_failed = false;
        {
            try {
                const auto result_value = parse_result("GET", header_value,
                    http1_close_policy::allow_reuse, {}, &resource);
                RUVIA_CHECK(result_value.parsed() != nullptr);
            } catch (const std::bad_alloc&) {
                allocation_failed = true;
            }
        }
        resource.allow_allocations();
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        if (!allocation_failed) {
            saw_success = true;
            break;
        }
        saw_failure = true;
        {
            const auto retry = parse_result("GET", header_value,
                http1_close_policy::allow_reuse, {}, &resource);
            RUVIA_CHECK(retry.parsed() != nullptr);
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(saw_failure);
    RUVIA_CHECK(saw_success);
}

RUVIA_TEST(http_client_transfer_coding_before_final_chunked_is_typed) {
    const auto combined = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    const auto& combined_chunked = require_chunked(combined.plan());
    RUVIA_CHECK_EQ(combined_chunked.transfer_codings().size(), std::size_t{1});
    RUVIA_CHECK(combined_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    RUVIA_CHECK(combined_chunked.persistence() == http1_close_policy::allow_reuse);

    // Transfer-Encoding is list-based: split field lines retain wire order.
    const auto split = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n"
        "Transfer-Encoding: chunked");
    const auto& split_chunked = require_chunked(split.plan());
    RUVIA_CHECK_EQ(split_chunked.transfer_codings().size(), std::size_t{1});
    RUVIA_CHECK(split_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::deflate);

    const auto repeated = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n"
        "Transfer-Encoding: deflate, chunked");
    const auto& repeated_chunked = require_chunked(repeated.plan());
    RUVIA_CHECK_EQ(repeated_chunked.transfer_codings().size(), std::size_t{2});
    RUVIA_CHECK(repeated_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    RUVIA_CHECK(repeated_chunked.transfer_codings()[1] == ruvia::http_transfer_coding::deflate);
}

RUVIA_TEST(http_client_transfer_encoding_ignores_empty_recipient_list_items) {
    for (const std::string_view fields : {
             "Transfer-Encoding: , chunked",
             "Transfer-Encoding: chunked,",
             "Transfer-Encoding: , , chunked, ,",
             "Transfer-Encoding: gzip, , chunked",
             "Transfer-Encoding: \r\nTransfer-Encoding: chunked",
             "Transfer-Encoding: chunked\r\nTransfer-Encoding: , ,",
             "Transfer-Encoding: ,\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: \r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: ,",
         }) {
        const std::string head = "HTTP/1.1 200 OK\r\n" + std::string(fields) + "\r\n\r\n";
        const std::string wire = head + "0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n";
        const auto result = parse_wire("GET", wire);
        const auto* parsed = result.parsed();
        RUVIA_CHECK(parsed != nullptr);
        if (parsed == nullptr) {
            continue;
        }
        const auto& chunked = require_chunked(parsed->plan());
        RUVIA_CHECK(chunked.persistence() == http1_close_policy::allow_reuse);
        const bool gzip = fields.find("gzip") != std::string_view::npos;
        RUVIA_CHECK_EQ(chunked.transfer_codings().size(), gzip ? std::size_t{1} : std::size_t{0});
        if (gzip && !chunked.transfer_codings().empty()) {
            RUVIA_CHECK(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
        }
        RUVIA_CHECK_EQ(parsed->consumed_bytes(), head.size());
        RUVIA_CHECK_EQ(std::string_view(wire).substr(parsed->consumed_bytes()),
            "0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n");
    }

    const auto non_chunked = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: , gzip, ,\r\nTransfer-Encoding: ");
    const auto& close_delimited = require_close_delimited(non_chunked.plan());
    RUVIA_CHECK_EQ(close_delimited.transfer_codings().size(), std::size_t{1});
    RUVIA_CHECK(close_delimited.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);

    // Empty items do not make the field absent or relax framing conflicts.
    const auto empty = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , ,");
    RUVIA_CHECK(require_close_delimited(empty.plan()).transfer_codings().empty());
    RUVIA_CHECK(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: , ,\r\nContent-Length: 0") ==
                http1_client_response_parse_error::content_length_and_transfer_encoding);
}

RUVIA_TEST(http_client_non_chunked_transfer_coding_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    const auto& close_delimited = require_close_delimited(head.plan());
    RUVIA_CHECK_EQ(close_delimited.transfer_codings().size(), std::size_t{1});
}

RUVIA_TEST(http_client_rejects_invalid_or_unsupported_transfer_coding) {
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;foo=bar"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress, chunked"));
    const auto malformed_after_unknown = parse_result("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress\r\n"
        "Transfer-Encoding: deflate, chunked, gzip");
    RUVIA_CHECK(malformed_after_unknown.failure() != nullptr);
    if (const auto* failure = malformed_after_unknown.failure()) {
        RUVIA_CHECK(*failure == http1_client_response_parse_error::invalid_transfer_encoding);
    }
    const auto stacked = parse_head(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked");
    const auto& stacked_chunked = require_chunked(stacked.plan());
    RUVIA_CHECK_EQ(stacked_chunked.transfer_codings().size(), std::size_t{2});
    RUVIA_CHECK(stacked_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    RUVIA_CHECK(stacked_chunked.transfer_codings()[1] == ruvia::http_transfer_coding::deflate);
}

RUVIA_TEST(http_client_content_length_and_transfer_encoding_rejected_for_body) {
    RUVIA_CHECK(parse_fails("GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked"));
    RUVIA_CHECK(parse_fails("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 5"));
}

RUVIA_TEST(http_client_no_body_precedence_ignores_framing_fields) {
    const auto head = parse_head("HEAD",
        "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n"
        "Transfer-Encoding: custom-coding");
    const auto& without_content = require_without_content(head.plan());
    RUVIA_CHECK(without_content.persistence() == http1_close_policy::allow_reuse);

    const auto not_modified = parse_head("GET",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 7\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(not_modified.plan().without_content() != nullptr);

    const auto no_content = parse_head("GET", "HTTP/1.1 204 No Content");
    RUVIA_CHECK(no_content.plan().without_content() != nullptr);
}

RUVIA_TEST(http_client_trailer_declaration_metadata_preserves_framed_body_and_next_response) {
    for (const std::string_view declaration :
        {"Content-Length", "Host", "Bad Field", "\"x-name\"", ",", "X-One,,X-Two"}) {
        for (const bool chunked : {false, true}) {
            std::string head = "HTTP/1.1 200 OK\r\nTrailer: \t";
            head.append(declaration);
            head.append(" \t\r\n");
            head.append(chunked ? "Transfer-Encoding: chunked\r\n\r\n" : "Content-Length: 3\r\n\r\n");
            const std::string wire = head + (chunked ? "3\r\nabc\r\n0\r\n\r\n" : "abc") +
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            const auto result = parse_wire("GET", wire);
            RUVIA_CHECK(result.parsed() != nullptr);
            if (const auto* parsed = result.parsed()) {
                RUVIA_CHECK_EQ(parsed->head().headers().front().value(), declaration);
                RUVIA_CHECK_EQ(parsed->consumed_bytes(), head.size());
                if (chunked) {
                    RUVIA_CHECK(parsed->plan().chunked() != nullptr);
                } else {
                    RUVIA_CHECK_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{3});
                }
                ruvia::http1_client_response_body_decoder decoder(
                    parsed->plan(), std::pmr::get_default_resource());
                std::array<char, 8> scratch{};
                auto remaining = std::string_view(wire).substr(parsed->consumed_bytes());
                std::string body;
                bool complete = false;
                for (int step = 0; step < 8; ++step) {
                    const auto decoded = decoder.decode(remaining, scratch);
                    RUVIA_CHECK(decoded.protocol_failure() == nullptr);
                    RUVIA_CHECK(decoded.decoder_failure() == nullptr);
                    remaining.remove_prefix(decoded.consumed_bytes());
                    if (const auto* output = decoded.output()) {
                        body.append(output->bytes());
                    }
                    if (decoded.complete()) {
                        complete = true;
                        break;
                    }
                }
                RUVIA_CHECK(complete);
                RUVIA_CHECK_EQ(body, "abc");
                const auto next = parse_wire("GET", remaining);
                RUVIA_CHECK(next.parsed() != nullptr);
                if (const auto* next_head = next.parsed()) {
                    RUVIA_CHECK_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
                    RUVIA_CHECK_EQ(remaining.substr(next_head->consumed_bytes()), "ok");
                }
            }
        }
    }
}

RUVIA_TEST(http_client_bodyless_trailer_declarations_preserve_message_boundaries) {
    for (const auto& [method, status_line] : {
             std::pair{std::string_view("HEAD"), std::string_view("HTTP/1.1 200 OK")},
             std::pair{std::string_view("GET"), std::string_view("HTTP/1.1 304 Not Modified")},
             std::pair{std::string_view("GET"), std::string_view("HTTP/1.1 204 No Content")},
         }) {
        ruvia::http_client_request_view request;
        request.method_ = method;
        std::array<char, 512> request_head;
        const auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head);
        const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
        RUVIA_CHECK(prepared_req != nullptr);
        if (prepared_req == nullptr) {
            continue;
        }
        for (const bool transfer_encoding : {false, true}) {
            std::string head(status_line);
            head.append("\r\n");
            if (transfer_encoding) {
                head.append("Transfer-Encoding: chunked\r\n");
            }
            // RFC 9110 section 6.6.2 makes Trailer a declaration, not a promise
            // that trailer fields follow. RFC 9112 section 6.3 ends both of
            // these responses at the empty line regardless of their fields.
            head.append("Trailer: Content-Length, Bad Field, \"x-name\"\r\n\r\n");
            const std::string wire = head + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            http1_client_response_parser parser(prepared_req->exchange_state());
            for (std::size_t size = 1; size < head.size(); ++size) {
                const auto partial = parser.parse(std::string_view(wire).substr(0, size));
                RUVIA_CHECK(partial.need_more() != nullptr);
            }
            const auto result = parser.parse(wire);
            const auto* parsed = result.parsed();
            RUVIA_CHECK(parsed != nullptr);
            if (parsed == nullptr) {
                continue;
            }
            RUVIA_CHECK(parsed->plan().without_content() != nullptr);
            RUVIA_CHECK_EQ(parsed->consumed_bytes(), head.size());
            RUVIA_CHECK_EQ(parsed->head().headers().back().name(), "Trailer");
            RUVIA_CHECK_EQ(parsed->head().headers().back().value(), "Content-Length, Bad Field, \"x-name\"");
            const auto following = std::string_view(wire).substr(parsed->consumed_bytes());
            const auto next = parse_wire("GET", following);
            RUVIA_CHECK(next.parsed() != nullptr);
            if (const auto* next_head = next.parsed()) {
                RUVIA_CHECK_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
                RUVIA_CHECK_EQ(following.substr(next_head->consumed_bytes()), "ok");
            }
        }
    }

    constexpr std::string_view tunnel_wire =
        "HTTP/1.1 200 Connection Established\r\nTrailer: Content-Length, Bad Field\r\n\r\ntunnel bytes";
    const auto tunnel = parse_wire("CONNECT", tunnel_wire);
    RUVIA_CHECK(tunnel.parsed() != nullptr);
    if (const auto* parsed = tunnel.parsed()) {
        RUVIA_CHECK(parsed->plan().connect_tunnel() != nullptr);
        RUVIA_CHECK_EQ(tunnel_wire.substr(parsed->consumed_bytes()), "tunnel bytes");
    }

    const ruvia::http_header_view upgrade_headers[] = {{"Connection", "Upgrade"}, {"Upgrade", "websocket"}};
    constexpr std::string_view upgrade_wire =
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Trailer: X-Checksum\r\n\r\nprotocol bytes";
    const auto upgrade = parse_wire("GET", upgrade_wire, http1_close_policy::allow_reuse, upgrade_headers);
    RUVIA_CHECK(upgrade.parsed() != nullptr);
    if (const auto* parsed = upgrade.parsed()) {
        RUVIA_CHECK(parsed->plan().protocol_upgrade() != nullptr);
        RUVIA_CHECK_EQ(upgrade_wire.substr(parsed->consumed_bytes()), "protocol bytes");
    }
}

RUVIA_TEST(http_client_auxiliary_response_fields_do_not_change_content_framing) {
    constexpr std::string_view auxiliary =
        "Upgrade: websocket\r\nTrailer: X-Checksum\r\nTE: trailers\r\n";
    for (const std::string_view framing : {
             std::string_view("Content-Length: 2\r\n"),
             std::string_view("Transfer-Encoding: chunked\r\n"),
             std::string_view("Transfer-Encoding: gzip\r\n"),
             std::string_view{},
         }) {
        std::string head("HTTP/1.1 200 OK\r\n");
        head.append(auxiliary);
        head.append(framing);
        head.append("\r\n");
        const std::string wire = head + "body bytes";
        const auto result = parse_wire("GET", wire);
        RUVIA_CHECK(result.parsed() != nullptr);
        if (const auto* parsed = result.parsed()) {
            RUVIA_CHECK_EQ(parsed->consumed_bytes(), head.size());
            RUVIA_CHECK_EQ(std::string_view(wire).substr(parsed->consumed_bytes()), "body bytes");
            if (framing.starts_with("Content-Length")) {
                const auto& body = require_known_length(parsed->plan());
                RUVIA_CHECK_EQ(body.content_length(), std::size_t{2});
                RUVIA_CHECK(body.persistence() == http1_close_policy::allow_reuse);
            } else if (framing == "Transfer-Encoding: chunked\r\n") {
                RUVIA_CHECK(require_chunked(parsed->plan()).persistence() == http1_close_policy::allow_reuse);
            } else {
                RUVIA_CHECK(parsed->plan().close_delimited() != nullptr);
            }
        }
    }
    for (const std::string_view malformed : {
             std::string_view("Content-Length: invalid\r\n"),
             std::string_view("Content-Length: 2\r\nTransfer-Encoding: chunked\r\n"),
             std::string_view("Transfer-Encoding: chunked, gzip\r\n"),
             std::string_view("Transfer-Encoding: custom-coding\r\n"),
         }) {
        std::string head("HTTP/1.1 200 OK\r\n");
        head.append(auxiliary);
        head.append(malformed);
        RUVIA_CHECK(parse_fails("GET", head));
    }
}

RUVIA_TEST(http_client_informational_auxiliary_fields_preserve_the_final_response_boundary) {
    for (const std::string_view status_line :
        {"HTTP/1.1 100 Continue\r\n", "HTTP/1.1 103 Early Hints\r\n"}) {
        ruvia::http_client_request_view request;
        std::array<char, 512> request_head;
        const auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head);
        const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
        RUVIA_CHECK(prepared_req != nullptr);
        if (prepared_req == nullptr) {
            continue;
        }
        std::string head(status_line);
        head.append(
            "Content-Length: ignored\r\nTransfer-Encoding: ignored\r\n"
            "Trailer: Content-Length, Bad Field\r\nTE: trailers\r\nUpgrade: websocket\r\n"
            "Content-Type: text/plain\r\nContent-Type: application/json\r\n"
            "Server: first\r\nServer: second\r\n\r\n");
        const std::string wire = head + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        http1_client_response_parser parser(prepared_req->exchange_state());
        for (std::size_t size = 1; size < head.size(); ++size) {
            const auto partial = parser.parse(std::string_view(wire).substr(0, size));
            RUVIA_CHECK(partial.need_more() != nullptr);
        }
        const auto interim = parser.parse(wire);
        RUVIA_CHECK(interim.parsed() != nullptr);
        if (const auto* parsed = interim.parsed()) {
            RUVIA_CHECK(parsed->plan().informational() != nullptr);
            if (const auto* informational = parsed->plan().informational()) {
                RUVIA_CHECK(informational->persistence() == http1_close_policy::allow_reuse);
            }
            RUVIA_CHECK_EQ(parsed->consumed_bytes(), head.size());
            RUVIA_CHECK_EQ(parsed->head().headers().size(), std::size_t{9});
            const auto final_response = parser.parse(std::string_view(wire).substr(parsed->consumed_bytes()));
            RUVIA_CHECK(final_response.parsed() != nullptr);
            if (const auto* final_head = final_response.parsed()) {
                RUVIA_CHECK_EQ(require_known_length(final_head->plan()).content_length(), std::size_t{2});
                RUVIA_CHECK(require_known_length(final_head->plan()).persistence() == http1_close_policy::allow_reuse);
            }
        }
        head.insert(head.size() - 2, "Connection: close\r\n");
        const auto closing = parse_head("GET", head);
        RUVIA_CHECK(closing.plan().informational() != nullptr);
        if (const auto* informational = closing.plan().informational()) {
            RUVIA_CHECK(informational->persistence() == http1_close_policy::close_after_response);
        }
    }
}

RUVIA_TEST(http_client_no_body_content_length_metadata_must_parse) {
    RUVIA_CHECK(parse_fails("HEAD", "HTTP/1.1 200 OK\r\nContent-Length: invalid"));
    RUVIA_CHECK(parse_fails("HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: invalid"));
    RUVIA_CHECK(
        parse_fails("GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5, 6"));

    const auto repeated =
        parse_head("GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\nContent-Length: 5");
    RUVIA_CHECK(repeated.plan().without_content() != nullptr);
}

RUVIA_TEST(http_client_bodyless_responses_ignore_framing_fields) {
    // RFC 9112 section 6.3 rule 1: 1xx, 204, 304 and HEAD responses end at the
    // header section whatever Content-Length or Transfer-Encoding they carry.
    for (const std::string_view head_value : {
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: 0"),
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: 5"),
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: invalid"),
             std::string_view("HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked"),
             std::string_view("HTTP/1.1 204 No Content\r\nTransfer-Encoding: custom-coding"),
         }) {
        const auto head = parse_head("GET", head_value);
        const auto& without_content = require_without_content(head.plan());
        RUVIA_CHECK(without_content.persistence() == http1_close_policy::allow_reuse);
        RUVIA_CHECK_EQ(head.consumed_bytes(), head_value.size() + 4);
    }
}

RUVIA_TEST(http_client_bodyless_framing_fields_do_not_desynchronize_the_connection) {
    // A smuggled body after a bodyless head is the next response, not content.
    constexpr std::string_view no_content_head =
        "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string wire(no_content_head);
    wire.append("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto no_content = parse_wire("GET", wire);
    RUVIA_CHECK(no_content.parsed() != nullptr);
    if (const auto* parsed = no_content.parsed()) {
        RUVIA_CHECK(parsed->plan().without_content() != nullptr);
        RUVIA_CHECK_EQ(parsed->consumed_bytes(), no_content_head.size());
        // The next exchange starts exactly after the empty line.
        const auto next = parse_wire("GET", std::string_view(wire).substr(parsed->consumed_bytes()));
        RUVIA_CHECK(next.parsed() != nullptr);
        if (const auto* next_head = next.parsed()) {
            RUVIA_CHECK_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
        }
    }

    ruvia::http_client_request_view request;
    request.method_ = "GET";
    std::array<char, 512> request_head;
    const auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    if (prepared_req == nullptr) {
        return;
    }
    http1_client_response_parser parser(prepared_req->exchange_state());
    constexpr std::string_view interim_head = "HTTP/1.1 103 Early Hints\r\nContent-Length: 5\r\n\r\n";
    std::string interim_wire(interim_head);
    interim_wire.append("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto interim = parser.parse(interim_wire);
    RUVIA_CHECK(interim.parsed() != nullptr);
    if (interim.parsed() == nullptr) {
        return;
    }
    RUVIA_CHECK(interim.parsed()->plan().informational() != nullptr);
    RUVIA_CHECK_EQ(interim.parsed()->consumed_bytes(), interim_head.size());
    const auto final_response =
        parser.parse(std::string_view(interim_wire).substr(interim.parsed()->consumed_bytes()));
    RUVIA_CHECK(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        RUVIA_CHECK_EQ(require_known_length(final_response.parsed()->plan()).content_length(), std::size_t{2});
    }
}

RUVIA_TEST(http_client_205_owns_zero_content_framing) {
    const auto zero_length = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 0");
    RUVIA_CHECK(zero_length.plan().requires_zero_content());
    const auto* zero_length_body = zero_length.plan().known_length();
    RUVIA_CHECK(zero_length_body != nullptr);
    if (zero_length_body == nullptr) {
        return;
    }
    RUVIA_CHECK(!zero_length_body->requires_body_consumption());
    RUVIA_CHECK(zero_length_body->persistence() == http1_close_policy::allow_reuse);
    RUVIA_CHECK_EQ(active_plan_alternative_count(zero_length.plan()), std::size_t{1});

    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3"));

    const auto chunked = parse_head("GET",
        "HTTP/1.1 205 Reset Content\r\n"
        "Transfer-Encoding: gzip, chunked");
    const auto& chunked_zero = chunked.plan();
    RUVIA_CHECK(chunked_zero.requires_zero_content());
    RUVIA_CHECK(chunked_zero.chunked() != nullptr);
    RUVIA_CHECK(chunked_zero.close_delimited() == nullptr);
    if (chunked_zero.chunked() != nullptr) {
        RUVIA_CHECK_EQ(chunked_zero.chunked()->transfer_codings().size(), std::size_t{1});
        RUVIA_CHECK(
            chunked_zero.chunked()->transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    }
    RUVIA_CHECK_EQ(active_plan_alternative_count(chunked.plan()), std::size_t{1});

    const auto transfer_coded =
        parse_head("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    const auto& transfer_coded_zero = transfer_coded.plan();
    RUVIA_CHECK(transfer_coded_zero.requires_zero_content());
    RUVIA_CHECK(transfer_coded_zero.close_delimited() != nullptr);
    if (transfer_coded_zero.close_delimited() != nullptr) {
        RUVIA_CHECK_EQ(transfer_coded_zero.close_delimited()->transfer_codings().size(), std::size_t{1});
        RUVIA_CHECK(transfer_coded_zero.close_delimited()->transfer_codings()[0] ==
                    ruvia::http_transfer_coding::gzip);
    }

    const auto unframed = parse_head("GET", "HTTP/1.1 205 Reset Content");
    const auto& close_zero = unframed.plan();
    RUVIA_CHECK(close_zero.requires_zero_content());
    RUVIA_CHECK(close_zero.close_delimited() != nullptr);
    RUVIA_CHECK(close_zero.chunked() == nullptr);
    RUVIA_CHECK_EQ(active_plan_alternative_count(unframed.plan()), std::size_t{1});

    const auto connect =
        parse_head("CONNECT", "HTTP/1.1 205 Reset Content\r\nContent-Length: invalid");
    RUVIA_CHECK(connect.plan().connect_tunnel() != nullptr);
    RUVIA_CHECK(!connect.plan().requires_zero_content());
}

RUVIA_TEST(http_client_final_after_continue_does_not_cancel_released_content) {
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
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        RUVIA_CHECK(continue_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }

    const auto final_response = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        RUVIA_CHECK(!final_response.parsed()->plan().request_content_signal());
        RUVIA_CHECK(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }
}

RUVIA_TEST(http_client_closing_final_stops_unfinished_request_content) {
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
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continue_response.parsed() != nullptr);

    const auto closing_final = parser.parse(
        "HTTP/1.1 413 Content Too Large\r\n"
        "Connection: close\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(closing_final.parsed() != nullptr);
    if (closing_final.parsed() != nullptr) {
        RUVIA_CHECK(closing_final.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
        RUVIA_CHECK(require_known_length(closing_final.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }
}

RUVIA_TEST(http_client_response_preserves_typed_protocol_version) {
    const auto http10 = parse_head("GET", "HTTP/1.0 204 No Content");
    const auto http11 = parse_head("GET", "HTTP/1.1 204 No Content");

    RUVIA_CHECK(http10.head().protocol_version() == http_protocol_version::http10);
    RUVIA_CHECK(http11.head().protocol_version() == http_protocol_version::http11);
}

RUVIA_TEST(http_client_unframed_body_response_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK");
    RUVIA_CHECK(head.plan().close_delimited() != nullptr);
}

RUVIA_TEST(http_client_http10_transfer_encoding_is_faulty_framing_only_with_content) {
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked"));
    // RFC 9112 section 6.3 rule 1: HEAD, 204 and 304 responses end at the
    // header section regardless of framing fields, so the HTTP/1.0 version
    // check does not apply to them.
    const auto head = parse_head("HEAD", "HTTP/1.0 200 OK\r\nTransfer-Encoding: gzip");
    RUVIA_CHECK(head.plan().without_content() != nullptr);
    const auto not_modified = parse_head("GET", "HTTP/1.0 304 Not Modified\r\nTransfer-Encoding: chunked");
    RUVIA_CHECK(not_modified.plan().without_content() != nullptr);
}

RUVIA_TEST(http_client_head_method_is_case_sensitive) {
    const auto head = parse_head("HEAD", "HTTP/1.1 200 OK");
    const auto lowercase = parse_head("head", "HTTP/1.1 200 OK");
    RUVIA_CHECK(head.plan().without_content() != nullptr);
    RUVIA_CHECK(lowercase.plan().close_delimited() != nullptr);
}
