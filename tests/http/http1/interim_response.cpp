#include <algorithm>
#include <array>
#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http1_interim_response_writer.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_limits.h"

#include "test_harness.h"

namespace {

using ruvia::http1_interim_connection_disposition;
using ruvia::http1_interim_response_prepare_error;
using ruvia::http1_interim_response_writer;
using ruvia::http_header_view;
using ruvia::http_interim_response_head;

[[nodiscard]] bool unchanged(const std::array<char, 64>& buffer, char sentinel) {
    return std::ranges::all_of(buffer, [sentinel](char value) { return value == sentinel; });
}

}  // namespace

RUVIA_TEST(http1_interim_response_writer_emits_exact_typed_head) {
    std::array<char, 64> buffer{};
    const auto result_value = http1_interim_response_writer().prepare(
        http_interim_response_head(ruvia::http_status::continue_value), buffer);
    const auto* const prepared = result_value.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        RUVIA_CHECK_EQ(prepared->head(), std::string_view("HTTP/1.1 100 Continue\r\n\r\n"));
        RUVIA_CHECK_EQ(
            prepared->connection_disposition(), http1_interim_connection_disposition::unchanged);
    }

    const http_header_view hints[] = {
        {"Link", "</style.css>; rel=preload"},
        {"Content-Type", "text/html; charset=utf-8"},
        {"X-Hint", "warm"},
    };
    std::array<char, 256> hints_buffer{};
    const auto hints_result = http1_interim_response_writer().prepare(
        http_interim_response_head(ruvia::http_status::early_hints, hints), hints_buffer);
    const auto* const prepared_hints = hints_result.prepared();
    RUVIA_CHECK(prepared_hints != nullptr);
    if (prepared_hints != nullptr) {
        RUVIA_CHECK_EQ(
            prepared_hints->head(), std::string_view("HTTP/1.1 103 Early Hints\r\n"
                                                     "Link: </style.css>; rel=preload\r\n"
                                                     "Content-Type: text/html; charset=utf-8\r\n"
                                                     "X-Hint: warm\r\n\r\n"));
        RUVIA_CHECK(!(prepared_hints->head().find("Server:") != std::string_view::npos));
        RUVIA_CHECK(!(prepared_hints->head().find("Date:") != std::string_view::npos));
    }
}

RUVIA_TEST(http1_interim_response_writer_preserves_required_status_line_space) {
    std::array<char, 32> buffer{};
    const auto result_value = http1_interim_response_writer().prepare(
        http_interim_response_head(ruvia::http_status_code::from_value(199)), buffer);
    const auto* const prepared = result_value.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        // RFC 9112 section 4 requires the SP after status-code even when the
        // optional reason phrase is absent.
        RUVIA_CHECK_EQ(prepared->head(), std::string_view("HTTP/1.1 199 \r\n\r\n"));
    }
}

RUVIA_TEST(http1_interim_response_writer_closes_after_containing_response) {
    const http_header_view fields_value[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "example/1"},
    };
    std::array<char, 128> buffer{};
    const auto result_value = http1_interim_response_writer().prepare(
        http_interim_response_head(ruvia::http_status::early_hints, fields_value), buffer);
    const auto* const prepared = result_value.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        RUVIA_CHECK_EQ(prepared->connection_disposition(),
            http1_interim_connection_disposition::close_after_interim_response);
        RUVIA_CHECK((prepared->head().find("Upgrade: example/1\r\n") != std::string_view::npos));
    }
}

RUVIA_TEST(http1_interim_response_writer_buffer_too_small_is_transactional) {
    std::array<char, 8> buffer;
    buffer.fill('#');
    const auto result_value = http1_interim_response_writer().prepare(
        http_interim_response_head(ruvia::http_status::continue_value), buffer);
    const auto* const too_small = result_value.buffer_too_small();
    RUVIA_CHECK(too_small != nullptr);
    if (too_small != nullptr) {
        RUVIA_CHECK_EQ(too_small->required_head_bytes(), std::size_t{25});
    }
    RUVIA_CHECK(std::ranges::all_of(buffer, [](char value) { return value == '#'; }));
}

RUVIA_TEST(http1_interim_response_writer_rejects_invalid_fields_transactionally) {
    const auto rejects = [&](std::span<const http_header_view> fields_value,
                             http1_interim_response_prepare_error expected) {
        std::array<char, 64> buffer;
        buffer.fill('@');
        const auto result_value = http1_interim_response_writer().prepare(
            http_interim_response_head(ruvia::http_status::early_hints, fields_value), buffer);
        const auto* const failure = result_value.failure();
        return failure != nullptr && failure->error() == expected && unchanged(buffer, '@');
    };

    const http_header_view malformed_name[] = {{"Bad Name", "value"}};
    const http_header_view malformed_value[] = {{"X-Test", "a\r\nb"}};
    const http_header_view malformed_content_encoding[] = {{"Content-Encoding", "gzip;level=9"}};
    const http_header_view empty_content_encoding[] = {{"Content-Encoding", ""}};
    const http_header_view malformed_content_type[] = {{"Content-Type", "not a media type"}};
    const http_header_view empty_content_type[] = {{"Content-Type", ""}};
    const http_header_view content_length[] = {{"Content-Length", "0"}};
    const http_header_view transfer_encoding[] = {{"Transfer-Encoding", "chunked"}};
    const http_header_view trailer[] = {{"Trailer", "X-Checksum"}};
    const http_header_view te[] = {{"TE", "trailers"}};
    const http_header_view duplicate_server[] = {
        {"Server", "one"},
        {"server", "two"},
    };
    const http_header_view invalid_connection[] = {{"Connection", ","}};
    const http_header_view managed_connection[] = {
        {"Connection", "close, date"},
    };
    const http_header_view invalid_upgrade[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "bad protocol"},
    };
    const http_header_view unlisted_upgrade[] = {{"Upgrade", "example/1"}};

    RUVIA_CHECK(rejects(malformed_name, http1_interim_response_prepare_error::invalid_header));
    RUVIA_CHECK(rejects(malformed_value, http1_interim_response_prepare_error::invalid_header));
    RUVIA_CHECK(
        rejects(malformed_content_encoding, http1_interim_response_prepare_error::invalid_header));
    RUVIA_CHECK(rejects(empty_content_encoding, http1_interim_response_prepare_error::invalid_header));
    RUVIA_CHECK(rejects(malformed_content_type, http1_interim_response_prepare_error::invalid_header));
    RUVIA_CHECK(rejects(empty_content_type, http1_interim_response_prepare_error::invalid_header));
    RUVIA_CHECK(rejects(content_length, http1_interim_response_prepare_error::content_length_forbidden));
    RUVIA_CHECK(
        rejects(transfer_encoding, http1_interim_response_prepare_error::transfer_encoding_forbidden));
    RUVIA_CHECK(rejects(trailer, http1_interim_response_prepare_error::trailer_forbidden));
    RUVIA_CHECK(rejects(te, http1_interim_response_prepare_error::te_field_forbidden));
    RUVIA_CHECK(rejects(duplicate_server, http1_interim_response_prepare_error::repeated_singleton));
    RUVIA_CHECK(rejects(invalid_connection, http1_interim_response_prepare_error::invalid_connection));
    RUVIA_CHECK(rejects(managed_connection, http1_interim_response_prepare_error::invalid_connection));
    RUVIA_CHECK(rejects(invalid_upgrade, http1_interim_response_prepare_error::invalid_upgrade));
    RUVIA_CHECK(rejects(
        unlisted_upgrade, http1_interim_response_prepare_error::upgrade_connection_option_required));
}

RUVIA_TEST(http1_interim_response_writer_enforces_field_and_size_limits) {
    std::vector<http_header_view> too_many(
        ruvia::max_http_header_fields + 1, http_header_view("X-Hint", "value"));
    std::array<char, 64> buffer;
    buffer.fill('!');
    const auto too_many_result = http1_interim_response_writer().prepare(
        http_interim_response_head(
            ruvia::http_status::early_hints, std::span<const http_header_view>(too_many)),
        buffer);
    RUVIA_CHECK(too_many_result.failure() != nullptr);
    if (too_many_result.failure() != nullptr) {
        RUVIA_CHECK_EQ(
            too_many_result.failure()->error(), http1_interim_response_prepare_error::too_many_headers);
    }
    RUVIA_CHECK(unchanged(buffer, '!'));

    const std::string oversized_value(ruvia::max_http_header_bytes, 'x');
    const http_header_view oversized[] = {{"X-Hint", oversized_value}};
    const auto oversized_result = http1_interim_response_writer().prepare(
        http_interim_response_head(ruvia::http_status::early_hints, oversized), buffer);
    RUVIA_CHECK(oversized_result.failure() != nullptr);
    if (oversized_result.failure() != nullptr) {
        RUVIA_CHECK_EQ(
            oversized_result.failure()->error(), http1_interim_response_prepare_error::header_too_large);
    }
    RUVIA_CHECK(unchanged(buffer, '!'));
}
