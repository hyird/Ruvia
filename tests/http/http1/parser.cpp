#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http1_request_parser.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_parse_error.h"
#include "ruvia/http/http_request.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {

using ruvia::http1_close_policy;
using ruvia::http1_server_request_parse_failure_source;
using ruvia::http1_server_request_parse_state;
using ruvia::http1_server_request_parser;
using ruvia::http_known_method;
using ruvia::http_parse_error;
using ruvia::http_protocol_version;
using ruvia::http_request_target_form;
using ruvia::http_unsupported_expectation_policy;

const ruvia::http1_known_length_request_body& require_known_length(
    const ruvia::http1_request_body_plan& plan) {
    const auto* known_length = plan.known_length();
    if (known_length == nullptr) {
        throw std::runtime_error("test expected known-length request framing");
    }
    return *known_length;
}

const ruvia::http1_chunked_request_body& require_chunked(const ruvia::http1_request_body_plan& plan) {
    const auto* chunked = plan.chunked();
    if (chunked == nullptr) {
        throw std::runtime_error("test expected chunked request framing");
    }
    return *chunked;
}

template <typename parse_type, typename ready_check>
void verify_parser_allocation_failures(ruvia::testing::test_context& ruvia_ctx,
    parse_type&& parse, ready_check&& is_ready) {
    bool saw_failure = false;
    bool saw_success = false;
    for (std::size_t fail_after = 0; fail_after < 32; ++fail_after) {
        failing_memory_resource resource;
        resource.fail_after(fail_after);
        bool allocation_failed = false;
        {
            try {
                const auto result_value = parse(resource);
                RUVIA_CHECK(is_ready(result_value));
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
            const auto retry = parse(resource);
            RUVIA_CHECK(is_ready(retry));
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(saw_failure);
    RUVIA_CHECK(saw_success);
}

[[nodiscard]] bool is_failure(
    const http1_server_request_parse_state& state_value, http_parse_error error) noexcept {
    const auto* failure = state_value.failure();
    if (failure == nullptr) {
        return false;
    }
    const auto actual = failure->protocol_error();
    const auto expected = ruvia::http_parse_protocol_error(error);
    return actual.status() == expected.status() &&
           std::string_view(actual.what()) == expected.what();
}

}  // namespace

RUVIA_TEST(http1_public_request_connection_plan_only_tightens_reuse) {
    const auto http10 = ruvia::plan_http10_request_connection(false, true);
    RUVIA_CHECK_EQ(http10.protocol_version(), http_protocol_version::http10);
    RUVIA_CHECK_EQ(http10.disposition(), http1_close_policy::allow_reuse);
    RUVIA_CHECK_EQ(http10.require_close().disposition(), http1_close_policy::close_after_response);

    const auto http11 = ruvia::plan_http11_request_connection(false);
    RUVIA_CHECK_EQ(http11.protocol_version(), http_protocol_version::http11);
    RUVIA_CHECK_EQ(http11.disposition(), http1_close_policy::allow_reuse);
    RUVIA_CHECK_EQ(ruvia::apply_request_body_consumption(
                       http11, ruvia::http1_request_body_consumption::incomplete)
                       .disposition(),
        http1_close_policy::close_after_response);
    RUVIA_CHECK_EQ(ruvia::apply_request_body_consumption(
                       http11, ruvia::http1_request_body_consumption::complete)
                       .disposition(),
        http1_close_policy::allow_reuse);
}

RUVIA_TEST(http1_public_parse_outcome_exposes_only_its_active_alternative) {
    const ruvia::http1_request_parser public_parser;

    const auto need_more = public_parser.parse("GET / HTTP/1.1\r\nHost: example.com\r\n");
    const auto* need_more_state = need_more.need_more();
    RUVIA_CHECK(need_more_state != nullptr);
    if (need_more_state != nullptr) {
        RUVIA_CHECK(!need_more_state->required_total_bytes().has_value());
    }
    RUVIA_CHECK(need_more.parsed() == nullptr);
    RUVIA_CHECK(need_more.failure() == nullptr);

    const auto failure = public_parser.parse("GET / HTTP/1.1\r\n\r\n");
    const auto* failure_state = failure.failure();
    RUVIA_CHECK(failure.need_more() == nullptr);
    RUVIA_CHECK(failure.parsed() == nullptr);
    RUVIA_CHECK(failure_state != nullptr);
    if (failure_state != nullptr) {
        const auto error = failure_state->protocol_error();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::bad_request);
        RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("missing Host header"));
    }
}

RUVIA_TEST(http1_parse_message_handles_deterministic_arbitrary_bytes) {
    http1_server_request_parser parser;
    std::uint64_t state_value = 0xD1CE'B00C'5EED'1234ULL;
    const auto next_value = [&state_value] {
        state_value ^= state_value << 7;
        state_value ^= state_value >> 9;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string input(static_cast<std::size_t>(next_value() % 1025), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        const auto result_value = parser.parse_message(input);
        const auto active_alternatives =
            static_cast<unsigned>(result_value.need_request_head() != nullptr) +
            static_cast<unsigned>(result_value.need_request_body() != nullptr) +
            static_cast<unsigned>(result_value.message_ready() != nullptr) +
            static_cast<unsigned>(result_value.failure() != nullptr);
        RUVIA_CHECK_EQ(active_alternatives, 1U);
        RUVIA_CHECK(result_value.head_ready() == nullptr);
    }
}

RUVIA_TEST(http1_parse_message_maps_random_valid_heads_to_stable_request_plans) {
    http1_server_request_parser parser;
    std::uint64_t state_value = 0x851f'2b7c'609d'e413ULL;
    constexpr std::string_view value_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    const auto next_value = [&state_value] {
        state_value ^= state_value << 13U;
        state_value ^= state_value >> 7U;
        state_value ^= state_value << 17U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 1024; ++sample) {
        const bool http11 = (next_value() & 1U) != 0;
        const auto connection_case = static_cast<unsigned>(next_value() % 3U);
        const auto body_case = static_cast<unsigned>(next_value() % (http11 ? 3U : 2U));
        const auto body_size =
            body_case == 2U ? static_cast<std::size_t>((next_value() % 9U) + 1U)
                            : static_cast<std::size_t>(next_value() % 17U);

        std::string fuzz_value;
        const auto fuzz_size = static_cast<std::size_t>(next_value() % 33U);
        for (std::size_t i = 0; i < fuzz_size; ++i) {
            fuzz_value.push_back(value_chars[static_cast<std::size_t>(next_value()) % value_chars.size()]);
        }

        std::string path = "/stable-" + std::to_string(sample);
        std::string body;
        std::string message = "POST " + path + (http11 ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n");
        message += "Host: example.test\r\n";
        if (connection_case == 1U) {
            message += "Connection: close\r\n";
        } else if (connection_case == 2U) {
            message += "Connection: keep-alive\r\n";
        }
        message += "X-Fuzz: " + fuzz_value + "\r\n";

        if (body_case == 1U) {
            for (std::size_t i = 0; i < body_size; ++i) {
                body.push_back(static_cast<char>('a' + (next_value() % 26U)));
            }
            message += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            message += body;
        } else if (body_case == 2U) {
            for (std::size_t i = 0; i < body_size; ++i) {
                body.push_back(static_cast<char>('A' + (next_value() % 26U)));
            }
            message += "Transfer-Encoding: chunked\r\n\r\n";
            message += std::to_string(body.size());
            message += "\r\n";
            message += body;
            message += "\r\n0\r\n\r\n";
        } else {
            message += "\r\n";
        }
        const auto framed_bytes = message.size();
        message += "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n";

        const auto result_value = parser.parse_message(message);
        RUVIA_CHECK(result_value.message_ready() != nullptr);
        RUVIA_CHECK(result_value.failure() == nullptr);
        if (result_value.message_ready() == nullptr || result_value.failure() != nullptr) {
            continue;
        }
        RUVIA_CHECK_EQ(result_value.request_.method(), std::string_view("POST"));
        RUVIA_CHECK_EQ(result_value.request_.path(), std::string_view(path));
        RUVIA_CHECK_EQ(result_value.request_.header("Host").value_or(std::string_view{}),
            std::string_view("example.test"));
        RUVIA_CHECK_EQ(result_value.request_.header("X-Fuzz").value_or(std::string_view{}),
            std::string_view(fuzz_value));
        RUVIA_CHECK(result_value.request_.protocol_version() ==
                    (http11 ? http_protocol_version::http11 : http_protocol_version::http10));

        const auto expected_close_policy =
            (!http11 && connection_case != 2U) || connection_case == 1U
                ? http1_close_policy::close_after_response
                : http1_close_policy::allow_reuse;
        RUVIA_CHECK(result_value.connection_plan_.disposition() == expected_close_policy);
        RUVIA_CHECK(result_value.connection_plan_.protocol_version() ==
                    (http11 ? http_protocol_version::http11 : http_protocol_version::http10));

        const auto* ready = result_value.message_ready();
        if (ready != nullptr) {
            RUVIA_CHECK_EQ(ready->message_bytes(), framed_bytes);
        }
        if (body_case == 1U) {
            RUVIA_CHECK_EQ(require_known_length(result_value.body_plan_).content_length(), body.size());
        } else if (body_case == 2U) {
            RUVIA_CHECK(result_value.body_plan_.chunked() != nullptr);
        } else {
            RUVIA_CHECK(result_value.body_plan_.without_body() != nullptr);
        }
    }
}

RUVIA_TEST(http1_internal_parse_failure_classifies_only_request_line_failures) {
    http1_server_request_parser parser;

    const auto request_line_failure =
        parser.parse_message("GET / HTTP/9.0\r\nHost: example.com\r\n\r\n");
    RUVIA_CHECK(request_line_failure.failure() != nullptr);
    if (const auto* failure = request_line_failure.failure()) {
        RUVIA_CHECK(failure->source() == http1_server_request_parse_failure_source::request_line);
    }

    const auto message_failure = parser.parse_message("GET / HTTP/1.1\r\n\r\n");
    RUVIA_CHECK(message_failure.failure() != nullptr);
    if (const auto* failure = message_failure.failure()) {
        RUVIA_CHECK(failure->source() == http1_server_request_parse_failure_source::message);
    }
}

RUVIA_TEST(http1_public_parse_need_more_separates_required_size_from_consumption) {
    const ruvia::http1_request_parser public_parser;
    constexpr std::string_view partial =
        "POST / HTTP/1.1\r\nHost: example.com\r\n"
        "Content-Length: 5\r\n\r\nhe";
    const auto result_value = public_parser.parse(partial);
    const auto* need_more = result_value.need_more();
    RUVIA_CHECK(need_more != nullptr);
    if (need_more != nullptr) {
        RUVIA_CHECK(need_more->required_total_bytes().has_value());
        if (need_more->required_total_bytes()) {
            RUVIA_CHECK_EQ(*need_more->required_total_bytes(), partial.size() + std::size_t{3});
        }
    }
}

RUVIA_TEST(http1_public_parse_success_retains_the_exact_framed_body) {
    const ruvia::http1_request_parser public_parser;

    constexpr std::string_view content_length_message =
        "POST /fixed HTTP/1.1\r\nHost: example.com\r\n"
        "Content-Length: 5\r\n\r\nhello";
    const std::string content_length_pipeline =
        std::string(content_length_message) + "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto fixed_result = public_parser.parse(content_length_pipeline);
    const auto* fixed = fixed_result.parsed();
    RUVIA_CHECK(fixed != nullptr);
    if (fixed != nullptr) {
        RUVIA_CHECK_EQ(fixed->request().path(), std::string_view("/fixed"));
        RUVIA_CHECK_EQ(require_known_length(fixed->body_plan()).content_length(), std::size_t{5});
        RUVIA_CHECK_EQ(fixed->wire_body(), std::string_view("hello"));
        RUVIA_CHECK_EQ(fixed->consumed_bytes(), content_length_message.size());
    }

    constexpr std::string_view chunked_message =
        "POST /chunked HTTP/1.1\r\nHost: example.com\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\nX-Checksum: ok\r\n\r\n";
    const std::string chunked_pipeline =
        std::string(chunked_message) + "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto chunked_result = public_parser.parse(chunked_pipeline);
    const auto* chunked = chunked_result.parsed();
    RUVIA_CHECK(chunked != nullptr);
    if (chunked != nullptr) {
        RUVIA_CHECK(chunked->body_plan().chunked() != nullptr);
        RUVIA_CHECK_EQ(
            chunked->wire_body(), std::string_view("3\r\nabc\r\n0\r\nX-Checksum: ok\r\n\r\n"));
        RUVIA_CHECK_EQ(chunked->consumed_bytes(), chunked_message.size());
    }
}

RUVIA_TEST(http1_public_parser_preserves_expect_extensions_as_semantics) {
    const auto result_value = ruvia::http1_request_parser().parse(
        "POST /extensions HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Expect: , 100-Continue, custom-feature,\r\n"
        "Content-Length: 1\r\n\r\nx");

    const auto* parsed_value = result_value.parsed();
    RUVIA_CHECK(parsed_value != nullptr);
    RUVIA_CHECK(result_value.failure() == nullptr);
    if (parsed_value != nullptr) {
        const auto plan =
            parsed_value->body_plan().expectation_plan(http_unsupported_expectation_policy::reject);
        RUVIA_CHECK(plan.rejection() != nullptr);
        RUVIA_CHECK(parsed_value->body_plan().expectations().has_continue());
        RUVIA_CHECK(parsed_value->body_plan().expectations().has_unsupported());
    }
}

RUVIA_TEST(http1_parse_rejects_malformed_expect_field) {
    const auto result_value = ruvia::http1_request_parser().parse(
        "POST /extensions HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Expect: bad value\r\n"
        "Content-Length: 1\r\n\r\nx");

    RUVIA_CHECK(result_value.parsed() == nullptr);
    RUVIA_CHECK(result_value.failure() != nullptr);
    if (const auto* failure = result_value.failure()) {
        const auto protocol_error = failure->protocol_error();
        const auto expected = ruvia::http_parse_protocol_error(http_parse_error::invalid_header);
        RUVIA_CHECK_EQ(protocol_error.status(), expected.status());
        RUVIA_CHECK_EQ(std::string_view(protocol_error.what()), std::string_view(expected.what()));
    }
}

RUVIA_TEST(http1_parse_valid_request) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message("GET /path?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(result_value.request_.method(), std::string_view("GET"));
    RUVIA_CHECK(result_value.request_.known_method() == http_known_method::get);
    RUVIA_CHECK_EQ(result_value.request_.path(), std::string_view("/path"));
    RUVIA_CHECK_EQ(result_value.request_.query_string(), std::string_view("q=1"));
    RUVIA_CHECK(result_value.request_.scheme().empty());
    RUVIA_CHECK(result_value.request_.authority().empty());
    RUVIA_CHECK(result_value.request_.target_form() == http_request_target_form::origin);
    RUVIA_CHECK_EQ(result_value.request_.header("host"), std::string_view("example.com"));
    RUVIA_CHECK(result_value.request_.protocol_version() == http_protocol_version::http11);
}

RUVIA_TEST(http1_parser_maps_wire_versions_to_typed_control_data) {
    http1_server_request_parser parser;
    const auto http11 = parser.parse_message("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
    const auto http10 = parser.parse_message("GET / HTTP/1.0\r\n\r\n");

    RUVIA_CHECK(http11.message_ready());
    RUVIA_CHECK(http10.message_ready());
    RUVIA_CHECK(http11.request_.protocol_version() == http_protocol_version::http11);
    RUVIA_CHECK(http10.request_.protocol_version() == http_protocol_version::http10);
}

RUVIA_TEST(http1_parse_incomplete_head) {
    http1_server_request_parser parser;
    // No terminating blank line yet -> incomplete, keep reading.
    const auto result_value = parser.parse_message("GET / HTTP/1.1\r\nHost: example.com\r\n");
    RUVIA_CHECK(result_value.need_request_head() != nullptr);
}

RUVIA_TEST(http1_parse_missing_host_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message("GET / HTTP/1.1\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::missing_host));
}

RUVIA_TEST(http1_parse_rejects_header_smuggling_vectors) {
    const auto rejected = [](std::string_view request) {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(request);
        return result_value.failure() != nullptr;
    };

    // obs-fold: a continuation line (leading SP or HTAB) must be rejected, not
    // folded into the previous value -- a classic request-smuggling vector
    // (RFC 9112 5.2 deprecates obs-fold in requests).
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo: bar\r\n baz\r\n\r\n"));
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo: bar\r\n\tbaz\r\n\r\n"));

    // Whitespace between the field name and the colon is rejected (it causes a
    // parsing differential across proxies that enables smuggling).
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo : bar\r\n\r\n"));

    // A bare LF or bare CR inside a field value cannot smuggle a second header
    // line: the value stops at the control byte and the required CRLF is absent.
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo: a\nb\r\n\r\n"));
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo: a\rb\r\n\r\n"));
}

RUVIA_TEST(http1_parse_invalid_request_line_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message("!!!garbage!!!\r\n\r\n");
    RUVIA_CHECK(result_value.failure() != nullptr);
}

RUVIA_TEST(http1_parse_content_length_body) {
    http1_server_request_parser parser;
    const auto result_value =
        parser.parse_message("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(result_value.request_.method(), std::string_view("POST"));
    RUVIA_CHECK(result_value.request_.known_method() == http_known_method::post);
    RUVIA_CHECK_EQ(require_known_length(result_value.body_plan_).content_length(), std::size_t{5});
}

RUVIA_TEST(http1_parse_conflicting_content_length_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::conflicting_content_length));
}

RUVIA_TEST(http1_parse_content_length_combined_equal_values) {
    // RFC 9112 section 6.3 explicitly permits a comma-combined field when every
    // decimal value is valid and identical. The shared parser must consume the
    // whole list rather than selecting one attacker-controlled member.
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 5, 5\r\n\r\nhello");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(require_known_length(result_value.body_plan_).content_length(), std::size_t{5});

    const auto conflict = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 5, 6\r\n\r\n");
    RUVIA_CHECK(is_failure(conflict, http_parse_error::conflicting_content_length));
}

RUVIA_TEST(http1_parse_invalid_content_length_forms_rejected) {
    // Signs, non-decimal notation, trailing junk, and empty combined members are
    // invalid; none can degrade into prefix parsing.
    for (const auto* value : {"+5", "-5", "0x10", "5abc", "5,", ",5"}) {
        http1_server_request_parser parser;
        const std::string raw =
            std::string("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: ") + value + "\r\n\r\n";
        const auto result_value = parser.parse_message(raw);
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_content_length));
    }
}

RUVIA_TEST(http1_parse_identical_duplicate_content_length_accepted) {
    // RFC 9112 section 6.3: multiple Content-Length fields carrying the SAME value are not
    // a conflict -- only differing values are (rejected above). Pin the accept side
    // so a refactor can neither start rejecting all duplicates (breaking clients that
    // legitimately repeat the header) nor, worse, start accepting differing ones
    // (which would reopen the CL-vs-CL smuggling hole).
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(require_known_length(result_value.body_plan_).content_length(), std::size_t{5});
}

RUVIA_TEST(http1_parse_content_length_with_transfer_encoding_rejected) {
    // TE + CL together is a request-smuggling vector and must be rejected
    // (RFC 9112 section 6.3).
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(result_value.failure() != nullptr);
}

RUVIA_TEST(http1_parse_duplicate_content_type_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n\r\n{}");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
}

RUVIA_TEST(http1_parse_duplicate_range_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "Range: bytes=0-99\r\n"
        "Range: bytes=200-299\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
}

RUVIA_TEST(http1_parse_repeated_etag_list_fields_accepted) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "If-None-Match: \"old\"\r\n"
        "If-None-Match: \"new\"\r\n\r\n");
    RUVIA_CHECK(result_value.message_ready());
}

RUVIA_TEST(http1_parse_upgrade_requires_connection_option) {
    {
        http1_server_request_parser parser;
        const auto result_value =
            parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n");
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_connection));
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(
            "GET / HTTP/1.1\r\nHost: x\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n\r\n");
        RUVIA_CHECK(result_value.message_ready());
        RUVIA_CHECK_EQ(result_value.request_.header("Upgrade").value_or(std::string_view{}),
            std::string_view("websocket"));
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message("GET / HTTP/1.0\r\nUpgrade: websocket\r\n\r\n");
        RUVIA_CHECK(result_value.message_ready());
        RUVIA_CHECK(result_value.request_.header("Upgrade").has_value());
    }
}

RUVIA_TEST(http1_parse_te_requires_connection_option) {
    {
        http1_server_request_parser parser;
        const auto result_value =
            parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\nTE: trailers\r\n\r\n");
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_connection));
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(
            "GET / HTTP/1.1\r\nHost: x\r\n"
            "Connection: TE\r\n"
            "TE: trailers\r\n\r\n");
        RUVIA_CHECK(result_value.message_ready());
        RUVIA_CHECK_EQ(
            result_value.request_.header("TE").value_or(std::string_view{}), std::string_view("trailers"));
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(
            "GET / HTTP/1.1\r\nHost: x\r\n"
            "Connection: keep-alive\r\n"
            "Connection: TE\r\n"
            "TE:\r\n\r\n");
        RUVIA_CHECK(result_value.message_ready());
        RUVIA_CHECK(result_value.request_.header("TE").has_value());
    }
}

RUVIA_TEST(http1_parse_invalid_te_field_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Connection: TE\r\n"
        "TE: chunked\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
}

RUVIA_TEST(http1_parse_duplicate_authorization_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Authorization: Bearer first\r\n"
        "Authorization: Bearer second\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
}

RUVIA_TEST(http1_parse_duplicate_websocket_identity_and_user_agent_rejected) {
    const std::string_view messages[] = {
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Key: MDEyMzQ1Njc4OWFiY2RlZg==\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "User-Agent: first/1\r\n"
        "User-Agent: second/2\r\n\r\n",
    };
    for (const auto message : messages) {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(message);
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
    }
}

RUVIA_TEST(http1_parse_chunked_body) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK(require_chunked(result_value.body_plan_).transfer_codings().empty());
}

RUVIA_TEST(http1_parse_rejects_non_empty_trailer_header_without_chunked_framing) {
    {
        http1_server_request_parser parser;
        const auto result_value =
            parser.parse_message("POST / HTTP/1.1\r\nHost: x\r\nTrailer: X-Checksum\r\n\r\n");
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(
            "POST / HTTP/1.1\r\nHost: x\r\nTrailer: X-Checksum\r\nContent-Length: 0\r\n\r\n");
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message("POST / HTTP/1.1\r\nHost: x\r\nTrailer: ,\r\n\r\n");
        RUVIA_CHECK(result_value.message_ready());
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(
            "POST / HTTP/1.1\r\nHost: x\r\nTrailer: X-Checksum\r\nTransfer-Encoding: "
            "chunked\r\n\r\n0\r\n\r\n");
        RUVIA_CHECK(result_value.message_ready());
        RUVIA_CHECK(result_value.body_plan_.chunked() != nullptr);
    }
}

RUVIA_TEST(http1_parse_transfer_coding_before_final_chunked) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n"
        "3\r\nraw\r\n0\r\n\r\n");
    RUVIA_CHECK(result_value.message_ready());
    const auto& chunked = require_chunked(result_value.body_plan_);
    RUVIA_CHECK_EQ(chunked.transfer_codings().values_.size(), std::size_t{1});
    RUVIA_CHECK(chunked.transfer_codings().values_[0] == ruvia::http_transfer_coding::gzip);
}

RUVIA_TEST(http1_parse_unsupported_version_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message("GET / HTTP/2.0\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::unsupported_http_version));
}

RUVIA_TEST(http1_parse_extension_method_preserves_case_sensitive_token) {
    http1_server_request_parser parser;
    const auto extension = parser.parse_message("PROPFIND /dav HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(extension.message_ready());
    RUVIA_CHECK_EQ(extension.request_.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(extension.request_.known_method() == http_known_method::unknown);
    RUVIA_CHECK_EQ(extension.request_.path(), std::string_view("/dav"));

    const auto lowercase = parser.parse_message("get /case-sensitive HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(lowercase.message_ready());
    RUVIA_CHECK_EQ(lowercase.request_.method(), std::string_view("get"));
    RUVIA_CHECK(lowercase.request_.known_method() == http_known_method::unknown);
}

RUVIA_TEST(http1_parse_invalid_method_token_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message("BAD(METHOD / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_request_line));
}

RUVIA_TEST(http1_parse_transfer_encoding_not_chunked_rejected) {
    // A non-chunked Transfer-Encoding leaves message framing undetermined.
    http1_server_request_parser parser;
    const auto result_value =
        parser.parse_message("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_transfer_encoding));
}

RUVIA_TEST(http1_request_unknown_transfer_coding_obeys_final_framing_precedence) {
    http1_server_request_parser parser;
    const auto unframed = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: unknown\r\n\r\n");
    RUVIA_CHECK(is_failure(unframed, http_parse_error::invalid_transfer_encoding));
    RUVIA_CHECK(unframed.connection_plan_.disposition() == http1_close_policy::close_after_response);

    const auto framed = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: unknown, chunked\r\n\r\n0\r\n\r\n");
    RUVIA_CHECK(is_failure(framed, http_parse_error::unsupported_transfer_encoding));
    RUVIA_CHECK(framed.connection_plan_.disposition() == http1_close_policy::close_after_response);
}

RUVIA_TEST(http1_request_parsers_propagate_transfer_plan_allocation_failures) {
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, deflate, chunked\r\n\r\n0\r\n\r\n";
    const ruvia::http1_request_parser public_parser;
    verify_parser_allocation_failures(ruvia_ctx, [&public_parser, wire](failing_memory_resource& resource) { return public_parser.parse(wire, {.resource_ = &resource}); }, [](const auto& result_value) { return result_value.parsed() != nullptr; });

    const http1_server_request_parser server_parser;
    verify_parser_allocation_failures(ruvia_ctx, [&server_parser, wire](failing_memory_resource& resource) { return server_parser.parse_message(wire, &resource); }, [](const auto& result_value) { return result_value.message_ready() != nullptr; });
}

RUVIA_TEST(http1_parse_transfer_encoding_in_http10_rejected) {
    // Transfer-Encoding in HTTP/1.0 is faulty framing (RFC 9112 6.1).
    http1_server_request_parser parser;
    const auto result_value =
        parser.parse_message("POST / HTTP/1.0\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_transfer_encoding));
}

RUVIA_TEST(http1_parse_failure_preserves_accepted_http10_version) {
    http1_server_request_parser parser;

    // The request line is valid HTTP/1.0, but the target grammar is rejected
    // afterwards. The error response must retain HTTP/1.0 as its wire version.
    const auto invalid_target = parser.parse_message("GET * HTTP/1.0\r\n\r\n");
    RUVIA_CHECK(invalid_target.failure() != nullptr);
    RUVIA_CHECK(invalid_target.connection_plan_.protocol_version() == http_protocol_version::http10);
    RUVIA_CHECK(
        invalid_target.connection_plan_.disposition() == http1_close_policy::close_after_response);

    // The same invariant applies to a framing rule checked after the version
    // line has already been accepted.
    const auto invalid_framing =
        parser.parse_message("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(invalid_framing.failure() != nullptr);
    RUVIA_CHECK(invalid_framing.connection_plan_.protocol_version() == http_protocol_version::http10);
    RUVIA_CHECK(
        invalid_framing.connection_plan_.disposition() == http1_close_policy::close_after_response);
}

RUVIA_TEST(http1_parse_absolute_uri_uses_target_authority) {
    // RFC 9112 section 3.2.2 requires an origin server to accept absolute-form,
    // ignore Host, and use the request-target authority. The parsed request must
    // expose one effective authority rather than preserve the conflicting field.
    http1_server_request_parser parser;
    const auto result_value =
        parser.parse_message("GET http://a.example/ HTTP/1.1\r\nHost: b.example\r\n\r\n");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(result_value.request_.target(), std::string_view("http://a.example/"));
    RUVIA_CHECK_EQ(result_value.request_.scheme(), std::string_view("http"));
    RUVIA_CHECK_EQ(result_value.request_.authority(), std::string_view("a.example"));
    RUVIA_CHECK(result_value.request_.target_form() == http_request_target_form::absolute);
    RUVIA_CHECK_EQ(result_value.request_.path(), std::string_view("/"));
    RUVIA_CHECK_EQ(
        result_value.request_.header("Host").value_or(std::string_view{}), std::string_view("a.example"));
    for (const auto& header : result_value.request_.headers()) {
        if (header.name() == "Host") {
            RUVIA_CHECK_EQ(header.value(), std::string_view("a.example"));
        }
    }
}

RUVIA_TEST(http1_parse_absolute_uri_accepts_generic_schemes_and_empty_host) {
    http1_server_request_parser parser;

    const auto hierarchical = parser.parse_message(
        "GET ftp://archive.example/pub/file HTTP/1.1\r\n"
        "Host: stale.example\r\n\r\n");
    RUVIA_CHECK(hierarchical.message_ready());
    RUVIA_CHECK_EQ(hierarchical.request_.path(), std::string_view("/pub/file"));
    RUVIA_CHECK_EQ(hierarchical.request_.header("Host").value_or(std::string_view{}),
        std::string_view("archive.example"));

    const auto without_authority = parser.parse_message(
        "GET urn:example:animal:ferret:nose HTTP/1.1\r\n"
        "Host:\r\n\r\n");
    RUVIA_CHECK(without_authority.message_ready());
    RUVIA_CHECK_EQ(without_authority.request_.path(), std::string_view("example:animal:ferret:nose"));
    RUVIA_CHECK(without_authority.request_.header("Host").has_value());
    RUVIA_CHECK(without_authority.request_.header("Host")->empty());
}

RUVIA_TEST(http1_parse_absolute_options_empty_path_is_server_wide) {
    http1_server_request_parser parser;

    const auto result_value = parser.parse_message(
        "OPTIONS http://api.example HTTP/1.1\r\n"
        "Host: stale.example\r\n\r\n");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(result_value.request_.path(), std::string_view("*"));
    RUVIA_CHECK(result_value.request_.query_string().empty());
    RUVIA_CHECK_EQ(result_value.request_.header("Host").value_or(std::string_view{}),
        std::string_view("api.example"));
}

RUVIA_TEST(http1_parse_absolute_options_empty_path_with_query_normalizes_to_slash) {
    http1_server_request_parser parser;

    const auto result_value = parser.parse_message(
        "OPTIONS http://api.example?debug=true HTTP/1.1\r\n"
        "Host: stale.example\r\n\r\n");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(result_value.request_.path(), std::string_view("/"));
    RUVIA_CHECK_EQ(result_value.request_.query_string(), std::string_view("debug=true"));
    RUVIA_CHECK_EQ(result_value.request_.header("Host").value_or(std::string_view{}),
        std::string_view("api.example"));
}

RUVIA_TEST(http1_parse_options_content_requires_valid_content_type) {
    http1_server_request_parser parser;

    const auto missing_for_empty_content = parser.parse_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(is_failure(missing_for_empty_content, http_parse_error::invalid_header));

    const auto missing_for_chunked_content = parser.parse_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n");
    RUVIA_CHECK(is_failure(missing_for_chunked_content, http_parse_error::invalid_header));

    const auto invalid = parser.parse_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: not a media type\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(is_failure(invalid, http_parse_error::invalid_header));

    const auto valid = parser.parse_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(valid.message_ready());

    const auto valid_chunked = parser.parse_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "1\r\nx\r\n0\r\n\r\n");
    RUVIA_CHECK(valid_chunked.message_ready());

    const auto no_content = parser.parse_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n");
    RUVIA_CHECK(no_content.message_ready());

    const auto extension_method = parser.parse_message(
        "options /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(extension_method.message_ready());
}

RUVIA_TEST(http1_parse_rejects_invalid_content_type_syntax) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST /items HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: not a media type\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
}

RUVIA_TEST(http1_parse_rejects_invalid_content_encoding_syntax) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        http1_server_request_parser parser;
        std::string request =
            "POST /items HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Content-Encoding: ";
        request.append(value);
        request.append("\r\nContent-Length: 0\r\n\r\n");
        const auto result_value = parser.parse_message(request);
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_header));
    }

    http1_server_request_parser tolerant;
    const auto accepted = tolerant.parse_message(
        "POST /items HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Encoding: , gzip,,\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(accepted.message_ready());
}

RUVIA_TEST(http1_parse_authority_uses_shared_uri_normalization) {
    http1_server_request_parser parser;
    // RFC 9110 section 4.2.3: an empty port is the scheme default, host is
    // case-insensitive, and percent-encoded unreserved octets normalize.
    const auto empty_port = parser.parse_message(
        "GET http://EXAMPLE.com:/path HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n");
    RUVIA_CHECK(empty_port.message_ready());

    const auto encoded_host = parser.parse_message(
        "GET http://exa%6Dple.com/path HTTP/1.1\r\n"
        "Host: example.com:\r\n\r\n");
    RUVIA_CHECK(encoded_host.message_ready());

    const auto future_literal = parser.parse_message(
        "GET http://[v1.future]/path HTTP/1.1\r\n"
        "Host: [V1.FUTURE]:\r\n\r\n");
    RUVIA_CHECK(future_literal.message_ready());
}

RUVIA_TEST(http1_parse_connect_requires_authority_form) {
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(
            "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n");
        RUVIA_CHECK(result_value.message_ready());
        RUVIA_CHECK_EQ(result_value.request_.method(), std::string_view("CONNECT"));
        RUVIA_CHECK(result_value.request_.known_method() == http_known_method::connect);
        RUVIA_CHECK_EQ(result_value.request_.target(), std::string_view("example.com:443"));
        RUVIA_CHECK(result_value.request_.scheme().empty());
        RUVIA_CHECK_EQ(result_value.request_.authority(), std::string_view("example.com:443"));
        RUVIA_CHECK(result_value.request_.target_form() == http_request_target_form::authority);
        RUVIA_CHECK_EQ(result_value.request_.path(), std::string_view("example.com:443"));
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message("CONNECT / HTTP/1.1\r\nHost: example.com\r\n\r\n");
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_request_target));
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(
            "CONNECT http://example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n");
        RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_request_target));
    }
}

RUVIA_TEST(http1_parse_connect_uses_target_authority) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "CONNECT tunnel.example:443 HTTP/1.1\r\n"
        "Host: decoy.example\r\n\r\n");

    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(result_value.request_.header("Host").value_or(std::string_view{}),
        std::string_view("tunnel.example:443"));
    for (const auto& header : result_value.request_.headers()) {
        if (header.name() == "Host") {
            RUVIA_CHECK_EQ(header.value(), std::string_view("tunnel.example:443"));
        }
    }
}

RUVIA_TEST(http1_parse_methods_without_content_reject_framing_fields) {
    const auto rejects = [&](std::string_view request, http_parse_error error) {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(request);
        RUVIA_CHECK(is_failure(result_value, error));
    };

    // RFC 9110 sections 9.3.6 and 9.3.8 define CONNECT as having no request
    // content and forbid a client from sending content in TRACE. A framing
    // field is still an explicit content signal when its length is zero.
    rejects(
        "CONNECT tunnel.example:443 HTTP/1.1\r\n"
        "Host: tunnel.example:443\r\nContent-Length: 0\r\n\r\n",
        http_parse_error::invalid_content_length);
    rejects(
        "CONNECT tunnel.example:443 HTTP/1.1\r\n"
        "Host: tunnel.example:443\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n",
        http_parse_error::invalid_transfer_encoding);
    rejects(
        "TRACE /diagnostic HTTP/1.1\r\n"
        "Host: example.test\r\nContent-Length: 4\r\n\r\nbody",
        http_parse_error::invalid_content_length);
    rejects(
        "TRACE /diagnostic HTTP/1.1\r\n"
        "Host: example.test\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n",
        http_parse_error::invalid_transfer_encoding);

    // Method tokens are case-sensitive. A lowercase extension method named
    // "trace" does not acquire the registered TRACE method's semantics.
    http1_server_request_parser extension_parser;
    const auto extension = extension_parser.parse_message(
        "trace /diagnostic HTTP/1.1\r\n"
        "Host: example.test\r\nContent-Length: 4\r\n\r\nbody");
    RUVIA_CHECK(extension.message_ready());
    RUVIA_CHECK_EQ(require_known_length(extension.body_plan_).content_length(), std::size_t{4});

    // Unframed bytes following a CONNECT head are not request content. The
    // whole-message parser returns the exact head boundary so a tunnel owner
    // can retain those bytes for its post-response state transition.
    constexpr std::string_view connect_head =
        "CONNECT tunnel.example:443 HTTP/1.1\r\n"
        "Host: tunnel.example:443\r\n\r\n";
    std::string early_tunnel_bytes(connect_head);
    early_tunnel_bytes += "opaque";
    const auto parsed_value = ruvia::http1_request_parser().parse(early_tunnel_bytes);
    RUVIA_CHECK(parsed_value.parsed() != nullptr);
    if (const auto* message = parsed_value.parsed()) {
        RUVIA_CHECK(message->body_plan().without_body() != nullptr);
        RUVIA_CHECK(message->wire_body().empty());
        RUVIA_CHECK_EQ(message->consumed_bytes(), connect_head.size());
    }
}

RUVIA_TEST(http1_parse_http10_without_host_allowed) {
    // HTTP/1.0 does not require a Host header.
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message("GET / HTTP/1.0\r\n\r\n");
    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK_EQ(result_value.request_.method(), std::string_view("GET"));
    RUVIA_CHECK(result_value.request_.known_method() == http_known_method::get);
    RUVIA_CHECK(result_value.request_.protocol_version() == http_protocol_version::http10);
}

RUVIA_TEST(http1_parse_http10_ignores_upgrade_field_semantics) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "GET / HTTP/1.0\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket/\r\n\r\n");

    RUVIA_CHECK(result_value.message_ready());
    RUVIA_CHECK(result_value.request_.protocol_version() == http_protocol_version::http10);
    RUVIA_CHECK_EQ(result_value.request_.header("Upgrade"), std::optional<std::string_view>("websocket/"));
}

RUVIA_TEST(http1_parse_non_numeric_content_length_rejected) {
    http1_server_request_parser parser;
    const auto result_value =
        parser.parse_message("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_content_length));
}

RUVIA_TEST(http1_parse_too_many_headers_rejected) {
    std::string request = "GET / HTTP/1.1\r\nHost: x\r\n";
    for (int i = 0; i < 70; ++i) {  // exceeds max_http_header_fields (64)
        request += "x-h-" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(request);
    RUVIA_CHECK(is_failure(result_value, http_parse_error::too_many_headers));
}

RUVIA_TEST(http1_parse_chunk_size_overflow_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "10000000000000000\r\nx\r\n0\r\n\r\n");  // 2^64 chunk size overflows
    RUVIA_CHECK(is_failure(result_value, http_parse_error::chunk_size_overflow));
}

RUVIA_TEST(http1_parse_invalid_chunk_size_rejected) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nx\r\n0\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_chunk_size));
}

RUVIA_TEST(http1_server_head_ready_is_distinct_from_message_ready) {
    http1_server_request_parser parser;
    http1_server_request_parse_state head;
    constexpr std::string_view request =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";

    parser.parse_head(request, head);
    const auto* request_head = head.head_ready();
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(head.message_ready() == nullptr);
    if (request_head != nullptr) {
        RUVIA_CHECK_EQ(
            request_head->header_bytes(), request.size() - std::string_view("hello").size());
    }
    RUVIA_CHECK_EQ(head.request_.method(), std::string_view("POST"));
    RUVIA_CHECK(head.request_.known_method() == http_known_method::post);

    const auto message = parser.parse_message(request);
    const auto* message_ready = message.message_ready();
    RUVIA_CHECK(message_ready != nullptr);
    RUVIA_CHECK(message.head_ready() == nullptr);
    if (message_ready != nullptr) {
        RUVIA_CHECK_EQ(message_ready->message_bytes(), request.size());
    }

    const auto body_pending = parser.parse_message(request.substr(0, request.size() - 1));
    const auto* need_body = body_pending.need_request_body();
    RUVIA_CHECK(need_body != nullptr);
    if (need_body != nullptr) {
        RUVIA_CHECK(need_body->required_total_bytes().has_value());
        if (need_body->required_total_bytes()) {
            RUVIA_CHECK_EQ(*need_body->required_total_bytes(), request.size());
        }
    }
}

RUVIA_TEST(http1_response_coding_folds_all_accept_encoding_field_lines) {
    http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0.2\r\n"
        "Accept-Encoding: br;q=0.8\r\n\r\n");
    RUVIA_CHECK(parsed_value.message_ready() != nullptr);
    const auto response_coding = parsed_value.response_coding_selection();
    RUVIA_CHECK(response_coding.selected() != nullptr);
    if (const auto* selected = response_coding.selected()) {
        RUVIA_CHECK(selected->coding() == ruvia::http_content_coding::brotli);
    }
}

RUVIA_TEST(http1_response_coding_rejects_when_every_coding_is_forbidden) {
    http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0, br;q=0, zstd;q=0\r\n\r\n");
    RUVIA_CHECK(parsed_value.message_ready() != nullptr);
    const auto response_coding = parsed_value.response_coding_selection();
    RUVIA_CHECK(response_coding.selected() == nullptr);
    RUVIA_CHECK(response_coding.failure() != nullptr);
}

RUVIA_TEST(http1_parse_header_block_too_large_rejected) {
    // A header section that reaches the byte cap without terminating is a DoS
    // guard (max_http_header_bytes is 64 KiB).
    std::string request = "GET / HTTP/1.1\r\nX-Big: ";
    request += std::string(64 * 1024, 'a');  // pushes past the cap, no blank line
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(request);
    RUVIA_CHECK(is_failure(result_value, http_parse_error::header_too_large));
}

RUVIA_TEST(http1_parse_invalid_request_target_rejected) {
    // An origin-form target must begin with '/'; a bare word is not a valid
    // request target for GET.
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message("GET foobar HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value, http_parse_error::invalid_request_target));
}
