#include <cstdint>

#include "http_client_response_fixture.h"

// HTTP/1 client responses: reading a response head off the wire.

RUVIA_TEST(http_client_response_parser_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state_value = 0x4854'5450'5245'5350ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string input(static_cast<std::size_t>(next_value() % 1025U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        const auto result_value = parse_wire(sample % 3 == 0 ? "GET" : sample % 3 == 1 ? "HEAD"
                                                                                       : "CONNECT",
            input);
        const auto alternatives = static_cast<unsigned int>(result_value.need_more() != nullptr) +
                                  static_cast<unsigned int>(result_value.parsed() != nullptr) +
                                  static_cast<unsigned int>(result_value.failure() != nullptr) +
                                  static_cast<unsigned int>(result_value.terminal() != nullptr);
        RUVIA_CHECK_EQ(alternatives, 1U);
        if (const auto* parsed = result_value.parsed()) {
            RUVIA_CHECK(parsed->consumed_bytes() <= input.size());
            RUVIA_CHECK(parsed->consumed_bytes() >= 4U);
            RUVIA_CHECK_EQ(active_plan_alternative_count(parsed->plan()), std::size_t{1});
        }
        if (result_value.need_more() != nullptr) {
            RUVIA_CHECK(input.size() < ruvia::max_http_header_bytes);
        }
        RUVIA_CHECK(result_value.terminal() == nullptr);
    }
}

RUVIA_TEST(http_client_response_head_commits_status_and_version_at_construction) {
    auto head = ruvia::detail::http_client_response_head_access::make(ruvia::http_status::multi_status,
        http_protocol_version::http10, std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(head.status(), ruvia::http_status::multi_status);
    RUVIA_CHECK(head.protocol_version() == http_protocol_version::http10);
}

RUVIA_TEST(http_client_rejects_malformed_status_and_length_fields) {
    const auto upper_boundary =
        parse_head("GET", "HTTP/1.1 599 Extension Status\r\nContent-Length: 0");
    RUVIA_CHECK_EQ(upper_boundary.head().status(), ruvia::http_status_code::from_value(599));
    RUVIA_CHECK(parse_failure_error("GET", "HTTP/2 200 OK") ==
                http1_client_response_parse_error::unsupported_http_version);
    RUVIA_CHECK(parse_failure_error("GET", "HTTP/1.1 99 Too Small") ==
                http1_client_response_parse_error::invalid_status_code);
    RUVIA_CHECK(parse_failure_error("GET", "HTTP/1.1 abc Bad") ==
                http1_client_response_parse_error::invalid_status_code);
    for (const std::string_view invalid : {"HTTP/1.1 600 Invalid", "HTTP/1.1 999 Invalid"}) {
        RUVIA_CHECK(
            parse_failure_error("GET", invalid) == http1_client_response_parse_error::invalid_status_code);
    }
    RUVIA_CHECK(parse_failure_error("GET", "HTTP/1.1 200") ==
                http1_client_response_parse_error::invalid_status_code);
    std::string invalid_reason("HTTP/1.1 200 ");
    invalid_reason.push_back('\x01');
    RUVIA_CHECK(parse_failure_error("GET", invalid_reason) ==
                http1_client_response_parse_error::invalid_reason_phrase);
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: notanumber"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5,"));
    RUVIA_CHECK(!ruvia::http1_client_response_parse_error_message(
        http1_client_response_parse_error::invalid_status_code)
            .empty());
}

RUVIA_TEST(http_client_rejects_invalid_or_repeated_content_type) {
    const auto invalid = parse_result("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: not a media type\r\n"
        "Content-Length: 0");
    RUVIA_CHECK(invalid.failure() != nullptr);
    if (invalid.failure() != nullptr) {
        RUVIA_CHECK(invalid.failure()->error() == http1_client_response_parse_error::invalid_header);
    }

    const auto repeated = parse_result("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 0");
    RUVIA_CHECK(repeated.failure() != nullptr);
    if (repeated.failure() != nullptr) {
        RUVIA_CHECK(repeated.failure()->error() == http1_client_response_parse_error::invalid_header);
    }

    const auto valid = parse_response("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: 0");
    RUVIA_CHECK_EQ(valid.head_.headers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(
        valid.head_.headers().front().value(), std::string_view("application/json; charset=utf-8"));
}

RUVIA_TEST(http_client_rejects_invalid_trailer_field_names_in_response_head) {
    for (const std::string_view value : {"Content-Length", "X-Checksum, bad field"}) {
        std::string head = "HTTP/1.1 200 OK\r\nTrailer: ";
        head.append(value);
        head.append("\r\nContent-Length: 0");
        RUVIA_CHECK(
            parse_failure_error("GET", head) == http1_client_response_parse_error::invalid_header);
    }

    RUVIA_CHECK(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\n"
                    "Trailer: ETag, X-Checksum\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_header);

    const auto valid = parse_response("GET",
        "HTTP/1.1 200 OK\r\n"
        "Trailer: ETag, X-Checksum\r\n"
        "Transfer-Encoding: chunked");
    RUVIA_CHECK_EQ(valid.head_.headers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(valid.head_.headers().front().name(), std::string_view("Trailer"));
}

RUVIA_TEST(http_client_rejects_request_only_te_in_final_response_head) {
    RUVIA_CHECK(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\n"
                    "TE: trailers\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_header);
}

RUVIA_TEST(http_client_response_parser_need_more_is_distinct) {
    const auto result_value = parse_wire("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");
    RUVIA_CHECK(result_value.need_more() != nullptr);
    RUVIA_CHECK(result_value.parsed() == nullptr);
    RUVIA_CHECK(result_value.failure() == nullptr);
}

RUVIA_TEST(http_client_response_parser_owns_exact_head_boundary) {
    std::string wire =
        "HTTP/1.1 200 OK\r\nX-Owner: response\r\nContent-Length: 4\r\n\r\n"
        "bodyHTTP/1.1 500 ignored\r\n\r\n";
    const auto expected_consumed = wire.find("\r\n\r\n") + 4;
    auto result_value = parse_wire("GET", wire);
    auto* parsed_value = result_value.parsed();
    RUVIA_CHECK(parsed_value != nullptr);
    if (parsed_value == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(parsed_value->consumed_bytes(), expected_consumed);
    RUVIA_CHECK_EQ(parsed_value->head().status(), ruvia::http_status::ok);
    RUVIA_CHECK(parsed_value->head().protocol_version() == http_protocol_version::http11);
    RUVIA_CHECK_EQ(parsed_value->head().headers().size(), std::size_t{2});

    wire.assign(wire.size(), 'x');
    const auto headers = parsed_value->head().headers();
    RUVIA_CHECK(headers[0].name() == "X-Owner");
    RUVIA_CHECK(headers[0].value() == "response");
    RUVIA_CHECK(headers[1].value() == "4");
}

RUVIA_TEST(http_client_response_parser_failure_is_typed_and_allocation_free) {
    counting_memory_resource counting;
    ruvia::http_client_request_view request;
    request.method_ = "GET";
    std::array<char, 512> request_head;
    const auto prepared_result = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head);
    const auto* prepared = prepared_result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }

    auto failure_parser =
        http1_client_response_parser(prepared->exchange_state(), {.resource_ = &counting});
    const auto failure = failure_parser.parse("HTTP/2 200 OK\r\n\r\n");
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(
        failure.failure()->error() == http1_client_response_parse_error::unsupported_http_version);
    RUVIA_CHECK_EQ(counting.allocation_count(), std::size_t{0});

    auto success_parser =
        http1_client_response_parser(prepared->exchange_state(), {.resource_ = &counting});
    const auto success = success_parser.parse(
        "HTTP/1.1 200 OK\r\n"
        "X-Requires-Ownership: a-long-enough-value-to-require-storage\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(success.parsed() != nullptr);
    RUVIA_CHECK(counting.allocation_count() > 0);
}

RUVIA_TEST(http_client_response_parser_enforces_the_complete_head_limit) {
    std::string oversized(ruvia::max_http_header_bytes, 'x');
    const auto result_value = parse_wire("GET", oversized);
    RUVIA_CHECK(result_value.failure() != nullptr);
    RUVIA_CHECK(result_value.failure()->error() == http1_client_response_parse_error::header_too_large);
}

RUVIA_TEST(http_client_response_parser_accepts_incremental_prefixes_and_resets_after_interim) {
    std::array<char, 2048> output{};
    auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), {}, output);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    http1_client_response_parser parser(prepared.prepared()->exchange_state());
    for (const std::string_view wire : {"HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"}) {
        for (std::size_t size = 0; size < wire.size(); ++size) {
            auto progress_value = parser.parse(wire.substr(0, size));
            RUVIA_CHECK(progress_value.need_more() != nullptr);
        }
        auto result_value = parser.parse(wire);
        RUVIA_CHECK(result_value.parsed() != nullptr);
        RUVIA_CHECK_EQ(result_value.parsed()->consumed_bytes(), wire.size());
    }
}
