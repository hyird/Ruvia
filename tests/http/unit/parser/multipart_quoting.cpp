#include <array>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/multipart_parser.h"

#include "test_harness.h"

// Quoted names and boundaries in a multipart body, and the delimiter terminator each form requires.

RUVIA_TEST(multipart_boundary_quoted_with_mime_special) {
    const std::string_view content_type_value = R"(multipart/form-data; boundary="a:b")";
    const auto result_value = ruvia::parse_multipart_boundary(content_type_value);
    RUVIA_CHECK(result_value.boundary() != nullptr);
    RUVIA_CHECK_EQ(result_value.boundary()->value(), std::string_view("a:b"));
}

RUVIA_TEST(multipart_part_fields_unfold_mime_continuations) {
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

    auto parsed = ruvia::parse_multipart_body(wire, {.boundary_ = ruvia::multipart_boundary("abc")});
    RUVIA_CHECK(parsed.body() != nullptr);
    if (const auto* body = parsed.body()) {
        RUVIA_CHECK_EQ(body->parts().size(), names.size());
        for (std::size_t index = 0; index < body->parts().size() && index < names.size(); ++index) {
            const auto& part = body->parts()[index];
            RUVIA_CHECK_EQ(part.name(), names[index]);
            RUVIA_CHECK_EQ(part.content_type(), types[index]);
            RUVIA_CHECK_EQ(part.body(), bodies[index]);
        }
        if (!body->parts().empty()) {
            RUVIA_CHECK_EQ(body->parts()[0].filename(), std::string_view("first \tfile.txt"));
        }
    }

    ruvia::multipart_parser parser({.boundary_ = ruvia::multipart_boundary("abc")});
    std::array<std::string, 3> streamed;
    std::size_t part_index = 0;
    bool done = false;
    for (std::size_t offset = 0; offset < wire.size() && !done; ++offset) {
        parser.feed(std::string_view(wire).substr(offset, 1));
        for (;;) {
            const auto result = parser.poll();
            RUVIA_CHECK(result.failure() == nullptr);
            if (const auto* part = result.part()) {
                RUVIA_CHECK(part_index < names.size());
                if (part_index < names.size()) {
                    RUVIA_CHECK_EQ(part->name(), names[part_index]);
                    RUVIA_CHECK_EQ(part->content_type(), types[part_index]);
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
    RUVIA_CHECK(done);
    RUVIA_CHECK_EQ(part_index, names.size());
    for (std::size_t index = 0; index < streamed.size(); ++index) {
        RUVIA_CHECK_EQ(std::string_view(streamed[index]), bodies[index]);
    }

    // Folded Content-Type values outlive the parser and survive moves even when
    // their normalized spelling fits the string's small inline buffer.
    auto moved = std::move(parsed);
    wire.assign(wire.size(), '#');
    if (const auto* body = moved.body(); body != nullptr && body->parts().size() == names.size()) {
        RUVIA_CHECK_EQ(body->parts()[0].content_type(), types[0]);
        RUVIA_CHECK_EQ(body->parts()[1].content_type(), types[1]);
    }
}

RUVIA_TEST(multipart_unfolding_preserves_wire_limits_and_field_validation) {
    const std::string headers =
        "Content-Disposition: form-data;\r\n name=field\r\n"
        "Content-Type: text/plain;\r\n charset=utf-8\r\n\r\n";
    const std::string wire = "--abc\r\n" + headers + "value\r\n--abc--\r\n";
    const auto accepted = ruvia::parse_multipart_body(wire,
        {.boundary_ = ruvia::multipart_boundary("abc"), .max_metadata_bytes_ = headers.size()});
    RUVIA_CHECK(accepted.body() != nullptr);
    const auto oversized = ruvia::parse_multipart_body(wire,
        {.boundary_ = ruvia::multipart_boundary("abc"), .max_metadata_bytes_ = headers.size() - 1});
    RUVIA_CHECK(oversized.failure() != nullptr);
    if (const auto* failure = oversized.failure()) {
        RUVIA_CHECK(failure->protocol_error().status() == ruvia::http_status::content_too_large);
    }
    ruvia::multipart_parser limited({.boundary_ = ruvia::multipart_boundary("abc"),
        .max_metadata_bytes_ = headers.size() - 1});
    limited.feed(wire);
    limited.finish_input();
    const auto limited_result = limited.poll();
    RUVIA_CHECK(limited_result.failure() != nullptr);
    if (const auto* failure = limited_result.failure()) {
        RUVIA_CHECK(failure->protocol_error().status() == ruvia::http_status::content_too_large);
    }

    for (const std::string_view invalid : {
             " Content-Disposition: form-data; name=field",
             "Content-Disposition: form-data;\n name=field",
             "Content-Disposition: form-data;\r name=field",
             "Content-Disposition: form-data; name=field\r\n Content-Type: text/plain",
             "Content-Disposition: form-data; name=field\r\nContent-Disposition:\r\n form-data; name=other"}) {
        const std::string malformed = "--abc\r\n" + std::string(invalid) + "\r\n\r\nvalue\r\n--abc--\r\n";
        const auto result = ruvia::parse_multipart_body(malformed,
            {.boundary_ = ruvia::multipart_boundary("abc")});
        RUVIA_CHECK(result.failure() != nullptr);
        ruvia::multipart_parser parser({.boundary_ = ruvia::multipart_boundary("abc")});
        parser.feed(malformed);
        parser.finish_input();
        const auto streamed = parser.poll();
        const auto repeated = parser.poll();
        RUVIA_CHECK(streamed.failure() != nullptr);
        RUVIA_CHECK(repeated.failure() != nullptr);
        if (streamed.failure() != nullptr && repeated.failure() != nullptr) {
            RUVIA_CHECK_EQ(std::string_view(streamed.failure()->protocol_error().what()),
                std::string_view(repeated.failure()->protocol_error().what()));
        }
    }
}
