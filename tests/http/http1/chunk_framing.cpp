#include <algorithm>
#include <array>
#include <limits>

#include "field_parsing_fixture.h"

// Chunked framing at its limits: quoted extensions, the discriminated scan result, and the trailer
// section bounds.

RUVIA_TEST(chunk_extension_quoted_pair_allows_escaped_htab) {
    using ruvia::detail::scan_http_chunked_body;

    const std::string_view body = "1;note=\"a\\\tb\"\r\nx\r\n0\r\n\r\n";
    const auto result_value = scan_http_chunked_body(body);
    RUVIA_CHECK(result_value.complete() != nullptr);
    RUVIA_CHECK_EQ(result_value.complete()->consumed_bytes(), body.size());
    RUVIA_CHECK(result_value.need_more() == nullptr);
    RUVIA_CHECK(result_value.failure() == nullptr);
}

RUVIA_TEST(chunk_scan_result_is_discriminated) {
    const auto need_more = ruvia::detail::scan_http_chunked_body("1\r\nx");
    RUVIA_CHECK(need_more.need_more() != nullptr);
    RUVIA_CHECK(need_more.complete() == nullptr);
    RUVIA_CHECK(need_more.failure() == nullptr);

    const auto failure = ruvia::detail::scan_http_chunked_body("xyz\r\n");
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(failure.failure()->error() == ruvia::detail::http_chunk_scan_error::invalid_size);
    RUVIA_CHECK(failure.need_more() == nullptr);
    RUVIA_CHECK(failure.complete() == nullptr);
}

RUVIA_TEST(chunk_trailer_section_enforces_field_and_byte_limits) {
    std::string too_many = "0\r\n";
    for (std::size_t i = 0; i <= ruvia::max_http_header_fields; ++i) {
        too_many.append("X-Trace: value\r\n");
    }
    too_many.append("\r\n");
    const auto too_many_result = ruvia::detail::scan_http_chunked_body(too_many);
    RUVIA_CHECK(too_many_result.failure() != nullptr);
    if (const auto* failure = too_many_result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::http_chunk_scan_error::too_large);
    }

    std::string oversized = "0\r\nX-Trace: ";
    oversized.append(ruvia::max_http_header_bytes, 'x');
    oversized.append("\r\n\r\n");
    const auto oversized_result = ruvia::detail::scan_http_chunked_body(oversized);
    RUVIA_CHECK(oversized_result.failure() != nullptr);
    if (const auto* failure = oversized_result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::http_chunk_scan_error::too_large);
    }
}

RUVIA_TEST(chunk_scan_rejects_unterminated_framing_at_the_header_limit) {
    std::string oversized_size_line(ruvia::max_http_header_bytes, '1');
    const auto size_line_result = ruvia::detail::scan_http_chunked_body(oversized_size_line);
    RUVIA_CHECK(size_line_result.failure() != nullptr);
    if (const auto* failure = size_line_result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::http_chunk_scan_error::too_large);
    }

    std::string oversized_terminated_size_line = "1;x=";
    oversized_terminated_size_line.append(ruvia::max_http_header_bytes, 'a');
    oversized_terminated_size_line.append("\r\n");
    const auto terminated_size_line_result =
        ruvia::detail::scan_http_chunked_body(oversized_terminated_size_line);
    RUVIA_CHECK(terminated_size_line_result.failure() != nullptr);
    if (const auto* failure = terminated_size_line_result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::http_chunk_scan_error::too_large);
    }

    std::string oversized_trailers = "0\r\nX-Trace: ";
    oversized_trailers.append(ruvia::max_http_header_bytes, 'x');
    const auto trailer_result = ruvia::detail::scan_http_chunked_body(oversized_trailers);
    RUVIA_CHECK(trailer_result.failure() != nullptr);
    if (const auto* failure = trailer_result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::http_chunk_scan_error::too_large);
    }

    std::string boundary_size_line = "1;x=";
    boundary_size_line.append(ruvia::max_http_header_bytes - boundary_size_line.size() - 2, 'a');
    boundary_size_line.append("\r\nx\r\n0\r\n\r\n");
    const auto boundary_result = ruvia::detail::scan_http_chunked_body(boundary_size_line);
    RUVIA_CHECK(boundary_result.complete() != nullptr);
}

RUVIA_TEST(chunk_scan_preserves_incomplete_prefixes_and_pipeline_boundary) {
    constexpr std::string_view message = "3;ext=yes\r\nabc\r\n2\r\nde\r\n0\r\nX-Trace: ok\r\n\r\n";
    for (std::size_t length = 0; length < message.size(); ++length) {
        const auto result_value = ruvia::detail::scan_http_chunked_body(message.substr(0, length));
        RUVIA_CHECK(result_value.need_more() != nullptr);
    }
    const std::string wire = std::string(message) + "NEXT";
    const auto complete_value = ruvia::detail::scan_http_chunked_body(wire);
    RUVIA_CHECK(complete_value.complete() != nullptr);
    if (const auto* boundary = complete_value.complete()) {
        RUVIA_CHECK_EQ(boundary->consumed_bytes(), message.size());
        RUVIA_CHECK_EQ(std::string_view(wire).substr(boundary->consumed_bytes()), "NEXT");
    }
}

RUVIA_TEST(chunk_scan_reports_detailed_framing_failures) {
    using ruvia::detail::http_chunk_scan_error;
    const std::array cases{
        std::pair{std::string_view("xyz\r\n"), http_chunk_scan_error::invalid_size},
        std::pair{std::string_view("1;=x\r\n"), http_chunk_scan_error::invalid_extension},
        std::pair{std::string_view("1\r\nxXY"), http_chunk_scan_error::invalid_crlf},
        std::pair{std::string_view("0\r\nContent-Length: 1\r\n\r\n"), http_chunk_scan_error::invalid_trailer},
    };
    for (const auto& [wire, expected] : cases) {
        const auto result_value = ruvia::detail::scan_http_chunked_body(wire);
        RUVIA_CHECK(result_value.failure() != nullptr);
        if (const auto* failure = result_value.failure()) {
            RUVIA_CHECK(failure->error() == expected);
        }
    }
    const std::string overflow = std::string(std::numeric_limits<std::size_t>::digits / 4 + 1, 'f') + "\r\n";
    const auto result_value = ruvia::detail::scan_http_chunked_body(overflow);
    RUVIA_CHECK(result_value.failure() != nullptr);
    if (const auto* failure = result_value.failure()) {
        RUVIA_CHECK(failure->error() == http_chunk_scan_error::size_overflow);
    }
}

RUVIA_TEST(chunk_scan_enforces_its_cumulative_framing_budget_at_exact_boundary) {
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        // Large valid extensions exhaust framing without exhausting payload.
        std::string wire;
        std::size_t remaining = ruvia::default_max_buffered_body_bytes - 5 + excess;
        while (remaining != 0) {
            const auto overhead = std::min(remaining, ruvia::max_http_header_bytes + 2);
            wire.append("1;x=");
            wire.append(overhead - 8, 'a');
            wire.append("\r\nx\r\n");
            remaining -= overhead;
        }
        wire.append("0\r\n\r\nNEXT");
        const auto result_value = ruvia::detail::scan_http_chunked_body(wire);
        if (excess == 0) {
            RUVIA_CHECK(result_value.complete() != nullptr);
            if (const auto* boundary = result_value.complete()) {
                RUVIA_CHECK_EQ(std::string_view(wire).substr(boundary->consumed_bytes()), "NEXT");
            }
        } else {
            RUVIA_CHECK(result_value.failure() != nullptr);
            if (const auto* failure = result_value.failure()) {
                RUVIA_CHECK(failure->error() == ruvia::detail::http_chunk_scan_error::too_large);
            }
        }
    }
}
