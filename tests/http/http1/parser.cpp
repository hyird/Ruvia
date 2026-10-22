#include <algorithm>
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_parse_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_content_decoding.h"

#include "failing_memory_resource.h"
#include "http1_request_message_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::http1_chunk_decode_error;
using ruvia::http1_close_policy;
using ruvia::http1_server_request_parse_failure_source;
using ruvia::http1_server_request_parse_state;
using ruvia::http1_server_request_parser;
using ruvia::http_known_method;
using ruvia::http_parse_error;
using ruvia::http_protocol_version;
using ruvia::http_request_target_form;
using ruvia::http_unsupported_expectation_policy;
using ruvia::testing::parse_http1_request_message;

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

RUVIA_TEST(http1_request_trailer_declaration_metadata_preserves_body_and_pipeline) {
    constexpr std::string_view next_wire = "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n";
    for (const std::string_view declaration :
        {"Content-Length", "Host", "bad field", "\"x-name\"", ",", "X-One,,X-Two"}) {
        for (const bool chunked : {false, true}) {
            std::string head = "POST /first HTTP/1.1\r\nHost: example.test\r\nTrailer: \t";
            head.append(declaration);
            head.append(" \t\r\n");
            head.append(chunked ? "Transfer-Encoding: chunked\r\n\r\n" : "Content-Length: 3\r\n\r\n");
            const std::string message = head + (chunked ? "3\r\nabc\r\n0\r\n\r\n" : "abc");
            const std::string wire = message + std::string(next_wire);
            const auto incomplete = parse_http1_request_message(head);
            RUVIA_CHECK(incomplete.need_body());
            const auto parsed = parse_http1_request_message(wire);
            RUVIA_CHECK(parsed.complete());
            if (!parsed.complete()) {
                continue;
            }
            RUVIA_CHECK_EQ(parsed.state_.request_.header("Trailer").value_or("missing"), declaration);
            RUVIA_CHECK_EQ(parsed.state_.request_.header("Host").value_or("missing"), "example.test");
            RUVIA_CHECK_EQ(parsed.state_.head_ready()->header_bytes(), head.size());
            if (chunked) {
                RUVIA_CHECK(parsed.state_.body_plan_.chunked() != nullptr);
            } else {
                RUVIA_CHECK_EQ(require_known_length(parsed.state_.body_plan_).content_length(), std::size_t{3});
            }
            RUVIA_CHECK_EQ(parsed.body_, "abc");
            RUVIA_CHECK_EQ(*parsed.message_bytes_, message.size());
            const auto remaining = std::string_view(wire).substr(*parsed.message_bytes_);
            RUVIA_CHECK_EQ(remaining, next_wire);
            const auto next = parse_http1_request_message(remaining);
            RUVIA_CHECK(next.complete());
            RUVIA_CHECK_EQ(next.state_.request_.path(), "/next");
            RUVIA_CHECK_EQ(next.message_bytes_.value_or(0), next_wire.size());
        }
    }
}

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

RUVIA_TEST(http1_parse_message_outcome_exposes_only_its_active_alternative) {
    const auto need_more = parse_http1_request_message("GET / HTTP/1.1\r\nHost: example.com\r\n");
    RUVIA_CHECK(need_more.state_.need_request_head() != nullptr);
    RUVIA_CHECK(need_more.state_.head_ready() == nullptr);
    RUVIA_CHECK(!need_more.need_body());
    RUVIA_CHECK(!need_more.complete());
    RUVIA_CHECK(need_more.state_.failure() == nullptr);

    const auto failure = parse_http1_request_message("GET / HTTP/1.1\r\n\r\n");
    const auto* failure_state = failure.state_.failure();
    RUVIA_CHECK(failure.state_.need_request_head() == nullptr);
    RUVIA_CHECK(failure.state_.head_ready() == nullptr);
    RUVIA_CHECK(!failure.need_body());
    RUVIA_CHECK(!failure.complete());
    RUVIA_CHECK(failure_state != nullptr);
    if (failure_state != nullptr) {
        const auto error = failure_state->protocol_error();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::bad_request);
        RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("missing Host header"));
    }
}

RUVIA_TEST(http1_parse_message_handles_deterministic_arbitrary_bytes) {
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

        const auto result_value = parse_http1_request_message(input);
        const auto active_alternatives =
            static_cast<unsigned>(result_value.state_.need_request_head() != nullptr) +
            static_cast<unsigned>(result_value.state_.head_ready() != nullptr) +
            static_cast<unsigned>(result_value.state_.failure() != nullptr);
        RUVIA_CHECK_EQ(active_alternatives, 1U);
        if (result_value.state_.head_ready() != nullptr) {
            const auto active_outcomes = static_cast<unsigned>(result_value.complete()) +
                                         static_cast<unsigned>(result_value.need_body()) +
                                         static_cast<unsigned>(result_value.chunk_error_.has_value());
            RUVIA_CHECK_EQ(active_outcomes, 1U);
        }
    }
}

RUVIA_TEST(http1_parse_message_maps_random_valid_heads_to_stable_request_plans) {
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

        const auto result_value = parse_http1_request_message(message);
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK(result_value.state_.failure() == nullptr);
        if (!result_value.complete() || result_value.state_.failure() != nullptr) {
            continue;
        }
        RUVIA_CHECK_EQ(result_value.state_.request_.method(), std::string_view("POST"));
        RUVIA_CHECK_EQ(result_value.state_.request_.path(), std::string_view(path));
        RUVIA_CHECK_EQ(result_value.state_.request_.header("Host").value_or(std::string_view{}),
            std::string_view("example.test"));
        RUVIA_CHECK_EQ(result_value.state_.request_.header("X-Fuzz").value_or(std::string_view{}),
            std::string_view(fuzz_value));
        RUVIA_CHECK(result_value.state_.request_.protocol_version() ==
                    (http11 ? http_protocol_version::http11 : http_protocol_version::http10));

        const auto expected_close_policy =
            (!http11 && connection_case != 2U) || connection_case == 1U
                ? http1_close_policy::close_after_response
                : http1_close_policy::allow_reuse;
        RUVIA_CHECK(result_value.state_.connection_plan_.disposition() == expected_close_policy);
        RUVIA_CHECK(result_value.state_.connection_plan_.protocol_version() ==
                    (http11 ? http_protocol_version::http11 : http_protocol_version::http10));

        RUVIA_CHECK_EQ(*result_value.message_bytes_, framed_bytes);
        RUVIA_CHECK_EQ(result_value.body_, body);
        if (body_case == 1U) {
            RUVIA_CHECK_EQ(require_known_length(result_value.state_.body_plan_).content_length(), body.size());
        } else if (body_case == 2U) {
            RUVIA_CHECK(result_value.state_.body_plan_.chunked() != nullptr);
        } else {
            RUVIA_CHECK(result_value.state_.body_plan_.without_body() != nullptr);
        }
    }
}

RUVIA_TEST(http1_parse_failure_classifies_only_request_line_failures) {
    const auto request_line_failure =
        parse_http1_request_message("GET / HTTP/9.0\r\nHost: example.com\r\n\r\n");
    RUVIA_CHECK(request_line_failure.state_.failure() != nullptr);
    if (const auto* failure = request_line_failure.state_.failure()) {
        RUVIA_CHECK(failure->source() == http1_server_request_parse_failure_source::request_line);
    }

    const auto message_failure = parse_http1_request_message("GET / HTTP/1.1\r\n\r\n");
    RUVIA_CHECK(message_failure.state_.failure() != nullptr);
    if (const auto* failure = message_failure.state_.failure()) {
        RUVIA_CHECK(failure->source() == http1_server_request_parse_failure_source::message);
    }
}

RUVIA_TEST(http1_parse_message_need_body_separates_required_size_from_consumption) {
    constexpr std::string_view partial =
        "POST / HTTP/1.1\r\nHost: example.com\r\n"
        "Content-Length: 5\r\n\r\nhe";
    const auto result_value = parse_http1_request_message(partial);
    RUVIA_CHECK(result_value.need_body());
    const auto* head = result_value.state_.head_ready();
    const auto* known_length = result_value.state_.body_plan_.known_length();
    RUVIA_CHECK(head != nullptr);
    RUVIA_CHECK(known_length != nullptr);
    if (head != nullptr && known_length != nullptr) {
        RUVIA_CHECK_EQ(head->header_bytes(), partial.size() - std::size_t{2});
        RUVIA_CHECK_EQ(head->header_bytes() + known_length->content_length(), partial.size() + std::size_t{3});
    }
}

RUVIA_TEST(http1_parse_message_success_frames_the_exact_wire_body) {
    constexpr std::string_view content_length_message =
        "POST /fixed HTTP/1.1\r\nHost: example.com\r\n"
        "Content-Length: 5\r\n\r\nhello";
    const std::string content_length_pipeline =
        std::string(content_length_message) + "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto fixed_result = parse_http1_request_message(content_length_pipeline);
    RUVIA_CHECK(fixed_result.complete());
    if (fixed_result.complete()) {
        RUVIA_CHECK_EQ(fixed_result.state_.request_.path(), std::string_view("/fixed"));
        RUVIA_CHECK_EQ(require_known_length(fixed_result.state_.body_plan_).content_length(), std::size_t{5});
        const auto header_bytes = fixed_result.state_.head_ready()->header_bytes();
        RUVIA_CHECK_EQ(std::string_view(content_length_pipeline)
                           .substr(header_bytes, *fixed_result.message_bytes_ - header_bytes),
            std::string_view("hello"));
        RUVIA_CHECK_EQ(*fixed_result.message_bytes_, content_length_message.size());
        RUVIA_CHECK_EQ(fixed_result.body_, std::string_view("hello"));
    }

    constexpr std::string_view chunked_message =
        "POST /chunked HTTP/1.1\r\nHost: example.com\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\nX-Checksum: ok\r\n\r\n";
    const std::string chunked_pipeline =
        std::string(chunked_message) + "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto chunked_result = parse_http1_request_message(chunked_pipeline);
    RUVIA_CHECK(chunked_result.complete());
    if (chunked_result.complete()) {
        RUVIA_CHECK(chunked_result.state_.body_plan_.chunked() != nullptr);
        const auto header_bytes = chunked_result.state_.head_ready()->header_bytes();
        RUVIA_CHECK_EQ(std::string_view(chunked_pipeline)
                           .substr(header_bytes, *chunked_result.message_bytes_ - header_bytes),
            std::string_view("3\r\nabc\r\n0\r\nX-Checksum: ok\r\n\r\n"));
        RUVIA_CHECK_EQ(*chunked_result.message_bytes_, chunked_message.size());
        RUVIA_CHECK_EQ(chunked_result.body_, std::string_view("abc"));
    }
}

RUVIA_TEST(http1_parse_message_preserves_expect_extensions_as_semantics) {
    const auto result_value = parse_http1_request_message(
        "POST /extensions HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Expect: , 100-Continue, custom-feature,\r\n"
        "Content-Length: 1\r\n\r\nx");

    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK(result_value.state_.failure() == nullptr);
    if (result_value.complete()) {
        const auto plan =
            result_value.state_.body_plan_.expectation_plan(http_unsupported_expectation_policy::reject);
        RUVIA_CHECK(plan.rejection() != nullptr);
        RUVIA_CHECK(result_value.state_.body_plan_.expectations().has_continue());
        RUVIA_CHECK(result_value.state_.body_plan_.expectations().has_unsupported());
    }
}

RUVIA_TEST(http1_parse_accepts_empty_expect_parameter_slots) {
    // RFC 9110 section 5.6.6 permits empty parameter slots after an expectation value.
    const auto result_value = parse_http1_request_message(
        "POST /extensions HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Expect: custom=value;, other=\"x\"; ; a=b\r\n"
        "Content-Length: 1\r\n\r\nx");

    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK(result_value.state_.failure() == nullptr);
    if (result_value.complete()) {
        RUVIA_CHECK(!result_value.state_.body_plan_.expectations().has_continue());
        RUVIA_CHECK(result_value.state_.body_plan_.expectations().has_unsupported());
    }
}

RUVIA_TEST(http1_parse_rejects_malformed_expect_field) {
    const auto result_value = parse_http1_request_message(
        "POST /extensions HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Expect: bad value\r\n"
        "Content-Length: 1\r\n\r\nx");

    RUVIA_CHECK(!result_value.complete());
    RUVIA_CHECK(result_value.state_.failure() != nullptr);
    if (const auto* failure = result_value.state_.failure()) {
        const auto protocol_error = failure->protocol_error();
        const auto expected = ruvia::http_parse_protocol_error(http_parse_error::invalid_header);
        RUVIA_CHECK_EQ(protocol_error.status(), expected.status());
        RUVIA_CHECK_EQ(std::string_view(protocol_error.what()), std::string_view(expected.what()));
    }
}

RUVIA_TEST(http1_parse_expect_equals_whitespace_is_not_parameter_whitespace) {
    constexpr std::string_view invalid[]{
        "custom =value", "custom= value", "custom\t=value", "custom=\tvalue",
        "custom=value; a =b", "custom=value; a= b",
        "custom=value; a\t=b", "custom=value; a=\tb",
        "custom=value; a= \"b\"", "custom=value; a=\"b\"extra"};
    for (const auto field : invalid) {
        const auto wire = std::string("POST / HTTP/1.1\r\nHost: example.test\r\nExpect: ") +
                          std::string(field) + "\r\nContent-Length: 1\r\n\r\nx";
        const auto result = parse_http1_request_message(wire);
        RUVIA_CHECK(is_failure(result.state_, http_parse_error::invalid_header));
    }

    constexpr std::string_view valid[]{
        "custom=value \t; \ta=b", "custom=\"a b\"; a=\" b \"",
        "custom=value; ; a=b;", "custom=\"a=b\"; a=\"x=y\""};
    for (const auto field : valid) {
        const auto wire = std::string("POST / HTTP/1.1\r\nHost: example.test\r\nExpect: ") +
                          std::string(field) + "\r\nContent-Length: 1\r\n\r\nx";
        const auto result = parse_http1_request_message(wire);
        RUVIA_CHECK(result.complete());
        RUVIA_CHECK_EQ(result.body_, std::string_view("x"));
        RUVIA_CHECK(result.state_.body_plan_.expectations().has_unsupported());
    }
}

RUVIA_TEST(http1_parse_valid_request) {
    const auto result_value = parse_http1_request_message("GET /path?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(result_value.state_.request_.method(), std::string_view("GET"));
    RUVIA_CHECK(result_value.state_.request_.known_method() == http_known_method::get);
    RUVIA_CHECK_EQ(result_value.state_.request_.path(), std::string_view("/path"));
    RUVIA_CHECK_EQ(result_value.state_.request_.query_string(), std::string_view("q=1"));
    RUVIA_CHECK(result_value.state_.request_.scheme().empty());
    RUVIA_CHECK(result_value.state_.request_.authority().empty());
    RUVIA_CHECK(result_value.state_.request_.target_form() == http_request_target_form::origin);
    RUVIA_CHECK_EQ(result_value.state_.request_.header("host"), std::string_view("example.com"));
    RUVIA_CHECK(result_value.state_.request_.protocol_version() == http_protocol_version::http11);
}

RUVIA_TEST(http1_parser_maps_wire_versions_to_typed_control_data) {
    const auto http11 = parse_http1_request_message("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
    const auto http10 = parse_http1_request_message("GET / HTTP/1.0\r\n\r\n");
    // RFC 9110 6.2: a higher minor version is processed as HTTP/1.1.
    const auto http19 = parse_http1_request_message("GET / HTTP/1.9\r\nHost: example.com\r\n\r\n");

    RUVIA_CHECK(http11.complete());
    RUVIA_CHECK(http10.complete());
    RUVIA_CHECK(http19.complete());
    RUVIA_CHECK(http11.state_.request_.protocol_version() == http_protocol_version::http11);
    RUVIA_CHECK(http10.state_.request_.protocol_version() == http_protocol_version::http10);
    RUVIA_CHECK(http19.state_.request_.protocol_version() == http_protocol_version::http11);
}

RUVIA_TEST(http1_parse_incomplete_head) {
    // No terminating blank line yet -> incomplete, keep reading.
    const auto result_value = parse_http1_request_message("GET / HTTP/1.1\r\nHost: example.com\r\n");
    RUVIA_CHECK(result_value.state_.need_request_head() != nullptr);
}

RUVIA_TEST(http1_parse_missing_host_rejected) {
    const auto result_value = parse_http1_request_message("GET / HTTP/1.1\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::missing_host));
}

RUVIA_TEST(http1_parse_rejects_header_smuggling_vectors) {
    const auto rejected = [](std::string_view request) {
        const auto result_value = parse_http1_request_message(request);
        return result_value.state_.failure() != nullptr;
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
    const auto result_value = parse_http1_request_message("!!!garbage!!!\r\n\r\n");
    RUVIA_CHECK(result_value.state_.failure() != nullptr);
}

RUVIA_TEST(http1_parse_content_length_body) {
    const auto result_value =
        parse_http1_request_message("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(result_value.state_.request_.method(), std::string_view("POST"));
    RUVIA_CHECK(result_value.state_.request_.known_method() == http_known_method::post);
    RUVIA_CHECK_EQ(require_known_length(result_value.state_.body_plan_).content_length(), std::size_t{5});
}

RUVIA_TEST(http1_parse_conflicting_content_length_rejected) {
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::conflicting_content_length));
}

RUVIA_TEST(http1_parse_content_length_combined_equal_values) {
    // RFC 9112 section 6.3 explicitly permits a comma-combined field when every
    // decimal value is valid and identical. The shared parser must consume the
    // whole list rather than selecting one attacker-controlled member.
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 5, 5\r\n\r\nhello");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(require_known_length(result_value.state_.body_plan_).content_length(), std::size_t{5});

    const auto conflict = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 5, 6\r\n\r\n");
    RUVIA_CHECK(is_failure(conflict.state_, http_parse_error::conflicting_content_length));
}

RUVIA_TEST(http1_parse_invalid_content_length_forms_rejected) {
    // Signs, non-decimal notation, trailing junk, and empty combined members are
    // invalid; none can degrade into prefix parsing.
    for (const auto* value : {"+5", "-5", "0x10", "5abc", "5,", ",5"}) {
        const std::string raw =
            std::string("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: ") + value + "\r\n\r\n";
        const auto result_value = parse_http1_request_message(raw);
        RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_content_length));
    }
}

RUVIA_TEST(http1_parse_identical_duplicate_content_length_accepted) {
    // RFC 9112 section 6.3: multiple Content-Length fields carrying the SAME value are not
    // a conflict -- only differing values are (rejected above). Pin the accept side
    // so a refactor can neither start rejecting all duplicates (breaking clients that
    // legitimately repeat the header) nor, worse, start accepting differing ones
    // (which would reopen the CL-vs-CL smuggling hole).
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(require_known_length(result_value.state_.body_plan_).content_length(), std::size_t{5});
}

RUVIA_TEST(http1_parse_content_length_with_transfer_encoding_rejected) {
    // TE + CL together is a request-smuggling vector and must be rejected
    // (RFC 9112 section 6.3).
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(result_value.state_.failure() != nullptr);
}

RUVIA_TEST(http1_parse_duplicate_content_type_rejected) {
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n\r\n{}");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_header));
}

RUVIA_TEST(http1_parse_duplicate_range_rejected) {
    const auto result_value = parse_http1_request_message(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "Range: bytes=0-99\r\n"
        "Range: bytes=200-299\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_header));
}

RUVIA_TEST(http1_parse_repeated_etag_list_fields_accepted) {
    const auto result_value = parse_http1_request_message(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "If-None-Match: \"old\"\r\n"
        "If-None-Match: \"new\"\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
}

RUVIA_TEST(http1_parse_upgrade_advertisement_does_not_require_connection_option) {
    {
        const auto result_value =
            parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK(result_value.state_.body_plan_.without_body() != nullptr);
        RUVIA_CHECK(result_value.state_.connection_plan_.disposition() == http1_close_policy::allow_reuse);
        RUVIA_CHECK_EQ(result_value.state_.request_.header("Upgrade").value_or(std::string_view{}),
            std::string_view("websocket"));
    }
    {
        const auto result_value = parse_http1_request_message(
            "GET / HTTP/1.1\r\nHost: x\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK_EQ(result_value.state_.request_.header("Upgrade").value_or(std::string_view{}),
            std::string_view("websocket"));
    }
    {
        const auto result_value = parse_http1_request_message("GET / HTTP/1.0\r\nUpgrade: websocket\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK(result_value.state_.request_.header("Upgrade").has_value());
    }
}

RUVIA_TEST(http1_parse_te_without_connection_option_is_ignored) {
    {
        const auto result_value =
            parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\nTE: trailers\r\nConnection: close\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
    }
    {
        const auto result_value = parse_http1_request_message(
            "GET / HTTP/1.1\r\nHost: x\r\n"
            "Connection: TE\r\n"
            "TE: trailers\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK_EQ(
            result_value.state_.request_.header("TE").value_or(std::string_view{}), std::string_view("trailers"));
    }
    {
        const auto result_value = parse_http1_request_message(
            "GET / HTTP/1.1\r\nHost: x\r\n"
            "Connection: keep-alive\r\n"
            "Connection: TE\r\n"
            "TE:\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK(result_value.state_.request_.header("TE").has_value());
    }
}

RUVIA_TEST(http1_parse_te_ignores_empty_received_list_members) {
    for (const auto value : {",trailers", "trailers,", ", trailers,,", ",,", ",gzip;q=0.5,,trailers,", ",chunked,"}) {
        std::string wire = "GET / HTTP/1.1\r\nHost: x\r\nConnection: TE\r\nTE: ";
        wire.append(value);
        wire.append("\r\n\r\n");
        const auto result_value = parse_http1_request_message(wire);
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK_EQ(result_value.state_.request_.header("TE").value_or("missing"), std::string_view(value));
    }
    for (const auto value : {",trailers;q=0.5,", ",gzip;q=2,"}) {
        std::string wire = "GET / HTTP/1.1\r\nHost: x\r\nConnection: TE\r\nTE: ";
        wire.append(value);
        wire.append("\r\n\r\n");
        RUVIA_CHECK(is_failure(parse_http1_request_message(wire).state_, http_parse_error::invalid_header));
    }
}

RUVIA_TEST(http1_parse_te_chunked_is_a_sender_rule_only) {
    // RFC 9112 section 7.4 forbids the client from listing chunked; the
    // recipient still accepts the request because chunked is always acceptable.
    const auto result_value = parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Connection: TE\r\n"
        "TE: chunked\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(result_value.state_.request_.header("TE").value_or("missing"), std::string_view("chunked"));
}

RUVIA_TEST(http1_parse_duplicate_authorization_rejected) {
    const auto result_value = parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Authorization: Bearer first\r\n"
        "Authorization: Bearer second\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_header));
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
        const auto result_value = parse_http1_request_message(message);
        RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_header));
    }
}

RUVIA_TEST(http1_parse_chunked_body) {
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK(require_chunked(result_value.state_.body_plan_).transfer_codings().empty());
}

RUVIA_TEST(http1_parse_chunked_body_accepts_many_small_chunks) {
    // RFC 9112 has no chunk-count limit; only the buffered message size bounds
    // the framing of a whole-message parse.
    std::string request = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    const auto header_size = request.size();
    for (std::size_t index = 0; index < 20000; ++index) {
        request.append("1\r\nx\r\n");
    }
    request.append("0\r\n\r\n");
    const auto result_value = parse_http1_request_message(request);
    RUVIA_CHECK(result_value.complete());
    if (result_value.complete()) {
        RUVIA_CHECK_EQ(result_value.state_.head_ready()->header_bytes(), header_size);
        RUVIA_CHECK_EQ(*result_value.message_bytes_, request.size());
        RUVIA_CHECK_EQ(result_value.body_.size(), std::size_t{20000});
    }
}

RUVIA_TEST(http1_parse_trailer_advertisement_does_not_establish_framing) {
    {
        const auto result_value =
            parse_http1_request_message("POST / HTTP/1.1\r\nHost: x\r\nTrailer: X-Checksum\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK(result_value.state_.body_plan_.without_body() != nullptr);
        RUVIA_CHECK(result_value.state_.connection_plan_.disposition() == http1_close_policy::allow_reuse);
    }
    {
        const auto result_value = parse_http1_request_message(
            "POST / HTTP/1.1\r\nHost: x\r\nTrailer: X-Checksum\r\nContent-Length: 0\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK_EQ(require_known_length(result_value.state_.body_plan_).content_length(), std::size_t{0});
        RUVIA_CHECK(result_value.state_.connection_plan_.disposition() == http1_close_policy::allow_reuse);
    }
    {
        const auto result_value = parse_http1_request_message(
            "POST / HTTP/1.1\r\nHost: x\r\nTrailer: X-Checksum\r\nContent-Length: 3\r\n\r\nabc");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK_EQ(require_known_length(result_value.state_.body_plan_).content_length(), std::size_t{3});
        RUVIA_CHECK_EQ(result_value.body_.size(), std::size_t{3});
        RUVIA_CHECK(result_value.state_.connection_plan_.disposition() == http1_close_policy::allow_reuse);
    }
    {
        const auto result_value = parse_http1_request_message("POST / HTTP/1.1\r\nHost: x\r\nTrailer: ,\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
    }
    {
        const auto result_value = parse_http1_request_message(
            "POST / HTTP/1.1\r\nHost: x\r\nTrailer: X-Checksum\r\nTransfer-Encoding: "
            "chunked\r\n\r\n0\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK(result_value.state_.body_plan_.chunked() != nullptr);
    }
}

RUVIA_TEST(http1_parse_transfer_coding_before_final_chunked) {
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n"
        "3\r\nraw\r\n0\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
    const auto& chunked = require_chunked(result_value.state_.body_plan_);
    RUVIA_CHECK_EQ(chunked.transfer_codings().size(), std::size_t{1});
    RUVIA_CHECK(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
}

RUVIA_TEST(http1_parse_transfer_encoding_ignores_empty_members_without_losing_framing) {
    for (const std::string_view fields : {
             "Transfer-Encoding: , , chunked, ,\r\n",
             "Transfer-Encoding: ,\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding:\r\n"}) {
        std::string wire = "POST / HTTP/1.1\r\nHost: x\r\n";
        wire.append(fields);
        wire.append("\r\n5\r\nhello\r\n0\r\n\r\n");
        const auto message_bytes = wire.size();
        wire.append("GET /next HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto parsed = parse_http1_request_message(wire);
        RUVIA_CHECK(parsed.complete());
        if (parsed.complete()) {
            RUVIA_CHECK_EQ(parsed.body_, std::string("hello"));
            RUVIA_CHECK_EQ(*parsed.message_bytes_, message_bytes);
            RUVIA_CHECK(parsed.state_.body_plan_.chunked() != nullptr);
        }
    }
    const auto unframed = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ,\r\n\r\n");
    RUVIA_CHECK(is_failure(unframed.state_, http_parse_error::invalid_transfer_encoding));
    RUVIA_CHECK(unframed.state_.connection_plan_.disposition() == http1_close_policy::close_after_response);

    const auto conflict = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ,\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(is_failure(conflict.state_, http_parse_error::invalid_transfer_encoding));
    RUVIA_CHECK(conflict.state_.connection_plan_.disposition() == http1_close_policy::close_after_response);
}

RUVIA_TEST(http1_parse_transfer_codings_do_not_retain_request_resource) {
    // A connection keeps its parse state across requests while each request
    // head is parsed from storage that dies with that request.
    http1_server_request_parser parser;
    http1_server_request_parse_state state;
    {
        failing_memory_resource request_resource;
        parser.parse_head(
            "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", state, 0, &request_resource);
        RUVIA_CHECK(state.head_ready() != nullptr);
        const auto& chunked = require_chunked(state.body_plan_);
        RUVIA_CHECK_EQ(chunked.transfer_codings().size(), std::size_t{1});
        state.request_.reset();
        RUVIA_CHECK_EQ(request_resource.live_allocations(), std::size_t{0});
    }
    failing_memory_resource next_resource;
    parser.parse_head("GET / HTTP/1.1\r\nHost: x\r\n\r\n", state, 0, &next_resource);
    RUVIA_CHECK(state.head_ready() != nullptr);
    RUVIA_CHECK(state.body_plan_.chunked() == nullptr);
    // The descriptor block borrows next_resource, which dies before state.
    state.request_.reset();
    RUVIA_CHECK_EQ(next_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http1_parse_unsupported_version_rejected) {
    for (const auto wire : {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"}) {
        const auto result_value = parse_http1_request_message(wire);
        RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::unsupported_http_version));
    }
}

RUVIA_TEST(http1_parse_extension_method_preserves_case_sensitive_token) {
    const auto extension = parse_http1_request_message("PROPFIND /dav HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(extension.complete());
    RUVIA_CHECK_EQ(extension.state_.request_.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(extension.state_.request_.known_method() == http_known_method::unknown);
    RUVIA_CHECK_EQ(extension.state_.request_.path(), std::string_view("/dav"));

    const auto lowercase = parse_http1_request_message("get /case-sensitive HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(lowercase.complete());
    RUVIA_CHECK_EQ(lowercase.state_.request_.method(), std::string_view("get"));
    RUVIA_CHECK(lowercase.state_.request_.known_method() == http_known_method::unknown);
}

RUVIA_TEST(http1_parse_invalid_method_token_rejected) {
    const auto result_value = parse_http1_request_message("BAD(METHOD / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_request_line));
}

RUVIA_TEST(http1_parse_transfer_encoding_not_chunked_rejected) {
    // A non-chunked Transfer-Encoding leaves message framing undetermined.
    const auto result_value =
        parse_http1_request_message("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_transfer_encoding));
}

RUVIA_TEST(http1_request_unknown_transfer_coding_obeys_final_framing_precedence) {
    const auto unframed = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: unknown\r\n\r\n");
    RUVIA_CHECK(is_failure(unframed.state_, http_parse_error::invalid_transfer_encoding));
    RUVIA_CHECK(unframed.state_.connection_plan_.disposition() == http1_close_policy::close_after_response);

    const auto framed = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: unknown, chunked\r\n\r\n0\r\n\r\n");
    RUVIA_CHECK(is_failure(framed.state_, http_parse_error::unsupported_transfer_encoding));
    RUVIA_CHECK(framed.state_.connection_plan_.disposition() == http1_close_policy::close_after_response);
}

RUVIA_TEST(http1_parse_message_propagates_transfer_plan_allocation_failures) {
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, deflate, chunked\r\n\r\n0\r\n\r\n";
    verify_parser_allocation_failures(ruvia_ctx, [wire](failing_memory_resource& resource) { return parse_http1_request_message(wire, &resource); }, [](const auto& result_value) { return result_value.complete(); });
}

RUVIA_TEST(http1_parse_transfer_encoding_in_http10_rejected) {
    // Transfer-Encoding in HTTP/1.0 is faulty framing (RFC 9112 6.1).
    const auto result_value =
        parse_http1_request_message("POST / HTTP/1.0\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_transfer_encoding));
}

RUVIA_TEST(http1_parse_failure_preserves_accepted_http10_version) {
    // The request line is valid HTTP/1.0, but the target grammar is rejected
    // afterwards. The error response must retain HTTP/1.0 as its wire version.
    const auto invalid_target = parse_http1_request_message("GET * HTTP/1.0\r\n\r\n");
    RUVIA_CHECK(invalid_target.state_.failure() != nullptr);
    RUVIA_CHECK(invalid_target.state_.connection_plan_.protocol_version() == http_protocol_version::http10);
    RUVIA_CHECK(
        invalid_target.state_.connection_plan_.disposition() == http1_close_policy::close_after_response);

    // The same invariant applies to a framing rule checked after the version
    // line has already been accepted.
    const auto invalid_framing =
        parse_http1_request_message("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(invalid_framing.state_.failure() != nullptr);
    RUVIA_CHECK(invalid_framing.state_.connection_plan_.protocol_version() == http_protocol_version::http10);
    RUVIA_CHECK(
        invalid_framing.state_.connection_plan_.disposition() == http1_close_policy::close_after_response);
}

RUVIA_TEST(http1_parse_head_failures_close_persistent_connections) {
    // Each request would otherwise be reusable; the rejected head leaves its
    // message boundary unknown.
    for (const auto wire : {
             "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
             "TRACE / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx",
             "POST / HTTP/1.0\r\nConnection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n",
             "POST / HTTP/1.1\r\nContent-Length: 1\r\n\r\nx",
         }) {
        const auto result_value = parse_http1_request_message(wire);
        RUVIA_CHECK(result_value.state_.failure() != nullptr);
        RUVIA_CHECK(result_value.state_.connection_plan_.disposition() == http1_close_policy::close_after_response);
    }
    const auto http10 =
        parse_http1_request_message("POST / HTTP/1.0\r\nConnection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(http10.state_.connection_plan_.protocol_version() == http_protocol_version::http10);
}

RUVIA_TEST(http1_parse_absolute_uri_uses_target_authority) {
    // RFC 9112 section 3.2.2 requires an origin server to accept absolute-form,
    // ignore Host, and use the request-target authority. The parsed request must
    // expose one effective authority rather than preserve the conflicting field.
    const auto result_value =
        parse_http1_request_message("GET http://a.example/ HTTP/1.1\r\nHost: b.example\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(result_value.state_.request_.target(), std::string_view("http://a.example/"));
    RUVIA_CHECK_EQ(result_value.state_.request_.scheme(), std::string_view("http"));
    RUVIA_CHECK_EQ(result_value.state_.request_.authority(), std::string_view("a.example"));
    RUVIA_CHECK(result_value.state_.request_.target_form() == http_request_target_form::absolute);
    RUVIA_CHECK_EQ(result_value.state_.request_.path(), std::string_view("/"));
    RUVIA_CHECK_EQ(
        result_value.state_.request_.header("Host").value_or(std::string_view{}), std::string_view("a.example"));
    for (const auto& header : result_value.state_.request_.headers()) {
        if (header.name() == "Host") {
            RUVIA_CHECK_EQ(header.value(), std::string_view("a.example"));
        }
    }
}

RUVIA_TEST(http1_parse_absolute_uri_rejects_non_http_schemes_and_empty_authority) {
    // An origin server routes only http(s) resources. Any other scheme, or an
    // http(s) URI without a non-empty authority, is a 400 and never rewrites
    // Host to an empty value.
    for (const std::string_view message : {
             "GET ftp://archive.example/pub/file HTTP/1.1\r\nHost: x\r\n\r\n",
             "GET javascript:/features/safe HTTP/1.1\r\nHost: x\r\n\r\n",
             "GET x:/features/safe HTTP/1.1\r\nHost: x\r\n\r\n",
             "GET urn:example:animal:ferret:nose HTTP/1.1\r\nHost:\r\n\r\n",
             "GET file:///path HTTP/1.1\r\nHost:\r\n\r\n",
             "GET x:80 HTTP/1.1\r\nHost: x\r\n\r\n",
             "GET http:/path HTTP/1.1\r\nHost: x\r\n\r\n",
             "GET http:///path HTTP/1.1\r\nHost: x\r\n\r\n",
             "GET https://user@a.example/ HTTP/1.1\r\nHost: x\r\n\r\n"}) {
        RUVIA_CHECK(is_failure(parse_http1_request_message(message).state_, http_parse_error::invalid_request_target));
    }

    const auto upper_case = parse_http1_request_message("GET HTTPS://a.example/p HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(upper_case.complete());
    RUVIA_CHECK_EQ(upper_case.state_.request_.path(), std::string_view("/p"));
    RUVIA_CHECK_EQ(upper_case.state_.request_.header("Host").value_or(std::string_view{}), std::string_view("a.example"));
}

RUVIA_TEST(http1_parse_absolute_options_empty_path_is_server_wide) {
    const auto result_value = parse_http1_request_message(
        "OPTIONS http://api.example HTTP/1.1\r\n"
        "Host: stale.example\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(result_value.state_.request_.path(), std::string_view("*"));
    RUVIA_CHECK(result_value.state_.request_.query_string().empty());
    RUVIA_CHECK_EQ(result_value.state_.request_.header("Host").value_or(std::string_view{}),
        std::string_view("api.example"));
}

RUVIA_TEST(http1_parse_absolute_options_empty_path_with_query_normalizes_to_slash) {
    const auto result_value = parse_http1_request_message(
        "OPTIONS http://api.example?debug=true HTTP/1.1\r\n"
        "Host: stale.example\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(result_value.state_.request_.path(), std::string_view("/"));
    RUVIA_CHECK_EQ(result_value.state_.request_.query_string(), std::string_view("debug=true"));
    RUVIA_CHECK_EQ(result_value.state_.request_.header("Host").value_or(std::string_view{}),
        std::string_view("api.example"));
}

RUVIA_TEST(http1_parse_options_content_type_presence_is_sender_obligation) {
    const auto missing_for_empty_content = parse_http1_request_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(missing_for_empty_content.complete());
    RUVIA_CHECK_EQ(require_known_length(missing_for_empty_content.state_.body_plan_).content_length(),
        std::size_t{0});
    RUVIA_CHECK(missing_for_empty_content.state_.connection_plan_.disposition() == http1_close_policy::allow_reuse);

    const auto missing_for_chunked_content = parse_http1_request_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n");
    RUVIA_CHECK(missing_for_chunked_content.complete());
    RUVIA_CHECK(missing_for_chunked_content.state_.body_plan_.chunked() != nullptr);
    RUVIA_CHECK(missing_for_chunked_content.state_.connection_plan_.disposition() == http1_close_policy::allow_reuse);

    const auto missing_for_nonempty_content = parse_http1_request_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 1\r\n\r\nx");
    RUVIA_CHECK(missing_for_nonempty_content.complete());
    RUVIA_CHECK_EQ(require_known_length(missing_for_nonempty_content.state_.body_plan_).content_length(),
        std::size_t{1});
    RUVIA_CHECK_EQ(missing_for_nonempty_content.body_.size(), std::size_t{1});

    const auto valid = parse_http1_request_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(valid.complete());

    const auto valid_chunked = parse_http1_request_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "1\r\nx\r\n0\r\n\r\n");
    RUVIA_CHECK(valid_chunked.complete());

    const auto no_content = parse_http1_request_message(
        "OPTIONS /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n");
    RUVIA_CHECK(no_content.complete());

    const auto extension_method = parse_http1_request_message(
        "options /diagnostics HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(extension_method.complete());
}

RUVIA_TEST(http1_parse_opaque_content_type_preserves_body_and_pipeline_boundaries) {
    for (const std::string_view value : {"", "not a media type", "text/plain; charset=\"unfinished"}) {
        for (const bool chunked : {false, true}) {
            std::string wire = "POST /items HTTP/1.1\r\nHost: example.com\r\nContent-Type: ";
            wire.append(value);
            wire.append("\r\nX-After: kept\r\n");
            wire.append(chunked ? "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\nNEXT"
                                : "Content-Length: 3\r\n\r\nabcNEXT");
            const auto parsed = parse_http1_request_message(wire);
            RUVIA_CHECK(parsed.complete());
            if (!parsed.complete()) {
                continue;
            }
            const auto content_type = parsed.state_.request_.header("Content-Type");
            RUVIA_CHECK(content_type.has_value());
            if (content_type) {
                RUVIA_CHECK_EQ(*content_type, value);
            }
            RUVIA_CHECK(parsed.state_.request_.header("X-After") == std::string_view("kept"));
            RUVIA_CHECK_EQ(parsed.body_, "abc");
            RUVIA_CHECK_EQ(std::string_view(wire).substr(*parsed.message_bytes_), "NEXT");
        }
    }
}

RUVIA_TEST(http1_parse_content_encoding_accepts_empty_list_members) {
    const auto accepted = parse_http1_request_message(
        "POST /items HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Encoding: , gzip,,\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(accepted.complete());
}

RUVIA_TEST(http1_parse_coding_metadata_preserves_body_and_rejects_decoder_fallback) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate", "future-coding"}) {
        for (const bool chunked : {false, true}) {
            std::string wire = "POST /items HTTP/1.1\r\nHost: example.com\r\nContent-Encoding: ";
            wire.append(value);
            wire.append("\r\nContent-Encoding: identity\r\n");
            wire.append(chunked ? "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\nNEXT"
                                : "Content-Length: 3\r\n\r\nabcNEXT");
            const auto parsed = parse_http1_request_message(wire);
            RUVIA_CHECK(parsed.complete());
            if (!parsed.complete()) {
                continue;
            }
            RUVIA_CHECK_EQ(parsed.body_, "abc");
            RUVIA_CHECK_EQ(std::string_view(wire).substr(*parsed.message_bytes_), "NEXT");
            const auto coding = ruvia::request_content_coding(
                parsed.state_.request_, std::pmr::get_default_resource());
            const bool unsupported = value == "future-coding";
            RUVIA_CHECK((coding.unsupported() != nullptr) == unsupported);
            RUVIA_CHECK((coding.invalid() != nullptr) == !unsupported);
            if (const auto* error = coding.invalid()) {
                RUVIA_CHECK(error->status() == ruvia::http_status::bad_request);
            }
            if (const auto* error = coding.unsupported()) {
                RUVIA_CHECK(error->status() == ruvia::http_status::unsupported_media_type);
            }
        }
    }
}

RUVIA_TEST(http1_parse_authority_uses_shared_uri_normalization) {
    // RFC 9110 section 4.2.3: an empty port is the scheme default, host is
    // case-insensitive, and percent-encoded unreserved octets normalize.
    const auto empty_port = parse_http1_request_message(
        "GET http://EXAMPLE.com:/path HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n");
    RUVIA_CHECK(empty_port.complete());

    const auto encoded_host = parse_http1_request_message(
        "GET http://exa%6Dple.com/path HTTP/1.1\r\n"
        "Host: example.com:\r\n\r\n");
    RUVIA_CHECK(encoded_host.complete());

    const auto future_literal = parse_http1_request_message(
        "GET http://[v1.future]/path HTTP/1.1\r\n"
        "Host: [V1.FUTURE]:\r\n\r\n");
    RUVIA_CHECK(future_literal.complete());
}

RUVIA_TEST(http1_parse_connect_requires_authority_form) {
    {
        const auto result_value = parse_http1_request_message(
            "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n");
        RUVIA_CHECK(result_value.complete());
        RUVIA_CHECK_EQ(result_value.state_.request_.method(), std::string_view("CONNECT"));
        RUVIA_CHECK(result_value.state_.request_.known_method() == http_known_method::connect);
        RUVIA_CHECK_EQ(result_value.state_.request_.target(), std::string_view("example.com:443"));
        RUVIA_CHECK(result_value.state_.request_.scheme().empty());
        RUVIA_CHECK_EQ(result_value.state_.request_.authority(), std::string_view("example.com:443"));
        RUVIA_CHECK(result_value.state_.request_.target_form() == http_request_target_form::authority);
        RUVIA_CHECK_EQ(result_value.state_.request_.path(), std::string_view("example.com:443"));
    }
    {
        const auto result_value = parse_http1_request_message("CONNECT / HTTP/1.1\r\nHost: example.com\r\n\r\n");
        RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_request_target));
    }
    {
        const auto result_value = parse_http1_request_message(
            "CONNECT http://example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n");
        RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_request_target));
    }
}

RUVIA_TEST(http1_parse_connect_uses_target_authority) {
    const auto result_value = parse_http1_request_message(
        "CONNECT tunnel.example:443 HTTP/1.1\r\n"
        "Host: decoy.example\r\n\r\n");

    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(result_value.state_.request_.header("Host").value_or(std::string_view{}),
        std::string_view("tunnel.example:443"));
    for (const auto& header : result_value.state_.request_.headers()) {
        if (header.name() == "Host") {
            RUVIA_CHECK_EQ(header.value(), std::string_view("tunnel.example:443"));
        }
    }
}

RUVIA_TEST(http1_parse_methods_without_content_accept_zero_length_only) {
    const auto rejects = [&](std::string_view request, http_parse_error error) {
        const auto result_value = parse_http1_request_message(request);
        RUVIA_CHECK(is_failure(result_value.state_, error));
    };

    // RFC 9110 sections 9.3.6 and 9.3.8 prohibit content, not zero-length
    // framing. Keep rejecting nonzero and unknown-length content.
    for (const auto wire : {
             "TRACE /diagnostic HTTP/1.1\r\nHost: example.test\r\nContent-Length: 0\r\n\r\n",
             "CONNECT tunnel.example:443 HTTP/1.1\r\nHost: tunnel.example:443\r\nContent-Length: 0\r\n\r\n",
         }) {
        const auto accepted = parse_http1_request_message(wire);
        RUVIA_CHECK(accepted.complete());
        RUVIA_CHECK_EQ(require_known_length(accepted.state_.body_plan_).content_length(), std::size_t{0});
        RUVIA_CHECK(accepted.body_.empty());
        RUVIA_CHECK(!accepted.state_.body_plan_.requires_consumption());
        RUVIA_CHECK(accepted.state_.connection_plan_.disposition() == http1_close_policy::allow_reuse);
        RUVIA_CHECK_EQ(accepted.state_.connection_plan_.tunnel_request(),
            accepted.state_.request_.known_method() == http_known_method::connect);
        std::string with_following_bytes(wire);
        with_following_bytes.append("opaque");
        const auto boundary = parse_http1_request_message(with_following_bytes);
        RUVIA_CHECK(boundary.complete());
        if (boundary.complete()) {
            RUVIA_CHECK_EQ(*boundary.message_bytes_, std::string_view(wire).size());
            RUVIA_CHECK_EQ(boundary.state_.head_ready()->header_bytes(), std::string_view(wire).size());
        }
    }
    rejects(
        "CONNECT tunnel.example:443 HTTP/1.1\r\n"
        "Host: tunnel.example:443\r\nContent-Length: 1\r\n\r\nx",
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
    const auto extension = parse_http1_request_message(
        "trace /diagnostic HTTP/1.1\r\n"
        "Host: example.test\r\nContent-Length: 4\r\n\r\nbody");
    RUVIA_CHECK(extension.complete());
    RUVIA_CHECK_EQ(require_known_length(extension.state_.body_plan_).content_length(), std::size_t{4});

    // Unframed bytes following a CONNECT head are not request content. The
    // whole-message parser returns the exact head boundary so a tunnel owner
    // can retain those bytes for its post-response state transition.
    constexpr std::string_view connect_head =
        "CONNECT tunnel.example:443 HTTP/1.1\r\n"
        "Host: tunnel.example:443\r\n\r\n";
    std::string early_tunnel_bytes(connect_head);
    early_tunnel_bytes += "opaque";
    const auto parsed_value = parse_http1_request_message(early_tunnel_bytes);
    RUVIA_CHECK(parsed_value.complete());
    if (parsed_value.complete()) {
        RUVIA_CHECK(parsed_value.state_.body_plan_.without_body() != nullptr);
        RUVIA_CHECK_EQ(parsed_value.state_.head_ready()->header_bytes(), connect_head.size());
        RUVIA_CHECK_EQ(*parsed_value.message_bytes_, connect_head.size());
    }
}

RUVIA_TEST(http1_parse_http10_without_host_allowed) {
    // HTTP/1.0 does not require a Host header.
    const auto result_value = parse_http1_request_message("GET / HTTP/1.0\r\n\r\n");
    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK_EQ(result_value.state_.request_.method(), std::string_view("GET"));
    RUVIA_CHECK(result_value.state_.request_.known_method() == http_known_method::get);
    RUVIA_CHECK(result_value.state_.request_.protocol_version() == http_protocol_version::http10);
}

RUVIA_TEST(http1_parse_http10_ignores_upgrade_field_semantics) {
    const auto result_value = parse_http1_request_message(
        "GET / HTTP/1.0\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket/\r\n\r\n");

    RUVIA_CHECK(result_value.complete());
    RUVIA_CHECK(result_value.state_.request_.protocol_version() == http_protocol_version::http10);
    RUVIA_CHECK_EQ(result_value.state_.request_.header("Upgrade"), std::optional<std::string_view>("websocket/"));
}

RUVIA_TEST(http1_parse_non_numeric_content_length_rejected) {
    const auto result_value =
        parse_http1_request_message("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_content_length));
}

RUVIA_TEST(http1_parse_too_many_headers_rejected) {
    std::string request = "GET / HTTP/1.1\r\nHost: x\r\n";
    for (int i = 0; i < 70; ++i) {  // exceeds max_http_header_fields (64)
        request += "x-h-" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";
    const auto result_value = parse_http1_request_message(request);
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::too_many_headers));
}

RUVIA_TEST(http1_parse_chunk_size_overflow_rejected) {
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "10000000000000000\r\nx\r\n0\r\n\r\n");  // 2^64 chunk size overflows
    RUVIA_CHECK(result_value.state_.head_ready() != nullptr);
    RUVIA_CHECK(!result_value.complete());
    RUVIA_CHECK(result_value.chunk_error_ == http1_chunk_decode_error::invalid_framing);
}

RUVIA_TEST(http1_parse_invalid_chunk_size_rejected) {
    const auto result_value = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nx\r\n0\r\n\r\n");
    RUVIA_CHECK(result_value.state_.head_ready() != nullptr);
    RUVIA_CHECK(!result_value.complete());
    RUVIA_CHECK(result_value.chunk_error_ == http1_chunk_decode_error::invalid_framing);
}

RUVIA_TEST(http1_server_head_ready_is_distinct_from_message_ready) {
    http1_server_request_parser parser;
    http1_server_request_parse_state head;
    constexpr std::string_view request =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";

    parser.parse_head(request, head);
    const auto* request_head = head.head_ready();
    RUVIA_CHECK(request_head != nullptr);
    if (request_head != nullptr) {
        RUVIA_CHECK_EQ(
            request_head->header_bytes(), request.size() - std::string_view("hello").size());
    }
    RUVIA_CHECK_EQ(head.request_.method(), std::string_view("POST"));
    RUVIA_CHECK(head.request_.known_method() == http_known_method::post);

    const auto message = parse_http1_request_message(request);
    RUVIA_CHECK(message.complete());
    if (message.complete()) {
        RUVIA_CHECK_EQ(*message.message_bytes_, request.size());
        RUVIA_CHECK_EQ(message.body_, std::string_view("hello"));
    }

    const auto body_pending = parse_http1_request_message(request.substr(0, request.size() - 1));
    RUVIA_CHECK(body_pending.need_body());
    RUVIA_CHECK(!body_pending.complete());
    const auto* pending_head = body_pending.state_.head_ready();
    const auto* pending_length = body_pending.state_.body_plan_.known_length();
    RUVIA_CHECK(pending_head != nullptr);
    RUVIA_CHECK(pending_length != nullptr);
    if (pending_head != nullptr && pending_length != nullptr) {
        RUVIA_CHECK_EQ(pending_head->header_bytes() + pending_length->content_length(), request.size());
    }
}

RUVIA_TEST(http1_parse_ignores_bounded_empty_lines_before_request_line) {
    // RFC 9112 section 2.2: a server SHOULD ignore at least one empty line
    // received before the request-line.
    http1_server_request_parser parser;
    for (const std::string_view request : {
             std::string_view("\r\nGET /a HTTP/1.1\r\nHost: x\r\n\r\n"),
             std::string_view("\r\n\r\n\nGET /a HTTP/1.1\r\nHost: x\r\n\r\n"),
         }) {
        const auto result_value = parse_http1_request_message(request);
        RUVIA_CHECK(result_value.complete());
        if (result_value.complete()) {
            RUVIA_CHECK_EQ(*result_value.message_bytes_, request.size());
        }
        RUVIA_CHECK_EQ(result_value.state_.request_.method(), std::string_view("GET"));
        RUVIA_CHECK_EQ(result_value.state_.request_.path(), std::string_view("/a"));
    }

    for (const std::string_view partial : {std::string_view("\r\n"), std::string_view("\r\n\r"),
             std::string_view("\r\n\r\n"), std::string_view("\r\nGET / HTTP/1.1\r\n")}) {
        http1_server_request_parse_state head;
        parser.parse_head(partial, head);
        RUVIA_CHECK(head.need_request_head() != nullptr);
    }

    // A bare CR is not an empty line.
    RUVIA_CHECK(is_failure(
        parse_http1_request_message("\rGET / HTTP/1.1\r\nHost: x\r\n\r\n").state_, http_parse_error::invalid_request_line));

    // Ignored lines count toward the head limit.
    std::string flood;
    while (flood.size() < ruvia::max_http_header_bytes) {
        flood.append("\r\n");
    }
    flood.append("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(is_failure(parse_http1_request_message(flood).state_, http_parse_error::header_too_large));
}

RUVIA_TEST(http1_parse_message_ignores_empty_line_after_pipelined_body) {
    constexpr std::string_view first_message =
        "POST /first HTTP/1.1\r\nHost: example.com\r\n"
        "Content-Length: 5\r\n\r\nhello";
    constexpr std::string_view second_message = "\r\nGET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const std::string pipeline = std::string(first_message) + std::string(second_message);

    const auto first_result = parse_http1_request_message(pipeline);
    RUVIA_CHECK(first_result.complete());
    if (!first_result.complete()) {
        return;
    }
    const auto first_header_bytes = first_result.state_.head_ready()->header_bytes();
    const auto first_message_bytes = *first_result.message_bytes_;
    RUVIA_CHECK_EQ(std::string_view(pipeline).substr(first_header_bytes, first_message_bytes - first_header_bytes),
        std::string_view("hello"));
    RUVIA_CHECK_EQ(first_message_bytes, first_message.size());

    const auto second_result = parse_http1_request_message(std::string_view(pipeline).substr(first_message_bytes));
    RUVIA_CHECK(second_result.complete());
    if (second_result.complete()) {
        RUVIA_CHECK_EQ(second_result.state_.request_.path(), std::string_view("/next"));
        RUVIA_CHECK_EQ(*second_result.message_bytes_, second_message.size());
    }
}

RUVIA_TEST(http1_response_coding_folds_all_accept_encoding_field_lines) {
    const auto parsed_value = parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0.2\r\n"
        "Accept-Encoding: br;q=0.8\r\n\r\n");
    RUVIA_CHECK(parsed_value.complete());
    const auto response_coding = parsed_value.state_.response_coding_selection();
    RUVIA_CHECK(response_coding.has_value());
    if (response_coding.has_value()) {
        RUVIA_CHECK(response_coding->coding() == ruvia::http_content_coding::brotli);
    }
}

RUVIA_TEST(http1_response_coding_rejects_when_every_coding_is_forbidden) {
    const auto parsed_value = parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0, br;q=0, zstd;q=0\r\n\r\n");
    RUVIA_CHECK(parsed_value.complete());
    const auto response_coding = parsed_value.state_.response_coding_selection();
    RUVIA_CHECK(!response_coding.has_value());
}

RUVIA_TEST(http1_parse_header_block_too_large_rejected) {
    // A header section that reaches the byte cap without terminating is a DoS
    // guard (max_http_header_bytes is 64 KiB).
    std::string request = "GET / HTTP/1.1\r\nX-Big: ";
    request += std::string(64 * 1024, 'a');  // pushes past the cap, no blank line
    const auto result_value = parse_http1_request_message(request);
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::header_too_large));
}

RUVIA_TEST(http1_parse_request_target_reaching_head_cap_is_uri_too_long) {
    // The cap hit inside the request-target is 414, not 431 (RFC 9112 section 3).
    std::string request = "\r\nGET /";
    request += std::string(ruvia::max_http_header_bytes, 'a');
    RUVIA_CHECK(is_failure(parse_http1_request_message(request).state_, http_parse_error::uri_too_long));
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::uri_too_long).status(), ruvia::http_status::uri_too_long);

    // A long method token or a capped request-line version stays a head-size failure.
    RUVIA_CHECK(is_failure(parse_http1_request_message(std::string(ruvia::max_http_header_bytes, 'G')).state_,
        http_parse_error::header_too_large));
    RUVIA_CHECK(is_failure(
        parse_http1_request_message("GET / " + std::string(ruvia::max_http_header_bytes, 'H')).state_,
        http_parse_error::header_too_large));
}

RUVIA_TEST(http1_parse_invalid_request_target_rejected) {
    // An origin-form target must begin with '/'; a bare word is not a valid
    // request target for GET.
    const auto result_value = parse_http1_request_message("GET foobar HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(is_failure(result_value.state_, http_parse_error::invalid_request_target));
}

RUVIA_TEST(http1_parse_te_trailers_tolerates_ows) {
    for (const std::string_view value : {"   trailers  ", "trailers , TRAILERS"}) {
        std::string wire = "GET / HTTP/1.1\r\nHost: x\r\nConnection: TE\r\nTE:";
        wire.append(value);
        wire.append("\r\n\r\n");
        const auto parsed = parse_http1_request_message(wire);
        RUVIA_CHECK(parsed.complete());
    }
}

RUVIA_TEST(http1_parse_cors_metadata_preserves_body_and_pipeline_boundary) {
    for (const auto& [name, value] : {
             std::pair<std::string_view, std::string_view>{"Origin", "https://app.example/path"},
             {"Access-Control-Request-Method", "POST GET"},
             {"Access-Control-Request-Headers", "x good, bad name"}}) {
        std::string wire = "POST /first HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n";
        wire.append(name).append(": ").append(value).append("\r\n\r\nabc");
        const auto message_bytes = wire.size();
        wire.append("GET /next HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto first = parse_http1_request_message(wire);
        RUVIA_CHECK(first.complete());
        if (!first.complete()) {
            continue;
        }
        RUVIA_CHECK_EQ(first.state_.request_.header(name).value_or(""), value);
        RUVIA_CHECK_EQ(first.body_, "abc");
        RUVIA_CHECK_EQ(*first.message_bytes_, message_bytes);
        const auto next = parse_http1_request_message(std::string_view(wire).substr(*first.message_bytes_));
        RUVIA_CHECK(next.complete());
        RUVIA_CHECK_EQ(next.state_.request_.path(), "/next");
    }
}

RUVIA_TEST(http1_parse_head_resumes_leading_empty_lines_and_request_line_across_segments) {
    http1_server_request_parser parser;
    constexpr std::string_view request = "\r\n\n\r\nGET /seg HTTP/1.1\r\nHost: x\r\n\r\n";
    for (const std::size_t segment : {std::size_t{1}, std::size_t{2}, std::size_t{5}}) {
        http1_server_request_parse_state state;
        std::size_t search_offset = 0;
        std::size_t delivered = 0;
        while (delivered < request.size()) {
            delivered = std::min(request.size(), delivered + segment);
            parser.parse_head(request.substr(0, delivered), state, search_offset);
            RUVIA_CHECK(state.failure() == nullptr);
            if (delivered < request.size()) {
                RUVIA_CHECK(state.need_request_head() != nullptr);
            }
            search_offset = delivered;
        }
        const auto* head = state.head_ready();
        RUVIA_CHECK(head != nullptr);
        if (head != nullptr) {
            RUVIA_CHECK_EQ(head->header_bytes(), request.size());
        }
        RUVIA_CHECK_EQ(state.request_.path(), std::string_view("/seg"));
    }
}

RUVIA_TEST(http1_parse_head_classifies_request_line_before_the_head_completes) {
    http1_server_request_parser parser;
    // TLS ClientHello prefix and other non-token first bytes fail at once.
    for (const std::string_view wire : {std::string_view("\x16\x03\x01\x00\xa5\x01", 6), std::string_view("\r\rGET"),
             std::string_view("\nGET / NOT-HTTP\r\nHost: x\r\n"), std::string_view("GET / HTTP/1.1 extra\r\n")}) {
        http1_server_request_parse_state state;
        parser.parse_head(wire, state);
        const auto* failure = state.failure();
        RUVIA_CHECK(failure != nullptr);
        if (failure != nullptr) {
            RUVIA_CHECK(failure->source() == http1_server_request_parse_failure_source::request_line);
        }
    }
    RUVIA_CHECK(ruvia::should_drop_invalid_cleartext_http1_input(
        std::string_view("\x16\x03\x01\x00\xa5\x01", 6), http1_server_request_parse_failure_source::request_line));
    RUVIA_CHECK(ruvia::should_drop_invalid_cleartext_http1_input(
        "\r\nGET / NOT-HTTP\r\nHost: x\r\n", http1_server_request_parse_failure_source::request_line));
    RUVIA_CHECK(!ruvia::should_drop_invalid_cleartext_http1_input(
        "GET / HTTP/9.0\r\n", http1_server_request_parse_failure_source::request_line));
    // A line naming an HTTP/ version anywhere is HTTP and gets a 400; a line
    // without one, separator or not, is not.
    RUVIA_CHECK(!ruvia::should_drop_invalid_cleartext_http1_input(
        "GET / HTTP/1.1 extra\r\n", http1_server_request_parse_failure_source::request_line));
    RUVIA_CHECK(ruvia::should_drop_invalid_cleartext_http1_input(
        "SSH-2.0-OpenSSH_9.6\r\n", http1_server_request_parse_failure_source::request_line));
    // A bare LF inside the request-line is invalid, but the HTTP/ version after
    // it still identifies an HTTP peer.
    RUVIA_CHECK(!ruvia::should_drop_invalid_cleartext_http1_input(
        "GET /\nHTTP/1.1\r\nHost: x\r\n\r\n", http1_server_request_parse_failure_source::request_line));

    {
        http1_server_request_parse_state state;
        parser.parse_head("GET / HTTP/9.0\r\nHost: x\r\n", state);
        RUVIA_CHECK(is_failure(state, http_parse_error::unsupported_http_version));
    }
    // A valid request-line waits for the rest of the head; an incomplete line
    // is not classified yet.
    for (const std::string_view partial : {std::string_view("G"), std::string_view("GET / HTTP/1.1"),
             std::string_view("GET / HTTP/1.1\r"), std::string_view("GET / HTTP/1.1\r\nHost: x\r\n"),
             std::string_view("\r\n\r")}) {
        http1_server_request_parse_state state;
        parser.parse_head(partial, state);
        RUVIA_CHECK(state.need_request_head() != nullptr);
    }
}
