#include <memory_resource>
#include <string>
#include <utility>

#include "failing_memory_resource.h"
#include "http_client_response_fixture.h"

// HTTP/1 client responses: what the head says about the body.

RUVIA_TEST(http_owned_header_assignment_preserves_resources_and_allows_retry) {
    failing_memory_resource first_resource;
    failing_memory_resource second_resource;
    {
        auto first = ruvia::http_header::copy_of("X-First", "retained", &first_resource);
        auto second = ruvia::http_header::copy_of("X-Second", "replacement", &second_resource);
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
    RUVIA_CHECK_EQ(combined_chunked.transfer_codings().values_.size(), std::size_t{1});
    RUVIA_CHECK(combined_chunked.transfer_codings().values_[0] == ruvia::http_transfer_coding::gzip);
    RUVIA_CHECK(combined_chunked.persistence() == http1_close_policy::allow_reuse);

    // Transfer-Encoding is list-based: split field lines retain wire order.
    const auto split = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n"
        "Transfer-Encoding: chunked");
    const auto& split_chunked = require_chunked(split.plan());
    RUVIA_CHECK_EQ(split_chunked.transfer_codings().values_.size(), std::size_t{1});
    RUVIA_CHECK(split_chunked.transfer_codings().values_[0] == ruvia::http_transfer_coding::deflate);

    const auto repeated = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n"
        "Transfer-Encoding: deflate, chunked");
    const auto& repeated_chunked = require_chunked(repeated.plan());
    RUVIA_CHECK_EQ(repeated_chunked.transfer_codings().values_.size(), std::size_t{2});
    RUVIA_CHECK(repeated_chunked.transfer_codings().values_[0] == ruvia::http_transfer_coding::gzip);
    RUVIA_CHECK(repeated_chunked.transfer_codings().values_[1] == ruvia::http_transfer_coding::deflate);
}

RUVIA_TEST(http_client_non_chunked_transfer_coding_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    const auto& close_delimited = require_close_delimited(head.plan());
    RUVIA_CHECK_EQ(close_delimited.transfer_codings().values_.size(), std::size_t{1});
}

RUVIA_TEST(http_client_rejects_invalid_or_unsupported_transfer_coding) {
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , chunked"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;foo=bar"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress, chunked"));
    const auto malformed_after_unknown = parse_result("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress\r\n"
        "Transfer-Encoding: deflate, chunked, gzip");
    RUVIA_CHECK(malformed_after_unknown.failure() != nullptr);
    if (const auto* failure = malformed_after_unknown.failure()) {
        RUVIA_CHECK(failure->error() == http1_client_response_parse_error::invalid_transfer_encoding);
    }
    const auto stacked = parse_head(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked");
    const auto& stacked_chunked = require_chunked(stacked.plan());
    RUVIA_CHECK_EQ(stacked_chunked.transfer_codings().values_.size(), std::size_t{2});
    RUVIA_CHECK(stacked_chunked.transfer_codings().values_[0] == ruvia::http_transfer_coding::gzip);
    RUVIA_CHECK(stacked_chunked.transfer_codings().values_[1] == ruvia::http_transfer_coding::deflate);
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

RUVIA_TEST(http_client_204_rejects_framing_fields) {
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 204 No Content\r\nContent-Length: 0"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 204 No Content\r\nContent-Length: invalid"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked"));
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 204 No Content\r\nTransfer-Encoding: custom-coding"));
}

RUVIA_TEST(http_client_205_owns_zero_content_framing) {
    const auto zero_length = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 0");
    const auto* zero_length_body = require_zero_content(zero_length.plan()).known_length();
    RUVIA_CHECK(zero_length_body != nullptr);
    if (zero_length_body == nullptr) {
        return;
    }
    RUVIA_CHECK(!zero_length_body->requires_body_consumption());
    RUVIA_CHECK(zero_length_body->persistence() == http1_close_policy::allow_reuse);
    RUVIA_CHECK_EQ(active_plan_alternative_count(zero_length.plan()), std::size_t{1});

    RUVIA_CHECK(parse_fails("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3"));
    RUVIA_CHECK(parse_fails("HEAD", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3"));

    const auto chunked = parse_head("GET",
        "HTTP/1.1 205 Reset Content\r\n"
        "Transfer-Encoding: gzip, chunked");
    const auto& chunked_zero = require_zero_content(chunked.plan());
    RUVIA_CHECK(chunked_zero.chunked() != nullptr);
    RUVIA_CHECK(chunked_zero.close_delimited() == nullptr);
    if (chunked_zero.chunked() != nullptr) {
        RUVIA_CHECK_EQ(chunked_zero.chunked()->transfer_codings().values_.size(), std::size_t{1});
        RUVIA_CHECK(
            chunked_zero.chunked()->transfer_codings().values_[0] == ruvia::http_transfer_coding::gzip);
    }
    RUVIA_CHECK_EQ(active_plan_alternative_count(chunked.plan()), std::size_t{1});

    const auto transfer_coded =
        parse_head("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    const auto& transfer_coded_zero = require_zero_content(transfer_coded.plan());
    RUVIA_CHECK(transfer_coded_zero.close_delimited() != nullptr);
    if (transfer_coded_zero.close_delimited() != nullptr) {
        RUVIA_CHECK_EQ(transfer_coded_zero.close_delimited()->transfer_codings().values_.size(), std::size_t{1});
        RUVIA_CHECK(transfer_coded_zero.close_delimited()->transfer_codings().values_[0] ==
                    ruvia::http_transfer_coding::gzip);
    }

    const auto unframed = parse_head("GET", "HTTP/1.1 205 Reset Content");
    const auto& close_zero = require_zero_content(unframed.plan());
    RUVIA_CHECK(close_zero.close_delimited() != nullptr);
    RUVIA_CHECK(close_zero.chunked() == nullptr);
    RUVIA_CHECK_EQ(active_plan_alternative_count(unframed.plan()), std::size_t{1});

    const auto connect =
        parse_head("CONNECT", "HTTP/1.1 205 Reset Content\r\nContent-Length: invalid");
    RUVIA_CHECK(connect.plan().connect_tunnel() != nullptr);
    RUVIA_CHECK(connect.plan().zero_content() == nullptr);
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
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    http1_client_response_parser parser(prepared.prepared()->exchange_state());
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
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    http1_client_response_parser parser(prepared.prepared()->exchange_state());
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

RUVIA_TEST(http_client_http10_transfer_encoding_is_faulty_framing) {
    RUVIA_CHECK(parse_fails("GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked"));
    RUVIA_CHECK(parse_fails("HEAD", "HTTP/1.0 200 OK\r\nTransfer-Encoding: gzip"));
}

RUVIA_TEST(http_client_head_method_is_case_sensitive) {
    const auto head = parse_head("HEAD", "HTTP/1.1 200 OK");
    const auto lowercase = parse_head("head", "HTTP/1.1 200 OK");
    RUVIA_CHECK(head.plan().without_content() != nullptr);
    RUVIA_CHECK(lowercase.plan().close_delimited() != nullptr);
}
