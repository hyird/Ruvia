#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "failing_memory_resource.h"
#include "http_client_response_fixture.h"
#include "stl_allocation_fixture.h"

// HTTP/1 client responses: reading a response head off the wire.

namespace {

constexpr std::string_view retained_head = "HTTP/1.1 200 OK\r\nX-Test: kept\r\nContent-Length: 0";
constexpr std::string_view replacement_head = "HTTP/1.1 200 OK\r\nX-Test: replacement\r\nContent-Length: 0";
constexpr std::size_t repeated_field_count = 160;

std::string repeated_response_wire(std::string_view status_line, std::string_view trailing_fields) {
    std::string wire(status_line);
    wire.append("\r\n");
    for (std::size_t index = 0; index < repeated_field_count; ++index) {
        wire.append("X-Repeat: value-");
        wire.append(std::to_string(index));
        wire.append("\r\n");
    }
    wire.append(trailing_fields);
    wire.append("\r\n");
    return wire;
}

void check_repeated_response_fields(ruvia::testing::test_context& ruvia_ctx,
    const http_client_response_head& head, std::size_t expected_count) {
    const auto fields = head.headers();
    RUVIA_CHECK_EQ(fields.size(), expected_count);
    if (fields.size() < repeated_field_count) {
        return;
    }
    for (std::size_t index = 0; index < repeated_field_count; ++index) {
        RUVIA_CHECK_EQ(fields[index].name(), std::string_view("X-Repeat"));
        RUVIA_CHECK_EQ(fields[index].value(), "value-" + std::to_string(index));
    }
}

void check_many_field_response(ruvia::testing::test_context& ruvia_ctx,
    const http1_client_response_parse_result& result, std::size_t consumed_bytes,
    bool informational) {
    const auto* parsed = result.parsed();
    RUVIA_CHECK(parsed != nullptr);
    if (parsed == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->consumed_bytes(), consumed_bytes);
    RUVIA_CHECK(parsed->head().protocol_version() == http_protocol_version::http11);
    check_repeated_response_fields(ruvia_ctx, parsed->head(),
        repeated_field_count + (informational ? 0 : 1));
    if (informational) {
        RUVIA_CHECK(parsed->head().status() == ruvia::http_status::continue_value);
        RUVIA_CHECK(parsed->plan().informational() != nullptr);
        RUVIA_CHECK(parsed->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
        return;
    }
    RUVIA_CHECK(parsed->head().status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{3});
    RUVIA_CHECK(require_known_length(parsed->plan()).persistence() == http1_close_policy::allow_reuse);
    RUVIA_CHECK(!parsed->plan().request_content_signal());
    const auto fields = parsed->head().headers();
    if (fields.size() == repeated_field_count + 1) {
        RUVIA_CHECK_EQ(fields.back().name(), std::string_view("Content-Length"));
        RUVIA_CHECK_EQ(fields.back().value(), std::string_view("3"));
    }
}

void check_response_head(ruvia::testing::test_context& ruvia_ctx,
    const http_client_response_head& head, std::string_view value) {
    RUVIA_CHECK(head.status() == ruvia::http_status::ok);
    RUVIA_CHECK(head.protocol_version() == ruvia::http_protocol_version::http11);
    const auto fields = head.headers();
    RUVIA_CHECK_EQ(fields.size(), std::size_t{2});
    if (fields.size() == 2) {
        RUVIA_CHECK_EQ(fields[0].name(), std::string_view("X-Test"));
        RUVIA_CHECK_EQ(fields[0].value(), value);
        RUVIA_CHECK_EQ(fields[1].name(), std::string_view("Content-Length"));
        RUVIA_CHECK_EQ(fields[1].value(), std::string_view("0"));
    }
}

void check_empty_response_head(ruvia::testing::test_context& ruvia_ctx,
    http_client_response_head& head, std::pmr::memory_resource* resource) {
    RUVIA_CHECK(head.headers().empty());
    RUVIA_CHECK(head.status() == ruvia::http_status::ok);
    RUVIA_CHECK(head.protocol_version() == ruvia::http_protocol_version::http11);
    for (int repeat = 0; repeat < 2; ++repeat) {
        const auto fields = std::move(head).take_headers();
        RUVIA_CHECK(fields.empty());
        RUVIA_CHECK(fields.get_allocator().resource() == resource);
        RUVIA_CHECK(head.headers().empty());
    }
}

}  // namespace

RUVIA_TEST(http_client_fieldless_exchange_completes_with_no_response_storage) {
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
    auto* resource = ruvia::test::stl_moves_allocate_debug_metadata
                         ? std::pmr::get_default_resource()
                         : std::pmr::null_memory_resource();
    http1_client_response_parser parser(prepared_req->exchange_state(), resource);
    constexpr std::array<std::string_view, 3> heads{
        "HTTP/1.1 103 Early Hints\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\n",
        "HTTP/1.1 204 No Content\r\n\r\n"};
    constexpr std::array<std::uint16_t, 3> statuses{103, 100, 204};
    for (std::size_t index = 0; index < heads.size(); ++index) {
        const auto result = parser.parse(heads[index]);
        const auto* parsed = result.parsed();
        RUVIA_CHECK(parsed != nullptr);
        if (parsed == nullptr) {
            return;
        }
        RUVIA_CHECK_EQ(parsed->head().status().value(), statuses[index]);
        RUVIA_CHECK(parsed->head().headers().empty());
        RUVIA_CHECK_EQ(parsed->consumed_bytes(), heads[index].size());
        RUVIA_CHECK((parsed->plan().informational() != nullptr) == (index < 2));
        if (index == 1) {
            RUVIA_CHECK(parsed->plan().request_content_signal() ==
                        http_client_request_content_signal::continue_value);
            RUVIA_CHECK(parser.complete_request_content() ==
                        http1_client_request_content_completion_status::completed);
        } else {
            RUVIA_CHECK(!parsed->plan().request_content_signal());
        }
    }
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)parser.parse({}); }));
}

RUVIA_TEST(http_client_take_head_leaves_empty_parsed_fields_and_retains_owned_headers) {
    failing_memory_resource memory;
    {
        auto result = parse_result("GET", retained_head, ruvia::http1_close_policy::allow_reuse, {}, &memory);
        auto* parsed = result.parsed();
        RUVIA_CHECK(parsed != nullptr);
        if (parsed == nullptr) {
            return;
        }
        if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
            memory.fail_after(0);
        }
        auto head = std::move(*parsed).take_head();
        memory.allow_allocations();
        check_response_head(ruvia_ctx, head, "kept");
        RUVIA_CHECK(parsed->head().headers().empty());
        RUVIA_CHECK(parsed->head().status() == ruvia::http_status::ok);
        for (int repeat = 0; repeat < 2; ++repeat) {
            auto empty_head = std::move(*parsed).take_head();
            check_empty_response_head(ruvia_ctx, empty_head, &memory);
            RUVIA_CHECK(parsed->head().headers().empty());
        }
        auto fields = std::move(head).take_headers();
        RUVIA_CHECK_EQ(fields.size(), std::size_t{2});
        RUVIA_CHECK(fields.get_allocator().resource() == &memory);
        check_empty_response_head(ruvia_ctx, head, &memory);
        if (fields.size() == 2) {
            RUVIA_CHECK_EQ(fields[0].value(), std::string_view("kept"));
        }
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http_client_response_result_move_preserves_fields_and_supports_source_replacement) {
    failing_memory_resource memory;
    {
        std::optional<http1_client_response_parse_result> source(
            parse_result("GET", retained_head, ruvia::http1_close_policy::allow_reuse, {}, &memory));
        if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
            memory.fail_after(0);
        }
        auto retained = std::move(*source);
        memory.allow_allocations();
        RUVIA_CHECK(retained.parsed() != nullptr);
        RUVIA_CHECK(source->parsed() != nullptr);
        if (retained.parsed() == nullptr || source->parsed() == nullptr) {
            return;
        }
        check_response_head(ruvia_ctx, retained.parsed()->head(), "kept");
        RUVIA_CHECK(source->parsed()->head().headers().empty());
        auto empty_head = std::move(*source->parsed()).take_head();
        check_empty_response_head(ruvia_ctx, empty_head, &memory);
        source.emplace(parse_result("GET", replacement_head, ruvia::http1_close_policy::allow_reuse, {}, &memory));
        RUVIA_CHECK(source->parsed() != nullptr);
        if (source->parsed()) {
            check_response_head(ruvia_ctx, source->parsed()->head(), "replacement");
        }
        check_response_head(ruvia_ctx, retained.parsed()->head(), "kept");
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http_client_response_head_move_preserves_fields_and_supports_source_replacement) {
    failing_memory_resource memory;
    {
        auto result = parse_result("GET", retained_head, ruvia::http1_close_policy::allow_reuse, {}, &memory);
        RUVIA_CHECK(result.parsed() != nullptr);
        if (result.parsed() == nullptr) {
            return;
        }
        std::optional<http_client_response_head> source(std::move(*result.parsed()).take_head());
        if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
            memory.fail_after(0);
        }
        auto retained = std::move(*source);
        memory.allow_allocations();
        check_response_head(ruvia_ctx, retained, "kept");
        check_empty_response_head(ruvia_ctx, *source, &memory);
        auto replacement = parse_result("GET", replacement_head, ruvia::http1_close_policy::allow_reuse, {}, &memory);
        RUVIA_CHECK(replacement.parsed() != nullptr);
        if (replacement.parsed()) {
            source.emplace(std::move(*replacement.parsed()).take_head());
            check_response_head(ruvia_ctx, *source, "replacement");
        }
        check_response_head(ruvia_ctx, retained, "kept");
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http_client_many_response_fields_survive_result_move_and_wire_destruction) {
    failing_memory_resource memory;
    {
        std::optional<http1_client_response_parse_result> retained;
        std::size_t consumed_bytes = 0;
        {
            auto wire = repeated_response_wire("HTTP/1.1 200 OK", "Content-Length: 3\r\n");
            consumed_bytes = wire.size();
            wire.append("abcNEXT");
            auto source = parse_wire("GET", wire, http1_close_policy::allow_reuse, {}, &memory);
            retained.emplace(std::move(source));
            wire.assign(wire.size(), 'x');
        }
        check_many_field_response(ruvia_ctx, *retained, consumed_bytes, false);
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http_client_upgrade_after_many_response_fields_validates_offered_protocol) {
    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto wire = repeated_response_wire("HTTP/1.1 101 Switching Protocols",
        "Connection: Upgrade\r\nUpgrade: irc/6.9\r\n");
    const auto accepted = parse_wire("GET", wire, http1_close_policy::allow_reuse, offered);
    RUVIA_CHECK(accepted.parsed() != nullptr);
    if (const auto* parsed = accepted.parsed()) {
        RUVIA_CHECK(parsed->head().status() == ruvia::http_status::switching_protocols);
        RUVIA_CHECK_EQ(parsed->consumed_bytes(), wire.size());
        RUVIA_CHECK(parsed->plan().protocol_upgrade() != nullptr);
        RUVIA_CHECK_EQ(active_plan_alternative_count(parsed->plan()), std::size_t{1});
        check_repeated_response_fields(ruvia_ctx, parsed->head(), repeated_field_count + 2);
        const auto fields = parsed->head().headers();
        if (fields.size() == repeated_field_count + 2) {
            RUVIA_CHECK_EQ(fields[repeated_field_count].name(), std::string_view("Connection"));
            RUVIA_CHECK_EQ(fields[repeated_field_count].value(), std::string_view("Upgrade"));
            RUVIA_CHECK_EQ(fields.back().name(), std::string_view("Upgrade"));
            RUVIA_CHECK_EQ(fields.back().value(), std::string_view("irc/6.9"));
        }
    }
    const auto unoffered_wire = repeated_response_wire("HTTP/1.1 101 Switching Protocols",
        "Connection: Upgrade\r\nUpgrade: IRC/6.10\r\n");
    const auto unoffered = parse_wire("GET", unoffered_wire, http1_close_policy::allow_reuse, offered);
    RUVIA_CHECK(unoffered.failure() != nullptr);
    if (unoffered.failure() != nullptr) {
        RUVIA_CHECK(*unoffered.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
    const auto unsolicited = parse_wire("GET", wire);
    RUVIA_CHECK(unsolicited.failure() != nullptr);
    if (unsolicited.failure() != nullptr) {
        RUVIA_CHECK(*unsolicited.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
}

RUVIA_TEST(http_client_many_response_fields_retry_every_allocation_failure_without_phase_commit) {
    const std::array<std::string, 2> wires{
        repeated_response_wire("HTTP/1.1 100 Continue", ""),
        repeated_response_wire("HTTP/1.1 200 OK", "Content-Length: 3\r\n")};
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

    counting_memory_resource counting;
    std::array<std::size_t, 2> allocation_counts{};
    {
        http1_client_response_parser parser(prepared_req->exchange_state(), &counting);
        for (std::size_t phase = 0; phase < wires.size(); ++phase) {
            const auto before = counting.allocation_count();
            const auto parsed = parser.parse(wires[phase]);
            check_many_field_response(ruvia_ctx, parsed, wires[phase].size(), phase == 0);
            allocation_counts[phase] = counting.allocation_count() - before;
            RUVIA_CHECK(allocation_counts[phase] > 0);
        }
    }

    for (std::size_t phase = 0; phase < wires.size(); ++phase) {
        for (std::size_t fail_after = 0; fail_after <= allocation_counts[phase]; ++fail_after) {
            if constexpr (ruvia::test::stl_moves_allocate_debug_metadata) {
                if (fail_after != 0 && fail_after != allocation_counts[phase]) {
                    continue;
                }
            }
            failing_memory_resource memory;
            {
                http1_client_response_parser parser(prepared_req->exchange_state(), &memory);
                if (phase == 1) {
                    const auto interim = parser.parse(wires[0]);
                    check_many_field_response(ruvia_ctx, interim, wires[0].size(), true);
                }
                RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
                memory.fail_after(fail_after);
                bool allocation_failed = false;
                try {
                    const auto parsed = parser.parse(wires[phase]);
                    check_many_field_response(ruvia_ctx, parsed, wires[phase].size(), phase == 0);
                } catch (const std::bad_alloc&) {
                    allocation_failed = true;
                }
                memory.allow_allocations();
                RUVIA_CHECK(allocation_failed == (fail_after < allocation_counts[phase]));
                RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
                if (allocation_failed) {
                    const auto retried = parser.parse(wires[phase]);
                    check_many_field_response(ruvia_ctx, retried, wires[phase].size(), phase == 0);
                }
                RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
                if (phase == 0) {
                    {
                        const auto repeated = parser.parse(wires[0]);
                        RUVIA_CHECK(repeated.parsed() != nullptr);
                        if (const auto* parsed = repeated.parsed()) {
                            RUVIA_CHECK(parsed->plan().informational() != nullptr);
                            RUVIA_CHECK(!parsed->plan().request_content_signal());
                            check_repeated_response_fields(ruvia_ctx, parsed->head(), repeated_field_count);
                        }
                    }
                    const auto final_response = parser.parse(wires[1]);
                    check_many_field_response(ruvia_ctx, final_response, wires[1].size(), false);
                }
                RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
                RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)parser.parse(wires[1]); }));
            }
            RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        }
    }
}

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
                                  static_cast<unsigned int>(result_value.failure() != nullptr);
        RUVIA_CHECK_EQ(alternatives, 1U);
        if (const auto* parsed = result_value.parsed()) {
            RUVIA_CHECK(parsed->consumed_bytes() <= input.size());
            RUVIA_CHECK(parsed->consumed_bytes() >= 4U);
            RUVIA_CHECK_EQ(active_plan_alternative_count(parsed->plan()), std::size_t{1});
        }
        if (result_value.need_more() != nullptr) {
            RUVIA_CHECK(input.size() < ruvia::max_http_header_bytes);
        }
    }
}

RUVIA_TEST(http_client_rejects_malformed_status_and_length_fields) {
    const auto upper_boundary =
        parse_head("GET", "HTTP/1.1 599 Extension Status\r\nContent-Length: 0");
    RUVIA_CHECK_EQ(upper_boundary.head().status(), ruvia::http_status_code::from_value(599));
    RUVIA_CHECK(parse_failure_error("GET", "HTTP/2 200 OK") ==
                http1_client_response_parse_error::unsupported_http_version);
    RUVIA_CHECK(parse_failure_error("GET", "HTTP/2.0 200 OK") ==
                http1_client_response_parse_error::unsupported_http_version);
    // RFC 9110 6.2: a higher HTTP/1 minor version is processed as HTTP/1.1.
    const auto higher_minor = parse_head("GET", "HTTP/1.2 200 OK\r\nContent-Length: 0");
    RUVIA_CHECK(higher_minor.head().protocol_version() == http_protocol_version::http11);
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

RUVIA_TEST(http_client_preserves_repeated_content_type_without_selecting_media_type) {
    const auto repeated = parse_head("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 0");
    RUVIA_CHECK_EQ(repeated.head().headers().size(), std::size_t{3});
    RUVIA_CHECK_EQ(repeated.head().headers()[0].value(), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(repeated.head().headers()[1].value(), std::string_view("application/json"));
    RUVIA_CHECK_EQ(require_known_length(repeated.plan()).content_length(), std::size_t{0});

    const auto valid = parse_response("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: 0");
    RUVIA_CHECK_EQ(valid.head_.headers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(
        valid.head_.headers().front().value(), std::string_view("application/json; charset=utf-8"));
}

RUVIA_TEST(http_client_opaque_content_type_owns_metadata_without_changing_body_boundary) {
    for (const std::string_view value : {"", "not a media type", "text/plain; charset=\"unfinished"}) {
        std::string wire = "HTTP/1.1 200 OK\r\nContent-Type: ";
        wire.append(value);
        wire.append("\r\nContent-Length: 3\r\n\r\nabcNEXT");
        const auto result = parse_wire("GET", wire);
        const auto* parsed = result.parsed();
        RUVIA_CHECK(parsed != nullptr);
        if (parsed == nullptr) {
            continue;
        }
        RUVIA_CHECK_EQ(parsed->head().headers().front().value(), value);
        const auto length = require_known_length(parsed->plan()).content_length();
        RUVIA_CHECK_EQ(length, std::size_t{3});
        RUVIA_CHECK_EQ(std::string_view(wire).substr(parsed->consumed_bytes(), length), "abc");
        RUVIA_CHECK_EQ(std::string_view(wire).substr(parsed->consumed_bytes() + length), "NEXT");
        wire.assign(wire.size(), 'x');
        RUVIA_CHECK_EQ(parsed->head().headers().front().value(), value);
    }
}

RUVIA_TEST(http_client_ignores_request_only_te_in_final_response_head) {
    const auto parsed = parse_head("GET",
        "HTTP/1.1 200 OK\r\n"
        "TE: trailers\r\n"
        "Content-Length: 0");
    RUVIA_CHECK_EQ(require_known_length(parsed.plan()).content_length(), std::size_t{0});
    RUVIA_CHECK(require_known_length(parsed.plan()).persistence() == http1_close_policy::allow_reuse);
    RUVIA_CHECK_EQ(parsed.head().headers().front().name(), std::string_view("TE"));
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
    const auto* prepared = std::get_if<ruvia::prepared_http1_client_request>(&prepared_result);
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }

    auto failure_parser =
        http1_client_response_parser(prepared->exchange_state(), &counting);
    const auto failure = failure_parser.parse("HTTP/2 200 OK\r\n\r\n");
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(
        *failure.failure() == http1_client_response_parse_error::unsupported_http_version);
    RUVIA_CHECK_EQ(counting.allocation_count(), std::size_t{0});
}

RUVIA_TEST(http_client_response_parser_enforces_the_complete_head_limit) {
    std::string oversized(ruvia::max_http_header_bytes, 'x');
    const auto result_value = parse_wire("GET", oversized);
    RUVIA_CHECK(result_value.failure() != nullptr);
    RUVIA_CHECK(*result_value.failure() == http1_client_response_parse_error::header_too_large);
}

RUVIA_TEST(http_client_response_parser_accepts_incremental_prefixes_and_resets_after_interim) {
    std::array<char, 2048> output{};
    auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), {}, output);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    RUVIA_CHECK(prepared_req != nullptr);
    http1_client_response_parser parser(prepared_req->exchange_state());
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
