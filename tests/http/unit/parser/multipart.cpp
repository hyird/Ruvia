#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/multipart_parser.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

RUVIA_TEST(multipart_parser_retries_owned_metadata_without_consuming_limits) {
    const std::string name(160, 'n');
    const std::string filename(160, 'f');
    const std::string content_type_value = "application/" + std::string(160, 't');
    const std::string headers = "Content-Disposition: form-data; name=\"" + name +
                                "\"; filename=\"" + filename + "\"\r\nContent-Type: " +
                                content_type_value + "\r\n\r\n";
    const std::string body = "--abc\r\n" + headers + "value\r\n--abc--\r\n";

    for (const bool limit_parts : {false, true}) {
        bool reached_success = false;
        std::size_t failed_allocations = 0;
        for (std::size_t failure_index = 0; failure_index < 64; ++failure_index) {
            failing_memory_resource resource;
            bool allocation_failed = false;
            {
                ruvia::multipart_parser parser({.boundary_ = ruvia::multipart_boundary("abc"),
                    .resource_ = &resource,
                    .max_parts_ = limit_parts ? 1U : 2U,
                    .max_metadata_bytes_ = limit_parts ? headers.size() * 2 : headers.size()});
                parser.feed(body);
                parser.finish_input();
                resource.fail_after(failure_index);
                try {
                    const auto result_value = parser.poll();
                    RUVIA_CHECK(result_value.part() != nullptr);
                    reached_success = true;
                } catch (const std::bad_alloc&) {
                    allocation_failed = true;
                    ++failed_allocations;
                }
                resource.allow_allocations();
                if (allocation_failed) {
                    const auto retried = parser.poll();
                    const auto* part = retried.part();
                    RUVIA_CHECK(part != nullptr);
                    RUVIA_CHECK(retried.failure() == nullptr);
                    if (part != nullptr) {
                        RUVIA_CHECK_EQ(part->name(), std::string_view(name));
                        RUVIA_CHECK_EQ(part->filename(), std::string_view(filename));
                        RUVIA_CHECK(part->has_filename());
                        RUVIA_CHECK_EQ(part->content_type(), std::string_view(content_type_value));
                        RUVIA_CHECK_EQ(part->body(), std::string_view("value"));
                        RUVIA_CHECK(part->phase() == ruvia::multipart_chunk_phase::complete);
                    }
                }
                const auto done = parser.poll();
                RUVIA_CHECK(done.done() != nullptr);
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
            if (reached_success) {
                break;
            }
        }
        RUVIA_CHECK(reached_success);
        RUVIA_CHECK(failed_allocations != 0);
    }
}

RUVIA_TEST(multipart_parser_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state_value = 0x4D55'4C54'4950'4152ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 2048; ++sample) {
        std::string input(static_cast<std::size_t>(next_value() % 1025U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        const auto boundary = ruvia::parse_multipart_boundary(input);
        const auto boundary_alternatives =
            static_cast<unsigned int>(boundary.boundary() != nullptr) +
            static_cast<unsigned int>(boundary.not_applicable() != nullptr) +
            static_cast<unsigned int>(boundary.failure() != nullptr);
        RUVIA_CHECK_EQ(boundary_alternatives, 1U);

        std::pmr::monotonic_buffer_resource resource;
        const auto complete_value = ruvia::parse_multipart_body(input,
            {.boundary_ = ruvia::multipart_boundary("FUZZ"), .resource_ = &resource});
        RUVIA_CHECK_EQ(static_cast<unsigned int>(complete_value.body() != nullptr) +
                           static_cast<unsigned int>(complete_value.failure() != nullptr),
            1U);

        ruvia::multipart_parser parser(
            {.boundary_ = ruvia::multipart_boundary("FUZZ"), .resource_ = &resource});
        parser.feed(input);
        parser.finish_input();

        bool terminal = false;
        for (std::size_t step = 0; step <= input.size() + 1; ++step) {
            const auto result_value = parser.poll();
            const auto alternatives = static_cast<unsigned int>(result_value.need_input() != nullptr) +
                                      static_cast<unsigned int>(result_value.part() != nullptr) +
                                      static_cast<unsigned int>(result_value.done() != nullptr) +
                                      static_cast<unsigned int>(result_value.failure() != nullptr);
            RUVIA_CHECK_EQ(alternatives, 1U);
            if (result_value.done() != nullptr || result_value.failure() != nullptr) {
                terminal = true;
                break;
            }
            RUVIA_CHECK(result_value.part() != nullptr);
        }
        RUVIA_CHECK(terminal);
    }
}

RUVIA_TEST(multipart_boundary_value_enforces_rfc2046_grammar) {
    const auto throws_on = [](std::string_view value) {
        try {
            (void)ruvia::multipart_boundary(value);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    const auto spaced = ruvia::multipart_boundary("a b");
    RUVIA_CHECK_EQ(spaced.value(), std::string_view("a b"));
    const auto maximum = ruvia::multipart_boundary(std::string(70, 'x'));
    RUVIA_CHECK_EQ(maximum.value().size(), std::size_t{70});
    RUVIA_CHECK(throws_on(""));
    RUVIA_CHECK(throws_on(std::string(71, 'x')));
    RUVIA_CHECK(throws_on("trailing "));
    RUVIA_CHECK(throws_on("semi;colon"));
    RUVIA_CHECK(throws_on("bad\r\nvalue"));
}

// Boundary extraction owns MIME parameter quoting and returns the same typed
// value consumed by buffered and streaming parsers.
RUVIA_TEST(multipart_boundary_from_content_type) {
    const auto plain = ruvia::parse_multipart_boundary("multipart/form-data; boundary=abc123");
    RUVIA_CHECK(plain.boundary() != nullptr);
    RUVIA_CHECK(plain.not_applicable() == nullptr);
    RUVIA_CHECK(plain.failure() == nullptr);
    RUVIA_CHECK_EQ(plain.boundary()->value(), std::string_view("abc123"));

    const auto quoted = ruvia::parse_multipart_boundary(R"(multipart/form-data; boundary="a b")");
    RUVIA_CHECK(quoted.boundary() != nullptr);
    RUVIA_CHECK_EQ(quoted.boundary()->value(), std::string_view("a b"));

    const auto quoted_special =
        ruvia::parse_multipart_boundary(R"(multipart/form-data; boundary="a:b")");
    RUVIA_CHECK(quoted_special.boundary() != nullptr);
    RUVIA_CHECK_EQ(quoted_special.boundary()->value(), std::string_view("a:b"));

    const auto quoted_pair =
        ruvia::parse_multipart_boundary(R"(multipart/form-data; boundary="a\?b")");
    RUVIA_CHECK(quoted_pair.boundary() != nullptr);
    RUVIA_CHECK_EQ(quoted_pair.boundary()->value(), std::string_view("a?b"));

    // A different media type is not applicable to the multipart parser.
    const auto wrong_type = ruvia::parse_multipart_boundary("text/plain; boundary=abc");
    RUVIA_CHECK(wrong_type.boundary() == nullptr);
    RUVIA_CHECK(wrong_type.not_applicable() != nullptr);
    RUVIA_CHECK(wrong_type.failure() == nullptr);

    // Once multipart/form-data is declared, an invalid boundary is an HTTP
    // request failure rather than a Web-layer parsing policy decision.
    for (const std::string_view invalid : {"multipart/form-data",
             "multipart/form-data; charset=utf-8", "multipart/form-data; boundary=",
             "multipart/form-data; boundary=a:b", R"(multipart/form-data; boundary="a;b")",
             "multipart/form-data; boundary=one; boundary=two",
             "multipart/form-data; boundary=abc; charset=utf-8; CHARSET=latin1",
             "multipart/form-data; boundary=abc; broken",
             "multipart/form-data; broken; boundary=abc", "multipart/form-data; boundary =abc",
             "multipart/form-data; boundary= abc",
             "multipart/form-data; boundary=abc; charset=unquoted value",
             R"(multipart/form-data; boundary=abc; charset="unterminated)",
             "multipart/form-data; boundary=abc; =value"}) {
        const auto result_value = ruvia::parse_multipart_boundary(invalid);
        RUVIA_CHECK(result_value.boundary() == nullptr);
        RUVIA_CHECK(result_value.not_applicable() == nullptr);
        RUVIA_CHECK(result_value.failure() != nullptr);
        if (result_value.failure() != nullptr) {
            const auto error = result_value.failure()->protocol_error();
            RUVIA_CHECK_EQ(error.status(), ruvia::http_status::bad_request);
            RUVIA_CHECK_EQ(
                std::string_view(error.what()), std::string_view("invalid multipart boundary"));
        }
    }

    const auto extension =
        ruvia::parse_multipart_boundary(R"(multipart/form-data; charset="utf-8"; boundary=abc)");
    RUVIA_CHECK(extension.boundary() != nullptr);
    if (extension.boundary() != nullptr) {
        RUVIA_CHECK_EQ(extension.boundary()->value(), std::string_view("abc"));
    }
}

RUVIA_TEST(multipart_parser_commits_an_eof_close_only_after_finish_input) {
    ruvia::multipart_parser parser({.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
        .resource_ = std::pmr::get_default_resource()});
    parser.feed(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY--");

    const auto first = parser.poll();
    const auto* first_part = first.part();
    RUVIA_CHECK(first_part != nullptr);
    if (first_part != nullptr) {
        RUVIA_CHECK(first_part->phase() == ruvia::multipart_chunk_phase::first);
        RUVIA_CHECK_EQ(first_part->body(), std::string_view("value"));
    }

    const auto waiting = parser.poll();
    RUVIA_CHECK(waiting.need_input() != nullptr);
    RUVIA_CHECK(waiting.part() == nullptr);
    RUVIA_CHECK(waiting.done() == nullptr);

    parser.finish_input();
    const auto last = parser.poll();
    const auto* last_part = last.part();
    RUVIA_CHECK(last_part != nullptr);
    if (last_part != nullptr) {
        RUVIA_CHECK(last_part->phase() == ruvia::multipart_chunk_phase::last);
        RUVIA_CHECK(last_part->body().empty());
    }
    const auto done = parser.poll();
    RUVIA_CHECK(done.done() != nullptr);
    RUVIA_CHECK(done.need_input() == nullptr);
    RUVIA_CHECK(done.part() == nullptr);

    bool feed_after_finish_threw = false;
    try {
        parser.feed("ignored");
    } catch (const std::logic_error&) {
        feed_after_finish_threw = true;
    }
    RUVIA_CHECK(feed_after_finish_threw);
}

RUVIA_TEST(multipart_parser_reports_typed_incomplete_body) {
    ruvia::multipart_parser parser({.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
        .resource_ = std::pmr::get_default_resource()});
    parser.feed(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "truncated");
    parser.finish_input();

    const auto result_value = parser.poll();
    RUVIA_CHECK(result_value.failure() != nullptr);
    RUVIA_CHECK(result_value.need_input() == nullptr);
    RUVIA_CHECK(result_value.part() == nullptr);
    RUVIA_CHECK(result_value.done() == nullptr);
    if (result_value.failure() != nullptr) {
        const auto error = result_value.failure()->protocol_error();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::bad_request);
        RUVIA_CHECK_EQ(
            std::string_view(error.what()), std::string_view("incomplete multipart body"));
    }
}

RUVIA_TEST(multipart_part_preserves_empty_filename_parameter_presence) {
    const std::string body =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"\"\r\n"
        "\r\n"
        "data\r\n"
        "--BOUNDARY--\r\n";
    const auto complete_value =
        ruvia::parse_multipart_body(body, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                                              .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(complete_value.failure() == nullptr);
    if (complete_value.body() != nullptr) {
        const auto& parts = complete_value.body()->parts();
        RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
        if (!parts.empty()) {
            RUVIA_CHECK(parts[0].has_filename());
            RUVIA_CHECK_EQ(parts[0].filename(), std::string_view());
        }
    }

    ruvia::multipart_parser parser({.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
        .resource_ = std::pmr::get_default_resource()});
    parser.feed(body);
    parser.finish_input();
    const auto streamed = parser.poll();
    const auto* part = streamed.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK(part->has_filename());
        RUVIA_CHECK_EQ(part->filename(), std::string_view());
    }
}

RUVIA_TEST(multipart_part_header_rejects_ambiguous_disposition_parameters) {
    for (const std::string_view invalid : {"Content-Disposition: form-data; name=\"unterminated",
             "Content-Disposition: form-data; name=unquoted value",
             "Content-Disposition: form-data; name=field; name=shadow",
             "Content-Disposition: form-data; name=field; filename=a; filename=b",
             "Content-Disposition: form-data; name=field; FileName*=UTF-8''evil.txt",
             "Content-Disposition: form-data; name=field; x=one; X=two",
             "Content-Disposition: form-data; name=field; broken",
             "Content-Disposition: form-data; name=field\r\n"
             "Content-Disposition: form-data; name=shadow"}) {
        std::string body = "--BOUNDARY\r\n";
        body.append(invalid);
        body.append("\r\n\r\nvalue\r\n--BOUNDARY--\r\n");
        const auto complete_value =
            ruvia::parse_multipart_body(body, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                                                  .resource_ = std::pmr::get_default_resource()});
        RUVIA_CHECK(complete_value.failure() != nullptr);
        if (complete_value.failure() != nullptr) {
            RUVIA_CHECK_EQ(
                complete_value.failure()->protocol_error().status(), ruvia::http_status::bad_request);
            RUVIA_CHECK_EQ(std::string_view(complete_value.failure()->protocol_error().what()),
                std::string_view("invalid multipart content disposition"));
        }
    }
}

RUVIA_TEST(multipart_part_header_rejects_ambiguous_header_blocks) {
    for (const std::string_view invalid : {"Broken-Line\r\n"
                                           "Content-Disposition: form-data; name=field",
             " Content-Disposition: form-data; name=field",
             "Content-Disposition : form-data; name=field",
             "Content-Disposition: form-data; name=field\r\n"
             " filename=shadow.txt",
             "Content-Disposition: form-data; name=field\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Type: application/json"}) {
        std::string body = "--BOUNDARY\r\n";
        body.append(invalid);
        body.append("\r\n\r\nvalue\r\n--BOUNDARY--\r\n");
        const auto complete_value =
            ruvia::parse_multipart_body(body, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                                                  .resource_ = std::pmr::get_default_resource()});
        RUVIA_CHECK(complete_value.failure() != nullptr);
        if (complete_value.failure() != nullptr) {
            RUVIA_CHECK_EQ(
                complete_value.failure()->protocol_error().status(), ruvia::http_status::bad_request);
            RUVIA_CHECK_EQ(std::string_view(complete_value.failure()->protocol_error().what()),
                std::string_view("invalid multipart part headers"));
        }
    }
}

RUVIA_TEST(multipart_complete_body_parser_returns_borrowed_part_bodies) {
    const std::string body =
        "preamble\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name = \"field\"\r\n\r\n"
        "value\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"a.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "file-data\r\n"
        "--BOUNDARY--\r\n";
    const auto parsed_value =
        ruvia::parse_multipart_body(body, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                                              .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(parsed_value.failure() == nullptr);
    const auto& parts = parsed_value.body()->parts();
    RUVIA_CHECK_EQ(parts.size(), std::size_t{2});
    RUVIA_CHECK_EQ(parts[0].name(), std::string_view("field"));
    RUVIA_CHECK_EQ(parts[0].body(), std::string_view("value"));
    RUVIA_CHECK_EQ(parts[1].name(), std::string_view("upload"));
    RUVIA_CHECK_EQ(parts[1].filename(), std::string_view("a.txt"));
    RUVIA_CHECK_EQ(parts[1].content_type(), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(parts[1].body(), std::string_view("file-data"));
    RUVIA_CHECK(parts[0].body().data() >= body.data());
    RUVIA_CHECK(parts[0].body().data() < body.data() + body.size());
}

RUVIA_TEST(multipart_complete_body_parser_rejects_malformed_body) {
    const auto parsed_value = ruvia::parse_multipart_body(
        "--BOUNDARY\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nmissing close",
        {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
            .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(parsed_value.body() == nullptr);
    RUVIA_CHECK(parsed_value.failure() != nullptr);
    RUVIA_CHECK_EQ(parsed_value.failure()->protocol_error().status(), ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(std::string_view(parsed_value.failure()->protocol_error().what()),
        std::string_view("incomplete multipart body"));
}

RUVIA_TEST(multipart_complete_body_parser_shares_incremental_limits) {
    std::string oversized_preamble(64 * 1024 + 1, 'x');
    const auto complete_value = ruvia::parse_multipart_body(
        oversized_preamble, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                                .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(complete_value.failure() != nullptr);
    RUVIA_CHECK_EQ(
        complete_value.failure()->protocol_error().status(), ruvia::http_status::content_too_large);
    RUVIA_CHECK_EQ(std::string_view(complete_value.failure()->protocol_error().what()),
        std::string_view("multipart preamble exceeds limit"));

    ruvia::multipart_parser incremental({.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
        .resource_ = std::pmr::get_default_resource()});
    incremental.feed(oversized_preamble);
    const auto streamed = incremental.poll();
    RUVIA_CHECK(streamed.failure() != nullptr);
    RUVIA_CHECK_EQ(
        streamed.failure()->protocol_error().status(), ruvia::http_status::content_too_large);
    RUVIA_CHECK_EQ(std::string_view(streamed.failure()->protocol_error().what()),
        std::string_view(complete_value.failure()->protocol_error().what()));
    const auto repeated = incremental.poll();
    RUVIA_CHECK(repeated.failure() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(repeated.failure()->protocol_error().what()),
        std::string_view(complete_value.failure()->protocol_error().what()));
    bool feed_after_failure_threw = false;
    try {
        incremental.feed("--BOUNDARY--");
    } catch (const std::logic_error&) {
        feed_after_failure_threw = true;
    }
    RUVIA_CHECK(feed_after_failure_threw);
}

RUVIA_TEST(multipart_complete_limits_cannot_be_bypassed_by_terminators) {
    auto* const resource = std::pmr::get_default_resource();
    const auto parse = [resource](std::string_view body) {
        return ruvia::parse_multipart_body(
            body, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"), .resource_ = resource});
    };
    const auto check_failure = [&ruvia_ctx, &parse, resource](
                                   const std::string& body, std::string_view message) {
        const auto result_value = parse(body);
        RUVIA_CHECK(result_value.failure() != nullptr);
        if (const auto* failure = result_value.failure()) {
            RUVIA_CHECK_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
            RUVIA_CHECK_EQ(std::string_view(failure->protocol_error().what()), message);
        }

        ruvia::multipart_parser incremental(
            {.boundary_ = ruvia::multipart_boundary("BOUNDARY"), .resource_ = resource});
        incremental.feed(body);
        incremental.finish_input();
        const auto streamed = incremental.poll();
        RUVIA_CHECK(streamed.failure() != nullptr);
        if (const auto* failure = streamed.failure()) {
            RUVIA_CHECK_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
            RUVIA_CHECK_EQ(std::string_view(failure->protocol_error().what()), message);
        }
    };

    // A complete boundary used to skip the preamble cap because the limit was
    // checked only while the parser was still searching for that boundary.
    std::string preamble(64 * 1024 + 1, 'p');
    preamble.append("\r\n--BOUNDARY--\r\n");
    check_failure(preamble, "multipart preamble exceeds limit");

    // Likewise, finding CRLF CRLF in the same input bypassed the part-header
    // check, even when the complete block was already larger than 64 KiB.
    std::string headers =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "X-Large: ";
    headers.append(64 * 1024, 'h');
    headers.append(
        "\r\n\r\nvalue\r\n"
        "--BOUNDARY--\r\n");
    check_failure(headers, "multipart part headers exceed limit");

    // A completed delimiter line with excessive transport-padding must be
    // bounded too; the previous check ran only while the line was incomplete.
    std::string delimiter = "--BOUNDARY";
    delimiter.append(64 * 1024, ' ');
    delimiter.append("\r\n");
    check_failure(delimiter, "multipart delimiter line exceeds limit");

    // Exercise the same completed-line check after a part body, where the
    // delimiter is discovered by the streaming body scanner rather than the
    // initial-boundary path.
    std::string body_delimiter =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY--";
    body_delimiter.append(64 * 1024, ' ');
    body_delimiter.append("\r\n");
    check_failure(body_delimiter, "multipart delimiter line exceeds limit");
}

RUVIA_TEST(multipart_part_and_metadata_limits_are_cumulative) {
    constexpr std::string_view header_value = "Content-Disposition: form-data; name=\"field\"\r\n\r\n";
    std::string body;
    for (std::size_t index = 0; index < 3; ++index) {
        body += "--abc\r\n";
        body += header_value;
        body += "value\r\n";
    }
    body += "--abc--\r\n";

    const auto accepted = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::multipart_boundary("abc"),
                                                                .max_parts_ = 3,
                                                                .max_metadata_bytes_ = 3 * header_value.size()});
    RUVIA_CHECK(accepted.body() != nullptr);
    const auto too_many = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::multipart_boundary("abc"), .max_parts_ = 2});
    RUVIA_CHECK(too_many.failure() != nullptr);
    if (too_many.failure() != nullptr) {
        RUVIA_CHECK_EQ(too_many.failure()->protocol_error().status(), ruvia::http_status::content_too_large);
    }
    const auto too_large = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::multipart_boundary("abc"), .max_metadata_bytes_ = 3 * header_value.size() - 1});
    RUVIA_CHECK(too_large.failure() != nullptr);
    if (too_large.failure() != nullptr) {
        RUVIA_CHECK_EQ(too_large.failure()->protocol_error().status(), ruvia::http_status::content_too_large);
    }

    ruvia::multipart_parser parser({.boundary_ = ruvia::multipart_boundary("abc"), .max_parts_ = 2});
    parser.feed(body);
    for (std::size_t index = 0; index < 4; ++index) {
        const auto result_value = parser.poll();
        if (index < 2) {
            RUVIA_CHECK(result_value.part() != nullptr);
        } else {
            RUVIA_CHECK(result_value.failure() != nullptr);
        }
    }
}

RUVIA_TEST(multipart_streaming_handles_bytewise_long_syntax_and_false_delimiters) {
    std::string body(16 * 1024, 'p');
    body += "\r\n--abc";
    body.append(16 * 1024, ' ');
    body += "\r\nContent-Disposition: form-data; name=\"field\"\r\nX-Long: ";
    body.append(16 * 1024, 'h');
    body += "\r\n\r\nvalue\r\n--abc";
    body.append(16 * 1024, ' ');
    body += "x\r\n--abc--";
    body.append(16 * 1024, '\t');
    body += "\r\n";

    const auto expected = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::multipart_boundary("abc")});
    RUVIA_CHECK(expected.body() != nullptr);
    ruvia::multipart_parser parser({.boundary_ = ruvia::multipart_boundary("abc")});
    std::string received;
    std::size_t ended = 0;
    bool done = false;
    for (std::size_t offset = 0; offset < body.size(); ++offset) {
        parser.feed(std::string_view(body).substr(offset, 1));
        for (;;) {
            const auto result_value = parser.poll();
            RUVIA_CHECK(result_value.failure() == nullptr);
            if (const auto* part = result_value.part()) {
                received += part->body();
                if (part->phase() == ruvia::multipart_chunk_phase::complete ||
                    part->phase() == ruvia::multipart_chunk_phase::last) {
                    ++ended;
                }
            } else {
                done = result_value.done() != nullptr;
                break;
            }
        }
    }
    RUVIA_CHECK(done);
    RUVIA_CHECK_EQ(ended, std::size_t{1});
    if (expected.body() != nullptr) {
        RUVIA_CHECK_EQ(std::string_view(received), expected.body()->parts()[0].body());
    }
}
