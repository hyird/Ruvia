#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "content_decoding_fixture.h"
#include "http_multipart.h"
#include "http_server_sent_events.h"

TEST(Ruvia, multipart_boundary_value_enforces_rfc2046_grammar) {
    const auto throws_on = [](std::string_view value) {
        try {
            (void)ruvia::multipart_boundary(value);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    const auto rejected = [&throws_on](std::string_view value) {
        return throws_on(value) && !ruvia::multipart_boundary::try_create(value).has_value();
    };

    const auto spaced = ruvia::multipart_boundary("a b");
    EXPECT_EQ(spaced.value(), std::string_view("a b"));
    const auto maximum = ruvia::multipart_boundary(std::string(70, 'x'));
    EXPECT_EQ(maximum.value().size(), std::size_t{70});
    const auto created = ruvia::multipart_boundary::try_create("a b");
    EXPECT_TRUE(created.has_value());
    if (created) {
        EXPECT_EQ(created->value(), std::string_view("a b"));
    }
    EXPECT_TRUE(rejected(""));
    EXPECT_TRUE(rejected(std::string(71, 'x')));
    EXPECT_TRUE(rejected("trailing "));
    EXPECT_TRUE(rejected("semi;colon"));
    EXPECT_TRUE(rejected("bad\r\nvalue"));
}

// Boundary extraction owns MIME parameter quoting and returns the same typed
// value consumed by buffered and streaming parsers.
TEST(Ruvia, multipart_boundary_from_content_type) {
    const auto plain = ruvia::parse_multipart_boundary("multipart/form-data; boundary=abc123");
    EXPECT_TRUE(plain.boundary() != nullptr);
    EXPECT_TRUE(plain.not_applicable() == nullptr);
    EXPECT_TRUE(plain.failure() == nullptr);
    EXPECT_EQ(plain.boundary()->value(), std::string_view("abc123"));

    const auto quoted = ruvia::parse_multipart_boundary(R"(multipart/form-data; boundary="a b")");
    EXPECT_TRUE(quoted.boundary() != nullptr);
    EXPECT_EQ(quoted.boundary()->value(), std::string_view("a b"));

    const auto quoted_special = ruvia::parse_multipart_boundary(R"(multipart/form-data; boundary="a:b")");
    EXPECT_TRUE(quoted_special.boundary() != nullptr);
    EXPECT_EQ(quoted_special.boundary()->value(), std::string_view("a:b"));

    const auto quoted_pair = ruvia::parse_multipart_boundary(R"(multipart/form-data; boundary="a\?b")");
    EXPECT_TRUE(quoted_pair.boundary() != nullptr);
    EXPECT_EQ(quoted_pair.boundary()->value(), std::string_view("a?b"));

    // A different media type is not applicable to the multipart parser.
    const auto wrong_type = ruvia::parse_multipart_boundary("text/plain; boundary=abc");
    EXPECT_TRUE(wrong_type.boundary() == nullptr);
    EXPECT_TRUE(wrong_type.not_applicable() != nullptr);
    EXPECT_TRUE(wrong_type.failure() == nullptr);

    // Once multipart/form-data is declared, an invalid boundary is an HTTP
    // request failure rather than a Web-layer parsing policy decision.
    for (const std::string_view invalid : {"multipart/form-data", "multipart/form-data; charset=utf-8", "multipart/form-data; boundary=", "multipart/form-data; boundary=a:b", R"(multipart/form-data; boundary="a;b")", "multipart/form-data; boundary=one; boundary=two", "multipart/form-data; boundary=abc; charset=utf-8; CHARSET=latin1", "multipart/form-data; boundary=abc; broken", "multipart/form-data; broken; boundary=abc", "multipart/form-data; boundary =abc", "multipart/form-data; boundary= abc", "multipart/form-data; boundary=abc; charset=unquoted value", R"(multipart/form-data; boundary=abc; charset="unterminated)", "multipart/form-data; boundary=abc; =value"}) {
        const auto result_value = ruvia::parse_multipart_boundary(invalid);
        EXPECT_TRUE(result_value.boundary() == nullptr);
        EXPECT_TRUE(result_value.not_applicable() == nullptr);
        EXPECT_TRUE(result_value.failure() != nullptr);
        if (result_value.failure() != nullptr) {
            const auto error = result_value.failure()->protocol_error();
            EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
            EXPECT_EQ(std::string_view(error.what()), std::string_view("invalid multipart boundary"));
        }
    }

    const auto extension = ruvia::parse_multipart_boundary(R"(multipart/form-data; charset="utf-8"; boundary=abc)");
    EXPECT_TRUE(extension.boundary() != nullptr);
    if (extension.boundary() != nullptr) {
        EXPECT_EQ(extension.boundary()->value(), std::string_view("abc"));
    }
}

TEST(Ruvia, multipart_boundary_allows_empty_http_parameter_slots_without_relaxing_validation) {
    for (const std::string_view valid : {"multipart/form-data;;boundary=abc;", "multipart/form-data; ; boundary=abc; \t;", R"(multipart/form-data;;charset="utf-8";;boundary="a b";;)"}) {
        const auto result = ruvia::parse_multipart_boundary(valid);
        EXPECT_TRUE(result.boundary() != nullptr);
        if (result.boundary()) {
            const auto expected = valid.find("a b") == std::string_view::npos ? "abc" : "a b";
            EXPECT_EQ(result.boundary()->value(), std::string_view(expected));
        }
    }
    for (const std::string_view invalid : {"multipart/form-data; ; ;", "multipart/form-data;;boundary=;", "multipart/form-data;;boundary=abc;;BOUNDARY=abc;", "multipart/form-data;;boundary=abc;;charset=utf-8;;CHARSET=latin1;", "multipart/form-data;;boundary=abc;;broken;", "multipart/form-data;;boundary =abc;", "multipart/form-data;;boundary= abc;"}) {
        const auto result = ruvia::parse_multipart_boundary(invalid);
        EXPECT_TRUE(result.failure() != nullptr);
    }
}

TEST(Ruvia, multipart_parser_commits_an_eof_close_only_after_finish_input) {
    ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    parser.feed(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY--");

    const auto first = parser.poll();
    const auto* first_part = first.part();
    EXPECT_TRUE(first_part != nullptr);
    if (first_part != nullptr) {
        EXPECT_TRUE(first_part->phase() == ruvia::multipart_chunk_phase::first);
        EXPECT_EQ(first_part->body(), std::string_view("value"));
    }

    const auto waiting = parser.poll();
    EXPECT_TRUE(waiting.need_input() != nullptr);
    EXPECT_TRUE(waiting.part() == nullptr);
    EXPECT_TRUE(waiting.done() == nullptr);

    parser.finish_input();
    const auto last = parser.poll();
    const auto* last_part = last.part();
    EXPECT_TRUE(last_part != nullptr);
    if (last_part != nullptr) {
        EXPECT_TRUE(last_part->phase() == ruvia::multipart_chunk_phase::last);
        EXPECT_TRUE(last_part->body().empty());
    }
    const auto done = parser.poll();
    EXPECT_TRUE(done.done() != nullptr);
    EXPECT_TRUE(done.need_input() == nullptr);
    EXPECT_TRUE(done.part() == nullptr);

    bool feed_after_finish_threw = false;
    try {
        parser.feed("ignored");
    } catch (const std::logic_error&) {
        feed_after_finish_threw = true;
    }
    EXPECT_TRUE(feed_after_finish_threw);
}

TEST(Ruvia, multipart_parser_reports_typed_incomplete_body) {
    ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    parser.feed(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "truncated");
    parser.finish_input();

    const auto result_value = parser.poll();
    EXPECT_TRUE(result_value.failure() != nullptr);
    EXPECT_TRUE(result_value.need_input() == nullptr);
    EXPECT_TRUE(result_value.part() == nullptr);
    EXPECT_TRUE(result_value.done() == nullptr);
    if (result_value.failure() != nullptr) {
        const auto error = result_value.failure()->protocol_error();
        EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("incomplete multipart body"));
    }
}

TEST(Ruvia, multipart_part_preserves_empty_filename_parameter_presence) {
    const std::string body =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"\"\r\n"
        "\r\n"
        "data\r\n"
        "--BOUNDARY--\r\n";
    const auto parts = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")}).take_parts();
    EXPECT_EQ(parts.size(), std::size_t{1});
    if (!parts.empty()) {
        EXPECT_TRUE(parts[0].has_filename());
        EXPECT_EQ(parts[0].filename(), std::string_view());
    }

    ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    parser.feed(body);
    parser.finish_input();
    const auto streamed = parser.poll();
    const auto* part = streamed.part();
    EXPECT_TRUE(part != nullptr);
    if (part != nullptr) {
        EXPECT_TRUE(part->has_filename());
        EXPECT_EQ(part->filename(), std::string_view());
    }
}

TEST(Ruvia, multipart_part_header_rejects_ambiguous_disposition_parameters) {
    for (const std::string_view invalid :
        {"Content-Disposition: form-data; name=\"unterminated", "Content-Disposition: form-data; name=unquoted value", "Content-Disposition: form-data; name=field; name=shadow", "Content-Disposition: form-data; name=field; filename=a; filename=b", "Content-Disposition: form-data; name=field; x=one; X=two", "Content-Disposition: form-data; name=field; broken",
            "Content-Disposition: form-data; name=field\r\n"
            " filename=shadow.txt",
            "Content-Disposition: form-data; name=field\r\n"
            "Content-Disposition: form-data; name=shadow"}) {
        std::string body = "--BOUNDARY\r\n";
        body.append(invalid);
        body.append("\r\n\r\nvalue\r\n--BOUNDARY--\r\n");
        const auto parsed = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
        EXPECT_TRUE(parsed.failure() != nullptr);
        if (const auto* failure = parsed.failure()) {
            const auto error = failure->protocol_error();
            EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
            EXPECT_EQ(std::string_view(error.what()), std::string_view("invalid multipart content disposition"));
        }
    }
}

TEST(Ruvia, multipart_part_header_rejects_ambiguous_header_blocks) {
    for (const std::string_view invalid :
        {"Broken-Line\r\n"
         "Content-Disposition: form-data; name=field",
            " Content-Disposition: form-data; name=field", "Content-Disposition : form-data; name=field",
            "Content-Disposition: form-data; name=field\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Type: application/json"}) {
        std::string body = "--BOUNDARY\r\n";
        body.append(invalid);
        body.append("\r\n\r\nvalue\r\n--BOUNDARY--\r\n");
        const auto parsed = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
        EXPECT_TRUE(parsed.failure() != nullptr);
        if (const auto* failure = parsed.failure()) {
            const auto error = failure->protocol_error();
            EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
            EXPECT_EQ(std::string_view(error.what()), std::string_view("invalid multipart part headers"));
        }
    }
}

TEST(Ruvia, multipart_complete_body_parser_returns_borrowed_part_bodies) {
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
    const auto parts = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")}).take_parts();
    EXPECT_EQ(parts.size(), std::size_t{2});
    EXPECT_EQ(parts[0].name(), std::string_view("field"));
    EXPECT_EQ(parts[0].body(), std::string_view("value"));
    EXPECT_EQ(parts[1].name(), std::string_view("upload"));
    EXPECT_EQ(parts[1].filename(), std::string_view("a.txt"));
    EXPECT_EQ(parts[1].content_type(), std::string_view("text/plain"));
    EXPECT_EQ(parts[1].body(), std::string_view("file-data"));
    EXPECT_TRUE(parts[0].body().data() >= body.data());
    EXPECT_TRUE(parts[0].body().data() < body.data() + body.size());
    EXPECT_TRUE(parts[0].name().data() >= body.data());
    EXPECT_TRUE(parts[0].name().data() < body.data() + body.size());
    EXPECT_TRUE(parts[1].name().data() >= body.data());
    EXPECT_TRUE(parts[1].name().data() < body.data() + body.size());
    EXPECT_TRUE(parts[1].filename().data() >= body.data());
    EXPECT_TRUE(parts[1].filename().data() < body.data() + body.size());
}

TEST(Ruvia, multipart_complete_body_parser_rejects_malformed_body) {
    constexpr std::string_view truncated = "--BOUNDARY\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nmissing close";
    const auto parsed = ruvia::parse_multipart_body(truncated, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    EXPECT_TRUE(parsed.parts() == nullptr);
    EXPECT_TRUE(parsed.failure() != nullptr);
    if (const auto* failure = parsed.failure()) {
        const auto error = failure->protocol_error();
        EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("incomplete multipart body"));
    }
    EXPECT_THROW((void)ruvia::parse_multipart_body(truncated, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")}).take_parts(), std::logic_error);
}

TEST(Ruvia, multipart_complete_body_parser_shares_incremental_limits) {
    std::string oversized_preamble(64 * 1024 + 1, 'x');
    std::string complete_error_message;
    const auto parsed = ruvia::parse_multipart_body(oversized_preamble, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    EXPECT_TRUE(parsed.failure() != nullptr);
    if (const auto* failure = parsed.failure()) {
        const auto error = failure->protocol_error();
        complete_error_message = error.what();
        EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("multipart preamble exceeds limit"));
    }

    ruvia::multipart_parser incremental({.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    incremental.feed(oversized_preamble);
    const auto streamed = incremental.poll();
    EXPECT_TRUE(streamed.failure() != nullptr);
    EXPECT_EQ(streamed.failure()->protocol_error().status(), ruvia::http_status::content_too_large);
    EXPECT_EQ(std::string_view(streamed.failure()->protocol_error().what()), std::string_view(complete_error_message));
    const auto repeated = incremental.poll();
    EXPECT_TRUE(repeated.failure() != nullptr);
    EXPECT_EQ(std::string_view(repeated.failure()->protocol_error().what()), std::string_view(complete_error_message));
    bool feed_after_failure_threw = false;
    try {
        incremental.feed("--BOUNDARY--");
    } catch (const std::logic_error&) {
        feed_after_failure_threw = true;
    }
    EXPECT_TRUE(feed_after_failure_threw);
}

TEST(Ruvia, multipart_complete_limits_cannot_be_bypassed_by_terminators) {
    const auto parse = [](std::string_view body) {
        return ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    };
    const auto check_failure = [&parse](const std::string& body, std::string_view message) {
        const auto parsed = parse(body);
        EXPECT_TRUE(parsed.failure() != nullptr);
        if (const auto* failure = parsed.failure()) {
            EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
            EXPECT_EQ(std::string_view(failure->protocol_error().what()), message);
        }

        ruvia::multipart_parser incremental({.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
        incremental.feed(body);
        incremental.finish_input();
        const auto streamed = incremental.poll();
        EXPECT_TRUE(streamed.failure() != nullptr);
        if (const auto* failure = streamed.failure()) {
            EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
            EXPECT_EQ(std::string_view(failure->protocol_error().what()), message);
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

TEST(Ruvia, multipart_part_and_metadata_limits_are_cumulative) {
    constexpr std::string_view header_value = "Content-Disposition: form-data; name=\"field\"\r\n\r\n";
    std::string body;
    for (std::size_t index = 0; index < 3; ++index) {
        body += "--abc\r\n";
        body += header_value;
        body += "value\r\n";
    }
    body += "--abc--\r\n";

    const auto accepted = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_parts_ = 3, .max_metadata_bytes_ = 3 * header_value.size()}).take_parts();
    EXPECT_EQ(accepted.size(), std::size_t{3});
    const auto too_many = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_parts_ = 2});
    EXPECT_TRUE(too_many.failure() != nullptr);
    if (const auto* failure = too_many.failure()) {
        EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
    }
    const auto too_large = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_metadata_bytes_ = 3 * header_value.size() - 1});
    EXPECT_TRUE(too_large.failure() != nullptr);
    if (const auto* failure = too_large.failure()) {
        EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
    }

    ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_parts_ = 2});
    parser.feed(body);
    for (std::size_t index = 0; index < 4; ++index) {
        const auto result_value = parser.poll();
        if (index < 2) {
            EXPECT_TRUE(result_value.part() != nullptr);
        } else {
            EXPECT_TRUE(result_value.failure() != nullptr);
        }
    }
}

TEST(Ruvia, multipart_streaming_handles_bytewise_long_syntax_and_false_delimiters) {
    std::string body(16 * 1024, 'p');
    body += "\r\n--abc";
    body.append(16 * 1024, ' ');
    body += "\r\nContent-Disposition: form-data; name=\"field\"\r\nX-Long: ";
    body.append(16 * 1024, 'h');
    body += "\r\n\r\nvalue\r\n--ab";
    body.append(16 * 1024, ' ');
    body += "x\r\n--abc--";
    body.append(16 * 1024, '\t');
    body += "\r\n";

    const auto expected = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("abc")}).take_parts();
    EXPECT_TRUE(!expected.empty());
    ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("abc")});
    std::string received;
    std::size_t ended = 0;
    bool done = false;
    for (std::size_t offset = 0; offset < body.size(); ++offset) {
        parser.feed(std::string_view(body).substr(offset, 1));
        for (;;) {
            const auto result_value = parser.poll();
            EXPECT_TRUE(result_value.failure() == nullptr);
            if (const auto* part = result_value.part()) {
                received += part->body();
                if (part->phase() == ruvia::multipart_chunk_phase::complete || part->phase() == ruvia::multipart_chunk_phase::last) {
                    ++ended;
                }
            } else {
                done = result_value.done() != nullptr;
                break;
            }
        }
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(ended, std::size_t{1});
    if (!expected.empty()) {
        EXPECT_EQ(std::string_view(received), expected[0].body());
    }
}

TEST(Ruvia, multipart_part_may_end_at_its_header_block) {
    // RFC 2046 section 5.1.1: body-part = MIME-part-headers [CRLF *OCTET]. The
    // CRLF after the header block may be the one that starts the next delimiter,
    // and a body line cannot begin with the dash-boundary, so "--b" at the body
    // start is a delimiter rather than content that swallows the next part.
    constexpr std::string_view body =
        "--b\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n"
        "--b \t\r\nContent-Disposition: form-data; name=\"c\"\r\n\r\nv\r\n"
        "--b\r\nContent-Disposition: form-data; name=\"d\"\r\n\r\n--ax\r\n"
        "--b\r\nContent-Disposition: form-data; name=\"e\"\r\n\r\n--b--";

    const auto parts = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("b")}).take_parts();
    EXPECT_EQ(parts.size(), std::size_t{4});
    if (parts.size() == 4) {
        EXPECT_EQ(parts[0].name(), std::string_view("a"));
        EXPECT_EQ(parts[0].body(), std::string_view(""));
        EXPECT_EQ(parts[1].name(), std::string_view("c"));
        EXPECT_EQ(parts[1].body(), std::string_view("v"));
        EXPECT_EQ(parts[2].name(), std::string_view("d"));
        EXPECT_EQ(parts[2].body(), std::string_view("--ax"));
        EXPECT_EQ(parts[3].name(), std::string_view("e"));
        EXPECT_EQ(parts[3].body(), std::string_view(""));
    }

    for (const std::size_t step : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}}) {
        ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("b")});
        std::string received;
        std::size_t ended = 0;
        bool done = false;
        const auto drain = [&]() {
            for (;;) {
                const auto result_value = parser.poll();
                EXPECT_TRUE(result_value.failure() == nullptr);
                if (const auto* part = result_value.part()) {
                    if (part->phase() == ruvia::multipart_chunk_phase::first || part->phase() == ruvia::multipart_chunk_phase::complete) {
                        received += '[';
                        received += part->name();
                        received += ':';
                    }
                    received += part->body();
                    if (part->phase() == ruvia::multipart_chunk_phase::complete || part->phase() == ruvia::multipart_chunk_phase::last) {
                        received += ']';
                        ++ended;
                    }
                } else {
                    done = result_value.done() != nullptr;
                    return;
                }
            }
        };
        for (std::size_t offset = 0; offset < body.size(); offset += step) {
            parser.feed(body.substr(offset, step));
            drain();
        }
        parser.finish_input();
        drain();
        EXPECT_TRUE(done);
        EXPECT_EQ(ended, std::size_t{4});
        EXPECT_EQ(std::string_view(received), std::string_view("[a:][c:v][d:--ax][e:]"));
    }
}

TEST(Ruvia, multipart_preamble_limit_counts_misleading_and_incomplete_marker_bytes) {
    constexpr std::size_t preamble_limit = 64 * 1024;
    for (const std::string_view misleading : {"-x", "--abx\r\n", "-"}) {
        std::string wire(preamble_limit - 2, 'p');
        wire += "\r\n";
        wire += misleading;
        if (misleading != "-") {
            wire += "\r\n--abc\r\nContent-Disposition: form-data; name=field\r\n\r\nvalue\r\n--abc--\r\n";
        }
        const auto buffered = ruvia::parse_multipart_body(wire, {.boundary_ = ruvia::test::make_multipart_boundary("abc")});
        EXPECT_TRUE(buffered.failure() != nullptr);
        if (const auto* failure = buffered.failure()) {
            EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
        }

        ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("abc")});
        bool failed = false;
        for (std::size_t offset = 0; offset < wire.size() && !failed; ++offset) {
            parser.feed(std::string_view(wire).substr(offset, 1));
            const auto result_value = parser.poll();
            EXPECT_TRUE(result_value.part() == nullptr);
            EXPECT_TRUE(result_value.done() == nullptr);
            if (const auto* failure = result_value.failure()) {
                failed = true;
                EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
            }
        }
        if (!failed) {
            parser.finish_input();
            const auto result_value = parser.poll();
            failed = result_value.failure() != nullptr;
            if (const auto* failure = result_value.failure()) {
                EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::content_too_large);
            }
        }
        EXPECT_TRUE(failed);
    }
}

TEST(Ruvia, multipart_truncated_body_at_eof_fails_without_emitting_partial_part) {
    const std::string truncated = "--abc\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\n" + std::string(64, 'x');
    ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("abc")});
    parser.feed(truncated);
    parser.finish_input();
    const auto polled = parser.poll();
    EXPECT_TRUE(polled.part() == nullptr);
    EXPECT_TRUE(polled.failure() != nullptr);
}

TEST(Ruvia, multipart_part_ignores_filename_star_parameter) {
    const std::string body =
        "--b\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"a.txt\"; filename*=UTF-8''b.txt\r\n"
        "\r\n"
        "data\r\n"
        "--b--\r\n";
    const auto parts = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("b")}).take_parts();
    EXPECT_EQ(parts.size(), std::size_t{1});
    if (!parts.empty()) {
        EXPECT_EQ(parts[0].filename(), std::string_view("a.txt"));
        EXPECT_EQ(parts[0].body(), std::string_view("data"));
    }
}

TEST(Ruvia, multipart_rejects_boundary_line_that_is_not_a_delimiter) {
    for (const std::string_view body : {
             std::string_view("--b\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nX\r\n--bX\r\nY\r\n--b--\r\n"),
             std::string_view("--b\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nX\r\n--b-x\r\n--b--\r\n"),
             std::string_view("--b\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nX\r\n--b \rY\r\n--b--\r\n"),
             std::string_view("preamble\r\n--bX\r\n--b\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nX\r\n--b--\r\n"),
         }) {
        const auto parsed = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("b")});
        const auto* failure = parsed.failure();
        EXPECT_TRUE(failure != nullptr && std::string_view(failure->protocol_error().code()) == "multipart_invalid_delimiter");

        ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("b")});
        parser.feed(body);
        parser.finish_input();
        bool failed = false;
        for (int step = 0; step < 8 && !failed; ++step) {
            const auto result_value = parser.poll();
            failed = result_value.failure() != nullptr;
            if (result_value.done() != nullptr) {
                break;
            }
        }
        EXPECT_TRUE(failed);
    }
}

// Quoted names and boundaries in a multipart body, and the delimiter terminator each form requires.

TEST(Ruvia, multipart_boundary_quoted_with_mime_special) {
    const std::string_view content_type_value = R"(multipart/form-data; boundary="a:b")";
    const auto result_value = ruvia::parse_multipart_boundary(content_type_value);
    EXPECT_TRUE(result_value.boundary() != nullptr);
    EXPECT_EQ(result_value.boundary()->value(), std::string_view("a:b"));
}

TEST(Ruvia, multipart_part_fields_unfold_mime_continuations) {
    std::string wire =
        "--abc\r\n"
        "Content-Disposition: form-data;\r\n name=\"first\";\r\n\tfilename=\"first \r\n\tfile.txt\"\r\n"
        "Content-Type:\r\n text/plain\r\n\r\n"
        "one\r\n"
        "--abc\r\n"
        "Content-Disposition: form-data; name=second\r\n"
        "Content-Type: text/plain;\r\n charset=utf-8\r\n\r\n"
        "two\r\n"
        "--abc\r\n"
        "Content-Disposition: form-data; name=third\r\n"
        "Content-Type: application/json\r\n\r\n"
        "three\r\n"
        "--abc--\r\n";
    const std::array<std::string_view, 3> names{"first", "second", "third"};
    const std::array<std::string_view, 3> types{"text/plain", "text/plain; charset=utf-8", "application/json"};
    const std::array<std::string_view, 3> bodies{"one", "two", "three"};

    auto parsed = ruvia::parse_multipart_body(wire, {.boundary_ = ruvia::test::make_multipart_boundary("abc")}).take_parts();
    EXPECT_EQ(parsed.size(), names.size());
    for (std::size_t index = 0; index < parsed.size() && index < names.size(); ++index) {
        const auto& part = parsed[index];
        EXPECT_EQ(part.name(), names[index]);
        EXPECT_EQ(part.content_type(), types[index]);
        EXPECT_EQ(part.body(), bodies[index]);
    }
    if (!parsed.empty()) {
        EXPECT_EQ(parsed[0].filename(), std::string_view("first \tfile.txt"));
    }

    ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("abc")});
    std::array<std::string, 3> streamed;
    std::size_t part_index = 0;
    bool done = false;
    for (std::size_t offset = 0; offset < wire.size() && !done; ++offset) {
        parser.feed(std::string_view(wire).substr(offset, 1));
        for (;;) {
            const auto result = parser.poll();
            EXPECT_TRUE(result.failure() == nullptr);
            if (const auto* part = result.part()) {
                EXPECT_TRUE(part_index < names.size());
                if (part_index < names.size()) {
                    EXPECT_EQ(part->name(), names[part_index]);
                    EXPECT_EQ(part->content_type(), types[part_index]);
                    streamed[part_index].append(part->body());
                }
                if (part->phase() == ruvia::multipart_chunk_phase::complete || part->phase() == ruvia::multipart_chunk_phase::last) {
                    ++part_index;
                }
                continue;
            }
            done = result.done() != nullptr;
            break;
        }
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(part_index, names.size());
    for (std::size_t index = 0; index < streamed.size(); ++index) {
        EXPECT_EQ(std::string_view(streamed[index]), bodies[index]);
    }

    // Folded Content-Type values outlive the parser and survive moves even when
    // their normalized spelling fits the string's small inline buffer.
    auto moved = std::move(parsed);
    wire.assign(wire.size(), '#');
    if (moved.size() == names.size()) {
        EXPECT_EQ(moved[0].content_type(), types[0]);
        EXPECT_EQ(moved[1].content_type(), types[1]);
    }
}

TEST(Ruvia, multipart_unfolding_preserves_wire_limits_and_field_validation) {
    const std::string headers =
        "Content-Disposition: form-data;\r\n name=field\r\n"
        "Content-Type: text/plain;\r\n charset=utf-8\r\n\r\n";
    const std::string wire = "--abc\r\n" + headers + "value\r\n--abc--\r\n";
    const auto accepted = ruvia::parse_multipart_body(wire, {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_metadata_bytes_ = headers.size()}).take_parts();
    EXPECT_EQ(accepted.size(), std::size_t{1});
    const auto oversized = ruvia::parse_multipart_body(wire, {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_metadata_bytes_ = headers.size() - 1});
    EXPECT_TRUE(oversized.failure() != nullptr);
    if (const auto* failure = oversized.failure()) {
        EXPECT_TRUE(failure->protocol_error().status() == ruvia::http_status::content_too_large);
    }
    ruvia::multipart_parser limited({.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_metadata_bytes_ = headers.size() - 1});
    limited.feed(wire);
    limited.finish_input();
    const auto limited_result = limited.poll();
    EXPECT_TRUE(limited_result.failure() != nullptr);
    if (const auto* failure = limited_result.failure()) {
        EXPECT_TRUE(failure->protocol_error().status() == ruvia::http_status::content_too_large);
    }

    for (const std::string_view invalid : {" Content-Disposition: form-data; name=field", "Content-Disposition: form-data;\n name=field", "Content-Disposition: form-data;\r name=field", "Content-Disposition: form-data; name=field\r\n Content-Type: text/plain", "Content-Disposition: form-data; name=field\r\nContent-Disposition:\r\n form-data; name=other"}) {
        const std::string malformed = "--abc\r\n" + std::string(invalid) + "\r\n\r\nvalue\r\n--abc--\r\n";
        const auto buffered = ruvia::parse_multipart_body(malformed, {.boundary_ = ruvia::test::make_multipart_boundary("abc")});
        EXPECT_TRUE(buffered.failure() != nullptr);
        ruvia::multipart_parser parser({.boundary_ = ruvia::test::make_multipart_boundary("abc")});
        parser.feed(malformed);
        parser.finish_input();
        const auto streamed = parser.poll();
        const auto repeated = parser.poll();
        EXPECT_TRUE(streamed.failure() != nullptr);
        EXPECT_TRUE(repeated.failure() != nullptr);
        if (streamed.failure() != nullptr && repeated.failure() != nullptr) {
            EXPECT_EQ(std::string_view(streamed.failure()->protocol_error().what()), std::string_view(repeated.failure()->protocol_error().what()));
        }
    }
}

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <span>
#include <variant>
#include <vector>

#include "http1_client_request.h"
#include "http1_server_response.h"
#include "http_capsule.h"
#include "http_connect_udp.h"
#include "http_header.h"
namespace {

using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::http_protocol_error;
using ruvia::http_reason_phrase;

template <typename value_type>
constexpr bool can_construct_received_status = requires(value_type value) { ruvia::http_received_status::try_from_value(value); };

enum received_status_enum : unsigned { received_ok = 200 };

static_assert(can_construct_received_status<std::int64_t>);
static_assert(can_construct_received_status<std::uint64_t>);
static_assert(can_construct_received_status<char>);
static_assert(can_construct_received_status<bool>);
static_assert(!can_construct_received_status<float>);
static_assert(!can_construct_received_status<double>);
static_assert(!can_construct_received_status<long double>);
static_assert(!can_construct_received_status<received_status_enum>);

}  // namespace

TEST(Ruvia, http_status_code_validates_the_wire_value_boundary) {
    EXPECT_TRUE(ruvia::http_status_code::try_from_value(100) == ruvia::http_status::continue_value);
    EXPECT_TRUE(ruvia::http_status_code::try_from_value(599) == ruvia::http_status_code::from_value(599));
    EXPECT_TRUE(!ruvia::http_status_code::try_from_value(99).has_value());
    EXPECT_TRUE(!ruvia::http_status_code::try_from_value(600).has_value());

    bool threw = false;
    try {
        (void)ruvia::http_status_code::from_value(600);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, http_status_code_rejects_original_integers_before_wire_narrowing) {
    const auto check_rejected = [&](auto input) {
        EXPECT_TRUE(!ruvia::http_status_code::is_valid_value(input));
        EXPECT_TRUE(!ruvia::http_status_code::try_from_value(input).has_value());
        bool threw = false;
        try {
            (void)ruvia::http_status_code::from_value(input);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
    };
    check_rejected(std::int64_t{65736});
    check_rejected(std::int64_t{-65336});
    check_rejected((std::numeric_limits<std::int64_t>::min)());
    check_rejected((std::numeric_limits<std::int64_t>::max)());
    check_rejected(std::uint64_t{65736});
    check_rejected((std::numeric_limits<std::uint64_t>::max)());

    EXPECT_TRUE(ruvia::http_status_code::try_from_value(std::int64_t{100}) == ruvia::http_status::continue_value);
    EXPECT_TRUE(ruvia::http_status_code::try_from_value(std::uint64_t{599}) == ruvia::http_status_code::from_value(599));
}

TEST(Ruvia, http_received_status_rejects_original_integers_before_wire_narrowing) {
    for (const auto value : {std::int64_t{-1}, std::int64_t{-65336}, std::int64_t{1000}, std::int64_t{65736}, std::int64_t{4'294'967'496}, (std::numeric_limits<std::int64_t>::min)(), (std::numeric_limits<std::int64_t>::max)()}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(ruvia::http_received_status::try_from_value(value).has_value());
    }
    for (const auto value : {std::uint64_t{1000}, std::uint64_t{65736}, std::uint64_t{4'294'967'496}, (std::numeric_limits<std::uint64_t>::max)() - 65'335, (std::numeric_limits<std::uint64_t>::max)()}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(ruvia::http_received_status::try_from_value(value).has_value());
    }
}

TEST(Ruvia, http_received_status_preserves_every_three_digit_value_and_its_semantics) {
    for (std::uint64_t value = 0; value <= 999; ++value) {
        SCOPED_TRACE(value);
        const auto received = ruvia::http_received_status::try_from_value(value);
        ASSERT_TRUE(received.has_value());
        EXPECT_EQ(received->value(), value);
        const auto expected = value >= 100 && value <= 599 ? ruvia::http_status_code::from_value(value) : ruvia::http_status::internal_server_error;
        EXPECT_EQ(received->status(), expected);
    }
}

TEST(Ruvia, http_reason_phrase_is_conventional_http1_presentation) {
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::ok), std::string_view("OK"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::not_found), std::string_view("Not Found"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::content_too_large), std::string_view("Content Too Large"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::internal_server_error), std::string_view("Internal Server Error"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::reset_content), std::string_view("Reset Content"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::bad_gateway), std::string_view("Bad Gateway"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::gateway_timeout), std::string_view("Gateway Timeout"));
}

TEST(Ruvia, http_reason_phrase_does_not_mislabel_extension_statuses) {
    // 104 is still a temporary draft registration, so its unstable name is not
    // promoted into the framework's stable public vocabulary.
    EXPECT_TRUE(http_reason_phrase(ruvia::http_status_code::from_value(104)).empty());
    EXPECT_TRUE(http_reason_phrase(ruvia::http_status_code::from_value(299)).empty());
    EXPECT_TRUE(http_reason_phrase(ruvia::http_status_code::from_value(499)).empty());
    EXPECT_TRUE(http_reason_phrase(ruvia::http_status_code::from_value(599)).empty());
}

TEST(Ruvia, http_protocol_error_owns_bounded_diagnostic_without_allocation) {
    std::string source_value(200, 'x');
    const http_protocol_error error(ruvia::http_status::content_too_large, "body_too_large", source_value);
    source_value.assign(200, 'y');

    EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
    EXPECT_EQ(error.code(), std::string_view("body_too_large"));
    const auto diagnostic = std::string_view(error.what());
    EXPECT_EQ(diagnostic.size(), std::size_t{127});
    EXPECT_TRUE(diagnostic.find_first_not_of('x') == std::string_view::npos);
}

TEST(Ruvia, parse_error_status_mapping) {
    // Size limits map to their specific statuses.
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::header_too_large).status(), ruvia::http_status::request_header_fields_too_large);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::uri_too_long).status(), ruvia::http_status::uri_too_long);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::unsupported_transfer_encoding).status(), ruvia::http_status::not_implemented);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::unsupported_http_version).status(), ruvia::http_status::http_version_not_supported);
    // Everything else is a 400 Bad Request.
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::missing_host).status(), ruvia::http_status::bad_request);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::invalid_connection).status(), ruvia::http_status::bad_request);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::invalid_upgrade).status(), ruvia::http_status::bad_request);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::conflicting_content_length).status(), ruvia::http_status::bad_request);
}

TEST(Ruvia, http_connect_udp_default_template_roundtrip_and_target_rejection) {
    for (const auto host : {"example.test", "192.0.2.6", "2001:db8::42"}) {
        const auto path = ruvia::encode_http_connect_udp_path({.host_ = host, .port_ = 443});
        EXPECT_TRUE((path.index() == 0));
        const auto target = ruvia::parse_http_connect_udp_path(std::get<0>(path));
        EXPECT_TRUE((target.index() == 0) && std::get<0>(target).host_ == host && std::get<0>(target).port_ == 443);
    }
    EXPECT_TRUE((ruvia::encode_http_connect_udp_path({.host_ = "fe80::1%eth0", .port_ = 80}).index() != 0));
    EXPECT_TRUE((ruvia::encode_http_connect_udp_path({.host_ = "[::1]", .port_ = 80}).index() != 0));
    EXPECT_TRUE((ruvia::encode_http_connect_udp_path({.host_ = "example.test", .port_ = 0}).index() != 0));
    EXPECT_TRUE((ruvia::parse_http_connect_udp_path("/.well-known/masque/udp/2001:db8::42/443/").index() != 0));
    EXPECT_TRUE((ruvia::parse_http_connect_udp_path("/.well-known/masque/udp/host/65536/").index() != 0));
}
TEST(Ruvia, http_connect_udp_default_template_requires_target_segments) {
    constexpr std::string_view prefix = "/.well-known/masque/udp/";
    for (std::size_t size = 0; size <= prefix.size(); ++size) {
        const auto parsed_value = ruvia::parse_http_connect_udp_path(prefix.substr(0, size));
        EXPECT_TRUE(std::get_if<ruvia::http_connect_udp_error>(&parsed_value) != nullptr);
        if (const auto* error = std::get_if<ruvia::http_connect_udp_error>(&parsed_value)) {
            EXPECT_TRUE(*error == ruvia::http_connect_udp_error::invalid_path);
        }
    }
    for (const auto path : {"/.well-known/masque/udp//", "/.well-known/masque/udp/host/", "/.well-known/masque/udp/host/443", "/.well-known/masque/udp/host/443/extra/"}) {
        const auto parsed_value = ruvia::parse_http_connect_udp_path(path);
        EXPECT_TRUE(std::get_if<ruvia::http_connect_udp_error>(&parsed_value) != nullptr);
    }
}
TEST(Ruvia, http_connect_udp_upgrade_handshake_requires_capsule_negotiation) {
    const std::array fields{ruvia::http_header_view{"Host", "example.test"}, ruvia::http_header_view{"Connection", "Upgrade"}, ruvia::http_header_view{"Upgrade", "connect-udp"}, ruvia::http_header_view{"Capsule-Protocol", "?1;version=1"}};
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request({.authority_ = "example.test", .path_ = "/proxy", .headers_ = fields}).has_value());
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101, fields).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request({.version_ = ruvia::http_protocol_version::http10, .authority_ = "example.test", .path_ = "/proxy", .headers_ = fields}).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request({.method_ = "CONNECT", .authority_ = "example.test", .path_ = "/proxy", .headers_ = fields}).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 200, fields).has_value());
    EXPECT_TRUE(!ruvia::parse_http_capsule_protocol("?1, ?0").has_value());
    EXPECT_TRUE(!ruvia::parse_http_capsule_protocol("true").has_value());
    const auto disabled = ruvia::parse_http_capsule_protocol("?0");
    EXPECT_TRUE(disabled.has_value() && !*disabled);
    const std::array bad_resp{ruvia::http_header_view{"Upgrade", "websocket"}};
    const auto resp_err = ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101, bad_resp);
    EXPECT_TRUE(resp_err == ruvia::http_connect_udp_error::invalid_response);
    const auto abs_req = ruvia::make_parsed_http_request("GET", "https://example.test/.well-known/masque/udp/192.0.2.1/443/", fields, {});
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(abs_req).has_value());
    const auto abs_query_req = ruvia::make_parsed_http_request("GET", "https://example.test/.well-known/masque/udp/192.0.2.1/443/?target=1", fields, {});
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(abs_query_req).has_value());
}
TEST(Ruvia, http_udp_capsule_session_checks_context_size_and_half_close) {
    ruvia::http_datagram_session session;
    const std::string payload = "udp";
    const auto plan = session.prepare_udp_datagram(payload);
    EXPECT_TRUE(plan.index() == 0);
    std::string wire(std::get<0>(plan).prefix());
    wire.append(payload);
    std::string capsule;
    struct capsule_capture final {
        std::string& bytes_;
        bool datagram_{true};
    } captured{capsule};
    ruvia::http_capsule_decoder decoder;
    const auto status = decoder.feed(wire, true, [](void* context, ruvia::http_capsule_event event) {
        auto& capture = *static_cast<capsule_capture*>(context);
        capture.datagram_ = capture.datagram_ && event.type_ == ruvia::http_datagram_capsule_type;
        if (!event.payload_.empty()) {
            capture.bytes_.append(event.payload_.data(), event.payload_.size());
        }
    }, &captured);
    EXPECT_TRUE(captured.datagram_);
    EXPECT_TRUE(status == ruvia::http_capsule_status::end);
    const auto value = std::string_view(capsule);
    const auto received = session.receive_udp_datagram(value);
    EXPECT_TRUE(received.index() == 0 && std::get<0>(received));
    EXPECT_TRUE(std::string_view(std::get<0>(received)->payload_.data(), std::get<0>(received)->payload_.size()) == payload);
    capsule[0] = 2;
    const auto dropped = session.receive_udp_datagram(value);
    EXPECT_TRUE(dropped.index() == 0 && !std::get<0>(dropped));
    const std::string too_large(65528, 'x');
    EXPECT_TRUE(session.prepare_udp_datagram(too_large).index() != 0);
    session.close_send();
    session.close_receive();
    EXPECT_TRUE(session.prepare_udp_datagram(payload).index() != 0);
    const auto closed = session.receive_udp_datagram(value);
    EXPECT_TRUE(closed.index() == 0 && !std::get<0>(closed));
}

TEST(Ruvia, http_connect_udp_response_preparation_owns_required_fields_and_http1_upgrade_framing) {
    ruvia::http_response response;
    response.status(ruvia::http_status::created);
    response.header("X-Proxy", "test");
    auto prepared = ruvia::prepare_http_connect_udp_response(std::move(response), ruvia::http_protocol_version::http11);
    EXPECT_TRUE((prepared.index() == 0));
    if (prepared.index() == 0) {
        EXPECT_TRUE(std::get<0>(prepared).header("capsule-protocol") == "?1");
        EXPECT_TRUE(std::get<0>(prepared).header("x-proxy") == "test");
        EXPECT_TRUE(std::get<0>(prepared).status() == ruvia::http_status::switching_protocols);
        const auto plan = ruvia::prepare_http1_connect_udp_response_head(std::get<0>(prepared));
        ruvia::http_response_head_buffer head;
        ruvia::append_http1_response_head(std::get<0>(prepared), head, plan);
        EXPECT_TRUE(head.view().starts_with("HTTP/1.1 101 "));
        EXPECT_TRUE(head.view().find("Connection: Upgrade\r\n") != std::string_view::npos);
        EXPECT_TRUE(head.view().find("Content-Length:") == std::string_view::npos);
        EXPECT_TRUE(head.view().find("Transfer-Encoding:") == std::string_view::npos);
    }
    ruvia::http_response parameterized;
    parameterized.status(ruvia::http_status::ok);
    parameterized.header("Capsule-Protocol", "?1;version=1");
    auto prepared_param = ruvia::prepare_http_connect_udp_response(std::move(parameterized), ruvia::http_protocol_version::http2);
    EXPECT_TRUE(prepared_param.index() == 0);
    if (prepared_param.index() == 0) {
        EXPECT_EQ(std::get<0>(prepared_param).header("capsule-protocol").value_or(""), "?1;version=1");
    }
    for (const auto field : {"Content-Length", "Transfer-Encoding", "Content-Type", "Content-Encoding", "Connection", "Upgrade", "Trailer"}) {
        ruvia::http_response invalid;
        invalid.header(field, field == std::string_view("Content-Length") ? "0" : (field == std::string_view("Content-Type") ? "application/octet-stream" : "test"));
        EXPECT_TRUE((ruvia::prepare_http_connect_udp_response(std::move(invalid), ruvia::http_protocol_version::http11).index() != 0));
    }
    ruvia::http_response disabled;
    disabled.header("Capsule-Protocol", "?0");
    EXPECT_TRUE((ruvia::prepare_http_connect_udp_response(std::move(disabled), ruvia::http_protocol_version::http11).index() != 0));
    ruvia::http_response content;
    content.body("forbidden");
    EXPECT_TRUE((ruvia::prepare_http_connect_udp_response(std::move(content), ruvia::http_protocol_version::http11).index() != 0));
    // Only the 101 head prepare_http_connect_udp_response() produces has an HTTP/1 plan.
    ruvia::http_response unprepared;
    unprepared.status(ruvia::http_status::ok);
    bool rejected = false;
    try {
        (void)ruvia::prepare_http1_connect_udp_response_head(unprepared);
    } catch (const ruvia::http1_response_head_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, http_connect_udp_http1_request_writer_generates_upgrade_without_message_content) {
    ruvia::http1_client_request_writer writer;
    std::array<char, 4096> buffer;
    const std::array fields_value{ruvia::http_header_view{"Capsule-Protocol", "?1"}, ruvia::http_header_view{"X-Proxy", "test"}};
    const auto origin = ruvia::http_origin_view::https({.host_ = "[::1]", .port_ = 8443});
    auto result_value = writer.prepare_connect_udp(origin, "/udp", fields_value, buffer);
    const auto* const prepared = std::get_if<ruvia::prepared_http1_client_request>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        const auto head = prepared->head();
        EXPECT_TRUE(head.starts_with("GET /udp HTTP/1.1\r\n"));
        EXPECT_TRUE(head.find("Host: [::1]:8443\r\n") != std::string_view::npos);
        EXPECT_TRUE(head.find("Upgrade: connect-udp\r\n") != std::string_view::npos);
        EXPECT_TRUE(head.find("Content-Length:") == std::string_view::npos);
        EXPECT_TRUE(head.find("Transfer-Encoding:") == std::string_view::npos);
    }
    auto missing = writer.prepare_connect_udp(origin, "/udp", {}, buffer);
    EXPECT_TRUE(std::holds_alternative<ruvia::http1_client_request_prepare_error>(missing));
    const std::array invalid{ruvia::http_header_view{"Capsule-Protocol", "?0"}};
    auto disabled = writer.prepare_connect_udp(origin, "/udp", invalid, buffer);
    EXPECT_TRUE(std::holds_alternative<ruvia::http1_client_request_prepare_error>(disabled));
    auto bad_target = writer.prepare_connect_udp(origin, "relative", fields_value, buffer);
    EXPECT_TRUE(std::holds_alternative<ruvia::http1_client_request_prepare_error>(bad_target) && std::get<ruvia::http1_client_request_prepare_error>(bad_target) == ruvia::http1_client_request_prepare_error::invalid_target);
    const std::array with_host{fields_value[0], ruvia::http_header_view{"Host", "[::1]:8443"}};
    auto managed_host = writer.prepare_connect_udp(origin, "/udp", with_host, buffer);
    EXPECT_TRUE(std::holds_alternative<ruvia::http1_client_request_prepare_error>(managed_host) && std::get<ruvia::http1_client_request_prepare_error>(managed_host) == ruvia::http1_client_request_prepare_error::host_header_managed_by_writer);
}

TEST(Ruvia, http_connect_udp_http2_uses_extended_connect_without_upgrade_fields) {
    const std::array fields{ruvia::http_header_view{"capsule-protocol", "?1"}};
    const ruvia::http_connect_udp_request_view request{.version_ = ruvia::http_protocol_version::http2, .method_ = "CONNECT", .protocol_ = "connect-udp", .scheme_ = "https", .authority_ = "example.test", .path_ = "/proxy", .headers_ = fields};
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(request).has_value());
    auto invalid = request;
    invalid.method_ = "GET";
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request(invalid).has_value());
    invalid = request;
    invalid.protocol_ = "websocket";
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request(invalid).has_value());
    invalid = request;
    invalid.scheme_ = "";
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request(invalid).has_value());
    const std::array upgrade_fields{fields[0], ruvia::http_header_view{"connection", "upgrade"}, ruvia::http_header_view{"upgrade", "connect-udp"}};
    invalid = request;
    invalid.headers_ = upgrade_fields;
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request(invalid).has_value());
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, fields).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 101, fields).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, upgrade_fields).has_value());

    ruvia::http_response response;
    response.status(ruvia::http_status::created);
    auto prepared = ruvia::prepare_http_connect_udp_response(std::move(response), ruvia::http_protocol_version::http2);
    EXPECT_TRUE(prepared.index() == 0);
    if (prepared.index() == 0) {
        EXPECT_TRUE(std::get<0>(prepared).status() == ruvia::http_status::created);
        EXPECT_TRUE(std::get<0>(prepared).header("capsule-protocol") == "?1");
        EXPECT_TRUE(!std::get<0>(prepared).header("connection").has_value());
        EXPECT_TRUE(!std::get<0>(prepared).header("upgrade").has_value());
    }
}

TEST(Ruvia, http_connect_udp_recipient_treats_capsule_protocol_as_optional) {
    const std::array upgrade{ruvia::http_header_view{"Host", "example.test"}, ruvia::http_header_view{"Connection", "Upgrade"}, ruvia::http_header_view{"Upgrade", "connect-udp"}};
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request({.authority_ = "example.test", .path_ = "/proxy", .headers_ = upgrade}).has_value());
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101, std::span<const ruvia::http_header_view>(upgrade).subspan(1)).has_value());
    for (const auto value : {"1", "?0", "\"yes\""}) {
        const std::array ignored{ruvia::http_header_view{"Capsule-Protocol", value}};
        const ruvia::http_connect_udp_request_view request{.version_ = ruvia::http_protocol_version::http2, .method_ = "CONNECT", .authority_ = "example.test", .path_ = "/proxy", .headers_ = ignored};
        EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(request).has_value());
        EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, ignored).has_value());
    }
    const std::array duplicate{ruvia::http_header_view{"Capsule-Protocol", "?1"}, ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, duplicate).has_value());
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, std::span<const ruvia::http_header_view>{}).has_value());
}

TEST(Ruvia, http_connect_udp_rejects_statuses_that_cannot_start_capsule_protocol) {
    const std::array capsule{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    for (const std::uint16_t status : {std::uint16_t{204}, std::uint16_t{205}, std::uint16_t{206}}) {
        EXPECT_TRUE(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, status, capsule) == ruvia::http_connect_udp_error::invalid_response);
        ruvia::http_response response;
        response.status(ruvia::http_status_code::from_value(status));
        EXPECT_TRUE(ruvia::prepare_http_connect_udp_response(std::move(response), ruvia::http_protocol_version::http2).index() != 0);
    }
}

TEST(Ruvia, http_connect_udp_sender_requires_exactly_one_capsule_protocol_true) {
    const auto check = [](std::span<const ruvia::http_header_view> fields) {
        return ruvia::validate_http_connect_udp_request_to_send({.version_ = ruvia::http_protocol_version::http2, .method_ = "CONNECT", .authority_ = "example.test", .path_ = "/proxy", .headers_ = fields});
    };
    EXPECT_TRUE(check({}).has_value());
    const std::array false_value{ruvia::http_header_view{"Capsule-Protocol", "?0"}};
    EXPECT_TRUE(check(false_value).has_value());
    const std::array true_value{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    EXPECT_TRUE(!check(true_value).has_value());
}

TEST(Ruvia, http_connect_udp_sender_and_recipient_field_validation) {
    // Sender rejects empty Connection list members and end-to-end field names (RFC 9110 5.6.1.1, 7.6.1).
    const std::array sender_bad_connection{
        ruvia::http_header_view{"Host", "example.test"},
        ruvia::http_header_view{"Connection", ", Upgrade, Cache-Control, Accept"},
        ruvia::http_header_view{"Upgrade", "connect-udp"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({.version_ = ruvia::http_protocol_version::http11, .method_ = "GET", .authority_ = "example.test", .path_ = "/.well-known/masque/udp/192.0.2.1/443/", .headers_ = sender_bad_connection}).has_value());

    // Sender validates Host syntax and authority match.
    const std::array bad_host_h1{
        ruvia::http_header_view{"Host", "invalid@host:99999"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", "connect-udp"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({.version_ = ruvia::http_protocol_version::http11, .method_ = "GET", .authority_ = "example.test", .path_ = "/proxy", .headers_ = bad_host_h1}).has_value());

    const std::array mismatched_host_h1{
        ruvia::http_header_view{"Host", "other.test"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", "connect-udp"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({.version_ = ruvia::http_protocol_version::http11, .method_ = "GET", .authority_ = "example.test", .path_ = "/proxy", .headers_ = mismatched_host_h1}).has_value());

    // HTTP/2 rejects connection-specific headers (Keep-Alive, Proxy-Connection, TE) and non-http(s) schemes.
    const std::array h2_keep_alive{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Keep-Alive", "timeout=5"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({.version_ = ruvia::http_protocol_version::http2, .method_ = "CONNECT", .protocol_ = "connect-udp", .scheme_ = "https", .authority_ = "example.test", .path_ = "/proxy", .headers_ = h2_keep_alive}).has_value());
    const std::array valid_capsule{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({.version_ = ruvia::http_protocol_version::http2, .method_ = "CONNECT", .protocol_ = "connect-udp", .scheme_ = "not a valid scheme!", .authority_ = "example.test", .path_ = "/proxy", .headers_ = valid_capsule}).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, h2_keep_alive).has_value());
    ruvia::http_response h2_resp;
    h2_resp.status(ruvia::http_status::ok);
    h2_resp.header("Keep-Alive", "timeout=5");
    EXPECT_TRUE(ruvia::prepare_http_connect_udp_response(std::move(h2_resp), ruvia::http_protocol_version::http2).index() != 0);

    // Recipient parses Upgrade = #protocol (ignoring empty elements and allowing additional tokens on requests).
    const std::array list_fields{
        ruvia::http_header_view{"Host", "example.test"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", ", connect-udp, fallback"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    const auto parsed_req = ruvia::make_parsed_http_request("GET", "/.well-known/masque/udp/192.0.2.1/443/", list_fields, {});
    EXPECT_TRUE(ruvia::is_http_connect_udp_upgrade_request(parsed_req));
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(parsed_req).has_value());

    const std::array empty_elem_resp{
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", ", connect-udp,"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101, empty_elem_resp).has_value());

    // RFC 9297 3.2: a Capsule Protocol message that carries Content-Length is
    // malformed regardless of its value.
    const std::array cl0_req{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Content-Length", "0"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request({.version_ = ruvia::http_protocol_version::http2, .method_ = "CONNECT", .protocol_ = "connect-udp", .scheme_ = "https", .authority_ = "example.test", .path_ = "/proxy", .headers_ = cl0_req}).has_value());
    const std::array cl5_req{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Content-Length", "5"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request({.version_ = ruvia::http_protocol_version::http2, .method_ = "CONNECT", .protocol_ = "connect-udp", .scheme_ = "https", .authority_ = "example.test", .path_ = "/proxy", .headers_ = cl5_req}).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, cl0_req).has_value());

    // RFC 9110 7.8: Upgrade is ignored on HTTP/1.0 requests.
    ruvia::http1_server_request_parse_state h10_parser;
    constexpr std::string_view raw_h10 =
        "GET /.well-known/masque/udp/192.0.2.1/443/ HTTP/1.0\r\n"
        "Host: example.test\r\n"
        "Upgrade: connect-udp\r\n\r\n";
    h10_parser.parse_head(raw_h10, 0);
    EXPECT_TRUE(h10_parser.head_ready() != nullptr);
    EXPECT_TRUE(!ruvia::is_http_connect_udp_upgrade_request(h10_parser.request()));

    // RFC 9113 8.2.2: TE: trailers is allowed on HTTP/2 requests, while other TE
    // values and TE on responses are forbidden.
    const std::array h2_te_trailers{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"te", "trailers"},
    };
    const ruvia::http_connect_udp_request_view h2_te_req{
        .version_ = ruvia::http_protocol_version::http2,
        .method_ = "CONNECT",
        .protocol_ = "connect-udp",
        .scheme_ = "https",
        .authority_ = "example.test",
        .path_ = "/.well-known/masque/udp/192.0.2.1/443/",
        .headers_ = h2_te_trailers,
    };
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(h2_te_req).has_value());
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request_to_send(h2_te_req).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, h2_te_trailers).has_value());
    const std::array h2_te_gzip{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"te", "gzip"},
    };
    auto h2_bad_te_req = h2_te_req;
    h2_bad_te_req.headers_ = h2_te_gzip;
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request(h2_bad_te_req).has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send(h2_bad_te_req).has_value());
}

TEST(Ruvia, http_connect_udp_views_accept_named_owners_and_preserve_addresses) {
    std::string host("example.test");
    std::string method("CONNECT");
    std::string protocol("connect-udp");
    std::string scheme("https");
    std::string authority("example.test");
    std::string path("/.well-known/masque/udp/192.0.2.1/443/");
    const ruvia::http_connect_udp_target_view target_view{.host_ = host, .port_ = 443};
    const ruvia::http_client_udp_tunnel_request_view tunnel_view{.target_ = path};
    const std::array fields{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    const ruvia::http_connect_udp_request_view request_view{
        .version_ = ruvia::http_protocol_version::http2,
        .method_ = method,
        .protocol_ = protocol,
        .scheme_ = scheme,
        .authority_ = authority,
        .path_ = path,
        .headers_ = fields,
    };
    EXPECT_TRUE(target_view.host_.data() == host.data());
    EXPECT_TRUE(tunnel_view.target_.data() == path.data());
    EXPECT_TRUE(request_view.method_.data() == method.data());
    EXPECT_TRUE(request_view.protocol_.data() == protocol.data());
    EXPECT_TRUE(request_view.scheme_.data() == scheme.data());
    EXPECT_TRUE(request_view.authority_.data() == authority.data());
    EXPECT_TRUE(request_view.path_.data() == path.data());
    const auto encoded = ruvia::encode_http_connect_udp_path(target_view);
    EXPECT_TRUE(std::holds_alternative<std::string>(encoded));
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request_to_send(request_view).has_value());
}

namespace {
struct capture {
    std::string body_;
    std::uint64_t type_{0};
    std::size_t ends_{0};
};
void capsule(void* context_value, ruvia::http_capsule_event event) {
    auto& capture_value = *static_cast<capture*>(context_value);
    capture_value.type_ = event.type_;
    if (!event.payload_.empty()) {
        capture_value.body_.append(event.payload_.data(), event.payload_.size());
    }
    capture_value.ends_ += event.end_capsule_;
}
}  // namespace
TEST(Ruvia, http_capsule_protocol_byte_parameters_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"?1;bytes=:YQ=:", "?1;bytes=:YQ==:", "?1;bytes=:YQ:", "?1;bytes=:YWJj:", "?1;bytes=:YWJ:", "?1;bytes=:YWJ=:", "?1;bytes=:YR==:", "?1;bytes=::"}) {
        const auto parsed_value = ruvia::parse_http_capsule_protocol(value);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value.has_value()) {
            EXPECT_TRUE(*parsed_value);
        }
    }
    const auto disabled = ruvia::parse_http_capsule_protocol("?0;bytes=:YQ=:");
    EXPECT_TRUE(disabled.has_value());
    if (disabled.has_value()) {
        EXPECT_TRUE(!*disabled);
    }
    // RFC 8941 4.2 discards only SP around a top-level Item.
    for (const auto value : {"?1;bytes=:Y:", "?1;bytes=:Y=:", "?1;bytes=:YWJj=:", "?1;bytes=:YWJ==:", "?1;bytes=:YQ===:", "?1;bytes=:Y=Q:", "?1;bytes=:YQ$:", "?1;bytes=:YQ\n:", "?1;bytes=:YQ", "?1\t", "?1 \t", "\t?1"}) {
        EXPECT_TRUE(!ruvia::parse_http_capsule_protocol(value).has_value());
    }
    const auto padded = ruvia::parse_http_capsule_protocol(" ?1  ");
    EXPECT_TRUE(padded.has_value() && *padded);
}

TEST(Ruvia, http_udp_datagram_context_prefix_roundtrips) {
    std::array<char, 8> prefix{};
    for (const auto id : {std::uint64_t{0}, std::uint64_t{63}, std::uint64_t{64}, std::uint64_t{16384}, (std::uint64_t{1} << 30), (std::uint64_t{1} << 62) - 1}) {
        const auto encoded = ruvia::encode_http_udp_datagram_prefix(prefix, id);
        EXPECT_TRUE(encoded.index() == 0);
        const auto decoded = ruvia::decode_http_udp_datagram(std::span<const char>(prefix).first(std::get<0>(encoded)));
        EXPECT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->context_id_, id);
        EXPECT_TRUE(decoded->payload_.empty());
    }
    EXPECT_TRUE(!ruvia::decode_http_udp_datagram({}).has_value());
    EXPECT_TRUE(ruvia::encode_http_udp_datagram_prefix(prefix, std::uint64_t{1} << 62).index() != 0);
}
TEST(Ruvia, http_capsule_incremental_unknown_empty_and_datagram) {
    std::array<char, 16> prefix{};
    auto count = ruvia::encode_http_capsule_header(prefix, 0x1234, 5);
    std::string wire(prefix.data(), std::get<0>(count));
    wire += "hello";
    auto empty = ruvia::encode_http_capsule_header(prefix, 0, 0);
    wire.append(prefix.data(), std::get<0>(empty));
    ruvia::http_capsule_decoder decoder;
    capture capture_state;
    for (const char& byte : wire) {
        EXPECT_TRUE(decoder.feed({&byte, 1}, false, capsule, &capture_state) == ruvia::http_capsule_status::need_more_data);
    }
    EXPECT_EQ(capture_state.body_, std::string("hello"));
    EXPECT_EQ(capture_state.ends_, 2u);
    EXPECT_TRUE(decoder.feed({}, true, capsule, &capture_state) == ruvia::http_capsule_status::end);
    ruvia::http_capsule_decoder lambda_decoder;
    capture lambda_capture;
    EXPECT_TRUE(lambda_decoder.feed(wire, true, [&](ruvia::http_capsule_event event) {
        capsule(&lambda_capture, event);
    }) == ruvia::http_capsule_status::end);
    EXPECT_EQ(lambda_capture.body_, std::string("hello"));
    EXPECT_EQ(lambda_capture.ends_, 2u);
    ruvia::http_capsule_decoder pull_lambda_decoder;
    capture pull_lambda_capture;
    const auto pull_first = pull_lambda_decoder.feed_one(wire, true, [&](ruvia::http_capsule_event event) {
        capsule(&pull_lambda_capture, event);
    });
    EXPECT_TRUE(pull_first.capsule_complete_);
    EXPECT_EQ(pull_lambda_capture.body_, std::string("hello"));
}
TEST(Ruvia, http_capsule_truncated_and_length_limit_are_terminal) {
    const std::array prefix{char(0), char(3)};
    ruvia::http_capsule_decoder limit({.max_capsule_length_ = 2});
    EXPECT_TRUE(limit.feed(prefix, false, nullptr, nullptr) == ruvia::http_capsule_status::limit);
    ruvia::http_capsule_decoder truncated;
    EXPECT_TRUE(truncated.feed(prefix, true, nullptr, nullptr) == ruvia::http_capsule_status::truncated);
}

TEST(Ruvia, http_capsule_pull_decoder_stops_at_each_capsule_and_commits_fin_after_the_suffix) {
    std::array<char, 16> header;
    std::string wire;
    for (const auto& item : std::array<std::pair<std::uint64_t, std::string_view>, 3>{{{0x123456789ULL, "first"}, {0, ""}, {7, "last"}}}) {
        const auto encoded = ruvia::encode_http_capsule_header(header, item.first, item.second.size());
        wire.append(header.data(), std::get<0>(encoded));
        wire.append(item.second);
    }
    for (std::size_t block : {std::size_t{1}, std::size_t{3}, wire.size()}) {
        ruvia::http_capsule_decoder decoder;
        capture captured;
        std::size_t offset{}, complete_value{};
        std::string all_payload;
        while (offset != wire.size()) {
            const auto count = std::min(block, wire.size() - offset);
            const auto result_value = decoder.feed_one(std::span<const char>(wire).subspan(offset, count), offset + count == wire.size(), capsule, &captured);
            EXPECT_TRUE(result_value.consumed_bytes_ > 0 && result_value.consumed_bytes_ <= count);
            offset += result_value.consumed_bytes_;
            if (result_value.capsule_complete_) {
                const std::array<std::uint64_t, 3> expected_types{0x123456789ULL, 0, 7};
                const std::array<std::string_view, 3> expected_payloads{"first", "", "last"};
                EXPECT_TRUE(captured.type_ == expected_types[complete_value]);
                EXPECT_TRUE(captured.body_ == expected_payloads[complete_value]);
                captured.body_.clear();
                ++complete_value;
            }
            EXPECT_TRUE(result_value.status_ == (offset == wire.size() ? ruvia::http_capsule_status::end : ruvia::http_capsule_status::need_more_data));
        }
        EXPECT_EQ(complete_value, std::size_t{3});
        EXPECT_EQ(captured.ends_, std::size_t{3});
        const auto ended = decoder.feed_one({}, true, capsule, &captured);
        EXPECT_TRUE(ended.status_ == ruvia::http_capsule_status::end && !ended.capsule_complete_ && ended.consumed_bytes_ == 0);
    }
    ruvia::http_capsule_decoder truncated;
    const std::array partial{char(0), char(3), char('a')};
    const auto result_value = truncated.feed_one(partial, true, nullptr, nullptr);
    EXPECT_TRUE(result_value.status_ == ruvia::http_capsule_status::truncated && !result_value.capsule_complete_ && result_value.consumed_bytes_ == partial.size());
}

// RFC 9297 section 3.2: capsules the reader does not keep are skipped at any
// length; max_capsule_length_ bounds only kept capsules (for CONNECT-UDP, after
// the RFC 9298 Context ID shows the datagram is kept).
TEST(Ruvia, http_capsule_retention_skips_dropped_capsules_beyond_the_length_limit) {
    std::array<char, 16> header;
    const auto append_capsule = [&](std::string& wire, std::uint64_t type, std::string_view value) {
        const auto encoded = ruvia::encode_http_capsule_header(header, type, value.size());
        wire.append(header.data(), std::get<0>(encoded));
        wire.append(value);
    };
    std::string wire;
    append_capsule(wire, 0x17, std::string(200, 'u'));
    append_capsule(wire, 0, std::string("\x03", 1) + std::string(200, 'c'));
    append_capsule(wire, 0, std::string("\x40\x00", 2) + "two-byte-zero");
    append_capsule(wire, 0, std::string("\0", 1) + "after");
    append_capsule(wire, 0, std::string("\x41", 1));
    for (const std::size_t block : {std::size_t{1}, std::size_t{7}, wire.size()}) {
        ruvia::http_capsule_decoder decoder({.max_capsule_length_ = 100});
        capture captured;
        std::vector<std::string> kept;
        std::size_t offset{};
        ruvia::http_capsule_feed_result result_value{};
        while (offset != wire.size()) {
            const auto count = std::min(block, wire.size() - offset);
            result_value = decoder.feed_one(std::span<const char>(wire).subspan(offset, count), offset + count == wire.size(), capsule, &captured, ruvia::http_capsule_retention::udp_payloads);
            EXPECT_TRUE(result_value.status_ != ruvia::http_capsule_status::limit);
            offset += result_value.consumed_bytes_;
            if (result_value.capsule_complete_) {
                EXPECT_EQ(captured.type_, std::uint64_t{0});
                kept.push_back(std::exchange(captured.body_, {}));
            }
        }
        EXPECT_TRUE(result_value.status_ == ruvia::http_capsule_status::end);
        // Context ID 0 in either encoding, and a value too short for its
        // Context ID (left to the reader as malformed), are kept.
        EXPECT_TRUE(kept == (std::vector<std::string>{std::string("\x40\x00", 2) + "two-byte-zero", std::string("\0", 1) + "after", std::string("\x41", 1)}));
        EXPECT_EQ(captured.ends_, std::size_t{3});
    }

    std::string datagrams;
    append_capsule(datagrams, 0x17, std::string(200, 'u'));
    append_capsule(datagrams, 0, "\x03kept");
    ruvia::http_capsule_decoder decoder({.max_capsule_length_ = 100});
    capture captured;
    EXPECT_TRUE(decoder.feed(datagrams, true, capsule, &captured, ruvia::http_capsule_retention::datagrams) == ruvia::http_capsule_status::end);
    EXPECT_EQ(captured.body_, std::string("\x03kept"));
    EXPECT_EQ(captured.ends_, std::size_t{1});

    // Kept capsules over the limit stay terminal: every type for a generic
    // reader, and a CONNECT-UDP payload once its Context ID is known.
    for (const auto retention : {ruvia::http_capsule_retention::all, ruvia::http_capsule_retention::udp_payloads}) {
        std::string oversized;
        append_capsule(oversized, retention == ruvia::http_capsule_retention::all ? 0x17 : 0, std::string("\0", 1) + std::string(200, 'o'));
        ruvia::http_capsule_decoder limited({.max_capsule_length_ = 100});
        EXPECT_TRUE(limited.feed(oversized, false, nullptr, nullptr, retention) == ruvia::http_capsule_status::limit);
    }
}

TEST(Ruvia, http_datagram_capsule_channel_frames_opaque_payloads_and_obeys_half_close) {
    ruvia::http_datagram_session session;
    const std::string_view opaque("\1\0\xff", 3);
    const auto plan = session.prepare_datagram(std::span(opaque.data(), opaque.size()));
    EXPECT_TRUE(plan.index() == 0);
    const auto received = session.receive_datagram(std::get<0>(plan).payload());
    EXPECT_TRUE(received.has_value());
    EXPECT_TRUE(std::string_view(received->data(), received->size()) == opaque);
    const auto empty = session.prepare_datagram({});
    EXPECT_TRUE(empty.index() == 0);
    const auto empty_received = session.receive_datagram(std::get<0>(empty).payload());
    EXPECT_TRUE(empty_received.has_value() && empty_received->empty());
    session.close_receive();
    EXPECT_TRUE(!session.receive_datagram(opaque).has_value());
    session.close_send();
    EXPECT_TRUE(session.prepare_datagram({}).index() != 0);
}

namespace {

std::string render(const ruvia::sse_message& message) {
    const auto frame = ruvia::format_sse_message(message);
    return std::string(frame.data(), frame.size());
}

struct parsed_event final {
    std::string type_;
    std::string data_;
    std::string id_;

    bool operator==(const parsed_event&) const = default;
};

// Feeds every chunk, re-offering each unconsumed suffix, until the parser
// leaves the active state.
std::vector<parsed_event> parse_chunks(ruvia::sse_parser& parser, std::initializer_list<std::string_view> chunks) {
    std::vector<parsed_event> events;
    for (auto chunk : chunks) {
        while (!chunk.empty() && parser.state() == ruvia::sse_parser_state::active) {
            const auto result = parser.parse(chunk);
            EXPECT_LE(result.consumed_bytes_, chunk.size());
            chunk.remove_prefix(result.consumed_bytes_);
            if (result.event_) {
                events.push_back({std::string(result.event_->type()), std::string(result.event_->data()), std::string(result.event_->last_event_id())});
            } else if (parser.state() == ruvia::sse_parser_state::active) {
                EXPECT_TRUE(chunk.empty());
            }
        }
    }
    return events;
}

std::vector<parsed_event> parse_stream(std::initializer_list<std::string_view> chunks) {
    ruvia::sse_parser parser;
    return parse_chunks(parser, chunks);
}

constexpr std::string_view whatwg_stream =
    ": comment\n"
    "event: update\n"
    "id: 7\n"
    "data: first\n"
    "data:second\n"
    "data\n"
    "unknown: x\n"
    "\n"
    "data: next\n"
    "\n"
    "id\n"
    "data:  spaced\n"
    "\n";

const std::vector<parsed_event> whatwg_events{
    {"update", "first\nsecond\n", "7"},
    {"message", "next", "7"},
    {"message", " spaced", ""},
};

}  // namespace

TEST(Ruvia, sse_formats_event_id_retry_and_multiline_data) {
    EXPECT_EQ(render({.data_ = "line1\nline2", .event_ = "update", .id_ = "7", .retry_ = std::chrono::milliseconds{3000}}), std::string("event: update\nid: 7\nretry: 3000\ndata: line1\ndata: line2\n\n"));
}

TEST(Ruvia, sse_distinguishes_absent_and_empty_data) {
    EXPECT_EQ(render({.retry_ = std::chrono::milliseconds{3000}}), std::string("retry: 3000\n\n"));
    EXPECT_EQ(render({.event_ = "ping"}), std::string("event: ping\n\n"));
    EXPECT_EQ(render({}), std::string("\n"));
    EXPECT_EQ(render({.data_ = ""}), std::string("data: \n\n"));
    EXPECT_EQ(render({.id_ = std::string_view{}}), std::string("id:\n\n"));
    EXPECT_EQ(render({.data_ = "hi"}), std::string("data: hi\n\n"));
    EXPECT_TRUE(render({.retry_ = std::chrono::milliseconds{1}}).find("data:") == std::string::npos);
}

TEST(Ruvia, sse_splits_data_on_cr_crlf_and_lf_never_emitting_raw_cr) {
    EXPECT_EQ(render({.data_ = "a\rb"}), std::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\nb"}), std::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\r\nb"}), std::string("data: a\ndata: b\n\n"));
    EXPECT_EQ(render({.data_ = "a\r\rb"}), std::string("data: a\ndata: \ndata: b\n\n"));
    EXPECT_TRUE(!(render({.data_ = "x\ry\r\rz"}).find('\r') != std::string_view::npos));
}

TEST(Ruvia, sse_rejects_newline_in_event_or_id_and_nul_in_id) {
    const auto throws_for = [](ruvia::sse_message message) {
        try {
            (void)ruvia::format_sse_message(message);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    EXPECT_TRUE(throws_for({.data_ = "x", .event_ = "a\nb"}));
    EXPECT_TRUE(throws_for({.data_ = "x", .event_ = "a\rb"}));
    EXPECT_TRUE(throws_for({.id_ = "1\n2"}));
    EXPECT_TRUE(throws_for({.id_ = std::string_view{"a\0b", 3}}));
    EXPECT_TRUE(throws_for({.retry_ = std::chrono::milliseconds{-1}}));

    // Every textual field is UTF-8; a stray byte or a surrogate never reaches the stream.
    EXPECT_TRUE(throws_for({.data_ = std::string_view("\xff", 1)}));
    EXPECT_TRUE(throws_for({.event_ = std::string_view("\xc3", 1)}));
    EXPECT_TRUE(throws_for({.id_ = std::string_view("\xed\xa0\x80", 3)}));
    EXPECT_EQ(render({.data_ = "caf\xc3\xa9", .event_ = "\xe2\x82\xac", .id_ = "\xf0\x9f\x98\x80"}), std::string("event: \xe2\x82\xac\nid: \xf0\x9f\x98\x80\ndata: caf\xc3\xa9\n\n"));
}

// WHATWG HTML "Interpreting an event stream": one optional space after the
// colon, a colon-less line names a field with an empty value, the event type
// resets after each dispatch while the last event ID persists, and comments and
// unknown fields are ignored.
TEST(Ruvia, sse_parser_interprets_fields_per_whatwg) {
    ruvia::sse_parser parser;
    EXPECT_EQ(parse_chunks(parser, {whatwg_stream}), whatwg_events);
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::active);
    EXPECT_EQ(parser.last_event_id(), "");

    // A block without data dispatches nothing but still commits its id.
    EXPECT_TRUE(parse_chunks(parser, {"id: 9\nevent: ignored\n\n"}).empty());
    EXPECT_EQ(parser.last_event_id(), "9");
    EXPECT_EQ(parse_chunks(parser, {"data:\n\n"}), (std::vector<parsed_event>{{"message", "", "9"}}));
}

TEST(Ruvia, sse_parser_is_independent_of_chunk_boundaries) {
    for (std::size_t split = 0; split <= whatwg_stream.size(); ++split) {
        EXPECT_EQ(parse_stream({whatwg_stream.substr(0, split), whatwg_stream.substr(split)}), whatwg_events) << "split at " << split;
    }
    ruvia::sse_parser parser;
    std::vector<parsed_event> events;
    for (std::size_t index = 0; index < whatwg_stream.size(); ++index) {
        const auto chunk_events = parse_chunks(parser, {whatwg_stream.substr(index, 1)});
        events.insert(events.end(), chunk_events.begin(), chunk_events.end());
    }
    EXPECT_EQ(events, whatwg_events);
}

TEST(Ruvia, sse_parser_returns_one_event_per_call_with_consumed_bytes) {
    ruvia::sse_parser parser;
    constexpr std::string_view input = "data: a\n\ndata: b\n\n: tail";
    const auto first = parser.parse(input);
    ASSERT_TRUE(first.event_.has_value());
    EXPECT_EQ(first.consumed_bytes_, std::size_t{9});
    EXPECT_EQ(first.event_->data(), "a");
    const auto second = parser.parse(input.substr(first.consumed_bytes_));
    ASSERT_TRUE(second.event_.has_value());
    EXPECT_EQ(second.consumed_bytes_, std::size_t{9});
    EXPECT_EQ(second.event_->data(), "b");
    const auto rest = parser.parse(input.substr(18));
    EXPECT_FALSE(rest.event_.has_value());
    EXPECT_EQ(rest.consumed_bytes_, std::size_t{6});
}

TEST(Ruvia, sse_parser_ends_lines_at_crlf_lf_and_cr_across_chunks) {
    EXPECT_EQ(parse_stream({"data: a\r\ndata: b\rdata: c\n\r\n"}), (std::vector<parsed_event>{{"message", "a\nb\nc", ""}}));
    // An LF opening the next chunk completes the CRLF the previous one began;
    // it is not a blank line that would dispatch early.
    EXPECT_EQ(parse_stream({"data: a\r", "\ndata: b\r", "\n\r", "\n"}), (std::vector<parsed_event>{{"message", "a\nb", ""}}));
    EXPECT_EQ(parse_stream({"data: a\r", "data: b\r\r"}), (std::vector<parsed_event>{{"message", "a\nb", ""}}));
    EXPECT_EQ(parse_stream({"data: a\r", "\r"}), (std::vector<parsed_event>{{"message", "a", ""}}));
}

TEST(Ruvia, sse_parser_skips_one_leading_bom) {
    EXPECT_EQ(parse_stream({"\xEF", "\xBB", "\xBF", "data: x\n\n"}), (std::vector<parsed_event>{{"message", "x", ""}}));
    // Only one BOM is stripped; a second one starts the first field name.
    EXPECT_EQ(parse_stream({"\xEF\xBB\xBF\xEF\xBB\xBF"
                            "data: y\n\ndata: z\n\n"}),
        (std::vector<parsed_event>{{"message", "z", ""}}));
    // A partial BOM is ordinary input.
    EXPECT_EQ(parse_stream({"\xEF\xBB", "data: y\n\ndata: z\n\n"}), (std::vector<parsed_event>{{"message", "z", ""}}));
    EXPECT_EQ(parse_stream({"\xEF", "\n\ndata: z\n\n"}), (std::vector<parsed_event>{{"message", "z", ""}}));
}

TEST(Ruvia, sse_parser_accepts_only_ascii_digit_retry_values) {
    ruvia::sse_parser parser;
    EXPECT_EQ(parser.reconnection_time(), std::nullopt);
    EXPECT_TRUE(parse_chunks(parser, {"retry: 3000\n"}).empty());
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{3000});
    for (const std::string_view invalid : {"retry: 12a\n", "retry:\n", "retry\n", "retry: -5\n", "retry:  7\n", "retry: 1.5\n", "retry: +1\n"}) {
        EXPECT_TRUE(parse_chunks(parser, {invalid}).empty());
        EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{3000}) << invalid;
    }
    EXPECT_TRUE(parse_chunks(parser, {"retry: 12", "34\r\n"}).empty());
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{1234});
    EXPECT_TRUE(parse_chunks(parser, {"retry: 0\n"}).empty());
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{0});
    EXPECT_TRUE(parse_chunks(parser, {"retry: 99999999999999999999999999\n"}).empty());
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds::max());
}

TEST(Ruvia, sse_parser_replaces_ill_formed_utf8_and_ignores_ids_with_nul) {
    constexpr std::string_view input("id: a\0b\nevent: \xE2\x82\ndata: \xFF\xC3\ndata: caf\xC3\xA9\n\n", 40);
    EXPECT_EQ(parse_stream({input}), (std::vector<parsed_event>{{"\xEF\xBF\xBD", "\xEF\xBF\xBD\xEF\xBF\xBD\ncaf\xC3\xA9", ""}}));
    // A truncated sequence at a line end is replaced; the LF stays a separator.
    EXPECT_EQ(parse_stream({"data: \xE2\x82\ndata: \xAC\n\n"}), (std::vector<parsed_event>{{"message", "\xEF\xBF\xBD\n\xEF\xBF\xBD", ""}}));
}

TEST(Ruvia, sse_parser_fails_an_event_over_its_limit_and_reset_reuses_it) {
    ruvia::sse_parser parser({.event_limit_ = ruvia::protocol_byte_limit::limited(8)});
    // Seven data bytes and their LF fill the limit exactly.
    EXPECT_EQ(parse_chunks(parser, {"id: 1\ndata: 1234567\n\n"}), (std::vector<parsed_event>{{"message", "1234567", "1"}}));
    // Comments and ignored fields buffer nothing, whatever their length.
    EXPECT_TRUE(parse_chunks(parser, {": " + std::string(1000, 'c') + "\nignored: " + std::string(1000, 'i') + "\n"}).empty());
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::active);

    const auto failed = parser.parse("data: 1234\ndata: 567\n\n");
    EXPECT_FALSE(failed.event_.has_value());
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::failed);
    EXPECT_THROW((void)parser.parse("data: x\n\n"), std::logic_error);
    parser.finish();
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::failed);

    parser.reset();
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::active);
    EXPECT_EQ(parser.last_event_id(), "");
    EXPECT_EQ(parse_chunks(parser,
                  {"\xEF\xBB\xBF"
                   "data: ok\n\n"}),
        (std::vector<parsed_event>{{"message", "ok", ""}}));

    // A pending event type counts too.
    EXPECT_TRUE(parse_chunks(parser, {"event: 123456789\n"}).empty());
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::failed);
}

TEST(Ruvia, sse_parser_finish_discards_an_incomplete_event) {
    ruvia::sse_parser parser;
    EXPECT_EQ(parse_chunks(parser, {"id: 9\n\nretry: 50\ndata: pending\n", "data: partial"}), std::vector<parsed_event>{});
    parser.finish();
    EXPECT_EQ(parser.state(), ruvia::sse_parser_state::finished);
    EXPECT_EQ(parser.last_event_id(), "9");
    EXPECT_EQ(parser.reconnection_time(), std::chrono::milliseconds{50});
    EXPECT_THROW((void)parser.parse("\n"), std::logic_error);

    parser.reset();
    EXPECT_EQ(parser.reconnection_time(), std::nullopt);
    EXPECT_EQ(parse_chunks(parser, {"data: fresh\n\n"}), (std::vector<parsed_event>{{"message", "fresh", ""}}));
}
