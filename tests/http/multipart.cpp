#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_content_coding.h"

#include "content_decoding_fixture.h"
#include "test_exceptions.h"

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

    const auto quoted_special =
        ruvia::parse_multipart_boundary(R"(multipart/form-data; boundary="a:b")");
    EXPECT_TRUE(quoted_special.boundary() != nullptr);
    EXPECT_EQ(quoted_special.boundary()->value(), std::string_view("a:b"));

    const auto quoted_pair =
        ruvia::parse_multipart_boundary(R"(multipart/form-data; boundary="a\?b")");
    EXPECT_TRUE(quoted_pair.boundary() != nullptr);
    EXPECT_EQ(quoted_pair.boundary()->value(), std::string_view("a?b"));

    // A different media type is not applicable to the multipart parser.
    const auto wrong_type = ruvia::parse_multipart_boundary("text/plain; boundary=abc");
    EXPECT_TRUE(wrong_type.boundary() == nullptr);
    EXPECT_TRUE(wrong_type.not_applicable() != nullptr);
    EXPECT_TRUE(wrong_type.failure() == nullptr);

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
        EXPECT_TRUE(result_value.boundary() == nullptr);
        EXPECT_TRUE(result_value.not_applicable() == nullptr);
        EXPECT_TRUE(result_value.failure() != nullptr);
        if (result_value.failure() != nullptr) {
            const auto error = result_value.failure()->protocol_error();
            EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
            EXPECT_EQ(std::string_view(error.what()), std::string_view("invalid multipart boundary"));
        }
    }

    const auto extension =
        ruvia::parse_multipart_boundary(R"(multipart/form-data; charset="utf-8"; boundary=abc)");
    EXPECT_TRUE(extension.boundary() != nullptr);
    if (extension.boundary() != nullptr) {
        EXPECT_EQ(extension.boundary()->value(), std::string_view("abc"));
    }
}

TEST(Ruvia, multipart_boundary_allows_empty_http_parameter_slots_without_relaxing_validation) {
    for (const std::string_view valid : {
             "multipart/form-data;;boundary=abc;",
             "multipart/form-data; ; boundary=abc; \t;",
             R"(multipart/form-data;;charset="utf-8";;boundary="a b";;)"}) {
        const auto result = ruvia::parse_multipart_boundary(valid);
        EXPECT_TRUE(result.boundary() != nullptr);
        if (result.boundary()) {
            const auto expected = valid.find("a b") == std::string_view::npos ? "abc" : "a b";
            EXPECT_EQ(result.boundary()->value(), std::string_view(expected));
        }
    }
    for (const std::string_view invalid : {
             "multipart/form-data; ; ;",
             "multipart/form-data;;boundary=;",
             "multipart/form-data;;boundary=abc;;BOUNDARY=abc;",
             "multipart/form-data;;boundary=abc;;charset=utf-8;;CHARSET=latin1;",
             "multipart/form-data;;boundary=abc;;broken;",
             "multipart/form-data;;boundary =abc;",
             "multipart/form-data;;boundary= abc;"}) {
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
    const auto parts =
        ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
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
    for (const std::string_view invalid : {"Content-Disposition: form-data; name=\"unterminated",
             "Content-Disposition: form-data; name=unquoted value",
             "Content-Disposition: form-data; name=field; name=shadow",
             "Content-Disposition: form-data; name=field; filename=a; filename=b",
             "Content-Disposition: form-data; name=field; x=one; X=two",
             "Content-Disposition: form-data; name=field; broken",
             "Content-Disposition: form-data; name=field\r\n"
             " filename=shadow.txt",
             "Content-Disposition: form-data; name=field\r\n"
             "Content-Disposition: form-data; name=shadow"}) {
        std::string body = "--BOUNDARY\r\n";
        body.append(invalid);
        body.append("\r\n\r\nvalue\r\n--BOUNDARY--\r\n");
        bool rejected = false;
        try {
            (void)ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
        } catch (const ruvia::http_protocol_error& error) {
            rejected = true;
            EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
            EXPECT_EQ(std::string_view(error.what()), std::string_view("invalid multipart content disposition"));
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, multipart_part_header_rejects_ambiguous_header_blocks) {
    for (const std::string_view invalid : {"Broken-Line\r\n"
                                           "Content-Disposition: form-data; name=field",
             " Content-Disposition: form-data; name=field",
             "Content-Disposition : form-data; name=field",
             "Content-Disposition: form-data; name=field\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Type: application/json"}) {
        std::string body = "--BOUNDARY\r\n";
        body.append(invalid);
        body.append("\r\n\r\nvalue\r\n--BOUNDARY--\r\n");
        bool rejected = false;
        try {
            (void)ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
        } catch (const ruvia::http_protocol_error& error) {
            rejected = true;
            EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
            EXPECT_EQ(std::string_view(error.what()), std::string_view("invalid multipart part headers"));
        }
        EXPECT_TRUE(rejected);
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
    const auto parts =
        ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
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
    bool rejected = false;
    try {
        (void)ruvia::parse_multipart_body(
            "--BOUNDARY\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nmissing close",
            {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    } catch (const ruvia::http_protocol_error& error) {
        rejected = true;
        EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("incomplete multipart body"));
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, multipart_complete_body_parser_shares_incremental_limits) {
    std::string oversized_preamble(64 * 1024 + 1, 'x');
    std::string complete_error_message;
    bool rejected = false;
    try {
        (void)ruvia::parse_multipart_body(
            oversized_preamble, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    } catch (const ruvia::http_protocol_error& error) {
        rejected = true;
        complete_error_message = error.what();
        EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("multipart preamble exceeds limit"));
    }
    EXPECT_TRUE(rejected);

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
        return ruvia::parse_multipart_body(
            body, {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
    };
    const auto check_failure = [&parse](
                                   const std::string& body, std::string_view message) {
        bool rejected = false;
        try {
            (void)parse(body);
        } catch (const ruvia::http_protocol_error& error) {
            rejected = true;
            EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
            EXPECT_EQ(std::string_view(error.what()), message);
        }
        EXPECT_TRUE(rejected);

        ruvia::multipart_parser incremental(
            {.boundary_ = ruvia::test::make_multipart_boundary("BOUNDARY")});
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

    const auto accepted = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("abc"),
                                                                .max_parts_ = 3,
                                                                .max_metadata_bytes_ = 3 * header_value.size()});
    EXPECT_EQ(accepted.size(), std::size_t{3});
    bool too_many_rejected = false;
    try {
        (void)ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_parts_ = 2});
    } catch (const ruvia::http_protocol_error& error) {
        too_many_rejected = true;
        EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
    }
    EXPECT_TRUE(too_many_rejected);
    bool too_large_rejected = false;
    try {
        (void)ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_metadata_bytes_ = 3 * header_value.size() - 1});
    } catch (const ruvia::http_protocol_error& error) {
        too_large_rejected = true;
        EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
    }
    EXPECT_TRUE(too_large_rejected);

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

    const auto expected = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("abc")});
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

    const auto parts = ruvia::parse_multipart_body(body, {.boundary_ = ruvia::test::make_multipart_boundary("b")});
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
                    if (part->phase() == ruvia::multipart_chunk_phase::first ||
                        part->phase() == ruvia::multipart_chunk_phase::complete) {
                        received += '[';
                        received += part->name();
                        received += ':';
                    }
                    received += part->body();
                    if (part->phase() == ruvia::multipart_chunk_phase::complete ||
                        part->phase() == ruvia::multipart_chunk_phase::last) {
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
        bool buffered_rejected = false;
        try {
            (void)ruvia::parse_multipart_body(wire, {.boundary_ = ruvia::test::make_multipart_boundary("abc")});
        } catch (const ruvia::http_protocol_error& error) {
            buffered_rejected = true;
            EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
        }
        EXPECT_TRUE(buffered_rejected);

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
    const std::string truncated =
        "--abc\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\n" + std::string(64, 'x');
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
    const auto parts = ruvia::parse_multipart_body(
        body, {.boundary_ = ruvia::test::make_multipart_boundary("b")});
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
        bool rejected = false;
        try {
            static_cast<void>(ruvia::parse_multipart_body(
                body, {.boundary_ = ruvia::test::make_multipart_boundary("b")}));
        } catch (const ruvia::http_protocol_error& error) {
            rejected = std::string_view(error.code()) == "multipart_invalid_delimiter";
        }
        EXPECT_TRUE(rejected);

        ruvia::multipart_parser parser(
            {.boundary_ = ruvia::test::make_multipart_boundary("b")});
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

    auto parsed = ruvia::parse_multipart_body(wire, {.boundary_ = ruvia::test::make_multipart_boundary("abc")});
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
                if (part->phase() == ruvia::multipart_chunk_phase::complete ||
                    part->phase() == ruvia::multipart_chunk_phase::last) {
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
    const auto accepted = ruvia::parse_multipart_body(wire,
        {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_metadata_bytes_ = headers.size()});
    EXPECT_EQ(accepted.size(), std::size_t{1});
    bool oversized_rejected = false;
    try {
        (void)ruvia::parse_multipart_body(wire,
            {.boundary_ = ruvia::test::make_multipart_boundary("abc"), .max_metadata_bytes_ = headers.size() - 1});
    } catch (const ruvia::http_protocol_error& error) {
        oversized_rejected = true;
        EXPECT_TRUE(error.status() == ruvia::http_status::content_too_large);
    }
    EXPECT_TRUE(oversized_rejected);
    ruvia::multipart_parser limited({.boundary_ = ruvia::test::make_multipart_boundary("abc"),
        .max_metadata_bytes_ = headers.size() - 1});
    limited.feed(wire);
    limited.finish_input();
    const auto limited_result = limited.poll();
    EXPECT_TRUE(limited_result.failure() != nullptr);
    if (const auto* failure = limited_result.failure()) {
        EXPECT_TRUE(failure->protocol_error().status() == ruvia::http_status::content_too_large);
    }

    for (const std::string_view invalid : {
             " Content-Disposition: form-data; name=field",
             "Content-Disposition: form-data;\n name=field",
             "Content-Disposition: form-data;\r name=field",
             "Content-Disposition: form-data; name=field\r\n Content-Type: text/plain",
             "Content-Disposition: form-data; name=field\r\nContent-Disposition:\r\n form-data; name=other"}) {
        const std::string malformed = "--abc\r\n" + std::string(invalid) + "\r\n\r\nvalue\r\n--abc--\r\n";
        bool rejected = false;
        try {
            (void)ruvia::parse_multipart_body(malformed,
                {.boundary_ = ruvia::test::make_multipart_boundary("abc")});
        } catch (const ruvia::http_protocol_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
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
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

namespace {

using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::http_protocol_error;
using ruvia::http_reason_phrase;

template <typename value_type>
constexpr bool can_construct_received_status = requires(value_type value) {
    ruvia::http_received_status::try_from_value(value);
};

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

    EXPECT_TRUE(ruvia::http_status_code::try_from_value(std::int64_t{100}) ==
                ruvia::http_status::continue_value);
    EXPECT_TRUE(ruvia::http_status_code::try_from_value(std::uint64_t{599}) ==
                ruvia::http_status_code::from_value(599));
}

TEST(Ruvia, http_received_status_rejects_original_integers_before_wire_narrowing) {
    for (const auto value : {std::int64_t{-1}, std::int64_t{-65336},
             std::int64_t{1000}, std::int64_t{65736}, std::int64_t{4'294'967'496},
             (std::numeric_limits<std::int64_t>::min)(), (std::numeric_limits<std::int64_t>::max)()}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(ruvia::http_received_status::try_from_value(value).has_value());
    }
    for (const auto value : {std::uint64_t{1000}, std::uint64_t{65736},
             std::uint64_t{4'294'967'496}, (std::numeric_limits<std::uint64_t>::max)() - 65'335,
             (std::numeric_limits<std::uint64_t>::max)()}) {
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
        const auto expected = value >= 100 && value <= 599
                                  ? ruvia::http_status_code::from_value(value)
                                  : ruvia::http_status::internal_server_error;
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
    const http_protocol_error error(
        ruvia::http_status::content_too_large, "body_too_large", source_value);
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
    for (const auto path : {"/.well-known/masque/udp//", "/.well-known/masque/udp/host/",
             "/.well-known/masque/udp/host/443", "/.well-known/masque/udp/host/443/extra/"}) {
        const auto parsed_value = ruvia::parse_http_connect_udp_path(path);
        EXPECT_TRUE(std::get_if<ruvia::http_connect_udp_error>(&parsed_value) != nullptr);
    }
}
TEST(Ruvia, http_connect_udp_upgrade_handshake_requires_capsule_negotiation) {
    const std::array fields{ruvia::http_header_view{"Host", "example.test"}, ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", "connect-udp"}, ruvia::http_header_view{"Capsule-Protocol", "?1;version=1"}};
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
    const auto resp_err =
        ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101, bad_resp);
    EXPECT_TRUE(resp_err == ruvia::http_connect_udp_error::invalid_response);
    const auto abs_req = ruvia::make_parsed_http_request("GET",
        "https://example.test/.well-known/masque/udp/192.0.2.1/443/", fields, {});
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(abs_req).has_value());
    const auto abs_query_req = ruvia::make_parsed_http_request("GET",
        "https://example.test/.well-known/masque/udp/192.0.2.1/443/?target=1", fields, {});
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
    const auto status = decoder.feed(
        wire, true, [](void* context, ruvia::http_capsule_event event) {
            auto& capture = *static_cast<capsule_capture*>(context);
            capture.datagram_ = capture.datagram_ && event.type_ == ruvia::http_datagram_capsule_type;
            if (!event.payload_.empty()) {
                capture.bytes_.append(event.payload_.data(), event.payload_.size());
            }
        },
        &captured);
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
    auto prepared_param = ruvia::prepare_http_connect_udp_response(
        std::move(parameterized), ruvia::http_protocol_version::http2);
    EXPECT_TRUE(prepared_param.index() == 0);
    if (prepared_param.index() == 0) {
        EXPECT_EQ(std::get<0>(prepared_param).header("capsule-protocol").value_or(""), "?1;version=1");
    }
    for (const auto field : {"Content-Length", "Transfer-Encoding", "Content-Type", "Content-Encoding", "Connection", "Upgrade", "Trailer"}) {
        ruvia::http_response invalid;
        invalid.header(field, field == std::string_view("Content-Length") ? "0" : field == std::string_view("Content-Type") ? "application/octet-stream"
                                                                                                                            : "test");
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
    EXPECT_TRUE(std::holds_alternative<ruvia::http1_client_request_prepare_error>(bad_target) &&
                std::get<ruvia::http1_client_request_prepare_error>(bad_target) ==
                    ruvia::http1_client_request_prepare_error::invalid_target);
    const std::array with_host{fields_value[0], ruvia::http_header_view{"Host", "[::1]:8443"}};
    auto managed_host = writer.prepare_connect_udp(origin, "/udp", with_host, buffer);
    EXPECT_TRUE(std::holds_alternative<ruvia::http1_client_request_prepare_error>(managed_host) &&
                std::get<ruvia::http1_client_request_prepare_error>(managed_host) ==
                    ruvia::http1_client_request_prepare_error::host_header_managed_by_writer);
}

TEST(Ruvia, http_connect_udp_http2_uses_extended_connect_without_upgrade_fields) {
    const std::array fields{ruvia::http_header_view{"capsule-protocol", "?1"}};
    const ruvia::http_connect_udp_request_view request{
        .version_ = ruvia::http_protocol_version::http2,
        .method_ = "CONNECT",
        .protocol_ = "connect-udp",
        .scheme_ = "https",
        .authority_ = "example.test",
        .path_ = "/proxy",
        .headers_ = fields};
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
    const std::array upgrade_fields{fields[0], ruvia::http_header_view{"connection", "upgrade"},
        ruvia::http_header_view{"upgrade", "connect-udp"}};
    invalid = request;
    invalid.headers_ = upgrade_fields;
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request(invalid).has_value());
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, fields)
            .has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 101, fields)
            .has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, upgrade_fields)
            .has_value());

    ruvia::http_response response;
    response.status(ruvia::http_status::created);
    auto prepared = ruvia::prepare_http_connect_udp_response(
        std::move(response), ruvia::http_protocol_version::http2);
    EXPECT_TRUE(prepared.index() == 0);
    if (prepared.index() == 0) {
        EXPECT_TRUE(std::get<0>(prepared).status() == ruvia::http_status::created);
        EXPECT_TRUE(std::get<0>(prepared).header("capsule-protocol") == "?1");
        EXPECT_TRUE(!std::get<0>(prepared).header("connection").has_value());
        EXPECT_TRUE(!std::get<0>(prepared).header("upgrade").has_value());
    }
}

TEST(Ruvia, http_connect_udp_recipient_treats_capsule_protocol_as_optional) {
    const std::array upgrade{ruvia::http_header_view{"Host", "example.test"},
        ruvia::http_header_view{"Connection", "Upgrade"}, ruvia::http_header_view{"Upgrade", "connect-udp"}};
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request({.authority_ = "example.test", .path_ = "/proxy", .headers_ = upgrade})
            .has_value());
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101,
        std::span<const ruvia::http_header_view>(upgrade).subspan(1))
            .has_value());
    for (const auto value : {"1", "?0", "\"yes\""}) {
        const std::array ignored{ruvia::http_header_view{"Capsule-Protocol", value}};
        const ruvia::http_connect_udp_request_view request{.version_ = ruvia::http_protocol_version::http2,
            .method_ = "CONNECT",
            .authority_ = "example.test",
            .path_ = "/proxy",
            .headers_ = ignored};
        EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(request).has_value());
        EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, ignored).has_value());
    }
    const std::array duplicate{ruvia::http_header_view{"Capsule-Protocol", "?1"}, ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, duplicate).has_value());
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, std::span<const ruvia::http_header_view>{})
            .has_value());
}

TEST(Ruvia, http_connect_udp_rejects_statuses_that_cannot_start_capsule_protocol) {
    const std::array capsule{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    for (const std::uint16_t status : {std::uint16_t{204}, std::uint16_t{205}, std::uint16_t{206}}) {
        EXPECT_TRUE(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, status, capsule) ==
                    ruvia::http_connect_udp_error::invalid_response);
        ruvia::http_response response;
        response.status(ruvia::http_status_code::from_value(status));
        EXPECT_TRUE(ruvia::prepare_http_connect_udp_response(std::move(response), ruvia::http_protocol_version::http2).index() != 0);
    }
}

TEST(Ruvia, http_connect_udp_sender_requires_exactly_one_capsule_protocol_true) {
    const auto check = [](std::span<const ruvia::http_header_view> fields) {
        return ruvia::validate_http_connect_udp_request_to_send({.version_ = ruvia::http_protocol_version::http2,
            .method_ = "CONNECT",
            .authority_ = "example.test",
            .path_ = "/proxy",
            .headers_ = fields});
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
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http11,
                                                                     .method_ = "GET",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/.well-known/masque/udp/192.0.2.1/443/",
                                                                     .headers_ = sender_bad_connection,
                                                                 })
            .has_value());

    // Sender validates Host syntax and authority match.
    const std::array bad_host_h1{
        ruvia::http_header_view{"Host", "invalid@host:99999"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", "connect-udp"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http11,
                                                                     .method_ = "GET",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/proxy",
                                                                     .headers_ = bad_host_h1,
                                                                 })
            .has_value());

    const std::array mismatched_host_h1{
        ruvia::http_header_view{"Host", "other.test"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", "connect-udp"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http11,
                                                                     .method_ = "GET",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/proxy",
                                                                     .headers_ = mismatched_host_h1,
                                                                 })
            .has_value());

    // HTTP/2 rejects connection-specific headers (Keep-Alive, Proxy-Connection, TE) and non-http(s) schemes.
    const std::array h2_keep_alive{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Keep-Alive", "timeout=5"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http2,
                                                                     .method_ = "CONNECT",
                                                                     .protocol_ = "connect-udp",
                                                                     .scheme_ = "https",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/proxy",
                                                                     .headers_ = h2_keep_alive,
                                                                 })
            .has_value());
    const std::array valid_capsule{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http2,
                                                                     .method_ = "CONNECT",
                                                                     .protocol_ = "connect-udp",
                                                                     .scheme_ = "not a valid scheme!",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/proxy",
                                                                     .headers_ = valid_capsule,
                                                                 })
            .has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, h2_keep_alive)
            .has_value());
    ruvia::http_response h2_resp;
    h2_resp.status(ruvia::http_status::ok);
    h2_resp.header("Keep-Alive", "timeout=5");
    EXPECT_TRUE(ruvia::prepare_http_connect_udp_response(
                    std::move(h2_resp), ruvia::http_protocol_version::http2)
                    .index() != 0);

    // Recipient parses Upgrade = #protocol (ignoring empty elements and allowing additional tokens on requests).
    const std::array list_fields{
        ruvia::http_header_view{"Host", "example.test"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", ", connect-udp, fallback"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    const auto parsed_req = ruvia::make_parsed_http_request(
        "GET", "/.well-known/masque/udp/192.0.2.1/443/", list_fields, {});
    EXPECT_TRUE(ruvia::is_http_connect_udp_upgrade_request(parsed_req));
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_request(parsed_req).has_value());

    const std::array empty_elem_resp{
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", ", connect-udp,"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    EXPECT_TRUE(!ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http11, 101, empty_elem_resp)
            .has_value());

    // RFC 9297 3.2: a Capsule Protocol message that carries Content-Length is
    // malformed regardless of its value.
    const std::array cl0_req{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Content-Length", "0"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request({
                                                             .version_ = ruvia::http_protocol_version::http2,
                                                             .method_ = "CONNECT",
                                                             .protocol_ = "connect-udp",
                                                             .scheme_ = "https",
                                                             .authority_ = "example.test",
                                                             .path_ = "/proxy",
                                                             .headers_ = cl0_req,
                                                         })
            .has_value());
    const std::array cl5_req{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Content-Length", "5"},
    };
    EXPECT_TRUE(ruvia::validate_http_connect_udp_request({
                                                             .version_ = ruvia::http_protocol_version::http2,
                                                             .method_ = "CONNECT",
                                                             .protocol_ = "connect-udp",
                                                             .scheme_ = "https",
                                                             .authority_ = "example.test",
                                                             .path_ = "/proxy",
                                                             .headers_ = cl5_req,
                                                         })
            .has_value());
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, cl0_req)
            .has_value());

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
    EXPECT_TRUE(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, h2_te_trailers)
            .has_value());
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
    for (const auto value : {"?1;bytes=:YQ=:", "?1;bytes=:YQ==:", "?1;bytes=:YQ:", "?1;bytes=:YWJj:",
             "?1;bytes=:YWJ:", "?1;bytes=:YWJ=:", "?1;bytes=:YR==:", "?1;bytes=::"}) {
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
    for (const auto value : {"?1;bytes=:Y:", "?1;bytes=:Y=:", "?1;bytes=:YWJj=:", "?1;bytes=:YWJ==:",
             "?1;bytes=:YQ===:", "?1;bytes=:Y=Q:", "?1;bytes=:YQ$:", "?1;bytes=:YQ\n:", "?1;bytes=:YQ", "?1\t",
             "?1 \t", "\t?1"}) {
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
            result_value = decoder.feed_one(std::span<const char>(wire).subspan(offset, count), offset + count == wire.size(),
                capsule, &captured, ruvia::http_capsule_retention::udp_payloads);
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
        EXPECT_TRUE(kept == (std::vector<std::string>{std::string("\x40\x00", 2) + "two-byte-zero",
                                std::string("\0", 1) + "after", std::string("\x41", 1)}));
        EXPECT_EQ(captured.ends_, std::size_t{3});
    }

    std::string datagrams;
    append_capsule(datagrams, 0x17, std::string(200, 'u'));
    append_capsule(datagrams, 0, "\x03kept");
    ruvia::http_capsule_decoder decoder({.max_capsule_length_ = 100});
    capture captured;
    EXPECT_TRUE(decoder.feed(datagrams, true, capsule, &captured, ruvia::http_capsule_retention::datagrams) ==
                ruvia::http_capsule_status::end);
    EXPECT_EQ(captured.body_, std::string("\x03kept"));
    EXPECT_EQ(captured.ends_, std::size_t{1});

    // Kept capsules over the limit stay terminal: every type for a generic
    // reader, and a CONNECT-UDP payload once its Context ID is known.
    for (const auto retention : {ruvia::http_capsule_retention::all, ruvia::http_capsule_retention::udp_payloads}) {
        std::string oversized;
        append_capsule(oversized, retention == ruvia::http_capsule_retention::all ? 0x17 : 0,
            std::string("\0", 1) + std::string(200, 'o'));
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
                events.push_back({std::string(result.event_->type()), std::string(result.event_->data()),
                    std::string(result.event_->last_event_id())});
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
    EXPECT_EQ(render({.data_ = "line1\nline2",
                  .event_ = "update",
                  .id_ = "7",
                  .retry_ = std::chrono::milliseconds{3000}}),
        std::string("event: update\nid: 7\nretry: 3000\ndata: line1\ndata: line2\n\n"));
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
        EXPECT_EQ(parse_stream({whatwg_stream.substr(0, split), whatwg_stream.substr(split)}), whatwg_events)
            << "split at " << split;
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
    EXPECT_EQ(parse_stream({"data: a\r\ndata: b\rdata: c\n\r\n"}),
        (std::vector<parsed_event>{{"message", "a\nb\nc", ""}}));
    // An LF opening the next chunk completes the CRLF the previous one began;
    // it is not a blank line that would dispatch early.
    EXPECT_EQ(parse_stream({"data: a\r", "\ndata: b\r", "\n\r", "\n"}),
        (std::vector<parsed_event>{{"message", "a\nb", ""}}));
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
    for (const std::string_view invalid :
        {"retry: 12a\n", "retry:\n", "retry\n", "retry: -5\n", "retry:  7\n", "retry: 1.5\n", "retry: +1\n"}) {
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
    EXPECT_EQ(parse_stream({input}),
        (std::vector<parsed_event>{{"\xEF\xBF\xBD", "\xEF\xBF\xBD\xEF\xBF\xBD\ncaf\xC3\xA9", ""}}));
    // A truncated sequence at a line end is replaced; the LF stays a separator.
    EXPECT_EQ(parse_stream({"data: \xE2\x82\ndata: \xAC\n\n"}),
        (std::vector<parsed_event>{{"message", "\xEF\xBF\xBD\n\xEF\xBF\xBD", ""}}));
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
    EXPECT_EQ(parse_chunks(parser, {"\xEF\xBB\xBF"
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

namespace {

using ruvia::proxy_protocol_command;
using ruvia::proxy_protocol_error;
using ruvia::proxy_protocol_header;
using ruvia::proxy_protocol_parser;
using ruvia::proxy_protocol_parser_options;
using ruvia::proxy_protocol_parser_state;
using ruvia::proxy_protocol_transport;
using ruvia::proxy_protocol_version;

constexpr std::string_view v1_tcp4 = "PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\r\n";
constexpr std::string_view http_request_line = "GET / HTTP/1.1\r\n";

std::span<const std::byte> wire(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

std::string text(std::span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

ruvia::ip_address ip(std::string_view literal) {
    return ruvia::ip_address::parse(literal).value();
}

ruvia::ip_endpoint endpoint(std::string_view literal, std::uint16_t port) {
    return ruvia::ip_endpoint(ip(literal), port);
}

std::string address_bytes(std::string_view literal) {
    const auto address = ip(literal);
    const auto bytes = address.bytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string port_bytes(std::uint16_t port) {
    return {static_cast<char>(port >> 8), static_cast<char>(port & 0xFF)};
}

std::string v2_header(std::uint8_t version_command, std::uint8_t family_protocol, std::string_view block) {
    std::string header("\r\n\r\n\0\r\nQUIT\n", 12);
    header.push_back(static_cast<char>(version_command));
    header.push_back(static_cast<char>(family_protocol));
    header.append(port_bytes(static_cast<std::uint16_t>(block.size())));
    header.append(block);
    return header;
}

std::string tlv(std::uint8_t type, std::string_view value) {
    std::string bytes(1, static_cast<char>(type));
    bytes.append(port_bytes(static_cast<std::uint16_t>(value.size())));
    bytes.append(value);
    return bytes;
}

std::string inet_block() {
    return address_bytes("192.168.0.1") + address_bytes("10.0.0.2") + port_bytes(56324) + port_bytes(443);
}

std::string inet6_block() {
    return address_bytes("2001:db8::1") + address_bytes("2001:db8::2") + port_bytes(1234) + port_bytes(80);
}

// PROXY TCP over IPv4 with ALPN, AUTHORITY and an empty-valued custom TLV.
std::string v2_inet_with_tlvs() {
    return v2_header(0x21, 0x11, inet_block() + tlv(0x01, "h2") + tlv(0x02, "example.com") + tlv(0xE0, ""));
}

void expect_v1_tcp4(const proxy_protocol_header& header) {
    EXPECT_EQ(header.version(), proxy_protocol_version::v1);
    EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
    EXPECT_EQ(header.transport(), proxy_protocol_transport::stream);
    EXPECT_EQ(header.source(), endpoint("192.168.0.1", 56324));
    EXPECT_EQ(header.destination(), endpoint("192.168.0.11", 443));
    EXPECT_TRUE(header.tlvs().empty());
}

void expect_v2_inet_with_tlvs(const proxy_protocol_header& header) {
    EXPECT_EQ(header.version(), proxy_protocol_version::v2);
    EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
    EXPECT_EQ(header.transport(), proxy_protocol_transport::stream);
    EXPECT_EQ(header.source(), endpoint("192.168.0.1", 56324));
    EXPECT_EQ(header.destination(), endpoint("10.0.0.2", 443));
    const auto tlvs = header.tlvs();
    ASSERT_EQ(tlvs.size(), 3U);
    EXPECT_EQ(tlvs[0].type_, 0x01);
    EXPECT_EQ(text(tlvs[0].value_), "h2");
    EXPECT_EQ(tlvs[1].type_, 0x02);
    EXPECT_EQ(text(tlvs[1].value_), "example.com");
    EXPECT_EQ(tlvs[2].type_, 0xE0);
    EXPECT_TRUE(tlvs[2].value_.empty());
}

const proxy_protocol_header& parse_whole(proxy_protocol_parser& parser, std::string_view input) {
    const auto result = parser.parse(wire(input));
    EXPECT_TRUE(result.complete_);
    EXPECT_EQ(result.consumed_bytes_, input.size());
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::finished);
    return parser.header();
}

// Feeds `chunks` in order; the last one must be rejected.
void expect_chunks_rejected(std::initializer_list<std::string_view> chunks, proxy_protocol_parser_options options = {}) {
    proxy_protocol_parser parser(options);
    std::size_t index = 0;
    for (const auto chunk : chunks) {
        if (++index < chunks.size()) {
            const auto result = parser.parse(wire(chunk));
            EXPECT_FALSE(result.complete_);
            EXPECT_EQ(result.consumed_bytes_, chunk.size());
            continue;
        }
        EXPECT_THROW((void)parser.parse(wire(chunk)), proxy_protocol_error);
    }
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::failed);
}

void expect_rejected(std::string_view input, proxy_protocol_parser_options options = {}) {
    SCOPED_TRACE(std::string(input));
    expect_chunks_rejected({input}, options);
}

template <typename check_type>
void expect_every_split_point(std::string_view input, check_type check) {
    for (std::size_t split = 0; split <= input.size(); ++split) {
        SCOPED_TRACE(split);
        proxy_protocol_parser parser;
        const auto first = parser.parse(wire(input.substr(0, split)));
        EXPECT_EQ(first.consumed_bytes_, split);
        if (split < input.size()) {
            EXPECT_FALSE(first.complete_);
            const auto second = parser.parse(wire(input.substr(split)));
            EXPECT_TRUE(second.complete_);
            EXPECT_EQ(second.consumed_bytes_, input.size() - split);
        } else {
            EXPECT_TRUE(first.complete_);
        }
        ASSERT_EQ(parser.state(), proxy_protocol_parser_state::finished);
        check(parser.header());
    }
}

template <typename check_type>
void expect_byte_by_byte(std::string_view input, check_type check) {
    proxy_protocol_parser parser;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto result = parser.parse(wire(input.substr(index, 1)));
        EXPECT_EQ(result.consumed_bytes_, 1U);
        EXPECT_EQ(result.complete_, index + 1 == input.size());
    }
    ASSERT_EQ(parser.state(), proxy_protocol_parser_state::finished);
    check(parser.header());
}

}  // namespace

TEST(Ruvia, proxy_protocol_v1_tcp4_yields_endpoints) {
    proxy_protocol_parser parser;
    expect_v1_tcp4(parse_whole(parser, v1_tcp4));
}

TEST(Ruvia, proxy_protocol_v1_tcp6_yields_endpoints) {
    proxy_protocol_parser parser;
    const auto& header = parse_whole(parser, "PROXY TCP6 2001:db8::1 ::ffff:192.0.2.1 0 65535\r\n");
    EXPECT_EQ(header.version(), proxy_protocol_version::v1);
    EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
    EXPECT_EQ(header.transport(), proxy_protocol_transport::stream);
    EXPECT_EQ(header.source(), endpoint("2001:db8::1", 0));
    EXPECT_EQ(header.destination(), endpoint("::ffff:192.0.2.1", 65535));
}

TEST(Ruvia, proxy_protocol_v1_unknown_keeps_real_endpoints) {
    for (const std::string_view input :
        {std::string_view("PROXY UNKNOWN\r\n"), std::string_view("PROXY UNKNOWN ffff::1 ffff::2 65535 65535\r\n"),
            std::string_view("PROXY UNKNOWN anything \x01 goes\r\n")}) {
        SCOPED_TRACE(std::string(input));
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, input);
        EXPECT_EQ(header.version(), proxy_protocol_version::v1);
        EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
        EXPECT_EQ(header.transport(), proxy_protocol_transport::unspecified);
        EXPECT_FALSE(header.source());
        EXPECT_FALSE(header.destination());
        EXPECT_TRUE(header.tlvs().empty());
    }
}

TEST(Ruvia, proxy_protocol_v2_proxy_inet_exposes_tlvs_in_wire_order) {
    proxy_protocol_parser parser;
    const auto input = v2_inet_with_tlvs();
    expect_v2_inet_with_tlvs(parse_whole(parser, input));
}

TEST(Ruvia, proxy_protocol_v2_proxy_inet6_datagram_yields_endpoints) {
    proxy_protocol_parser parser;
    const auto input = v2_header(0x21, 0x22, inet6_block() + tlv(0x03, std::string("\x01\x02\x03\x04", 4)));
    const auto& header = parse_whole(parser, input);
    EXPECT_EQ(header.version(), proxy_protocol_version::v2);
    EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
    EXPECT_EQ(header.transport(), proxy_protocol_transport::datagram);
    EXPECT_EQ(header.source(), endpoint("2001:db8::1", 1234));
    EXPECT_EQ(header.destination(), endpoint("2001:db8::2", 80));
    // CRC32C is exposed raw, not verified.
    ASSERT_EQ(header.tlvs().size(), 1U);
    EXPECT_EQ(header.tlvs()[0].type_, 0x03);
    EXPECT_EQ(text(header.tlvs()[0].value_), std::string("\x01\x02\x03\x04", 4));
}

TEST(Ruvia, proxy_protocol_v2_local_ignores_address_block) {
    for (const auto& input : {v2_header(0x20, 0x00, ""), v2_header(0x20, 0x11, inet_block() + tlv(0x01, "h2")),
             v2_header(0x20, 0x21, "short"), v2_header(0x20, 0x31, "")}) {
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, input);
        EXPECT_EQ(header.version(), proxy_protocol_version::v2);
        EXPECT_EQ(header.command(), proxy_protocol_command::local);
        EXPECT_FALSE(header.source());
        EXPECT_FALSE(header.destination());
        EXPECT_TRUE(header.tlvs().empty());
    }
}

TEST(Ruvia, proxy_protocol_v2_unspec_and_unix_keep_real_endpoints) {
    {
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, v2_header(0x21, 0x00, "opaque"));
        EXPECT_EQ(header.command(), proxy_protocol_command::proxy);
        EXPECT_EQ(header.transport(), proxy_protocol_transport::unspecified);
        EXPECT_FALSE(header.source());
        EXPECT_TRUE(header.tlvs().empty());
    }
    {
        // An UNSPEC transport carries no address information to use.
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, v2_header(0x21, 0x10, inet_block() + tlv(0x02, "a")));
        EXPECT_EQ(header.transport(), proxy_protocol_transport::unspecified);
        EXPECT_FALSE(header.source());
        EXPECT_FALSE(header.destination());
        ASSERT_EQ(header.tlvs().size(), 1U);
        EXPECT_EQ(text(header.tlvs()[0].value_), "a");
    }
    {
        proxy_protocol_parser parser;
        const auto& header = parse_whole(parser, v2_header(0x21, 0x31, std::string(216, '\0') + tlv(0x05, "id")));
        EXPECT_EQ(header.transport(), proxy_protocol_transport::stream);
        EXPECT_FALSE(header.source());
        EXPECT_FALSE(header.destination());
        ASSERT_EQ(header.tlvs().size(), 1U);
        EXPECT_EQ(header.tlvs()[0].type_, 0x05);
        EXPECT_EQ(text(header.tlvs()[0].value_), "id");
    }
}

TEST(Ruvia, proxy_protocol_v1_every_split_point) {
    expect_every_split_point(v1_tcp4, expect_v1_tcp4);
}

TEST(Ruvia, proxy_protocol_v2_every_split_point) {
    const auto input = v2_inet_with_tlvs();
    expect_every_split_point(input, expect_v2_inet_with_tlvs);
}

TEST(Ruvia, proxy_protocol_byte_by_byte) {
    expect_byte_by_byte(v1_tcp4, expect_v1_tcp4);
    const auto input = v2_inet_with_tlvs();
    expect_byte_by_byte(input, expect_v2_inet_with_tlvs);
}

TEST(Ruvia, proxy_protocol_consumed_bytes_exclude_payload) {
    {
        proxy_protocol_parser parser;
        const auto input = std::string(v1_tcp4) + std::string(http_request_line);
        const auto result = parser.parse(wire(input));
        EXPECT_TRUE(result.complete_);
        EXPECT_EQ(result.consumed_bytes_, v1_tcp4.size());
        expect_v1_tcp4(parser.header());
    }
    {
        // The CR of the CRLF ends one chunk; the LF and the payload follow.
        proxy_protocol_parser parser;
        const auto first = parser.parse(wire(v1_tcp4.substr(0, v1_tcp4.size() - 1)));
        EXPECT_FALSE(first.complete_);
        const auto rest = std::string("\n") + std::string(http_request_line);
        const auto second = parser.parse(wire(rest));
        EXPECT_TRUE(second.complete_);
        EXPECT_EQ(second.consumed_bytes_, 1U);
    }
    {
        proxy_protocol_parser parser;
        const auto header = v2_inet_with_tlvs();
        const auto input = header + std::string(http_request_line);
        const auto result = parser.parse(wire(input));
        EXPECT_TRUE(result.complete_);
        EXPECT_EQ(result.consumed_bytes_, header.size());
        expect_v2_inet_with_tlvs(parser.header());
    }
    {
        // A zero-length v2 header ends with its fixed part.
        proxy_protocol_parser parser;
        const auto input = v2_header(0x20, 0x00, "") + std::string(http_request_line);
        const auto result = parser.parse(wire(input));
        EXPECT_TRUE(result.complete_);
        EXPECT_EQ(result.consumed_bytes_, 16U);
    }
}

TEST(Ruvia, proxy_protocol_rejects_invalid_signature_immediately) {
    expect_rejected("G");
    expect_rejected(std::string_view("\0", 1));
    expect_rejected("PROXX");
    expect_rejected("proxy ");
    expect_rejected(std::string_view("\r\n\r\n\x01", 5));
    expect_rejected(std::string_view("\r\n\r\n\0\r\nQUIT\r", 12));
    expect_rejected(http_request_line);
    // A signature prefix is not rejected while it may still complete.
    for (const std::string_view prefix : {std::string_view("PROX"), std::string_view("\r\n\r\n\0\r\nQ", 8)}) {
        proxy_protocol_parser parser;
        const auto result = parser.parse(wire(prefix));
        EXPECT_FALSE(result.complete_);
        EXPECT_EQ(result.consumed_bytes_, prefix.size());
        EXPECT_EQ(parser.state(), proxy_protocol_parser_state::active);
    }
}

TEST(Ruvia, proxy_protocol_v1_rejects_overlong_line) {
    constexpr std::string_view prefix = "PROXY UNKNOWN ";
    const auto longest = std::string(prefix) + std::string(107 - prefix.size() - 2, 'a') + "\r\n";
    ASSERT_EQ(longest.size(), 107U);
    proxy_protocol_parser parser;
    EXPECT_EQ(parse_whole(parser, longest).transport(), proxy_protocol_transport::unspecified);

    expect_rejected(std::string(prefix) + std::string(107 - prefix.size() - 1, 'a') + "\r\n");
    // 107 bytes without CRLF reject without waiting for more input.
    expect_rejected(std::string(prefix) + std::string(107 - prefix.size(), 'a'));
    // A CR as the 107th byte leaves no room for its LF.
    expect_rejected(std::string(prefix) + std::string(107 - prefix.size() - 1, 'a') + "\r");
}

TEST(Ruvia, proxy_protocol_v1_rejects_bad_port) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 65536 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 123456 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 -1 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 +1 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 http 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 0x1BB\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324\r\n"),
         }) {
        expect_rejected(input);
    }
}

TEST(Ruvia, proxy_protocol_v1_rejects_port_leading_zero) {
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 056324 443\r\n");
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 56324 00\r\n");
    proxy_protocol_parser parser;
    const auto& header = parse_whole(parser, "PROXY TCP4 192.168.0.1 192.168.0.11 0 0\r\n");
    EXPECT_EQ(header.source(), endpoint("192.168.0.1", 0));
    EXPECT_EQ(header.destination(), endpoint("192.168.0.11", 0));
}

TEST(Ruvia, proxy_protocol_v1_rejects_bad_address) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4 2001:db8::1 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP6 2001:db8::1 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP6 fe80::1%1 2001:db8::2 1 2\r\n"),
             std::string_view("PROXY TCP6 [2001:db8::1] 2001:db8::2 1 2\r\n"),
             std::string_view("PROXY TCP4 192.168.000.1 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP4 192.168.0 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP4 256.0.0.1 192.168.0.11 1 2\r\n"),
             std::string_view("PROXY TCP4 localhost 192.168.0.11 1 2\r\n"),
         }) {
        expect_rejected(input);
    }
}

TEST(Ruvia, proxy_protocol_v1_rejects_bare_line_endings) {
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\n");
    expect_rejected("PROXY UNKNOWN\n");
    expect_rejected("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\rX");
    expect_chunks_rejected({"PROXY TCP4 192.168.0.1 192.168.0.11 56324 443\r", "X"});
    expect_rejected("PROXY UNKNOWN\r\r\n");
}

TEST(Ruvia, proxy_protocol_v1_rejects_bad_syntax) {
    for (const std::string_view input : {
             std::string_view("PROXY TCP4  192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1  192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443 \r\n"),
             std::string_view("PROXY TCP4 192.168.0.1 192.168.0.11 56324 443 1\r\n"),
             std::string_view("PROXY TCP5 192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY tcp4 192.168.0.1 192.168.0.11 56324 443\r\n"),
             std::string_view("PROXY TCP4\r\n"),
             std::string_view("PROXY UNKNOWNX\r\n"),
             std::string_view("PROXY \r\n"),
         }) {
        expect_rejected(input);
    }
}

TEST(Ruvia, proxy_protocol_v2_rejects_invalid_fixed_fields_immediately) {
    const auto signature = std::string("\r\n\r\n\0\r\nQUIT\n", 12);
    // Version nibble other than 2.
    expect_rejected(signature + "\x11");
    expect_rejected(signature + "\x31");
    // Unassigned command.
    expect_rejected(signature + "\x22");
    expect_rejected(signature + "\x2F");
    // Unspecified address family.
    expect_rejected(signature + "\x21\x41");
    expect_rejected(signature + "\x21\xF1");
    // Unspecified transport protocol.
    expect_rejected(signature + "\x21\x13");
    expect_rejected(signature + "\x20\x0F");
}

TEST(Ruvia, proxy_protocol_v2_rejects_over_limit_length) {
    const proxy_protocol_parser_options options{.max_v2_header_bytes_ = 64};
    // Rejected once the length field arrives, before any block byte.
    expect_rejected(v2_header(0x21, 0x11, inet_block() + std::string(37, 'x')).substr(0, 16), options);
    expect_rejected(v2_header(0x20, 0x00, std::string(0xFFFF, 'x')).substr(0, 16));

    proxy_protocol_parser parser(options);
    const auto at_limit = v2_header(0x21, 0x11, inet_block() + tlv(0xE0, std::string(33, 'x')));
    ASSERT_EQ(at_limit.size(), 64U);
    const auto& header = parse_whole(parser, at_limit);
    ASSERT_EQ(header.tlvs().size(), 1U);
    EXPECT_EQ(header.tlvs()[0].value_.size(), 33U);
}

TEST(Ruvia, proxy_protocol_v2_rejects_short_address_block) {
    expect_rejected(v2_header(0x21, 0x11, inet_block().substr(0, 11)).substr(0, 16));
    expect_rejected(v2_header(0x21, 0x21, inet6_block().substr(0, 35)).substr(0, 16));
    expect_rejected(v2_header(0x21, 0x31, std::string(215, '\0')).substr(0, 16));
    expect_rejected(v2_header(0x21, 0x12, ""));
}

TEST(Ruvia, proxy_protocol_v2_rejects_tlv_overrun) {
    expect_rejected(v2_header(0x21, 0x11, inet_block() + std::string("\x01\x00\x05"
                                                                     "ab",
                                                             5)));
    expect_rejected(v2_header(0x21, 0x11, inet_block() + std::string("\x01\x00", 2)));
    expect_rejected(v2_header(0x21, 0x11, inet_block() + tlv(0x01, "h2") + std::string("\x02", 1)));
}

TEST(Ruvia, proxy_protocol_options_reject_small_v2_limit) {
    EXPECT_THROW((void)proxy_protocol_parser(proxy_protocol_parser_options{.max_v2_header_bytes_ = 51}), std::invalid_argument);
    EXPECT_NO_THROW((void)proxy_protocol_parser(proxy_protocol_parser_options{.max_v2_header_bytes_ = 52}));
}

TEST(Ruvia, proxy_protocol_parse_after_finished_or_failed_throws_logic_error) {
    proxy_protocol_parser parser;
    EXPECT_THROW((void)parser.header(), std::logic_error);
    (void)parse_whole(parser, v1_tcp4);
    EXPECT_THROW((void)parser.parse(wire(http_request_line)), std::logic_error);
    EXPECT_THROW((void)parser.parse({}), std::logic_error);
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::finished);

    proxy_protocol_parser failed;
    EXPECT_THROW((void)failed.parse(wire("X")), proxy_protocol_error);
    EXPECT_THROW((void)failed.parse(wire(v1_tcp4)), std::logic_error);
    EXPECT_THROW((void)failed.header(), std::logic_error);
    EXPECT_EQ(failed.state(), proxy_protocol_parser_state::failed);
}

TEST(Ruvia, proxy_protocol_reset_reuses_parser) {
    proxy_protocol_parser parser;
    const auto v2 = v2_inet_with_tlvs();
    expect_v2_inet_with_tlvs(parse_whole(parser, v2));
    parser.reset();
    EXPECT_EQ(parser.state(), proxy_protocol_parser_state::active);
    expect_v1_tcp4(parse_whole(parser, v1_tcp4));

    parser.reset();
    EXPECT_THROW((void)parser.parse(wire("PROXY TCP4 1.2.3.4\n")), proxy_protocol_error);
    parser.reset();
    expect_v2_inet_with_tlvs(parse_whole(parser, v2));

    // A reset in the middle of a header starts over.
    parser.reset();
    (void)parser.parse(wire(v2.substr(0, 20)));
    parser.reset();
    expect_v1_tcp4(parse_whole(parser, v1_tcp4));
}

TEST(Ruvia, proxy_protocol_moved_parser_keeps_header_views) {
    proxy_protocol_parser parser;
    const auto input = v2_inet_with_tlvs();
    (void)parse_whole(parser, input);
    const auto tlvs = parser.header().tlvs();
    const proxy_protocol_parser moved(std::move(parser));
    EXPECT_EQ(moved.state(), proxy_protocol_parser_state::finished);
    expect_v2_inet_with_tlvs(moved.header());
    EXPECT_EQ(moved.header().tlvs().data(), tlvs.data());
}

static_assert(std::is_trivially_copyable_v<ruvia::ip_address>);
static_assert(std::is_trivially_copyable_v<ruvia::ip_endpoint>);
static_assert(ruvia::ip_address{}.family() == ruvia::ip_address_family::ipv4);
static_assert(ruvia::ip_endpoint(ruvia::ip_address{}, 443).port() == 443);

TEST(RuviaHttp, ip_address_owns_network_order_bytes_and_endpoint_values) {
    std::array<std::uint8_t, 4> bytes{192, 0, 2, 1};
    const auto address = ruvia::ip_address::ipv4(bytes);
    bytes[0] = 127;
    EXPECT_EQ(address.family(), ruvia::ip_address_family::ipv4);
    ASSERT_EQ(address.bytes().size(), 4);
    EXPECT_EQ(address.bytes()[0], 192);
    EXPECT_EQ(address.bytes()[3], 1);
    EXPECT_EQ(address.scope_id(), 0);
    EXPECT_EQ(address.to_string(), "192.0.2.1");
    const ruvia::ip_endpoint endpoint(address, 65535);
    EXPECT_EQ(endpoint.address(), address);
    EXPECT_EQ(endpoint.port(), 65535);
    EXPECT_EQ(endpoint, ruvia::ip_endpoint(address, 65535));
    EXPECT_NE(endpoint, ruvia::ip_endpoint(address, 0));
    EXPECT_EQ(ruvia::ip_endpoint{}.port(), 0);
    EXPECT_EQ(ruvia::ip_address{}.to_string(), "0.0.0.0");
}

TEST(RuviaHttp, ip_address_formats_rfc5952_and_round_trips) {
    struct example final {
        std::string_view input;
        std::string_view canonical;
    };
    for (const auto& entry : std::array{
             example{"0.0.0.0", "0.0.0.0"},
             example{"255.255.255.255", "255.255.255.255"},
             example{"0:0:0:0:0:0:0:0", "::"},
             example{"0:0:0:0:0:0:0:1", "::1"},
             example{"2001:0DB8:0000:0000:0001:0000:0000:0001", "2001:db8::1:0:0:1"},
             example{"2001:db8:1:2:3:4:0:0", "2001:db8:1:2:3:4::"},
             example{"2001:db8:0:1:2:3:4:5", "2001:db8:0:1:2:3:4:5"},
             example{"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295"},
             example{"::ffff:c000:201", "::ffff:192.0.2.1"},
             example{"fe80::1%12", "fe80::1%12"}}) {
        SCOPED_TRACE(entry.input);
        const auto address = ruvia::ip_address::parse(entry.input);
        ASSERT_TRUE(address.has_value());
        EXPECT_EQ(address->to_string(), entry.canonical);
        EXPECT_EQ(ruvia::ip_address::parse(address->to_string()), address);
    }
}

TEST(RuviaHttp, ip_address_preserves_ipv6_family_scope_and_mapped_identity) {
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = 0xfe;
    bytes[1] = 0x80;
    bytes[15] = 1;
    const auto address = ruvia::ip_address::ipv6(bytes, 12);
    EXPECT_EQ(address.family(), ruvia::ip_address_family::ipv6);
    EXPECT_EQ(address.bytes().size(), 16);
    EXPECT_EQ(address.scope_id(), 12);
    EXPECT_EQ(address.to_string(), "fe80::1%12");
    EXPECT_NE(address, ruvia::ip_address::ipv6(bytes, 13));
    EXPECT_NE(ruvia::ip_address::parse("192.0.2.1"), ruvia::ip_address::parse("::ffff:192.0.2.1"));
}

TEST(RuviaHttp, ip_address_rejects_nonliteral_and_malformed_input) {
    for (const std::string_view text : {"", "localhost", "1", "192.0.2", "01.2.3.4", "192.0.2.256",
             "192.0.2.1:80", "192.0.2.1%1", "[::1]", "[::1]:80", " ::1", "::1 ",
             "1::2::3", "1:2:3:4:5:6:7:8:9", "::ffff:01.2.3.4", "fe80::1%",
             "fe80::1%eth0", "fe80::1%-1", "fe80::1%+1", "fe80::1%4294967296", "fe80::1%1%2"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(ruvia::ip_address::parse(text).has_value());
    }
    EXPECT_FALSE(ruvia::ip_address::parse(std::string_view("::1\0suffix", 10)).has_value());
    constexpr std::string_view bounded = "192.0.2.1suffix";
    EXPECT_TRUE(ruvia::ip_address::parse(bounded.substr(0, 9)).has_value());
    EXPECT_FALSE(ruvia::ip_address::parse(bounded).has_value());
}

TEST(RuviaHttp, ip_address_formats_maximal_scoped_ipv6) {
    const auto address = ruvia::ip_address::parse("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->to_string(), "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
}

TEST(Ruvia, port_number_rejects_wide_inputs_without_changing_an_existing_origin) {
    const auto rejects = [](auto input) {
        try {
            (void)ruvia::http_origin_view::https({.host_ = "example.test", .port_ = input});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const std::array<std::int64_t, 7> invalid{
        std::numeric_limits<std::int64_t>::min(), -65093, -1,
        65536, 65979, 131071, std::numeric_limits<std::int64_t>::max()};
    for (const auto input : invalid) {
        EXPECT_TRUE(rejects(input));
        EXPECT_TRUE(!ruvia::port_number::try_from_value(input).has_value());
        ruvia::http_origin_options options{.host_ = "example.test", .port_ = 8443};
        bool assignment_rejected = false;
        try {
            options.port_ = input;
        } catch (const std::invalid_argument&) {
            assignment_rejected = true;
        }
        EXPECT_TRUE(assignment_rejected);
        const auto retained = ruvia::http_origin_view::https(options);
        EXPECT_EQ(ruvia::make_http_origin_authority(retained), "example.test:8443");
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(rejects(maximum));
    EXPECT_TRUE(!ruvia::port_number::try_from_value(maximum).has_value());
}

TEST(Ruvia, port_number_preserves_zero_maximum_and_scheme_default_authorities) {
    struct port_case final {
        std::uint64_t input_;
        std::string_view authority_;
    };
    for (const auto entry : std::array{
             port_case{0, "[::1]:0"},
             port_case{1, "[::1]:1"},
             port_case{80, "[::1]:80"},
             port_case{443, "[::1]"},
             port_case{65535, "[::1]:65535"}}) {
        const auto origin = ruvia::http_origin_view::https({.host_ = "[::1]", .port_ = entry.input_});
        EXPECT_EQ(ruvia::make_http_origin_authority(origin), entry.authority_);
    }
    const auto absent = ruvia::http_origin_view::http({.host_ = "example.test"});
    const auto zero = ruvia::http_origin_view::http({.host_ = "example.test", .port_ = 0});
    EXPECT_EQ(ruvia::make_http_origin_authority(absent), "example.test");
    EXPECT_EQ(ruvia::make_http_origin_authority(zero), "example.test:0");
}

TEST(Ruvia, port_number_rejects_invalid_authority_defaults_before_origin_comparison) {
    const auto authority = ruvia::parse_http_authority("example.test");
    EXPECT_TRUE(authority.has_value());
    if (!authority) {
        return;
    }
    const auto rejects = [](auto&& operation) {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    for (const auto source : std::array<std::int64_t, 7>{
             std::numeric_limits<std::int64_t>::min(), -65093, -1,
             65536, 65979, 131071, std::numeric_limits<std::int64_t>::max()}) {
        EXPECT_TRUE(rejects([&] { (void)authority->effective_port(source); }));
        EXPECT_TRUE(rejects([&] {
            (void)ruvia::http_authorities_equal("example.test", "example.test:443", source);
        }));
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(rejects([&] { (void)authority->effective_port(maximum); }));
    EXPECT_TRUE(rejects([&] {
        (void)ruvia::http_authorities_equal("example.test", "example.test:443", maximum);
    }));
    EXPECT_TRUE(ruvia::http_authorities_equal("example.test", "EXAMPLE.test:443", 443));
    EXPECT_TRUE(!ruvia::http_authorities_equal("example.test", "example.test:443", 80));
}

TEST(Ruvia, port_number_preserves_absent_empty_and_explicit_authority_port_rules) {
    const auto absent = ruvia::parse_http_authority("[::1]");
    const auto empty = ruvia::parse_http_authority("[::1]:");
    const auto explicit_port = ruvia::parse_http_authority("[::1]:8443");
    EXPECT_TRUE(absent && empty && explicit_port);
    if (!absent || !empty || !explicit_port) {
        return;
    }
    for (const auto source : std::array<std::uint64_t, 5>{0, 1, 80, 443, 65535}) {
        EXPECT_TRUE(absent->effective_port(source) == source);
        EXPECT_TRUE(empty->effective_port(source) == source);
        EXPECT_TRUE(explicit_port->effective_port(source) == 8443);
        EXPECT_TRUE(ruvia::http_authorities_equal("[::1]", "[::1]:", source));
    }
    EXPECT_TRUE(!ruvia::http_authorities_equal("[::1]", "[::1]:0", 0));
    EXPECT_TRUE(ruvia::http_authorities_equal("[::1]:", "[::1]:65535", 65535));
    EXPECT_TRUE(!ruvia::http_authorities_equal("[::1]:8443", "[::1]:443", 443));
}

namespace {

void write_text(std::span<char> target, std::string_view text) {
    std::memcpy(target.data(), text.data(), text.size());
}

enum class reallocation_operation { reserve,
    shrink,
    append,
    append_alias };

}  // namespace

TEST(Ruvia, io_buffer_publishes_only_committed_bytes) {
    {
        ruvia::http_io_buffer buffer;
        EXPECT_TRUE(buffer.empty());
        auto space = buffer.prepare(16);
        EXPECT_EQ(space.size(), std::size_t{16});
        EXPECT_TRUE(buffer.empty());
        write_text(space, "hello");
        buffer.commit(5);
        EXPECT_EQ(buffer.view(), "hello");
        EXPECT_TRUE(buffer.capacity() >= std::size_t{16});
        EXPECT_TRUE(ruvia::test::throws_on([&] { buffer.commit(buffer.capacity()); }));
        EXPECT_EQ(buffer.view(), "hello");
    }
}

TEST(Ruvia, io_buffer_grows_geometrically_and_keeps_readable_bytes) {
    ruvia::http_io_buffer buffer;
    buffer.append("abc");
    EXPECT_EQ(buffer.capacity(), std::size_t{3});
    write_text(buffer.prepare(1), "d");
    buffer.commit(1);
    EXPECT_EQ(buffer.capacity(), std::size_t{6});
    EXPECT_EQ(buffer.view(), "abcd");
    const std::string large(100, 'x');
    buffer.append(large);
    EXPECT_EQ(buffer.view(), "abcd" + large);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)buffer.prepare(static_cast<std::size_t>(-1)); }));
    EXPECT_EQ(buffer.view(), "abcd" + large);
}

TEST(Ruvia, io_buffer_append_accepts_its_own_bytes) {
    ruvia::http_io_buffer buffer;
    buffer.reserve(8);
    buffer.append("abcdef");
    // Fits the tail.
    buffer.append(buffer.view().substr(0, 2));
    EXPECT_EQ(buffer.view(), "abcdefab");
    // Needs new storage while the source lives in the old one.
    buffer.append(buffer.view());
    EXPECT_EQ(buffer.view(), "abcdefababcdefab");
    // Needs the room of the consumed prefix while the source lies inside it.
    ruvia::http_io_buffer window;
    window.reserve(8);
    window.append("abcdefgh");
    const auto consumed = window.view();
    window.consume(6);
    window.append(consumed.substr(0, 4));
    EXPECT_EQ(window.view(), "ghabcd");
    EXPECT_EQ(window.capacity(), std::size_t{8});
}

TEST(Ruvia, io_buffer_small_reads_preserve_the_sliding_window) {
    for (const bool prepared : {false, true}) {
        {
            ruvia::http_io_buffer buffer;
            std::string expected(64, '\0');
            for (std::size_t index = 0; index < expected.size(); ++index) {
                expected[index] = static_cast<char>(index);
            }
            buffer.append(expected);
            for (std::size_t index = 0; index < 1024; ++index) {
                buffer.consume(1);
                expected.erase(0, 1);
                const char byte = static_cast<char>(index);
                if (prepared) {
                    buffer.prepare(1)[0] = byte;
                    buffer.commit(1);
                } else {
                    buffer.append(std::string_view(&byte, 1));
                }
                expected.push_back(byte);
                EXPECT_EQ(buffer.view(), std::string_view(expected));
            }
            // A self-append can grow or compact; it must read the old window.
            buffer.consume(32);
            expected.erase(0, 32);
            const auto copied = expected;
            buffer.append(buffer.view());
            expected.append(copied);
            EXPECT_EQ(buffer.view(), std::string_view(expected));
        }
    }
}

TEST(Ruvia, io_buffer_reader_prepares_when_no_spare_is_available) {
    ruvia::http_io_buffer buffer;
    buffer.reserve(8);
    buffer.append("abcdefgh");
    buffer.consume(1);
    auto space = buffer.prepare_spare();
    if (space.empty()) {
        space = buffer.prepare(1);
    }
    ASSERT_FALSE(space.empty());
    space[0] = 'i';
    buffer.commit(1);
    EXPECT_EQ(buffer.view(), "bcdefghi");
}

TEST(Ruvia, http_priority_structured_dictionary_types_duplicates_and_parameters) {
    const auto parsed_value = ruvia::parse_http_priority("u=5;note=\"x\", i, extra=(token :YWJj:);p=?0");
    EXPECT_TRUE((parsed_value.index() == 0));
    EXPECT_EQ(*std::get<0>(parsed_value).urgency_, 5);
    EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
    const auto defaults = ruvia::parse_http_priority("u=8, i=42, u=2, u=\"ignored\"");
    EXPECT_TRUE((defaults.index() == 0) && !std::get<0>(defaults).urgency_ && !std::get<0>(defaults).incremental_);
    EXPECT_EQ(std::get<0>(defaults).request_priority().urgency_, 3);
    const auto partial = ruvia::parse_http_priority("u=0");
    EXPECT_TRUE((partial.index() == 0) && std::get<0>(partial).urgency_ == 0 && !std::get<0>(partial).incremental_);
    for (const auto invalid : {"u=", "u=2,", "U=2", "i=?2", "u=1.1234", "u=(x\t y)", "x=\"bad\\q\""}) {
        EXPECT_TRUE((ruvia::parse_http_priority(invalid).index() != 0));
    }
}
TEST(Ruvia, http_priority_byte_sequences_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"u=2, i;bytes=:YQ=:", "u=2, i;bytes=:YQ==:", "u=2, i;bytes=:YQ:",
             "u=2, i;bytes=:YWJj:", "u=2, i;bytes=:YWJ:", "u=2, i;bytes=:YWJ=:", "u=2, i;bytes=:YR==:",
             "u=2, i, x=:YQ=:", "u=2, i, x=(:YQ=:)", "u=2, i;bytes=::"}) {
        const auto parsed_value = ruvia::parse_http_priority(value);
        EXPECT_TRUE((parsed_value.index() == 0));
        if ((parsed_value.index() == 0)) {
            EXPECT_TRUE(std::get<0>(parsed_value).urgency_ == 2);
            EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
        }
    }
    for (const auto value : {"u=2, i;bytes=:Y:", "u=2, i;bytes=:Y=:", "u=2, i;bytes=:YWJj=:",
             "u=2, i;bytes=:YWJ==:", "u=2, i;bytes=:YQ===:", "u=2, i;bytes=:Y=Q:",
             "u=2, i;bytes=:YQ$:", "u=2, i;bytes=:YQ\n:", "u=2, i;bytes=:YQ"}) {
        EXPECT_TRUE((ruvia::parse_http_priority(value).index() != 0));
    }
}

TEST(Ruvia, http_priority_repeated_headers_combine_members_and_replace_invalid_values) {
    const std::array headers{ruvia::http_header_view{"Priority", "u=2, i"},
        ruvia::http_header_view{"X-Other", "u=0"},
        ruvia::http_header_view{"pRIORITY", "u=\"ignored\""}};
    const auto parsed_value = ruvia::parse_http_priority(headers);
    EXPECT_TRUE((parsed_value.index() == 0) && !std::get<0>(parsed_value).urgency_ && std::get<0>(parsed_value).incremental_ == true);
    const std::array malformed{ruvia::http_header_view{"priority", "u=2"},
        ruvia::http_header_view{"priority", "i=?2"}};
    EXPECT_TRUE((ruvia::parse_http_priority(malformed).index() != 0));
    const std::array empty_repeated{ruvia::http_header_view{"priority", ""},
        ruvia::http_header_view{"priority", "u=1"}};
    EXPECT_TRUE((ruvia::parse_http_priority(empty_repeated).index() != 0));
    EXPECT_TRUE(std::get<0>(ruvia::parse_http_priority(std::span<const ruvia::http_header_view>{})).request_priority().urgency_ == 3);
}

TEST(Ruvia, http_priority_repeated_fields_match_their_comma_joined_dictionary) {
    const std::array headers{ruvia::http_header_view{"Priority", "extra=\"alpha"},
        ruvia::http_header_view{"X-Other", "ignored"},
        ruvia::http_header_view{"priority", "beta\", u=1, i"}};
    const auto combined = ruvia::parse_http_priority("extra=\"alpha, beta\", u=1, i");
    EXPECT_TRUE((combined.index() == 0) && std::get<0>(combined).urgency_ == 1 && std::get<0>(combined).incremental_ == true);
    const auto parsed_value = ruvia::parse_http_priority(headers);
    EXPECT_TRUE((parsed_value.index() == 0));
    if (parsed_value.index() == 0) {
        EXPECT_TRUE(std::get<0>(parsed_value).urgency_ == 1);
        EXPECT_TRUE(std::get<0>(parsed_value).incremental_ == true);
    }

    const std::array parameter_headers{ruvia::http_header_view{"priority", "i;note=\"a"},
        ruvia::http_header_view{"priority", "b\", u=0"}};
    const auto parameters = ruvia::parse_http_priority(parameter_headers);
    EXPECT_TRUE((parameters.index() == 0));
    if (parameters.index() == 0) {
        EXPECT_TRUE(std::get<0>(parameters).urgency_ == 0);
        EXPECT_TRUE(std::get<0>(parameters).incremental_ == true);
    }
}

TEST(Ruvia, http_priority_repeated_fields_preserve_separators_and_empty_values) {
    const std::array with_empty{ruvia::http_header_view{"Priority", "extra=\"alpha"},
        ruvia::http_header_view{"priority", ""},
        ruvia::http_header_view{"pRIORITY", "beta\", u=1, i"}};
    const auto parsed_value = ruvia::parse_http_priority(with_empty);
    EXPECT_TRUE((parsed_value.index() == 0) && std::get<0>(parsed_value).urgency_ == 1 && std::get<0>(parsed_value).incremental_ == true);

    const std::array tab_separated{ruvia::http_header_view{"priority", "u=0"},
        ruvia::http_header_view{"priority", "\ti"}};
    const auto tab = ruvia::parse_http_priority(tab_separated);
    EXPECT_TRUE((tab.index() == 0) && std::get<0>(tab).urgency_ == 0 && std::get<0>(tab).incremental_ == true);

    const std::array fragments{
        std::array<std::string_view, 2>{"u=1", ""},
        std::array<std::string_view, 2>{"u=1", "\t"},
        std::array<std::string_view, 2>{"u=1, i=?", "1"},
        std::array<std::string_view, 2>{"u=1, i, x=:YQ", ":"},
        std::array<std::string_view, 2>{"u=1", "2, i"},
        std::array<std::string_view, 2>{"x=\"a\\", "\"b\", u=1, i"},
    };
    for (const auto& values : fragments) {
        const std::array headers{ruvia::http_header_view{"priority", values[0]},
            ruvia::http_header_view{"priority", values[1]}};
        EXPECT_TRUE(ruvia::parse_http_priority(headers).index() != 0);
    }
}

TEST(Ruvia, http_priority_http2_frame_roundtrips_and_output_bounds) {
    std::array<char, 64> bytes_value{};
    auto h2 = ruvia::encode_http2_priority_update(bytes_value, 5, {.urgency_ = 1, .incremental_ = true});
    EXPECT_TRUE((h2.index() == 0));
    auto decoded = ruvia::decode_http2_priority_update(std::span(bytes_value).subspan(9, std::get<0>(h2) - 9));
    EXPECT_TRUE((decoded.index() == 0) && std::get<0>(decoded).element_id_ == 5 && !std::get<0>(decoded).push_ && std::get<0>(decoded).fields_.urgency_ == 1);
    EXPECT_TRUE((ruvia::encode_http_priority(std::span(bytes_value).first(1), {.urgency_ = 2}).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_priority_update(bytes_value, 0, {}).index() != 0));
}

TEST(Ruvia, http_priority_rejects_wide_local_urgency_without_replacing_an_existing_signal) {
    std::array<char, 64> bytes{};
    const auto rejects = [&](auto input) {
        try {
            (void)ruvia::encode_http_priority(bytes, {.urgency_ = input});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    for (const auto input : std::array<std::int64_t, 7>{
             std::numeric_limits<std::int64_t>::min(), -256, -1, 8,
             256, 259, std::numeric_limits<std::int64_t>::max()}) {
        EXPECT_TRUE(rejects(input));
    }
    EXPECT_TRUE(rejects(std::numeric_limits<std::uint64_t>::max()));

    ruvia::http_priority_fields fields{.urgency_ = 2, .incremental_ = true};
    bool rejected = false;
    try {
        fields.urgency_ = 259;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    const auto encoded = ruvia::encode_http_priority(bytes, fields);
    EXPECT_TRUE(encoded.index() == 0);
    if (const auto* count = std::get_if<std::size_t>(&encoded)) {
        const auto reparsed = ruvia::parse_http_priority(std::string_view(bytes.data(), *count));
        EXPECT_TRUE(reparsed.index() == 0);
        if (const auto* retained = std::get_if<ruvia::http_priority_fields>(&reparsed)) {
            EXPECT_TRUE(retained->urgency_ == 2);
            EXPECT_TRUE(retained->incremental_ == true);
        }
    }
}

TEST(Ruvia, http_priority_ignores_out_of_range_peer_urgency_instead_of_narrowing_it) {
    // RFC 9218 section 4 requires ignoring successfully parsed values outside
    // the urgency domain, including one that replaces a valid earlier member.
    for (const auto value : {"u=-256", "u=-1", "u=8", "u=256", "u=259", "u=2, u=256"}) {
        const auto parsed = ruvia::parse_http_priority(value);
        EXPECT_TRUE(parsed.index() == 0);
        if (const auto* fields = std::get_if<ruvia::http_priority_fields>(&parsed)) {
            EXPECT_TRUE(!fields->urgency_.has_value());
            EXPECT_TRUE(fields->request_priority().urgency_ == 3);
        }
    }
    const auto maximum = ruvia::parse_http_priority("u=7, i=?0");
    EXPECT_TRUE(maximum.index() == 0);
    if (const auto* fields = std::get_if<ruvia::http_priority_fields>(&maximum)) {
        EXPECT_TRUE(fields->urgency_ == 7);
        EXPECT_TRUE(fields->incremental_ == false);
    }
}

namespace {

// Item 1: Stateless CORS Policy
TEST(RuviaHttpPublicApi, cors_policy_preflight_and_response_rewrite) {
    ruvia::http_cors_policy_config config;
    config.allowed_origins_.emplace_back("https://app.example.com");
    config.allowed_origin_suffixes_.emplace_back(".trusted.example");
    config.allowed_origin_suffixes_.emplace_back("https://partner.test:8443");
    config.allowed_methods_.emplace_back("GET");
    config.allowed_methods_.emplace_back("POST");
    config.allowed_headers_.emplace_back("X-Custom-Auth");
    config.allowed_headers_.emplace_back("Content-Type");
    config.expose_headers_.emplace_back("X-Request-Id");
    config.expose_headers_.emplace_back("X-RateLimit-Remaining");
    config.allow_credentials_ = true;
    config.max_age_ = std::chrono::seconds(600);

    const ruvia::http_cors_policy policy(config);
    EXPECT_TRUE(policy.allows_origin("https://app.example.com"));
    EXPECT_TRUE(policy.allows_origin("https://sub.trusted.example"));
    EXPECT_TRUE(policy.allows_origin("http://a.b.trusted.example:8080"));
    EXPECT_FALSE(policy.allows_origin("https://trusted.example"));
    EXPECT_FALSE(policy.allows_origin("https://evil-trusted.example"));
    EXPECT_TRUE(policy.allows_origin("https://partner.test:8443"));
    EXPECT_TRUE(policy.allows_origin("https://api.partner.test:8443"));
    EXPECT_FALSE(policy.allows_origin("http://partner.test:8443"));
    EXPECT_FALSE(policy.allows_origin("https://partner.test:9443"));

    const auto preflight_ok = policy.evaluate_preflight({
        .method_ = "OPTIONS",
        .origin_ = "https://sub.trusted.example",
        .access_control_request_method_ = "POST",
        .access_control_request_headers_ = "content-type, x-custom-auth",
    });
    EXPECT_TRUE(preflight_ok.accepted());

    const auto find_header = [](std::span<const ruvia::http_header> headers, std::string_view name)
        -> std::optional<std::string_view> {
        for (const auto& field : headers) {
            if (ruvia::http_ascii_equals_ignore_case(field.name(), name)) {
                return field.value();
            }
        }
        return std::nullopt;
    };

    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Origin"),
        std::optional<std::string_view>("https://sub.trusted.example"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Credentials"),
        std::optional<std::string_view>("true"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Methods"),
        std::optional<std::string_view>("GET, POST"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Headers"),
        std::optional<std::string_view>("X-Custom-Auth, Content-Type"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Max-Age"),
        std::optional<std::string_view>("600"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Vary"),
        std::optional<std::string_view>("Origin, Access-Control-Request-Method"));

    // Disallowed header or method in preflight is rejected.
    const auto preflight_bad_header = policy.evaluate_preflight({
        .method_ = "OPTIONS",
        .origin_ = "https://sub.trusted.example",
        .access_control_request_method_ = "POST",
        .access_control_request_headers_ = "X-Forbidden-Header",
    });
    EXPECT_FALSE(preflight_bad_header.accepted());
    EXPECT_TRUE(preflight_bad_header.headers().empty());

    // Actual response header rewrite: strips upstream Access-Control-* headers,
    // writes policy headers, and merges Origin into Vary without duplicates.
    std::vector<ruvia::http_header> response_headers;
    response_headers.emplace_back("Content-Type", "application/json");
    response_headers.emplace_back("Access-Control-Allow-Origin", "*");
    response_headers.emplace_back("Access-Control-Allow-Methods", "DELETE");
    response_headers.emplace_back("Vary", "Accept-Encoding, Origin");

    EXPECT_TRUE(
        policy.rewrite_response_headers("https://app.example.com", response_headers));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Allow-Origin"),
        std::optional<std::string_view>("https://app.example.com"));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Allow-Credentials"),
        std::optional<std::string_view>("true"));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Expose-Headers"),
        std::optional<std::string_view>("X-Request-Id, X-RateLimit-Remaining"));
    EXPECT_FALSE(find_header(response_headers, "Access-Control-Allow-Methods").has_value());
    EXPECT_EQ(find_header(response_headers, "Vary"),
        std::optional<std::string_view>("Accept-Encoding, Origin"));

    // Server-side apply_response_headers emits Vary tokens before rejecting disallowed Origin/method/headers.
    {
        const std::array<ruvia::http_header_view, 1> bad_origin_headers{
            ruvia::http_header_view{"Origin", "https://untrusted.example"},
        };
        const auto get_bad_origin =
            ruvia::make_parsed_http_request("GET", "/resource", bad_origin_headers, {});
        ruvia::http_response get_resp;
        EXPECT_FALSE(policy.apply_response_headers(get_bad_origin, get_resp));
        EXPECT_FALSE(get_resp.header("Access-Control-Allow-Origin").has_value());
        EXPECT_EQ(get_resp.header("Vary"), std::optional<std::string_view>("Origin"));

        const std::array<ruvia::http_header_view, 3> bad_options_headers{
            ruvia::http_header_view{"Origin", "https://untrusted.example"},
            ruvia::http_header_view{"Access-Control-Request-Method", "DELETE"},
            ruvia::http_header_view{"Access-Control-Request-Headers", "X-Forbidden"},
        };
        const auto opts_bad =
            ruvia::make_parsed_http_request("OPTIONS", "/resource", bad_options_headers, {});
        ruvia::http_response opts_resp;
        EXPECT_FALSE(policy.apply_response_headers(opts_bad, opts_resp));
        EXPECT_FALSE(opts_resp.header("Access-Control-Allow-Origin").has_value());
        EXPECT_EQ(opts_resp.header("Vary"),
            std::optional<std::string_view>("Origin, Access-Control-Request-Method"));

        ruvia::http_cors_policy reflecting_policy(ruvia::http_cors_policy_config{
            .allowed_origins_ = {"https://app.example.com"},
            .reflect_request_headers_ = true,
        });
        ruvia::http_response reflect_opts_resp;
        EXPECT_FALSE(reflecting_policy.apply_response_headers(opts_bad, reflect_opts_resp));
        EXPECT_EQ(reflect_opts_resp.header("Vary"),
            std::optional<std::string_view>(
                "Origin, Access-Control-Request-Method, Access-Control-Request-Headers"));
    }

    // Vary and Connection use non-quoted comma-token scanning (unclosed quote does not hide later tokens).
    {
        std::vector<ruvia::http_header> vary_list;
        vary_list.emplace_back("Vary", "\"unclosed, Origin");
        ruvia::merge_http_vary_token(vary_list, "Origin");
        EXPECT_EQ(vary_list.size(), 1U);
        EXPECT_EQ(vary_list[0].value(), "\"unclosed, Origin");

        const auto parsed_vary = ruvia::parse_http_vary("\"unclosed, Accept-Encoding, *");
        EXPECT_TRUE(parsed_vary.is_wildcard());

        const auto parsed_conn =
            ruvia::parse_http_connection_nominated_fields("\"unclosed, close, X-Custom-Hop");
        EXPECT_TRUE(parsed_conn.has_close());
        EXPECT_TRUE(parsed_conn.contains("x-custom-hop"));
    }
}

// Item 2: Accept-Encoding candidate order, Cache-Control no-transform, and bounded streaming transcoder
TEST(RuviaHttpPublicApi, accept_encoding_candidate_order_and_transcoder) {
    const std::array gzip_first{ruvia::http_content_coding::gzip, ruvia::http_content_coding::brotli};
    const std::array br_first{ruvia::http_content_coding::brotli, ruvia::http_content_coding::gzip};

    // Equal qvalues resolve by the caller's candidate order.
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip, br", gzip_first),
        std::optional(ruvia::http_content_coding::gzip));
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip, br", br_first),
        std::optional(ruvia::http_content_coding::brotli));

    // Higher qvalue wins regardless of candidate order.
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip;q=0.5, br;q=0.9", gzip_first),
        std::optional(ruvia::http_content_coding::brotli));

    // Explicit identity weight beats lower compressed weight, while implicit identity stays fallback.
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip;q=0.5, identity;q=0.8", gzip_first),
        std::optional(ruvia::http_content_coding::identity));
    EXPECT_EQ(ruvia::negotiate_http_content_coding("gzip;q=0.5", gzip_first),
        std::optional(ruvia::http_content_coding::gzip));
    EXPECT_EQ(ruvia::negotiate_http_content_coding("zstd", gzip_first),
        std::optional(ruvia::http_content_coding::identity));
    EXPECT_EQ(ruvia::negotiate_http_content_coding("zstd, identity;q=0", gzip_first),
        std::nullopt);
    EXPECT_EQ(ruvia::negotiate_http_content_coding("*;q=0", gzip_first),
        std::nullopt);

    // Quote-aware Cache-Control: no-transform detection.
    EXPECT_FALSE(ruvia::http_cache_control_has_no_transform(
        "public, ext=\"a, no-transform, b\", max-age=60"));
    EXPECT_TRUE(ruvia::http_cache_control_has_no_transform(
        "public, ext=\"a, b\", no-transform, max-age=60"));

    // Multi-coding streaming transcoding (gzip + br -> zstd) with native codec memory cap.
    const std::string original(4096, 'R');
    const std::array source_stack{
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::brotli};
    const auto double_encoded = ruvia::encode_http_content(
        source_stack, original, {.max_encoded_bytes_ = 65536});
    ASSERT_TRUE(double_encoded.has_value());

    ruvia::http_content_transcoder transcoder(source_stack, ruvia::http_content_coding::zstd,
        {.max_decoded_bytes_ = 8192, .max_encoded_bytes_ = 8192, .max_memory_bytes_ = 8 * 1024 * 1024});
    std::string transcoded;
    for (std::size_t pos = 0; pos < double_encoded->size(); pos += 17) {
        transcoder.write(
            std::string_view(*double_encoded).substr(pos, 17), transcoded, false);
    }
    transcoder.finish(transcoded);

    const auto final_decoded = ruvia::decode_http_content(ruvia::http_content_coding::zstd,
        transcoded, {.max_decoded_bytes_ = 8192});
    ASSERT_TRUE(std::holds_alternative<std::string>(final_decoded));
    EXPECT_EQ(std::string_view(std::get<std::string>(final_decoded)), original);

    // Exhausted native codec memory cap throws std::bad_alloc.
    EXPECT_THROW(
        (void)ruvia::http_content_encoder(ruvia::http_content_coding::gzip, {.max_memory_bytes_ = 64}),
        std::bad_alloc);
}

// Item 3: Strict Range mode, Content-Range parser, and http_byte_range_slicer
TEST(RuviaHttpPublicApi, strict_byte_ranges_content_range_and_streaming_slicer) {
    // Strict ascending non-overlapping mode rejects out-of-order and overlapping ranges.
    const auto strict_ok = ruvia::resolve_http_byte_range_set(
        "bytes=0-4, 5-9, 20-29", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_FALSE(strict_ok.ignored());
    ASSERT_EQ(strict_ok.size(), 3U);
    EXPECT_EQ(strict_ok[0].offset(), 0U);
    EXPECT_EQ(strict_ok[0].length(), 5U);
    EXPECT_EQ(strict_ok[1].offset(), 5U);
    EXPECT_EQ(strict_ok[1].length(), 5U);

    const auto strict_out_of_order = ruvia::resolve_http_byte_range_set(
        "bytes=20-29, 0-4", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_TRUE(strict_out_of_order.ignored());

    const auto strict_overlapping = ruvia::resolve_http_byte_range_set(
        "bytes=0-10, 10-20", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_TRUE(strict_overlapping.ignored());

    // Configurable max_ranges > 16 works.
    std::string twenty_ranges = "bytes=";
    for (std::size_t i = 0; i < 20; ++i) {
        if (i != 0) {
            twenty_ranges.push_back(',');
        }
        twenty_ranges += std::to_string(i * 2) + "-" + std::to_string(i * 2);
    }
    const auto many = ruvia::resolve_http_byte_range_set(
        twenty_ranges, 100, {.max_ranges_ = 24, .strict_order_ = true});
    EXPECT_FALSE(many.ignored());
    EXPECT_EQ(many.size(), 20U);

    // Content-Range parser.
    const auto parsed_cr = ruvia::parse_http_content_range("bytes 10-19/100");
    ASSERT_TRUE(parsed_cr.has_value());
    EXPECT_EQ(parsed_cr->first(), 10U);
    EXPECT_EQ(parsed_cr->last(), 19U);
    EXPECT_EQ(parsed_cr->total(), 100U);
    EXPECT_EQ(parsed_cr->length(), 10U);
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes 20-10/100").has_value());
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes 0-100/100").has_value());
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes */100").has_value());

    // Streaming slicer for multipart/byteranges (including offset-based feeding).
    constexpr std::string_view payload = "0123456789abcdefghijklmnopqrstuvwxyz";
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=2-5, 10-14, 30-35", payload.size());
    const auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, payload.size(), "text/plain", "slice_boundary");
    std::string expected;
    for (const auto& seg : plan.segments()) {
        if (seg.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::metadata) {
            expected.append(plan.metadata().substr(seg.metadata_offset_, seg.metadata_length_));
        } else {
            expected.append(payload.substr(
                static_cast<std::size_t>(seg.file_offset_),
                static_cast<std::size_t>(seg.file_length_)));
        }
    }

    ruvia::http_byte_range_slicer slicer(
        ranges, payload.size(), "text/plain", "slice_boundary");
    EXPECT_TRUE(slicer.multipart());
    EXPECT_EQ(slicer.content_length(), expected.size());

    std::string sliced;
    // Feed starting from offset 2 in two partial segments, skipping the gap 15..29!
    slicer.write(2, payload.substr(2, 13), sliced);
    EXPECT_FALSE(slicer.complete());
    slicer.write(30, payload.substr(30, 6), sliced);
    EXPECT_TRUE(slicer.complete());
    slicer.finish(sliced);
    EXPECT_EQ(sliced, expected);
}

// Item 4: Trusted Proxy & Client IP Extractor
TEST(RuviaHttpPublicApi, trusted_proxy_client_ip_extractor) {
    ruvia::http_client_ip_config config;
    config.enabled_ = true;
    config.trusted_proxies_.emplace_back("127.0.0.0/8");
    config.trusted_proxies_.emplace_back("10.0.0.0/8");
    config.trusted_proxies_.emplace_back("2001:db8:1::/48");
    config.forwarding_headers_.emplace_back("X-Forwarded-For");
    config.forwarding_headers_.emplace_back("Forwarded");
    config.forwarding_headers_.emplace_back(
        "X-Real-IP", ruvia::http_forwarded_header_kind::single_ip);

    const ruvia::http_client_ip_extractor extractor(config);

    const std::array<ruvia::http_header_view, 2> headers{{
        {"X-Forwarded-For", "198.51.100.7, ::ffff:203.0.113.9, 10.1.2.3"},
        {"X-Real-IP", "192.0.2.55"},
    }};

    // Untrusted peer ignores forwarding headers.
    const auto direct = extractor.resolve("192.0.2.1", headers);
    ASSERT_TRUE(direct.has_value());
    EXPECT_FALSE(direct->from_forwarded_header());
    EXPECT_EQ(direct->address().to_string(), "192.0.2.1");

    // Trusted IPv4-mapped IPv6 peer strips 10.1.2.3 and normalizes ::ffff:203.0.113.9 to IPv4.
    const auto proxied = extractor.resolve("::ffff:127.0.0.1", headers);
    ASSERT_TRUE(proxied.has_value());
    EXPECT_TRUE(proxied->from_forwarded_header());
    EXPECT_EQ(proxied->address().family(), ruvia::ip_address_family::ipv4);
    EXPECT_EQ(proxied->address().to_string(), "203.0.113.9");

    // RFC 7239 Forwarded fallback when X-Forwarded-For is absent.
    const std::array<ruvia::http_header_view, 1> fwd_headers{{
        {"Forwarded", "for=\"[2001:db8:2::42]:8443\";proto=https, for=\"[2001:db8:1::1]\""},
    }};
    const auto from_rfc7239 = extractor.resolve("127.0.0.1", fwd_headers);
    ASSERT_TRUE(from_rfc7239.has_value());
    EXPECT_TRUE(from_rfc7239->from_forwarded_header());
    EXPECT_EQ(from_rfc7239->address().to_string(), "2001:db8:2::42");
}

// Item 5: Hop-by-hop fields & Connection nomination set
TEST(RuviaHttpPublicApi, hop_by_hop_fields_and_connection_nomination) {
    const std::array<ruvia::http_header_view, 2> headers{{
        {"Connection", "keep-alive, X-Custom-Hop, Upgrade"},
        {"Connection", "close, X-Second-Hop"},
    }};
    const auto nominated = ruvia::parse_http_connection_nominated_fields(headers);
    EXPECT_TRUE(nominated.has_keep_alive());
    EXPECT_TRUE(nominated.has_close());
    EXPECT_TRUE(nominated.has_upgrade());
    EXPECT_TRUE(nominated.contains("x-custom-hop"));
    EXPECT_TRUE(nominated.contains("X-SECOND-HOP"));

    EXPECT_TRUE(ruvia::is_http_hop_by_hop_field_name("Proxy-Authorization"));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_field_name("Transfer-Encoding"));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_or_nominated_field("X-Custom-Hop", nominated));
    EXPECT_TRUE(ruvia::is_http_hop_by_hop_or_nominated_field("Upgrade", nominated, false));
    EXPECT_FALSE(ruvia::is_http_hop_by_hop_or_nominated_field("Upgrade", nominated, true));

    EXPECT_EQ(nominated.rebuild_connection_field(true, true),
        "Upgrade, x-custom-hop, x-second-hop");
    EXPECT_EQ(nominated.rebuild_connection_field(true, false), "Upgrade");
}

// Item 6: RFC 9111 Shared Cache Primitives
TEST(RuviaHttpPublicApi, rfc9111_shared_cache_primitives) {
    const auto cc = ruvia::parse_cache_control(
        "public, max-age=120, s-maxage=300, stale-while-revalidate=30, stale-if-error=600, must-revalidate");
    EXPECT_TRUE(cc.is_public());
    EXPECT_TRUE(cc.must_revalidate());
    EXPECT_EQ(cc.max_age(), std::optional<std::uint64_t>(120));
    EXPECT_EQ(cc.s_maxage(), std::optional<std::uint64_t>(300));
    EXPECT_EQ(cc.stale_while_revalidate(), std::optional<std::uint64_t>(30));
    EXPECT_EQ(cc.stale_if_error(), std::optional<std::uint64_t>(600));

    std::vector<ruvia::http_header> resp_headers;
    resp_headers.emplace_back("Cache-Control", "public, s-maxage=300, max-age=120, must-revalidate");
    resp_headers.emplace_back("Expires", "Wed, 21 Oct 2015 07:28:00 GMT");
    ruvia::rewrite_http_response_cache_max_age(resp_headers, 45);
    ASSERT_EQ(resp_headers.size(), 1U);
    EXPECT_EQ(resp_headers[0].name(), "Cache-Control");
    EXPECT_EQ(resp_headers[0].value(), "public, must-revalidate, max-age=45");

    // Vary normalization & matching.
    const std::array<ruvia::http_header_view, 2> vary_headers{{
        {"Vary", "Origin, Accept-Encoding"},
        {"Vary", "accept-language, ORIGIN"},
    }};
    const auto vary = ruvia::parse_http_vary(vary_headers);
    EXPECT_FALSE(vary.is_wildcard());
    ASSERT_EQ(vary.field_names().size(), 3U);
    EXPECT_EQ(vary.field_names()[0], "accept-encoding");
    EXPECT_EQ(vary.field_names()[1], "accept-language");
    EXPECT_EQ(vary.field_names()[2], "origin");

    const std::array<ruvia::http_header_view, 3> req1{{
        {"Origin", "https://example.com"},
        {"Accept-Encoding", "gzip,  br"},
        {"Accept-Language", "en-US"},
    }};
    const std::array<ruvia::http_header_view, 3> req2{{
        {"Accept-Language", "en-US"},
        {"Accept-Encoding", "gzip,\tbr"},
        {"Origin", "https://example.com"},
    }};
    EXPECT_TRUE(ruvia::http_vary_requests_match(vary, req1, req2));

    // RFC 9111 §4.2.3 Age calculation.
    const auto age = ruvia::compute_http_cache_age({
        .date_value_ = 1000,
        .age_value_ = 10,
        .request_time_ = 1005,
        .response_time_ = 1008,
        .now_ = 1020,
    });
    // apparent_age = 8, response_delay = 3, corrected_age_value = 13,
    // corrected_initial_age = 13, resident_time = 12 => 25.
    EXPECT_EQ(age, 25U);

    // 304 Not Modified sanitization & merging.
    EXPECT_TRUE(ruvia::is_http_304_forbidden_header("Content-Length"));
    EXPECT_TRUE(ruvia::is_http_304_forbidden_header("Content-Type"));
    EXPECT_FALSE(ruvia::is_http_304_forbidden_header("ETag"));
    EXPECT_FALSE(ruvia::is_http_304_forbidden_header("Cache-Control"));

    std::vector<ruvia::http_header> cached;
    cached.emplace_back("Content-Type", "text/plain");
    cached.emplace_back("Content-Length", "1024");
    cached.emplace_back("ETag", "\"v1\"");
    cached.emplace_back("Cache-Control", "max-age=60");

    const std::array<ruvia::http_header_view, 4> not_modified{{
        {"ETag", "\"v2\""},
        {"Cache-Control", "max-age=300"},
        {"Content-Length", "0"},
        {"Connection", "keep-alive"},
    }};
    ruvia::merge_http_304_response_headers(cached, not_modified);
    EXPECT_EQ(ruvia::parse_cache_control(cached).max_age(), std::optional<std::uint64_t>(300));
    bool has_text_plain = false;
    bool has_len_1024 = false;
    bool has_etag_v2 = false;
    for (const auto& field : cached) {
        if (field.name() == "Content-Type" && field.value() == "text/plain") {
            has_text_plain = true;
        }
        if (field.name() == "Content-Length" && field.value() == "1024") {
            has_len_1024 = true;
        }
        if (field.name() == "ETag" && field.value() == "\"v2\"") {
            has_etag_v2 = true;
        }
    }
    EXPECT_TRUE(has_text_plain);
    EXPECT_TRUE(has_len_1024);
    EXPECT_TRUE(has_etag_v2);
}

// Item 7: Proxy request target normalization, Max-Forwards, method semantics, and HTTP/1 wire estimation
TEST(RuviaHttpPublicApi, proxy_target_max_forwards_method_semantics_and_wire_size) {
    EXPECT_EQ(
        ruvia::normalize_http_proxy_request_target("GET", "http://example.com:8080/a/b?x=1"),
        "/a/b?x=1");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("GET", "https://example.com"), "/");
    EXPECT_EQ(
        ruvia::normalize_http_proxy_request_target("GET", "https://example.com?foo=bar"),
        "/?foo=bar");
    EXPECT_EQ(
        ruvia::normalize_http_proxy_request_target("OPTIONS", "http://example.com"), "*");
    EXPECT_EQ(
        ruvia::normalize_http_proxy_request_target("OPTIONS", "http://example.com?query"),
        "/?query");
    EXPECT_EQ(ruvia::normalize_http_proxy_request_target("GET", "/already/origin"),
        "/already/origin");

    const std::array<ruvia::http_header_view, 1> mf_five{{{"Max-Forwards", "5"}}};
    const auto parsed_mf = ruvia::parse_http_max_forwards(mf_five);
    EXPECT_TRUE(parsed_mf.is_valid());
    EXPECT_EQ(parsed_mf.current(), std::optional<std::uint64_t>(5));
    EXPECT_EQ(parsed_mf.decremented(), std::optional<std::uint64_t>(4));
    EXPECT_EQ(parsed_mf.format_decremented(), "4");

    const auto mf_zero = ruvia::parse_http_max_forwards("0");
    EXPECT_TRUE(mf_zero.exhausted());
    EXPECT_EQ(mf_zero.decremented(), std::nullopt);

    const std::array<ruvia::http_header_view, 2> mf_dup{{
        {"Max-Forwards", "5"},
        {"Max-Forwards", "4"},
    }};
    EXPECT_TRUE(ruvia::parse_http_max_forwards(mf_dup).is_invalid());
    EXPECT_TRUE(ruvia::parse_http_max_forwards("5, 4").is_invalid());

    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("GET"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("HEAD"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("DELETE"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("OPTIONS"));
    EXPECT_FALSE(ruvia::http_method_sends_zero_content_length("TRACE"));
    EXPECT_TRUE(ruvia::http_method_sends_zero_content_length("POST"));
    EXPECT_TRUE(ruvia::http_method_sends_zero_content_length("PUT"));

    const std::array<ruvia::http_header_view, 2> wire_headers{{
        {"Content-Type", "text/plain"},
        {"Content-Length", "5"},
    }};
    // "HTTP/1.1 200 OK\r\n" (17) + "Content-Type: text/plain\r\n" (26) +
    // "Content-Length: 5\r\n" (19) + "\r\n" (2) = 64.
    EXPECT_EQ(ruvia::estimate_http1_response_head_bytes(ruvia::http_status::ok, wire_headers),
        64U);
}

// Item 8: TLS ClientHello (JA3/JA4/PeetPrint) & HTTP/2 Akamai Fingerprint
TEST(RuviaHttpPublicApi, tls_client_hello_and_http2_akamai_fingerprints) {
    // Construct a synthetic TLS 1.3 ClientHello handshake message containing
    // GREASE values, SNI ("example.com"), ALPN ("h2", "http/1.1"),
    // supported_groups (GREASE + x25519 29 + secp256r1 23), ec_point_formats (0),
    // signature_algorithms (0x0403, 0x0804), and supported_versions (GREASE, 0x0304, 0x0303).
    std::vector<std::uint8_t> body;
    const auto push_u8 = [&](std::uint8_t v) { body.push_back(v); };
    const auto push_u16 = [&](std::uint16_t v) {
        body.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xffU));
        body.push_back(static_cast<std::uint8_t>(v & 0xffU));
    };

    push_u16(0x0303);                   // legacy_version = TLS 1.2 (771)
    body.insert(body.end(), 32, 0x42);  // random
    push_u8(0);                         // empty session_id

    // Cipher suites: GREASE (0x0a0a), TLS_AES_128_GCM_SHA256 (0x1301 = 4865),
    // TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xc02f = 49199)
    push_u16(6);
    push_u16(0x0a0a);
    push_u16(0x1301);
    push_u16(0xc02f);

    // Compression methods: [0]
    push_u8(1);
    push_u8(0);

    std::vector<std::uint8_t> exts;
    const auto ext_u8 = [&](std::uint8_t v) { exts.push_back(v); };
    const auto ext_u16 = [&](std::uint16_t v) {
        exts.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xffU));
        exts.push_back(static_cast<std::uint8_t>(v & 0xffU));
    };

    // GREASE extension 0x1a1a
    ext_u16(0x1a1a);
    ext_u16(0);

    // server_name (0x0000): "example.com" (11 bytes)
    ext_u16(0x0000);
    ext_u16(16);
    ext_u16(14);
    ext_u8(0);
    ext_u16(11);
    for (const char ch : std::string_view("example.com")) {
        ext_u8(static_cast<std::uint8_t>(ch));
    }

    // supported_groups (0x000a): GREASE 0x2a2a, 29 (x25519), 23 (secp256r1)
    ext_u16(0x000a);
    ext_u16(8);
    ext_u16(6);
    ext_u16(0x2a2a);
    ext_u16(29);
    ext_u16(23);

    // ec_point_formats (0x000b): [0]
    ext_u16(0x000b);
    ext_u16(2);
    ext_u8(1);
    ext_u8(0);

    // signature_algorithms (0x000d): 0x0403 (1027), 0x0804 (2052)
    ext_u16(0x000d);
    ext_u16(6);
    ext_u16(4);
    ext_u16(0x0403);
    ext_u16(0x0804);

    // ALPN (0x0010): "h2", "http/1.1"
    ext_u16(0x0010);
    ext_u16(14);
    ext_u16(12);
    ext_u8(2);
    ext_u8('h');
    ext_u8('2');
    ext_u8(8);
    for (const char ch : std::string_view("http/1.1")) {
        ext_u8(static_cast<std::uint8_t>(ch));
    }

    // supported_versions (0x002b): GREASE 0x3a3a, 0x0304 (772), 0x0303 (771)
    ext_u16(0x002b);
    ext_u16(7);
    ext_u8(6);
    ext_u16(0x3a3a);
    ext_u16(0x0304);
    ext_u16(0x0303);

    push_u16(static_cast<std::uint16_t>(exts.size()));
    body.insert(body.end(), exts.begin(), exts.end());

    std::vector<std::byte> message;
    message.push_back(std::byte{0x01});
    message.push_back(static_cast<std::byte>((body.size() >> 16U) & 0xffU));
    message.push_back(static_cast<std::byte>((body.size() >> 8U) & 0xffU));
    message.push_back(static_cast<std::byte>(body.size() & 0xffU));
    for (const auto b : body) {
        message.push_back(static_cast<std::byte>(b));
    }

    const auto parsed = ruvia::parse_tls_client_hello(message);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->legacy_version(), 0x0303U);
    EXPECT_EQ(parsed->version(), 0x0304U);
    EXPECT_EQ(parsed->server_name(), "example.com");
    EXPECT_EQ(parsed->ja3_raw(), "771,4865-49199,0-10-11-13-16-43,29-23,0");
    EXPECT_EQ(parsed->ja3().size(), 32U);

    const auto tcp_ja4 = parsed->ja4(false);
    const auto quic_ja4 = parsed->ja4(true);
    EXPECT_TRUE(tcp_ja4.starts_with("t13d0206h2_"));
    EXPECT_TRUE(quic_ja4.starts_with("q13d0206h2_"));
    EXPECT_EQ(tcp_ja4.substr(1), quic_ja4.substr(1));
    EXPECT_EQ(parsed->peetprint(),
        "772-771|4865-49199|0-10-11-13-16-43|29-23|0|h2-http/1.1|1027-2052");

    // HTTP/2 Akamai fingerprint collector.
    ruvia::http2_akamai_fingerprint_collector collector;
    ruvia::http2_frame_metadata settings_frame{};
    settings_frame.type_ = ruvia::http2_frame_type::settings;
    settings_frame.stream_id_ = 0;
    settings_frame.settings_[0] = {ruvia::http2_setting_id::header_table_size, 65536};
    settings_frame.settings_[1] = {ruvia::http2_setting_id::enable_push, 0};
    settings_frame.settings_[2] = {ruvia::http2_setting_id::initial_window_size, 6291456};
    settings_frame.settings_[3] = {ruvia::http2_setting_id::max_header_list_size, 262144};
    settings_frame.settings_count_ = 4;
    collector.observe_frame(settings_frame);

    ruvia::http2_frame_metadata wu_frame{};
    wu_frame.type_ = ruvia::http2_frame_type::window_update;
    wu_frame.stream_id_ = 0;
    wu_frame.window_increment_ = 15663105;
    collector.observe_frame(wu_frame);

    ruvia::http2_frame_metadata pri_frame{};
    pri_frame.type_ = ruvia::http2_frame_type::priority;
    pri_frame.stream_id_ = 3;
    pri_frame.priority_ = ruvia::http2_received_priority{
        .stream_id_ = 3, .stream_dependency_ = 0, .weight_ = 201, .exclusive_ = false};
    collector.observe_frame(pri_frame);

    collector.observe_header(
        {.stream_id_ = 1, .phase_ = ruvia::http2_header_observation_phase::begin});
    for (const std::string_view pseudo : {":method", ":authority", ":scheme", ":path"}) {
        collector.observe_header({
            .stream_id_ = 1,
            .phase_ = ruvia::http2_header_observation_phase::field,
            .name_ = pseudo,
            .value_ = "x",
        });
    }
    collector.observe_header({
        .stream_id_ = 1,
        .phase_ = ruvia::http2_header_observation_phase::field,
        .name_ = "user-agent",
        .value_ = "Ruvia",
    });
    collector.observe_header(
        {.stream_id_ = 1, .phase_ = ruvia::http2_header_observation_phase::end});

    EXPECT_TRUE(collector.complete());
    EXPECT_FALSE(collector.failed());
    EXPECT_EQ(collector.fingerprint(),
        "1:65536;2:0;4:6291456;6:262144|15663105|3:0:0:201|m,a,s,p");
}

TEST(RuviaHttpPublicApi, media_type_compressibility_worker_codecs_cookie_jar_etag_and_http3_fingerprint) {
    // 1. Media type compressibility allowlist
    EXPECT_TRUE(ruvia::is_http_media_type_compressible("text/html; charset=utf-8"));
    EXPECT_TRUE(ruvia::is_http_media_type_compressible("application/problem+json"));
    EXPECT_TRUE(ruvia::is_http_media_type_compressible("application/wasm"));
    EXPECT_FALSE(ruvia::is_http_media_type_compressible("application/zip"));
    EXPECT_FALSE(ruvia::is_http_media_type_compressible("image/png"));

    // 2. Reusable http_worker_codecs and http_stream_encoder_lease
    ruvia::http_worker_codecs codecs;
    {
        auto lease = codecs.acquire_stream_encoder(ruvia::http_content_coding::gzip);
        std::string out;
        lease.encoder().write("streamed payload", out, true);
        lease.encoder().finish(out);
        const auto decoded = ruvia::decode_http_content(
            ruvia::http_content_coding::gzip, out, {.max_decoded_bytes_ = 4096});
        ASSERT_TRUE(std::holds_alternative<std::string>(decoded));
        EXPECT_EQ(std::get<std::string>(decoded), "streamed payload");
    }
    {
        // Second lease reuses the reset encoder from the pool.
        auto lease = codecs.acquire_stream_encoder(ruvia::http_content_coding::gzip);
        std::string out;
        lease.encoder().write("reused encoder payload", out);
        lease.encoder().finish(out);
        const auto decoded = ruvia::decode_http_content(
            ruvia::http_content_coding::gzip, out, {.max_decoded_bytes_ = 4096});
        ASSERT_TRUE(std::holds_alternative<std::string>(decoded));
        EXPECT_EQ(std::get<std::string>(decoded), "reused encoder payload");
    }

    // 3. Sans-I/O http_cookie_jar
    const auto now = std::chrono::system_clock::now();
    ruvia::http_cookie_jar jar(
        ruvia::http_scheme::https, "sub.example.com",
        {.max_cookies_ = 8, .max_cookie_bytes_ = 1024});
    jar.add_configured_cookie("static", "1");
    EXPECT_TRUE(jar.absorb_set_cookie("/app/login", "sid=abc; Path=/app; Secure; HttpOnly", now));
    EXPECT_TRUE(jar.absorb_set_cookie("/app/login", "__Host-token=xyz; Path=/; Secure", now));
    // __Host- with Domain must be rejected.
    EXPECT_FALSE(
        jar.absorb_set_cookie("/app/login", "__Host-bad=1; Domain=example.com; Path=/; Secure", now));
    EXPECT_EQ(jar.cookie_header("/app/dashboard", true, now),
        "sid=abc; static=1; __Host-token=xyz");
    EXPECT_EQ(jar.cookie_header("/other", true, now), "static=1; __Host-token=xyz");
    // Max-Age=0 deletes the stored cookie.
    EXPECT_TRUE(jar.absorb_set_cookie("/app/logout", "sid=deleted; Path=/app; Max-Age=0", now));
    EXPECT_EQ(jar.cookie_header("/app/dashboard", true, now), "static=1; __Host-token=xyz");

    // 4. Single-value ETag validation, strong/weak comparison, and weakening
    EXPECT_TRUE(ruvia::is_valid_http_entity_tag("\"v1\""));
    EXPECT_TRUE(ruvia::is_strong_http_entity_tag("\"v1\""));
    EXPECT_FALSE(ruvia::is_weak_http_entity_tag("\"v1\""));
    EXPECT_TRUE(ruvia::is_weak_http_entity_tag("W/\"v1\""));
    EXPECT_FALSE(ruvia::is_valid_http_entity_tag("v1"));
    EXPECT_TRUE(ruvia::http_strong_etag_equals("\"v1\"", "\"v1\""));
    EXPECT_FALSE(ruvia::http_strong_etag_equals("W/\"v1\"", "\"v1\""));
    EXPECT_TRUE(ruvia::http_weak_etag_equals("W/\"v1\"", "\"v1\""));
    EXPECT_EQ(ruvia::weaken_http_etag("\"v1\""), "W/\"v1\"");
    EXPECT_EQ(ruvia::weaken_http_etag("W/\"v1\""), "W/\"v1\"");

    // 5. HTTP/3 fingerprint collector
    ruvia::http3_fingerprint_collector h3_collector;
    h3_collector.observe_frame({
        .observation_sequence_ = 0,
        .stream_id_ = 2,
        .type_ = ruvia::http3_frame_type::settings,
        .payload_length_ = 12,
    });
    h3_collector.observe_field({
        .stream_id_ = 0,
        .section_ = ruvia::http3_field_section_kind::header,
        .phase_ = ruvia::http3_field_observation_phase::begin,
    });
    for (const std::string_view pseudo : {":method", ":authority", ":scheme", ":path"}) {
        h3_collector.observe_field({
            .stream_id_ = 0,
            .section_ = ruvia::http3_field_section_kind::header,
            .phase_ = ruvia::http3_field_observation_phase::field,
            .name_ = pseudo,
            .value_ = "x",
        });
    }
    h3_collector.observe_field({
        .stream_id_ = 0,
        .section_ = ruvia::http3_field_section_kind::header,
        .phase_ = ruvia::http3_field_observation_phase::end,
    });
    EXPECT_TRUE(h3_collector.complete());
    EXPECT_FALSE(h3_collector.failed());
    EXPECT_EQ(h3_collector.fingerprint(), "4:12|0|m,a,s,p");
}

TEST(RuviaHttpPublicApi, targeted_cache_control_field_qualified_private_and_staged_transcoder) {
    // 1. RFC 9213 Targeted Cache-Control (Structured Field Dictionary & priority selection)
    const auto cdn_cc = ruvia::parse_targeted_cache_control(
        "max-age=60, max-age=300, stale-while-revalidate=45; param=1, no-store=?0, must-revalidate=?1");
    ASSERT_TRUE(cdn_cc.has_value());
    EXPECT_EQ(cdn_cc->max_age(), std::optional<std::uint64_t>(300));
    EXPECT_EQ(cdn_cc->stale_while_revalidate(), std::optional<std::uint64_t>(45));
    EXPECT_FALSE(cdn_cc->no_store());
    EXPECT_TRUE(cdn_cc->must_revalidate());

    // Invalid SF-Dictionary syntax (uppercase key or unterminated string) returns std::nullopt per RFC 9213 §2.1.
    EXPECT_FALSE(ruvia::parse_targeted_cache_control("MAX-AGE=60").has_value());
    EXPECT_FALSE(ruvia::parse_targeted_cache_control("").has_value());

    const std::array<ruvia::http_header_view, 3> response_headers{{
        {"Cache-Control", "private, no-store"},
        {"CDN-Cache-Control", "max-age=600, public"},
        {"Cloudflare-CDN-Cache-Control", "invalid=\"unterminated"},
    }};
    const std::array<std::string_view, 2> target_list{{
        "Cloudflare-CDN-Cache-Control",
        "CDN-Cache-Control",
    }};
    // Invalid first targeted header is ignored; second valid targeted header wins over Cache-Control.
    const auto selected = ruvia::resolve_targeted_cache_control(response_headers, target_list);
    EXPECT_TRUE(selected.targeted_);
    EXPECT_EQ(selected.selected_header_, "CDN-Cache-Control");
    EXPECT_TRUE(selected.control_.is_public());
    EXPECT_FALSE(selected.control_.no_store());
    EXPECT_EQ(selected.control_.max_age(), std::optional<std::uint64_t>(600));

    // 2. RFC 9111 §5.2.2.7 private="field" & §5.2.2.3 no-cache="field" field-level stripping
    const auto field_priv = ruvia::parse_cache_control(
        "public, max-age=120, private=\"Set-Cookie, X-User-Secret\", no-cache=\"X-Auth-State\"");
    EXPECT_TRUE(field_priv.is_private());
    EXPECT_FALSE(field_priv.is_unqualified_private());
    EXPECT_TRUE(field_priv.is_field_qualified_private());
    EXPECT_EQ(field_priv.private_field_count(), 2U);
    EXPECT_TRUE(field_priv.has_private_field("set-cookie"));
    EXPECT_TRUE(field_priv.has_private_field("X-USER-SECRET"));
    EXPECT_FALSE(field_priv.has_private_field("Content-Type"));

    EXPECT_TRUE(field_priv.no_cache());
    EXPECT_FALSE(field_priv.is_unqualified_no_cache());
    EXPECT_TRUE(field_priv.is_field_qualified_no_cache());
    EXPECT_EQ(field_priv.no_cache_field_count(), 1U);
    EXPECT_TRUE(field_priv.has_no_cache_field("x-auth-state"));

    std::vector<ruvia::http_header> strip_headers;
    strip_headers.emplace_back("Content-Type", "application/json");
    strip_headers.emplace_back("Set-Cookie", "sid=123");
    strip_headers.emplace_back("X-User-Secret", "top-secret");
    strip_headers.emplace_back("X-Public", "kept");
    EXPECT_TRUE(ruvia::strip_http_cache_private_headers(strip_headers, field_priv));
    ASSERT_EQ(strip_headers.size(), 2U);
    EXPECT_EQ(strip_headers[0].name(), "Content-Type");
    EXPECT_EQ(strip_headers[1].name(), "X-Public");

    // Unqualified private alongside field-qualified private forbids shared-cache storage.
    const auto unqual_priv =
        ruvia::parse_cache_control("private=\"Set-Cookie\", private");
    EXPECT_TRUE(unqual_priv.is_unqualified_private());
    EXPECT_FALSE(unqual_priv.is_field_qualified_private());
    EXPECT_FALSE(ruvia::strip_http_cache_private_headers(strip_headers, unqual_priv));

    // 3. Staged content transcoder for unknown-length (chunked) responses with [min_bytes, max_bytes]
    std::string payload;
    for (int i = 0; i < 32; ++i) {
        payload.append("chunked-response-staging-compressible-line-0123456789\n");
    }
    const auto gzipped = ruvia::encode_http_content(
        ruvia::http_content_coding::gzip, payload, {.max_encoded_bytes_ = payload.size() + 128});
    ASSERT_TRUE(gzipped.has_value());

    // Case A: Decoded size within [min_bytes, max_bytes] -> transcodes gzip to zstd.
    ruvia::http_staged_content_transcoder staged(
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::zstd,
        {.min_bytes_ = 100, .max_bytes_ = 4096});
    for (std::size_t pos = 0; pos < gzipped->size(); pos += 19) {
        EXPECT_TRUE(staged.write(std::string_view(*gzipped).substr(pos, 19)));
    }
    auto res_in_range = staged.finish();
    EXPECT_TRUE(res_in_range.transformed());
    EXPECT_EQ(res_in_range.coding(), ruvia::http_content_coding::zstd);
    EXPECT_EQ(res_in_range.decoded_bytes(), payload.size());
    const auto decoded_zstd = ruvia::decode_http_content(
        ruvia::http_content_coding::zstd, res_in_range.bytes(), {.max_decoded_bytes_ = 8192});
    ASSERT_TRUE(std::holds_alternative<std::string>(decoded_zstd));
    EXPECT_EQ(std::get<std::string>(decoded_zstd), payload);

    // Case B: Decoded size < min_bytes -> falls back to original untransformed stream!
    ruvia::http_staged_content_transcoder too_small(
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::zstd,
        {.min_bytes_ = payload.size() + 100, .max_bytes_ = 8192});
    EXPECT_TRUE(too_small.write(*gzipped));
    auto res_small = too_small.finish();
    EXPECT_FALSE(res_small.transformed());
    EXPECT_EQ(res_small.coding(), ruvia::http_content_coding::gzip);
    EXPECT_EQ(res_small.bytes(), *gzipped);

    // Case C: Decoded size > max_bytes -> spills into passthrough_output and falls back to original stream!
    ruvia::http_staged_content_transcoder too_large(
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::zstd,
        {.min_bytes_ = 16, .max_bytes_ = 256});
    std::string passthrough_stream;
    for (std::size_t pos = 0; pos < gzipped->size(); pos += 13) {
        (void)too_large.write(std::string_view(*gzipped).substr(pos, 13), passthrough_stream);
    }
    EXPECT_TRUE(too_large.passthrough());
    auto res_large = too_large.finish();
    EXPECT_FALSE(res_large.transformed());
    EXPECT_EQ(passthrough_stream + std::string(res_large.bytes()), *gzipped);
}

}  // namespace
