#include <concepts>
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
#include "parser/multipart_delimiter.h"
#include "parser/multipart_part_access.h"
#include "parser/multipart_part_headers.h"
#include "parser/multipart_stream_part_access.h"
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

// A lone '-' after the boundary token is not the closing "--" delimiter.
RUVIA_TEST(multipart_boundary_lone_dash_is_not_a_delimiter) {
    using ruvia::detail::http_find_multipart_body_delimiter;
    const std::string_view body = "\r\n--abc-x\r\n--abc\r\n";
    const auto match = http_find_multipart_body_delimiter(
        body, ruvia::multipart_boundary("abc"), /*input_finished=*/true);
    const auto* part = match.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK_EQ(part->offset(), body.find("\r\n--abc\r\n"));
    }
}

RUVIA_TEST(multipart_boundary_prefix_of_longer_token_is_not_a_delimiter) {
    using ruvia::detail::http_find_initial_multipart_delimiter;
    using ruvia::detail::http_find_multipart_body_delimiter;
    const std::string_view body = "\r\n--abcXYZ\r\n--abc\r\n";
    const auto body_match = http_find_multipart_body_delimiter(
        body, ruvia::multipart_boundary("abc"), /*input_finished=*/true);
    const auto* body_part = body_match.part();
    RUVIA_CHECK(body_part != nullptr);
    if (body_part != nullptr) {
        RUVIA_CHECK_EQ(body_part->offset(), body.find("\r\n--abc\r\n"));
    }

    // The initial delimiter must begin the entity or a new line; a matching
    // token embedded in preamble text is not a delimiter.
    const std::string_view preamble = "prefix--abc\r\ntext\r\n--abc\r\n";
    const auto initial_value = http_find_initial_multipart_delimiter(
        preamble, ruvia::multipart_boundary("abc"), /*input_finished=*/true);
    const auto* initial_part = initial_value.part();
    RUVIA_CHECK(initial_part != nullptr);
    if (initial_part != nullptr) {
        RUVIA_CHECK_EQ(initial_part->offset(), preamble.rfind("--abc\r\n"));
    }
}

RUVIA_TEST(multipart_boundary_close_delimiter_still_matches) {
    using ruvia::detail::http_match_multipart_delimiter_line;
    const auto boundary = ruvia::multipart_boundary("abc");
    const auto close = http_match_multipart_delimiter_line("--abc--\r\n", boundary, false);
    RUVIA_CHECK(close.close() != nullptr);
    const auto part = http_match_multipart_delimiter_line("--abc\r\nrest", boundary, false);
    RUVIA_CHECK(part.part() != nullptr);

    // RFC 2046 transport-padding is accepted on both delimiter forms.
    const auto padded_part = http_match_multipart_delimiter_line("--abc \t\r\n", boundary, false);
    RUVIA_CHECK(padded_part.part() != nullptr);
    const auto padded_close = http_match_multipart_delimiter_line("--abc-- \t\r\n", boundary, false);
    RUVIA_CHECK(padded_close.close() != nullptr);

    // A close delimiter at the current chunk edge is ambiguous until EOF.
    const auto ambiguous_close = http_match_multipart_delimiter_line("--abc--", boundary, false);
    RUVIA_CHECK(ambiguous_close.need_input() != nullptr);
    const auto eof_close = http_match_multipart_delimiter_line("--abc--", boundary, true);
    RUVIA_CHECK(eof_close.close() != nullptr);
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

RUVIA_TEST(multipart_input_lifecycle_has_three_exclusive_states) {
    ruvia::detail::multipart_input_lifecycle streaming(std::pmr::get_default_resource());
    RUVIA_CHECK(streaming.streaming_open() != nullptr);
    RUVIA_CHECK(streaming.streaming_eof() == nullptr);
    RUVIA_CHECK(streaming.borrowed() == nullptr);
    RUVIA_CHECK(!streaming.eof());

    streaming.feed("abcdef");
    streaming.consume(2);
    RUVIA_CHECK_EQ(streaming.view(), std::string_view("cdef"));
    streaming.finish_input();
    RUVIA_CHECK(streaming.streaming_open() == nullptr);
    RUVIA_CHECK(streaming.streaming_eof() != nullptr);
    RUVIA_CHECK(streaming.eof());
    RUVIA_CHECK_EQ(streaming.view(), std::string_view("cdef"));

    // EOF is an idempotent transition and cannot discard pending bytes.
    streaming.finish_input();
    RUVIA_CHECK(streaming.streaming_eof() != nullptr);
    RUVIA_CHECK_EQ(streaming.view(), std::string_view("cdef"));
}

RUVIA_TEST(multipart_borrowed_input_is_complete_and_rejects_feed) {
    ruvia::detail::multipart_input_lifecycle borrowed(
        ruvia::detail::multipart_borrowed_input{"--BOUNDARY--"});
    RUVIA_CHECK(borrowed.borrowed() != nullptr);
    RUVIA_CHECK(borrowed.eof());
    RUVIA_CHECK_EQ(borrowed.view(), std::string_view("--BOUNDARY--"));

    borrowed.finish_input();
    RUVIA_CHECK(borrowed.borrowed() != nullptr);
    bool feed_threw = false;
    try {
        borrowed.feed("ignored");
    } catch (const std::logic_error&) {
        feed_threw = true;
    }
    RUVIA_CHECK(feed_threw);
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

// Part header parsing owns either the parsed views or a typed failure.
RUVIA_TEST(multipart_part_header_result_is_discriminated) {
    using ruvia::detail::http_parse_multipart_part_headers;

    const auto parsed_value = http_parse_multipart_part_headers(
        "Content-Disposition: form-data; name=\"field\"; filename=\"f.txt\"\r\n"
        "Content-Type: text/plain");
    const auto* headers = parsed_value.headers();
    RUVIA_CHECK(headers != nullptr);
    RUVIA_CHECK(parsed_value.failure() == nullptr);
    if (headers != nullptr) {
        RUVIA_CHECK_EQ(headers->name(), std::string_view("field"));
        RUVIA_CHECK_EQ(headers->filename(), std::string_view("f.txt"));
        RUVIA_CHECK(headers->has_filename());
        RUVIA_CHECK_EQ(headers->content_type(), std::string_view("text/plain"));
    }

    // form-data with no name parameter.
    const auto missing_name = http_parse_multipart_part_headers("Content-Disposition: form-data");
    RUVIA_CHECK(missing_name.failure() != nullptr);
    if (missing_name.failure() != nullptr) {
        RUVIA_CHECK(
            missing_name.failure()->parse_error() == ruvia::multipart_parse_error::missing_field_name);
    }

    // A non-form-data disposition, and no disposition at all, are invalid.
    for (const std::string_view invalid :
        {"Content-Disposition: attachment; name=\"x\"", "Content-Type: text/plain"}) {
        const auto result_value = http_parse_multipart_part_headers(invalid);
        RUVIA_CHECK(result_value.failure() != nullptr);
        if (result_value.failure() != nullptr) {
            RUVIA_CHECK(result_value.failure()->parse_error() ==
                        ruvia::multipart_parse_error::invalid_content_disposition);
        }
    }
}

RUVIA_TEST(multipart_part_preserves_empty_filename_parameter_presence) {
    using ruvia::detail::http_parse_multipart_part_headers;

    const auto headers_only = http_parse_multipart_part_headers(
        "Content-Disposition: form-data; name=\"upload\"; filename=\"\"");
    const auto* headers = headers_only.headers();
    RUVIA_CHECK(headers != nullptr);
    if (headers != nullptr) {
        RUVIA_CHECK(headers->has_filename());
        RUVIA_CHECK_EQ(headers->filename(), std::string_view());
    }

    const auto no_filename =
        http_parse_multipart_part_headers("Content-Disposition: form-data; name=\"upload\"");
    const auto* no_filename_headers = no_filename.headers();
    RUVIA_CHECK(no_filename_headers != nullptr);
    if (no_filename_headers != nullptr) {
        RUVIA_CHECK(!no_filename_headers->has_filename());
        RUVIA_CHECK_EQ(no_filename_headers->filename(), std::string_view());
    }

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
    using ruvia::detail::http_parse_multipart_part_headers;

    for (const std::string_view invalid : {"Content-Disposition: form-data; name=\"unterminated",
             "Content-Disposition: form-data; name=unquoted value",
             "Content-Disposition: form-data; name=field; name=shadow",
             "Content-Disposition: form-data; name=field; filename=a; filename=b",
             "Content-Disposition: form-data; name=field; FileName*=UTF-8''evil.txt",
             "Content-Disposition: form-data; name=field; x=one; X=two",
             "Content-Disposition: form-data; name=field; broken",
             "Content-Disposition: form-data; name=field\r\n"
             "Content-Disposition: form-data; name=shadow"}) {
        const auto parsed_value = http_parse_multipart_part_headers(invalid);
        RUVIA_CHECK(parsed_value.failure() != nullptr);
        if (parsed_value.failure() != nullptr) {
            RUVIA_CHECK(parsed_value.failure()->parse_error() ==
                        ruvia::multipart_parse_error::invalid_content_disposition);
        }

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

    const auto escaped = http_parse_multipart_part_headers(
        "Content-Disposition: form-data; name=\"a\\\"b\"; filename=\"x\\\\y\"");
    RUVIA_CHECK(escaped.headers() != nullptr);
    if (escaped.headers() != nullptr) {
        RUVIA_CHECK_EQ(escaped.headers()->name(), std::string_view("a\\\"b"));
        RUVIA_CHECK_EQ(escaped.headers()->filename(), std::string_view("x\\\\y"));
    }

    // MIME structured fields allow linear whitespace around separator
    // characters; this differs from top-level HTTP media-type parameters.
    const auto spaced = http_parse_multipart_part_headers(
        "Content-Disposition: form-data; name = field; filename = \"a.txt\"");
    RUVIA_CHECK(spaced.headers() != nullptr);
    if (spaced.headers() != nullptr) {
        RUVIA_CHECK_EQ(spaced.headers()->name(), std::string_view("field"));
        RUVIA_CHECK_EQ(spaced.headers()->filename(), std::string_view("a.txt"));
    }
}

RUVIA_TEST(multipart_part_header_rejects_ambiguous_header_blocks) {
    using ruvia::detail::http_parse_multipart_part_headers;

    for (const std::string_view invalid : {"Broken-Line\r\n"
                                           "Content-Disposition: form-data; name=field",
             " Content-Disposition: form-data; name=field",
             "Content-Disposition : form-data; name=field",
             "Content-Disposition: form-data; name=field\r\n"
             " filename=shadow.txt",
             "Content-Disposition: form-data; name=field\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Type: application/json"}) {
        const auto parsed_value = http_parse_multipart_part_headers(invalid);
        RUVIA_CHECK(parsed_value.failure() != nullptr);
        if (parsed_value.failure() != nullptr) {
            RUVIA_CHECK(
                parsed_value.failure()->parse_error() == ruvia::multipart_parse_error::invalid_part_headers);
        }

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

RUVIA_TEST(multipart_part_header_rejects_invalid_content_types) {
    using ruvia::detail::http_parse_multipart_part_headers;

    for (const std::string_view content_type :
        {"", "text", "text/", "/plain", "*/plain", "text/*", "text/plain; charset",
            "text/plain; charset=", "text/plain; charset=utf-8; CHARSET=latin1"}) {
        std::string headers =
            "Content-Disposition: form-data; name=field\r\n"
            "Content-Type: ";
        headers.append(content_type);
        const auto parsed_value = http_parse_multipart_part_headers(headers);
        RUVIA_CHECK(parsed_value.failure() != nullptr);
        if (parsed_value.failure() != nullptr) {
            RUVIA_CHECK(
                parsed_value.failure()->parse_error() == ruvia::multipart_parse_error::invalid_part_headers);
        }
    }

    const auto valid = http_parse_multipart_part_headers(
        "Content-Disposition: form-data; name=field\r\n"
        "Content-Type: text/plain; charset = \"UTF-8\"");
    RUVIA_CHECK(valid.headers() != nullptr);
    if (valid.headers() != nullptr) {
        RUVIA_CHECK_EQ(
            valid.headers()->content_type(), std::string_view("text/plain; charset = \"UTF-8\""));
    }
}

RUVIA_TEST(multipart_part_header_names_are_case_insensitive) {
    using ruvia::detail::http_parse_multipart_part_headers;

    // HTTP field names are case-insensitive; a part that lowercases them (some
    // clients do) must still be recognized, with name and content type extracted.
    const auto parsed_value = http_parse_multipart_part_headers(
        "content-disposition: form-data; name=\"field\"\r\n"
        "content-type: image/png");
    const auto* headers = parsed_value.headers();
    RUVIA_CHECK(headers != nullptr);
    if (headers != nullptr) {
        RUVIA_CHECK_EQ(headers->name(), std::string_view("field"));
        RUVIA_CHECK_EQ(headers->content_type(), std::string_view("image/png"));
    }
}

RUVIA_TEST(multipart_header_value_in_block_lookup) {
    using ruvia::detail::http_header_value_in_block;
    const std::string_view block =
        "Content-Disposition: form-data; name=\"a\"\r\n"
        "Content-Type: text/plain";
    // Case-insensitive name match with OWS-trimmed value; the last line has no
    // trailing CRLF and must still be found.
    RUVIA_CHECK(http_header_value_in_block(block, "content-type") == std::string_view("text/plain"));
    RUVIA_CHECK(http_header_value_in_block(block, "CONTENT-TYPE") == std::string_view("text/plain"));
    RUVIA_CHECK(http_header_value_in_block(block, "Content-Disposition") ==
                std::string_view("form-data; name=\"a\""));
    // Missing header -> nullopt.
    RUVIA_CHECK(!http_header_value_in_block(block, "X-Absent").has_value());
    // A line without a colon is skipped, not matched by name.
    RUVIA_CHECK(!http_header_value_in_block("garbageline\r\nX: v", "garbageline").has_value());
    // Surrounding OWS on the value is trimmed.
    RUVIA_CHECK(http_header_value_in_block("X:   spaced   ", "X") == std::string_view("spaced"));
}

RUVIA_TEST(multipart_header_value_in_block_uses_last_match) {
    using ruvia::detail::http_header_value_in_block;
    const std::string_view block =
        "Content-Type: text/plain\r\n"
        "X-Other: value\r\n"
        "content-type: image/png";

    RUVIA_CHECK(http_header_value_in_block(block, "Content-Type") == std::string_view("image/png"));
}

RUVIA_TEST(multipart_disposition_parameter_extraction) {
    using ruvia::detail::http_disposition_parameter;
    const std::string_view disposition = "form-data; name=\"field\"; filename=\"a.txt\"";
    RUVIA_CHECK(http_disposition_parameter(disposition, "name") == std::string_view("field"));
    RUVIA_CHECK(http_disposition_parameter(disposition, "filename") == std::string_view("a.txt"));
    // An unquoted parameter value is returned as-is.
    RUVIA_CHECK(
        http_disposition_parameter("form-data; name=plain", "name") == std::string_view("plain"));
    // An absent parameter is nullopt.
    RUVIA_CHECK(!http_disposition_parameter(disposition, "charset").has_value());
    // Parameter names are case-insensitive (RFC 6266 §4.1), like the Content-Type
    // boundary parameter -- `Name`/`FileName` must resolve, not be rejected.
    const std::string_view mixed_case = "form-data; Name=\"field\"; FileName=\"a.txt\"";
    RUVIA_CHECK(http_disposition_parameter(mixed_case, "name") == std::string_view("field"));
    RUVIA_CHECK(http_disposition_parameter(mixed_case, "filename") == std::string_view("a.txt"));
}

RUVIA_TEST(multipart_is_form_data_disposition) {
    using ruvia::detail::http_is_form_data_disposition;
    RUVIA_CHECK(http_is_form_data_disposition("form-data; name=\"x\""));
    RUVIA_CHECK(http_is_form_data_disposition("FORM-DATA"));                      // case-insensitive
    RUVIA_CHECK(http_is_form_data_disposition("  form-data  ; filename=\"y\""));  // OWS-trimmed type
    RUVIA_CHECK(!http_is_form_data_disposition("attachment; name=\"x\""));
    RUVIA_CHECK(!http_is_form_data_disposition("form-data-extra"));  // whole type compared
    RUVIA_CHECK(!http_is_form_data_disposition(""));
}

RUVIA_TEST(multipart_part_access_decodes_quoted_pairs) {
    // The buffered parser builds parts via multipart_part_access::make, which must
    // decode RFC 7230 §3.2.6 quoted-pairs in name/filename (they are part-owned so
    // they may differ from the raw request bytes); content_type/body stay verbatim.
    auto* resource = std::pmr::get_default_resource();
    const auto part = ruvia::detail::multipart_part_access::make(
        "a\\\"b", "x\\\\y.txt", "text/plain", "the body", resource);
    RUVIA_CHECK_EQ(std::string(part.name()), std::string("a\"b"));
    RUVIA_CHECK_EQ(std::string(part.filename()), std::string("x\\y.txt"));
    RUVIA_CHECK_EQ(std::string(part.content_type()), std::string("text/plain"));
    RUVIA_CHECK_EQ(std::string(part.body()), std::string("the body"));
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
