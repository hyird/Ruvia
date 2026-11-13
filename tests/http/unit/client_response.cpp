#include <gtest/gtest.h>

#include <array>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_client_response_parser.h"

#include "content_decoding_fixture.h"
#include "http_client_response_fixture.h"

using namespace ruvia::test;

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"

#include "test_exceptions.h"

namespace {

// Published trailer fields as "name=value;" in received order.
std::string rendered(std::span<const ruvia::http_header> fields) {
    std::string text;
    for (const auto& field : fields) {
        text.append(field.name()).append("=").append(field.value()).append(";");
    }
    return text;
}

}  // namespace

TEST(Ruvia, http1_response_body_decoder_fixed_length_preserves_tail_and_eof) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 8> scratch{};
    auto first = decoder.decode("ab", scratch);
    EXPECT_TRUE(first.output() != nullptr);
    EXPECT_EQ(first.consumed_bytes(), std::size_t{2});
    EXPECT_EQ(first.output()->bytes(), "ab");
    auto second = decoder.finish_input("cNEXT", scratch);
    EXPECT_TRUE(second.output() != nullptr);
    EXPECT_EQ(second.consumed_bytes(), std::size_t{1});
    auto complete_value = decoder.finish_input("NEXT", scratch);
    EXPECT_TRUE(complete_value.complete() != nullptr);
    EXPECT_EQ(complete_value.consumed_bytes(), std::size_t{0});
}

TEST(Ruvia, http1_response_body_decoder_head_and_no_content_complete_without_consuming_tail) {
    for (const auto& [method, status] : {std::pair{"HEAD", "200 OK"},
             std::pair{"GET", "204 No Content"}, std::pair{"GET", "304 Not Modified"}}) {
        const auto head = parse_head(method, std::string("HTTP/1.1 ") + status);
        ruvia::http1_client_response_body_decoder decoder(head.plan());
        std::array<char, 4> scratch{};
        const auto result_value = decoder.decode("TAIL", scratch);
        EXPECT_TRUE(result_value.complete() != nullptr);
        EXPECT_EQ(result_value.consumed_bytes(), std::size_t{0});
    }
}

TEST(Ruvia, http1_response_body_decoder_head_205_keeps_metadata_and_following_response) {
    const auto head = parse_head("HEAD", "HTTP/1.1 205 Reset Content\r\nContent-Length: 7");
    EXPECT_TRUE(head.plan().without_content() != nullptr);
    const auto headers = head.head().headers();
    EXPECT_TRUE(!headers.empty());
    if (!headers.empty()) {
        EXPECT_EQ(headers.front().value(), "7");
    }
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 4> scratch{};
    constexpr std::string_view following = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxNEXT";
    const auto complete = decoder.decode(following, scratch);
    EXPECT_TRUE(complete.complete() != nullptr);
    EXPECT_EQ(complete.consumed_bytes(), std::size_t{0});

    auto remaining = following.substr(complete.consumed_bytes());
    const auto next = ruvia::test::parse_wire("GET", remaining);
    EXPECT_TRUE(next.parsed() != nullptr);
    if (next.parsed() == nullptr) {
        return;
    }
    remaining.remove_prefix(next.parsed()->consumed_bytes());
    ruvia::http1_client_response_body_decoder next_decoder(
        next.parsed()->plan());
    const auto body = next_decoder.decode(remaining, scratch);
    EXPECT_TRUE(body.output() != nullptr);
    if (const auto* output = body.output()) {
        EXPECT_EQ(output->bytes(), "x");
    }
    remaining.remove_prefix(body.consumed_bytes());
    const auto next_complete = next_decoder.decode(remaining, scratch);
    EXPECT_TRUE(next_complete.complete() != nullptr);
    EXPECT_EQ(next_complete.consumed_bytes(), std::size_t{0});
    EXPECT_EQ(remaining, "NEXT");
}

TEST(Ruvia, http1_response_body_decoder_chunked_trailers_and_tail) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 8> scratch{};
    auto body = decoder.decode("3\r\nabc\r\n0\r\nX-Test: yes\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(body.output() != nullptr);
    EXPECT_EQ(body.output()->bytes(), "abc");
    auto trailer = decoder.decode("0\r\nX-Test: yes\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(trailer.trailers() != nullptr);
    EXPECT_EQ(rendered(trailer.trailers()->fields()), "x-test=yes;");
    EXPECT_EQ(trailer.consumed_bytes(), std::size_t{18});
    auto complete_value = decoder.decode("TAIL", scratch);
    EXPECT_TRUE(complete_value.complete() != nullptr);
    EXPECT_EQ(complete_value.consumed_bytes(), std::size_t{0});
}

// RFC 9112 section 5.2: a user agent replaces each obs-fold received in a
// response with SP before interpreting the value; a folded trailer is not
// broken framing.

// RFC 9110 section 6.5.1 and RFC 9112 section 7.1.2: fields a server may not
// send as trailers are discarded rather than published, like the HTTP/2 and
// HTTP/3 clients do; a section without kept fields allocates nothing.

TEST(Ruvia, http1_response_body_decoder_close_delimited_completes_only_at_eof) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 3> scratch{};
    auto body = decoder.decode("abcdef", scratch);
    EXPECT_TRUE(body.output() != nullptr);
    EXPECT_EQ(body.consumed_bytes(), std::size_t{3});
    EXPECT_EQ(body.output()->bytes(), "abc");
    auto rest = decoder.finish_input("def", scratch);
    EXPECT_TRUE(rest.output() != nullptr);
    EXPECT_EQ(rest.consumed_bytes(), std::size_t{3});
    auto complete_value = decoder.finish_input({}, scratch);
    EXPECT_TRUE(complete_value.complete() != nullptr);
}

TEST(Ruvia, http1_response_body_decoder_transfer_gzip_and_deflate_stream_with_tiny_scratch) {
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    for (const auto& [coding, bytes] : {std::pair{"gzip", gzip_hello},
             std::pair{"deflate", deflate_hello}}) {
        const auto head = parse_head("GET", std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: ") + coding);
        ruvia::http1_client_response_body_decoder decoder(head.plan());
        std::array<char, 1> scratch{};
        std::string output;
        std::string input(bytes);
        auto result_value = decoder.finish_input(input, scratch);
        for (std::size_t step = 0; step < 100 && !result_value.complete() &&
                                   !result_value.protocol_failure() && !result_value.decoder_failure();
            ++step) {
            input.erase(0, result_value.consumed_bytes());
            if (result_value.output()) {
                output.append(result_value.output()->bytes());
            }
            result_value = decoder.finish_input(input, scratch);
        }
        if (result_value.output()) {
            output.append(result_value.output()->bytes());
        }
        EXPECT_EQ(output, "hello");
        EXPECT_TRUE(result_value.complete() != nullptr);
    }
}

TEST(Ruvia, http1_response_body_decoder_gzip_concatenation_and_chunked_transfer) {
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    std::string concatenated(gzip_hello);
    concatenated.append(gzip_hello);
    const auto close_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder close_decoder(
        close_head.plan());
    std::array<char, 2> scratch{};
    std::string decoded;
    auto result_value = close_decoder.finish_input(concatenated, scratch);
    for (std::size_t step = 0; step < 100 && !result_value.complete() &&
                               !result_value.protocol_failure() && !result_value.decoder_failure();
        ++step) {
        concatenated.erase(0, result_value.consumed_bytes());
        if (result_value.output()) {
            decoded.append(result_value.output()->bytes());
        }
        result_value = close_decoder.finish_input(concatenated, scratch);
    }
    if (result_value.output()) {
        decoded.append(result_value.output()->bytes());
    }
    EXPECT_EQ(decoded, "hellohello");
    EXPECT_TRUE(result_value.complete() != nullptr);

    const auto chunked_head =
        parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    ruvia::http1_client_response_body_decoder chunked_decoder(
        chunked_head.plan());
    char size[3]{};
    std::snprintf(size, sizeof(size), "%zx", gzip_hello.size());
    std::string wire(size);
    wire.append("\r\n");
    wire.append(gzip_hello);
    wire.append("\r\n0\r\n\r\nTAIL");
    decoded.clear();
    bool trailers_seen = false;
    for (;;) {
        result_value = chunked_decoder.decode(wire, scratch);
        wire.erase(0, result_value.consumed_bytes());
        if (result_value.output() != nullptr) {
            decoded.append(result_value.output()->bytes());
        } else if (result_value.trailers() != nullptr) {
            trailers_seen = true;
        } else if (result_value.complete() != nullptr) {
            break;
        } else {
            EXPECT_TRUE(result_value.need_input() != nullptr);
        }
    }
    EXPECT_EQ(decoded, "hello");
    EXPECT_TRUE(trailers_seen);
    EXPECT_EQ(wire, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_transfer_completion_waits_for_http_framing_and_rejects_tail) {
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 16> scratch{};
    const auto output = decoder.decode(deflate_hello, scratch);
    EXPECT_TRUE(output.output() != nullptr);
    EXPECT_EQ(output.output()->bytes(), "hello");
    const auto still_waiting = decoder.decode({}, scratch);
    EXPECT_TRUE(still_waiting.need_input() != nullptr);
    const auto illegal_tail = decoder.decode("X", scratch);
    EXPECT_TRUE(illegal_tail.protocol_failure() != nullptr);
    EXPECT_TRUE(illegal_tail.protocol_failure()->error() ==
                ruvia::http1_client_response_body_error::invalid_transfer_coding);
    EXPECT_EQ(illegal_tail.consumed_bytes(), std::size_t{0});

    const auto eof_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
    ruvia::http1_client_response_body_decoder eof_decoder(
        eof_head.plan());
    auto eof_result = eof_decoder.finish_input(deflate_hello, scratch);
    while (eof_result.output() || eof_result.need_input()) {
        eof_result = eof_decoder.finish_input({}, scratch);
    }
    EXPECT_TRUE(eof_result.complete() != nullptr);
}

TEST(Ruvia, http1_response_body_decoder_chunked_deflate_waits_for_valid_trailers) {
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate, chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::string input = "d\r\n";
    input.append(deflate_hello);
    input.append("\r\n0\r\nX-End: yes\r\n\r\nTAIL");
    std::array<char, 8> scratch{};
    std::string body;
    bool saw_trailers = false;
    auto result_value = decoder.decode(input, scratch);
    for (int step = 0; step < 8 && !result_value.complete() && !result_value.protocol_failure(); ++step) {
        input.erase(0, result_value.consumed_bytes());
        if (result_value.output()) {
            body.append(result_value.output()->bytes());
        }
        saw_trailers = saw_trailers || result_value.trailers() != nullptr;
        result_value = decoder.decode(input, scratch);
    }
    if (result_value.output()) {
        body.append(result_value.output()->bytes());
    }
    EXPECT_TRUE(result_value.complete() != nullptr);
    EXPECT_EQ(body, "hello");
    EXPECT_TRUE(saw_trailers);
    EXPECT_EQ(input, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_terminal_result_is_stable_and_replayed) {
    const auto head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 4> scratch{};
    const auto failed = decoder.decode("1\r\nxXY0\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(failed.protocol_failure() != nullptr);
    EXPECT_TRUE(failed.protocol_failure()->error() == ruvia::http1_client_response_body_error::invalid_framing);
    const auto replayed = decoder.decode("ignored", scratch);
    EXPECT_TRUE(replayed.protocol_failure() != nullptr);
    EXPECT_TRUE(replayed.protocol_failure()->error() == failed.protocol_failure()->error());
    EXPECT_EQ(replayed.consumed_bytes(), std::size_t{0});
    const auto eof_replay = decoder.finish_input({}, scratch);
    EXPECT_TRUE(eof_replay.protocol_failure() != nullptr);
    EXPECT_TRUE(eof_replay.protocol_failure()->error() == failed.protocol_failure()->error());

    const auto no_body = parse_head("GET", "HTTP/1.1 204 No Content");
    ruvia::http1_client_response_body_decoder complete_decoder(
        no_body.plan());
    const auto first_complete = complete_decoder.decode("TAIL", scratch);
    EXPECT_TRUE(first_complete.complete() != nullptr);
    const auto complete_replay = complete_decoder.decode("ignored", scratch);
    EXPECT_TRUE(complete_replay.complete() != nullptr);
    EXPECT_EQ(complete_replay.consumed_bytes(), std::size_t{0});
    const auto complete_eof_replay = complete_decoder.finish_input({}, scratch);
    EXPECT_TRUE(complete_eof_replay.complete() != nullptr);
    EXPECT_EQ(complete_eof_replay.consumed_bytes(), std::size_t{0});
}

TEST(Ruvia, http1_response_body_decoder_205_gzip_chunked_eof_drains_buffered_terminator) {
    constexpr std::string_view gzip_empty{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x03"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        20};
    const auto head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip, chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::string input = "14\r\n";
    input.append(gzip_empty);
    input.append("\r\n0\r\n\r\nTAIL");
    std::array<char, 8> scratch{};
    bool trailers_seen = false;
    auto result_value = decoder.finish_input(input, scratch);
    for (std::size_t step = 0; step < 16 && !result_value.complete() &&
                               !result_value.protocol_failure() && !result_value.decoder_failure();
        ++step) {
        input.erase(0, result_value.consumed_bytes());
        trailers_seen = trailers_seen || result_value.trailers() != nullptr;
        result_value = decoder.finish_input(input, scratch);
    }
    EXPECT_TRUE(result_value.complete() != nullptr);
    EXPECT_TRUE(trailers_seen);
    EXPECT_EQ(input, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_205_accepts_empty_gzip_and_rejects_decoded_content) {
    constexpr std::string_view gzip_empty{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x03"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        20};
    const auto empty_head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder empty(empty_head.plan());
    std::array<char, 4> scratch{};
    auto accepted = empty.finish_input(gzip_empty, scratch);
    while (accepted.need_input() != nullptr) {
        accepted = empty.finish_input({}, scratch);
    }
    EXPECT_TRUE(accepted.protocol_failure() == nullptr);
    EXPECT_TRUE(accepted.decoder_failure() == nullptr);
    EXPECT_TRUE(accepted.complete() != nullptr);

    const auto nonempty_head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder nonempty(
        nonempty_head.plan());
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    // RFC 9110 section 15.3.6 forbids only generating 205 content: it is
    // consumed and discarded, never emitted.
    const auto discarded = nonempty.finish_input(gzip_hello, scratch);
    EXPECT_TRUE(discarded.output() == nullptr);
    EXPECT_TRUE(discarded.complete() != nullptr);
    EXPECT_EQ(discarded.consumed_bytes(), gzip_hello.size());

    const auto fixed_head = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 2");
    EXPECT_TRUE(fixed_head.plan().discards_content());
    ruvia::http1_client_response_body_decoder fixed(fixed_head.plan());
    const auto fixed_result = fixed.decode("okNEXT", scratch);
    EXPECT_TRUE(fixed_result.complete() != nullptr);
    EXPECT_EQ(fixed_result.consumed_bytes(), std::size_t{2});
    if (const auto* complete = fixed_result.complete()) {
        EXPECT_TRUE(complete->persistence() == ruvia::http1_close_policy::allow_reuse);
    }
}

TEST(Ruvia, http1_response_body_decoder_rejects_truncated_transfer_coding) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    constexpr std::string_view truncated_gzip{"\x1f\x8b\x08", 3};
    std::array<char, 8> scratch{};
    auto result_value = decoder.finish_input(truncated_gzip, scratch);
    while (result_value.output() != nullptr ||
           result_value.need_input() != nullptr) {
        result_value = decoder.finish_input({}, scratch);
    }
    EXPECT_TRUE(result_value.protocol_failure() != nullptr);
    EXPECT_TRUE(result_value.protocol_failure()->error() == ruvia::http1_client_response_body_error::incomplete_body);
}

TEST(Ruvia, http1_response_body_decoder_accepts_empty_transfer_coded_body) {
    std::array<char, 8> scratch{};
    const auto chunked_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    ruvia::http1_client_response_body_decoder chunked(
        chunked_head.plan());
    std::string input = "0\r\n\r\nTAIL";
    std::string body;
    auto result_value = chunked.decode(input, scratch);
    for (int step = 0; step < 8 && !result_value.complete() && !result_value.protocol_failure(); ++step) {
        input.erase(0, result_value.consumed_bytes());
        if (result_value.output()) {
            body.append(result_value.output()->bytes());
        }
        result_value = chunked.decode(input, scratch);
    }
    EXPECT_TRUE(result_value.complete() != nullptr);
    input.erase(0, result_value.consumed_bytes());
    EXPECT_TRUE(body.empty());
    EXPECT_EQ(input, "TAIL");

    const auto close_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder close_delimited(
        close_head.plan());
    auto eof_result = close_delimited.finish_input({}, scratch);
    for (int step = 0; step < 8 && eof_result.need_input() != nullptr; ++step) {
        eof_result = close_delimited.finish_input({}, scratch);
    }
    EXPECT_TRUE(eof_result.complete() != nullptr);
}

TEST(Ruvia, http1_response_body_decoder_accepts_many_small_chunks) {
    constexpr std::size_t chunk_count = 20000;
    std::string wire;
    wire.reserve(chunk_count * 6 + 9);
    for (std::size_t index = 0; index < chunk_count; ++index) {
        wire.append("1\r\nx\r\n");
    }
    wire.append("0\r\n\r\nTAIL");

    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 64> scratch{};
    std::string_view remaining = wire;
    std::size_t body_bytes = 0;
    bool complete_value = false;
    for (std::size_t step = 0; step <= chunk_count * 2 + 8; ++step) {
        const auto result_value = decoder.decode(remaining, scratch);
        EXPECT_TRUE(result_value.protocol_failure() == nullptr);
        if (result_value.protocol_failure() != nullptr) {
            break;
        }
        remaining.remove_prefix(result_value.consumed_bytes());
        if (const auto* output = result_value.output()) {
            body_bytes += output->bytes().size();
        }
        if (result_value.complete() != nullptr) {
            complete_value = true;
            break;
        }
    }
    EXPECT_TRUE(complete_value);
    EXPECT_EQ(body_bytes, chunk_count);
    EXPECT_EQ(remaining, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_handles_chunk_extension_and_trailer_over_8k) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::string wire = "1;";
    wire.append(9000, 'a');
    wire.append("\r\nz\r\n0\r\nX-Large: ");
    wire.append(9000, 'b');
    wire.append("\r\n\r\nTAIL");
    std::array<char, 4> scratch{};
    bool saw_body = false;
    bool saw_trailers = false;
    for (int step = 0; step < 8; ++step) {
        const auto result_value = decoder.decode(wire, scratch);
        EXPECT_TRUE(result_value.protocol_failure() == nullptr);
        if (result_value.output()) {
            saw_body = result_value.output()->bytes() == "z";
        }
        if (result_value.trailers()) {
            const auto fields = result_value.trailers()->fields();
            saw_trailers = fields.size() == 1 && fields[0].value().size() == 9000;
        }
        wire.erase(0, result_value.consumed_bytes());
        if (saw_trailers) {
            break;
        }
        EXPECT_TRUE(result_value.output() != nullptr || result_value.need_input() != nullptr);
    }
    EXPECT_TRUE(saw_body);
    EXPECT_TRUE(saw_trailers);
    EXPECT_EQ(wire, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_fragmented_chunk_and_205_empty_chunk_complete) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 4> scratch{};
    auto first = decoder.decode("3\r\nab", scratch);
    EXPECT_TRUE(first.output() != nullptr);
    EXPECT_EQ(first.output()->bytes(), "ab");
    EXPECT_EQ(first.consumed_bytes(), std::size_t{5});
    auto second = decoder.decode("c\r\n0\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(second.output() != nullptr);
    EXPECT_EQ(second.output()->bytes(), "c");
    auto trailers = decoder.decode("0\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(trailers.trailers() != nullptr);
    EXPECT_EQ(trailers.consumed_bytes(), std::size_t{5});
    EXPECT_TRUE(trailers.trailers()->fields().empty());
    const auto complete_value = decoder.decode("TAIL", scratch);
    EXPECT_TRUE(complete_value.complete() != nullptr);

    const auto reset_head = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder reset(
        reset_head.plan());
    auto empty_chunk = reset.decode("0\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(empty_chunk.trailers() != nullptr);
    EXPECT_TRUE(empty_chunk.trailers()->fields().empty());
    const auto reset_complete = reset.decode("TAIL", scratch);
    EXPECT_TRUE(reset_complete.complete() != nullptr);
}

TEST(Ruvia, http1_response_body_decoder_fixed_length_eof_is_incomplete) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 4> scratch{};
    const auto body = decoder.finish_input("ab", scratch);
    EXPECT_TRUE(body.output() != nullptr);
    const auto failure = decoder.finish_input({}, scratch);
    EXPECT_TRUE(failure.protocol_failure() != nullptr);
    EXPECT_TRUE(failure.protocol_failure()->error() == ruvia::http1_client_response_body_error::incomplete_body);
}

TEST(Ruvia, http1_response_body_decoder_decodes_transfer_coding_stack_in_reverse_order) {
    constexpr std::string_view plain = "HTTP/1 transfer coding stack";
    const auto wire = zlib_deflate_compress(gzip_compress(plain));
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 1> scratch{};
    std::string output;
    std::string_view input(wire);
    bool complete_value = false;
    for (std::size_t count = 0; count < 1024 && !complete_value; ++count) {
        const auto result_value = decoder.finish_input(input, scratch);
        input.remove_prefix(result_value.consumed_bytes());
        if (const auto* decoded = result_value.output()) {
            output.append(decoded->bytes());
        }
        if (result_value.protocol_failure() != nullptr || result_value.decoder_failure() != nullptr) {
            EXPECT_TRUE(false);
            return;
        }
        complete_value = result_value.complete() != nullptr;
    }
    EXPECT_TRUE(complete_value);
    EXPECT_TRUE(input.empty());
    EXPECT_EQ(output, std::string(plain));
}

TEST(Ruvia, http1_response_body_decoder_drains_pending_transfer_output_without_wire_input) {
    constexpr std::string_view deflate_aaaa{"\x78\x9c\x73\x04\x02\x00\x02\x8e\x01\x05", 10};
    for (const bool chunked : {false, true}) {
        const auto head = parse_head("GET", chunked
                                                ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate, chunked"
                                                : "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
        ruvia::http1_client_response_body_decoder decoder(head.plan());
        std::array<char, 1> scratch{};
        std::string input(deflate_aaaa.substr(0, 5));
        if (chunked) {
            input = "5\r\n" + input + "\r\n";
        }
        std::string content;
        const auto first = decoder.decode(input, scratch);
        EXPECT_TRUE(first.output() != nullptr);
        if (first.output()) {
            content.append(first.output()->bytes());
        }
        input.erase(0, first.consumed_bytes());
        EXPECT_TRUE(input.empty());
        for (int count = 0; count < 3; ++count) {
            const auto pending = decoder.decode(input, scratch);
            EXPECT_TRUE(pending.output() != nullptr);
            EXPECT_EQ(pending.consumed_bytes(), std::size_t{0});
            if (!pending.output()) {
                break;
            }
            content.append(pending.output()->bytes());
        }
        // All four bytes must be observable before the peer supplies the rest
        // of the encoding or any subsequent chunk framing.
        EXPECT_EQ(content, "AAAA");
        const auto needs_input = decoder.decode(input, scratch);
        EXPECT_TRUE(needs_input.need_input() != nullptr);

        input.assign(deflate_aaaa.substr(5));
        if (chunked) {
            input = "5\r\n" + input + "\r\n0\r\n\r\nTAIL";
        }
        bool complete_value = false;
        for (int count = 0; count < 16 && !complete_value; ++count) {
            const auto result_value = decoder.finish_input(input, scratch);
            EXPECT_TRUE(result_value.protocol_failure() == nullptr);
            EXPECT_TRUE(result_value.decoder_failure() == nullptr);
            EXPECT_TRUE(result_value.need_input() == nullptr);
            if (result_value.output()) {
                content.append(result_value.output()->bytes());
            }
            input.erase(0, result_value.consumed_bytes());
            complete_value = result_value.complete() != nullptr;
            if (result_value.protocol_failure() || result_value.decoder_failure()) {
                break;
            }
        }
        EXPECT_TRUE(complete_value);
        EXPECT_EQ(content, "AAAA");
        EXPECT_EQ(input, chunked ? "TAIL" : "");
    }
}

TEST(Ruvia, http1_response_role_chunk_decoder_preserves_typed_views_and_wire_prefix) {
    constexpr std::string_view wire = "1\r\nx\r\n0\r\nAccept-Ranges: bytes\r\n\r\nNEXT";
    ruvia::http1_chunked_body_decoder decoder({.trailer_message_ = ruvia::http1_trailer_message::response});
    bool zero_budget_rejected = false;
    try {
        (void)decoder.decode(wire, 0);
    } catch (const std::invalid_argument&) {
        zero_budget_rejected = true;
    }
    EXPECT_TRUE(zero_budget_rejected);
    const auto body = decoder.decode(wire, 1);
    EXPECT_TRUE(body.body_chunk() != nullptr);
    EXPECT_EQ(body.consumed_bytes(), std::size_t{6});
    if (const auto* chunk = body.body_chunk()) {
        EXPECT_EQ(chunk->bytes(), "x");
        EXPECT_TRUE(chunk->bytes().data() == wire.data() + 3);
    }
    auto pending = wire.substr(body.consumed_bytes());
    const auto terminal = decoder.decode(pending, 1);
    EXPECT_TRUE(terminal.complete() != nullptr);
    if (const auto* complete = terminal.complete()) {
        EXPECT_EQ(rendered(complete->trailers()), "accept-ranges=bytes;");
    }
    pending.remove_prefix(terminal.consumed_bytes());
    EXPECT_EQ(pending, "NEXT");
    const auto replay = decoder.decode(pending);
    EXPECT_TRUE(replay.complete() != nullptr);
    EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});

    ruvia::http1_chunked_body_decoder invalid;
    const auto failure = invalid.decode("1\r\nxXX");
    EXPECT_TRUE(failure.failure() != nullptr);
    if (const auto* error = failure.failure()) {
        EXPECT_TRUE(error->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
    const auto repeated_failure = invalid.decode("NEXT", 1);
    EXPECT_TRUE(repeated_failure.failure() != nullptr);
    EXPECT_EQ(repeated_failure.consumed_bytes(), std::size_t{0});
    if (const auto* error = repeated_failure.failure()) {
        EXPECT_TRUE(error->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
}

TEST(Ruvia, http1_response_body_decoder_requires_nonempty_scratch) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    bool rejected = false;
    try {
        (void)decoder.decode({}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, response_body_decode_continues_draining_after_framing_eof) {
    const std::string plain(128, 'a');
    for (const std::string_view framing : {std::string_view("fixed"), std::string_view("chunked"),
             std::string_view("gzip"), std::string_view("deflate"),
             std::string_view("gzip, chunked"), std::string_view("deflate, chunked")}) {
        const bool fixed = framing == "fixed";
        const bool chunk_framed = framing.ends_with("chunked");
        const bool compressed = framing != "fixed" && framing != "chunked";
        std::string header = "HTTP/1.1 200 OK\r\n";
        header += fixed ? "Content-Length: 128" : "Transfer-Encoding: " + std::string(framing);
        const auto head = ruvia::test::parse_head("GET", header);
        ruvia::http1_client_response_body_decoder decoder(head.plan());
        std::string pending = compressed
                                  ? (framing.starts_with("gzip") ? gzip_compress(plain) : zlib_deflate_compress(plain))
                                  : plain;
        if (chunk_framed) {
            pending = chunked(pending);
        }
        if (fixed || chunk_framed) {
            pending.append("NEXT");
        }
        std::array<char, 1> scratch{};
        std::string decoded;
        auto result = decoder.finish_input(pending, scratch);
        bool complete = false;
        for (std::size_t step = 0; step < plain.size() * 2 + 16; ++step) {
            EXPECT_TRUE(result.need_input() == nullptr);
            EXPECT_TRUE(result.protocol_failure() == nullptr);
            EXPECT_TRUE(result.decoder_failure() == nullptr);
            if (const auto* output = result.output()) {
                decoded.append(output->bytes());
            }
            pending.erase(0, result.consumed_bytes());
            if (result.complete() != nullptr) {
                complete = true;
                break;
            }
            if (result.protocol_failure() != nullptr || result.decoder_failure() != nullptr) {
                break;
            }
            result = decoder.decode(pending, scratch);
        }
        EXPECT_TRUE(complete);
        EXPECT_EQ(decoded, plain);
        EXPECT_EQ(pending, fixed || chunk_framed ? "NEXT" : "");
        if (complete) {
            const auto replay = decoder.decode("NEXT", scratch);
            EXPECT_TRUE(replay.complete() != nullptr);
            EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
        }
    }
}

TEST(Ruvia, response_body_decode_preserves_incomplete_eof_after_output) {
    const auto head = ruvia::test::parse_head(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 2");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 1> scratch{};
    const auto output = decoder.finish_input("a", scratch);
    EXPECT_TRUE(output.output() != nullptr);
    EXPECT_EQ(output.consumed_bytes(), std::size_t{1});
    const auto failure = decoder.decode({}, scratch);
    EXPECT_TRUE(failure.protocol_failure() != nullptr);
    if (const auto* error = failure.protocol_failure()) {
        EXPECT_TRUE(error->error() == ruvia::http1_client_response_body_error::incomplete_body);
    }
    const auto replay = decoder.finish_input("NEXT", scratch);
    EXPECT_TRUE(replay.protocol_failure() != nullptr);
    EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
}

using namespace ruvia::test;

// HTTP/1 client responses: what the head says about the body.

namespace {

}  // namespace

TEST(Ruvia, http_client_response_plan_alternatives_are_exclusive) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto informational = parse_head("GET", "HTTP/1.1 103 Early Hints");
    const auto without_content = parse_head("GET", "HTTP/1.1 204 No Content");
    const auto known_length = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 1");
    const auto chunked = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    const auto close_delimited = parse_head("GET", "HTTP/1.1 200 OK");
    const auto tunnel = parse_head("CONNECT", "HTTP/1.1 200 Connection Established");
    const auto upgrade = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, upgrade_headers);

    for (const auto* plan : {&informational.plan(), &without_content.plan(), &known_length.plan(),
             &chunked.plan(), &close_delimited.plan(), &tunnel.plan(), &upgrade.plan()}) {
        EXPECT_EQ(active_plan_alternative_count(*plan), std::size_t{1});
    }
}

TEST(Ruvia, http_client_response_plan_owns_content_length_framing) {
    constexpr std::string_view header_value = "HTTP/1.1 200 OK\r\nContent-Length: 5";
    const auto head = parse_head("GET", header_value);
    const auto& known_length = require_known_length(head.plan());
    EXPECT_EQ(known_length.content_length(), std::size_t{5});
    EXPECT_TRUE(known_length.requires_body_consumption());
    EXPECT_TRUE(known_length.persistence() == http1_close_policy::allow_reuse);
    EXPECT_EQ(head.consumed_bytes(), header_value.size() + 4);

    const auto empty = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    EXPECT_TRUE(!require_known_length(empty.plan()).requires_body_consumption());
}

TEST(Ruvia, http_client_content_length_combined_and_repeated_equal_values) {
    const auto combined = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 5");
    EXPECT_EQ(require_known_length(combined.plan()).content_length(), std::size_t{5});

    const auto repeated =
        parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5");
    EXPECT_EQ(require_known_length(repeated.plan()).content_length(), std::size_t{5});

    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 6"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
}

TEST(Ruvia, http_client_response_unfolds_fields_before_framing_and_owns_values) {
    ruvia::http_client_request_view request;
    request.method_ = "GET";
    http1_client_response_parser parser =
        exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
    std::string wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0,\r\n \t0\r\n"
        "X-Trace: one \t\r\n\t two\r\n\r\n";
    for (std::size_t size = 1; size < wire.size(); ++size) {
        const auto partial = parser.parse(std::string_view(wire).substr(0, size));
        EXPECT_TRUE(partial.need_more() != nullptr);
    }

    const auto result_value = parser.parse(wire);
    const auto* parsed = result_value.parsed();
    EXPECT_TRUE(parsed != nullptr);
    if (parsed == nullptr) {
        return;
    }
    EXPECT_EQ(parsed->consumed_bytes(), wire.size());
    EXPECT_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{0});
    wire.assign(wire.size(), 'x');
    const auto headers = parsed->head().headers();
    EXPECT_EQ(headers.size(), std::size_t{2});
    if (headers.size() == 2) {
        EXPECT_EQ(headers[0].name(), "Content-Length");
        EXPECT_EQ(headers[0].value(), "0, 0");
        EXPECT_EQ(headers[1].name(), "X-Trace");
        EXPECT_EQ(headers[1].value(), "one two");
    }
}

TEST(Ruvia, http_client_response_unfolding_preserves_framing_validation) {
    const auto transfer = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip,\r\n\tchunked");
    const auto& chunked = require_chunked(transfer.plan());
    EXPECT_EQ(chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_EQ(transfer.head().headers().front().value(), "gzip, chunked");

    const std::pair<std::string_view, http1_client_response_parse_error> invalid[] = {
        {"Content-Length: 4\r\n 2", http1_client_response_parse_error::invalid_content_length},
        {"Content-Length: 4,\r\n 2", http1_client_response_parse_error::conflicting_content_length},
        {"Transfer-Encoding: chunked,\r\n gzip", http1_client_response_parse_error::invalid_transfer_encoding},
        {"Content-Length: 0\r\nTransfer-Encoding: gzip,\r\n chunked",
            http1_client_response_parse_error::content_length_and_transfer_encoding},
        {" Content-Length: 0", http1_client_response_parse_error::invalid_header},
        {"\tContent-Length: 0\r\nX-Trace: one\r\n two", http1_client_response_parse_error::invalid_header},
    };
    for (const auto& [fields, expected_error] : invalid) {
        const std::string head = "HTTP/1.1 200 OK\r\n" + std::string(fields);
        EXPECT_TRUE(parse_failure_error("GET", head) == expected_error);
    }
}

TEST(Ruvia, http_client_response_plan_owns_chunked_framing_and_reuse) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked");
    const auto& chunked = require_chunked(head.plan());
    EXPECT_TRUE(chunked.transfer_codings().empty());
    EXPECT_TRUE(chunked.persistence() == http1_close_policy::allow_reuse);
}

TEST(Ruvia, http_client_transfer_coding_before_final_chunked_is_typed) {
    const auto combined = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    const auto& combined_chunked = require_chunked(combined.plan());
    EXPECT_EQ(combined_chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(combined_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(combined_chunked.persistence() == http1_close_policy::allow_reuse);

    // Transfer-Encoding is list-based: split field lines retain wire order.
    const auto split = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n"
        "Transfer-Encoding: chunked");
    const auto& split_chunked = require_chunked(split.plan());
    EXPECT_EQ(split_chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(split_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::deflate);

    const auto repeated = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n"
        "Transfer-Encoding: deflate, chunked");
    const auto& repeated_chunked = require_chunked(repeated.plan());
    EXPECT_EQ(repeated_chunked.transfer_codings().size(), std::size_t{2});
    EXPECT_TRUE(repeated_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(repeated_chunked.transfer_codings()[1] == ruvia::http_transfer_coding::deflate);
}

TEST(Ruvia, http_client_transfer_encoding_ignores_empty_recipient_list_items) {
    for (const std::string_view fields : {
             "Transfer-Encoding: , chunked",
             "Transfer-Encoding: chunked,",
             "Transfer-Encoding: , , chunked, ,",
             "Transfer-Encoding: gzip, , chunked",
             "Transfer-Encoding: \r\nTransfer-Encoding: chunked",
             "Transfer-Encoding: chunked\r\nTransfer-Encoding: , ,",
             "Transfer-Encoding: ,\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: \r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: ,",
         }) {
        const std::string head = "HTTP/1.1 200 OK\r\n" + std::string(fields) + "\r\n\r\n";
        const std::string wire = head + "0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n";
        const auto result = parse_wire("GET", wire);
        const auto* parsed = result.parsed();
        EXPECT_TRUE(parsed != nullptr);
        if (parsed == nullptr) {
            continue;
        }
        const auto& chunked = require_chunked(parsed->plan());
        EXPECT_TRUE(chunked.persistence() == http1_close_policy::allow_reuse);
        const bool gzip = fields.find("gzip") != std::string_view::npos;
        EXPECT_EQ(chunked.transfer_codings().size(), gzip ? std::size_t{1} : std::size_t{0});
        if (gzip && !chunked.transfer_codings().empty()) {
            EXPECT_TRUE(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
        }
        EXPECT_EQ(parsed->consumed_bytes(), head.size());
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes()), "0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n");
    }

    const auto non_chunked = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: , gzip, ,\r\nTransfer-Encoding: ");
    const auto& close_delimited = require_close_delimited(non_chunked.plan());
    EXPECT_EQ(close_delimited.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(close_delimited.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);

    // Empty items do not make the field absent or relax framing conflicts.
    const auto empty = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , ,");
    EXPECT_TRUE(require_close_delimited(empty.plan()).transfer_codings().empty());
    EXPECT_TRUE(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: , ,\r\nContent-Length: 0") ==
                http1_client_response_parse_error::content_length_and_transfer_encoding);
}

TEST(Ruvia, http_client_non_chunked_transfer_coding_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    const auto& close_delimited = require_close_delimited(head.plan());
    EXPECT_EQ(close_delimited.transfer_codings().size(), std::size_t{1});
}

TEST(Ruvia, http_client_rejects_invalid_or_unsupported_transfer_coding) {
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;foo=bar"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress, chunked"));
    const auto malformed_after_unknown = parse_result("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress\r\n"
        "Transfer-Encoding: deflate, chunked, gzip");
    EXPECT_TRUE(malformed_after_unknown.failure() != nullptr);
    if (const auto* failure = malformed_after_unknown.failure()) {
        EXPECT_TRUE(*failure == http1_client_response_parse_error::invalid_transfer_encoding);
    }
    const auto stacked = parse_head(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked");
    const auto& stacked_chunked = require_chunked(stacked.plan());
    EXPECT_EQ(stacked_chunked.transfer_codings().size(), std::size_t{2});
    EXPECT_TRUE(stacked_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(stacked_chunked.transfer_codings()[1] == ruvia::http_transfer_coding::deflate);
}

TEST(Ruvia, http_client_content_length_and_transfer_encoding_rejected_for_body) {
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked"));
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 5"));
}

TEST(Ruvia, http_client_no_body_precedence_ignores_framing_fields) {
    const auto head = parse_head("HEAD",
        "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n"
        "Transfer-Encoding: custom-coding");
    const auto& without_content = require_without_content(head.plan());
    EXPECT_TRUE(without_content.persistence() == http1_close_policy::allow_reuse);

    const auto not_modified = parse_head("GET",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 7\r\n"
        "Transfer-Encoding: custom-coding");
    EXPECT_TRUE(not_modified.plan().without_content() != nullptr);

    const auto no_content = parse_head("GET", "HTTP/1.1 204 No Content");
    EXPECT_TRUE(no_content.plan().without_content() != nullptr);
}

TEST(Ruvia, http_client_trailer_declaration_metadata_preserves_framed_body_and_next_response) {
    for (const std::string_view declaration :
        {"Content-Length", "Host", "Bad Field", "\"x-name\"", ",", "X-One,,X-Two"}) {
        for (const bool chunked : {false, true}) {
            std::string head = "HTTP/1.1 200 OK\r\nTrailer: \t";
            head.append(declaration);
            head.append(" \t\r\n");
            head.append(chunked ? "Transfer-Encoding: chunked\r\n\r\n" : "Content-Length: 3\r\n\r\n");
            const std::string wire = head + (chunked ? "3\r\nabc\r\n0\r\n\r\n" : "abc") +
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            const auto result = parse_wire("GET", wire);
            EXPECT_TRUE(result.parsed() != nullptr);
            if (const auto* parsed = result.parsed()) {
                EXPECT_EQ(parsed->head().headers().front().value(), declaration);
                EXPECT_EQ(parsed->consumed_bytes(), head.size());
                if (chunked) {
                    EXPECT_TRUE(parsed->plan().chunked() != nullptr);
                } else {
                    EXPECT_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{3});
                }
                ruvia::http1_client_response_body_decoder decoder(
                    parsed->plan());
                std::array<char, 8> scratch{};
                auto remaining = std::string_view(wire).substr(parsed->consumed_bytes());
                std::string body;
                bool complete = false;
                for (int step = 0; step < 8; ++step) {
                    const auto decoded = decoder.decode(remaining, scratch);
                    EXPECT_TRUE(decoded.protocol_failure() == nullptr);
                    EXPECT_TRUE(decoded.decoder_failure() == nullptr);
                    remaining.remove_prefix(decoded.consumed_bytes());
                    if (const auto* output = decoded.output()) {
                        body.append(output->bytes());
                    }
                    if (decoded.complete()) {
                        complete = true;
                        break;
                    }
                }
                EXPECT_TRUE(complete);
                EXPECT_EQ(body, "abc");
                const auto next = parse_wire("GET", remaining);
                EXPECT_TRUE(next.parsed() != nullptr);
                if (const auto* next_head = next.parsed()) {
                    EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
                    EXPECT_EQ(remaining.substr(next_head->consumed_bytes()), "ok");
                }
            }
        }
    }
}

TEST(Ruvia, http_client_bodyless_trailer_declarations_preserve_message_boundaries) {
    for (const auto& [method, status_line] : {
             std::pair{std::string_view("HEAD"), std::string_view("HTTP/1.1 200 OK")},
             std::pair{std::string_view("GET"), std::string_view("HTTP/1.1 304 Not Modified")},
             std::pair{std::string_view("GET"), std::string_view("HTTP/1.1 204 No Content")},
         }) {
        ruvia::http_client_request_view request;
        request.method_ = method;
        for (const bool transfer_encoding : {false, true}) {
            std::string head(status_line);
            head.append("\r\n");
            if (transfer_encoding) {
                head.append("Transfer-Encoding: chunked\r\n");
            }
            // RFC 9110 section 6.6.2 makes Trailer a declaration, not a promise
            // that trailer fields follow. RFC 9112 section 6.3 ends both of
            // these responses at the empty line regardless of their fields.
            head.append("Trailer: Content-Length, Bad Field, \"x-name\"\r\n\r\n");
            const std::string wire = head + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            http1_client_response_parser parser =
                exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
            for (std::size_t size = 1; size < head.size(); ++size) {
                const auto partial = parser.parse(std::string_view(wire).substr(0, size));
                EXPECT_TRUE(partial.need_more() != nullptr);
            }
            const auto result = parser.parse(wire);
            const auto* parsed = result.parsed();
            EXPECT_TRUE(parsed != nullptr);
            if (parsed == nullptr) {
                continue;
            }
            EXPECT_TRUE(parsed->plan().without_content() != nullptr);
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(parsed->head().headers().back().name(), "Trailer");
            EXPECT_EQ(parsed->head().headers().back().value(), "Content-Length, Bad Field, \"x-name\"");
            const auto following = std::string_view(wire).substr(parsed->consumed_bytes());
            const auto next = parse_wire("GET", following);
            EXPECT_TRUE(next.parsed() != nullptr);
            if (const auto* next_head = next.parsed()) {
                EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
                EXPECT_EQ(following.substr(next_head->consumed_bytes()), "ok");
            }
        }
    }

    constexpr std::string_view tunnel_wire =
        "HTTP/1.1 200 Connection Established\r\nTrailer: Content-Length, Bad Field\r\n\r\ntunnel bytes";
    const auto tunnel = parse_wire("CONNECT", tunnel_wire);
    EXPECT_TRUE(tunnel.parsed() != nullptr);
    if (const auto* parsed = tunnel.parsed()) {
        EXPECT_TRUE(parsed->plan().connect_tunnel() != nullptr);
        EXPECT_EQ(tunnel_wire.substr(parsed->consumed_bytes()), "tunnel bytes");
    }

    const ruvia::http_header_view upgrade_headers[] = {{"Connection", "Upgrade"}, {"Upgrade", "websocket"}};
    constexpr std::string_view upgrade_wire =
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Trailer: X-Checksum\r\n\r\nprotocol bytes";
    const auto upgrade = parse_wire("GET", upgrade_wire, http1_close_policy::allow_reuse, upgrade_headers);
    EXPECT_TRUE(upgrade.parsed() != nullptr);
    if (const auto* parsed = upgrade.parsed()) {
        EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
        EXPECT_EQ(upgrade_wire.substr(parsed->consumed_bytes()), "protocol bytes");
    }
}

TEST(Ruvia, http_client_auxiliary_response_fields_do_not_change_content_framing) {
    constexpr std::string_view auxiliary =
        "Upgrade: websocket\r\nTrailer: X-Checksum\r\nTE: trailers\r\n";
    for (const std::string_view framing : {
             std::string_view("Content-Length: 2\r\n"),
             std::string_view("Transfer-Encoding: chunked\r\n"),
             std::string_view("Transfer-Encoding: gzip\r\n"),
             std::string_view{},
         }) {
        std::string head("HTTP/1.1 200 OK\r\n");
        head.append(auxiliary);
        head.append(framing);
        head.append("\r\n");
        const std::string wire = head + "body bytes";
        const auto result = parse_wire("GET", wire);
        EXPECT_TRUE(result.parsed() != nullptr);
        if (const auto* parsed = result.parsed()) {
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes()), "body bytes");
            if (framing.starts_with("Content-Length")) {
                const auto& body = require_known_length(parsed->plan());
                EXPECT_EQ(body.content_length(), std::size_t{2});
                EXPECT_TRUE(body.persistence() == http1_close_policy::allow_reuse);
            } else if (framing == "Transfer-Encoding: chunked\r\n") {
                EXPECT_TRUE(require_chunked(parsed->plan()).persistence() == http1_close_policy::allow_reuse);
            } else {
                EXPECT_TRUE(parsed->plan().close_delimited() != nullptr);
            }
        }
    }
    for (const std::string_view malformed : {
             std::string_view("Content-Length: invalid\r\n"),
             std::string_view("Content-Length: 2\r\nTransfer-Encoding: chunked\r\n"),
             std::string_view("Transfer-Encoding: chunked, gzip\r\n"),
             std::string_view("Transfer-Encoding: custom-coding\r\n"),
         }) {
        std::string head("HTTP/1.1 200 OK\r\n");
        head.append(auxiliary);
        head.append(malformed);
        EXPECT_TRUE(parse_fails("GET", head));
    }
}

TEST(Ruvia, http_client_informational_auxiliary_fields_preserve_the_final_response_boundary) {
    for (const std::string_view status_line :
        {"HTTP/1.1 100 Continue\r\n", "HTTP/1.1 103 Early Hints\r\n"}) {
        ruvia::http_client_request_view request;
        std::string head(status_line);
        head.append(
            "Content-Length: ignored\r\nTransfer-Encoding: ignored\r\n"
            "Trailer: Content-Length, Bad Field\r\nTE: trailers\r\nUpgrade: websocket\r\n"
            "Content-Type: text/plain\r\nContent-Type: application/json\r\n"
            "Server: first\r\nServer: second\r\n\r\n");
        const std::string wire = head + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        http1_client_response_parser parser =
            exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
        for (std::size_t size = 1; size < head.size(); ++size) {
            const auto partial = parser.parse(std::string_view(wire).substr(0, size));
            EXPECT_TRUE(partial.need_more() != nullptr);
        }
        const auto interim = parser.parse(wire);
        EXPECT_TRUE(interim.parsed() != nullptr);
        if (const auto* parsed = interim.parsed()) {
            EXPECT_TRUE(parsed->plan().informational() != nullptr);
            if (const auto* informational = parsed->plan().informational()) {
                EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
            }
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(parsed->head().headers().size(), std::size_t{9});
            const auto final_response = parser.parse(std::string_view(wire).substr(parsed->consumed_bytes()));
            EXPECT_TRUE(final_response.parsed() != nullptr);
            if (const auto* final_head = final_response.parsed()) {
                EXPECT_EQ(require_known_length(final_head->plan()).content_length(), std::size_t{2});
                EXPECT_TRUE(require_known_length(final_head->plan()).persistence() == http1_close_policy::allow_reuse);
            }
        }
        head.insert(head.size() - 2, "Connection: close\r\n");
        const auto closing = parse_head("GET", head);
        EXPECT_TRUE(closing.plan().informational() != nullptr);
        if (const auto* informational = closing.plan().informational()) {
            // A 1xx cannot close the connection; its final response decides.
            EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
}

TEST(Ruvia, http_client_no_body_content_length_metadata_does_not_fail_the_response) {
    for (const auto& [method, response] : {std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: invalid"},
             std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: invalid"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5, 6"}}) {
        const auto parsed = parse_head(method, response);
        EXPECT_TRUE(parsed.plan().without_content() != nullptr);
        EXPECT_FALSE(parsed.head().content_length().has_value());
    }

    const auto repeated =
        parse_head("GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\nContent-Length: 5");
    EXPECT_TRUE(repeated.plan().without_content() != nullptr);
    EXPECT_EQ(repeated.head().content_length(), std::optional<std::uint64_t>(5));
    const auto head = parse_head("HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 42");
    EXPECT_EQ(head.head().content_length(), std::optional<std::uint64_t>(42));
    const auto invalid_then_valid = parse_head(
        "HEAD", "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\nContent-Length: 42");
    EXPECT_FALSE(invalid_then_valid.head().content_length().has_value());
}

TEST(Ruvia, http_client_bodyless_responses_ignore_framing_fields) {
    // RFC 9112 section 6.3 rule 1: 1xx, 204, 304 and HEAD responses end at the
    // header section whatever Content-Length or Transfer-Encoding they carry.
    for (const std::string_view head_value : {
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: 0"),
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: 5"),
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: invalid"),
             std::string_view("HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked"),
             std::string_view("HTTP/1.1 204 No Content\r\nTransfer-Encoding: custom-coding"),
         }) {
        const auto head = parse_head("GET", head_value);
        const auto& without_content = require_without_content(head.plan());
        EXPECT_TRUE(without_content.persistence() == http1_close_policy::allow_reuse);
        EXPECT_EQ(head.consumed_bytes(), head_value.size() + 4);
    }
}

TEST(Ruvia, http_client_bodyless_framing_fields_do_not_desynchronize_the_connection) {
    // A smuggled body after a bodyless head is the next response, not content.
    constexpr std::string_view no_content_head =
        "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string wire(no_content_head);
    wire.append("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto no_content = parse_wire("GET", wire);
    EXPECT_TRUE(no_content.parsed() != nullptr);
    if (const auto* parsed = no_content.parsed()) {
        EXPECT_TRUE(parsed->plan().without_content() != nullptr);
        EXPECT_EQ(parsed->consumed_bytes(), no_content_head.size());
        // The next exchange starts exactly after the empty line.
        const auto next = parse_wire("GET", std::string_view(wire).substr(parsed->consumed_bytes()));
        EXPECT_TRUE(next.parsed() != nullptr);
        if (const auto* next_head = next.parsed()) {
            EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
        }
    }

    ruvia::http_client_request_view request;
    request.method_ = "GET";
    http1_client_response_parser parser =
        exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
    constexpr std::string_view interim_head = "HTTP/1.1 103 Early Hints\r\nContent-Length: 5\r\n\r\n";
    std::string interim_wire(interim_head);
    interim_wire.append("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto interim = parser.parse(interim_wire);
    EXPECT_TRUE(interim.parsed() != nullptr);
    if (interim.parsed() == nullptr) {
        return;
    }
    EXPECT_TRUE(interim.parsed()->plan().informational() != nullptr);
    EXPECT_EQ(interim.parsed()->consumed_bytes(), interim_head.size());
    const auto final_response =
        parser.parse(std::string_view(interim_wire).substr(interim.parsed()->consumed_bytes()));
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_EQ(require_known_length(final_response.parsed()->plan()).content_length(), std::size_t{2});
    }
}

TEST(Ruvia, http_client_205_owns_zero_content_framing) {
    const auto zero_length = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 0");
    EXPECT_TRUE(zero_length.plan().discards_content());
    const auto* zero_length_body = zero_length.plan().known_length();
    EXPECT_TRUE(zero_length_body != nullptr);
    if (zero_length_body == nullptr) {
        return;
    }
    EXPECT_TRUE(!zero_length_body->requires_body_consumption());
    EXPECT_TRUE(zero_length_body->persistence() == http1_close_policy::allow_reuse);
    EXPECT_EQ(active_plan_alternative_count(zero_length.plan()), std::size_t{1});

    // RFC 9110 section 15.3.6 binds the sender: delimited content is consumed.
    const auto with_content = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3");
    EXPECT_TRUE(with_content.plan().discards_content());
    EXPECT_EQ(require_known_length(with_content.plan()).content_length(), std::size_t{3});
    EXPECT_TRUE(require_known_length(with_content.plan()).persistence() == http1_close_policy::allow_reuse);

    const auto chunked = parse_head("GET",
        "HTTP/1.1 205 Reset Content\r\n"
        "Transfer-Encoding: gzip, chunked");
    const auto& chunked_zero = chunked.plan();
    EXPECT_TRUE(chunked_zero.discards_content());
    EXPECT_TRUE(chunked_zero.chunked() != nullptr);
    EXPECT_TRUE(chunked_zero.close_delimited() == nullptr);
    if (chunked_zero.chunked() != nullptr) {
        EXPECT_EQ(chunked_zero.chunked()->transfer_codings().size(), std::size_t{1});
        EXPECT_TRUE(chunked_zero.chunked()->transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    }
    EXPECT_EQ(active_plan_alternative_count(chunked.plan()), std::size_t{1});

    const auto transfer_coded =
        parse_head("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    const auto& transfer_coded_zero = transfer_coded.plan();
    EXPECT_TRUE(transfer_coded_zero.discards_content());
    EXPECT_TRUE(transfer_coded_zero.close_delimited() != nullptr);
    if (transfer_coded_zero.close_delimited() != nullptr) {
        EXPECT_EQ(transfer_coded_zero.close_delimited()->transfer_codings().size(), std::size_t{1});
        EXPECT_TRUE(transfer_coded_zero.close_delimited()->transfer_codings()[0] ==
                    ruvia::http_transfer_coding::gzip);
    }

    const auto unframed = parse_head("GET", "HTTP/1.1 205 Reset Content");
    const auto& close_zero = unframed.plan();
    EXPECT_TRUE(close_zero.discards_content());
    EXPECT_TRUE(close_zero.close_delimited() != nullptr);
    EXPECT_TRUE(close_zero.chunked() == nullptr);
    EXPECT_EQ(active_plan_alternative_count(unframed.plan()), std::size_t{1});

    const auto connect =
        parse_head("CONNECT", "HTTP/1.1 205 Reset Content\r\nContent-Length: invalid");
    EXPECT_TRUE(connect.plan().connect_tunnel() != nullptr);
    EXPECT_TRUE(!connect.plan().discards_content());
}

// RFC 9110 section 15: a received 000..099 or 600..999 is processed as a 5xx
// response; the status-code is still exactly three digits.
TEST(Ruvia, http_client_processes_out_of_range_status_as_server_error) {
    for (const auto& [line, value] : {std::pair{std::string_view("HTTP/1.1 600 Odd"), 600},
             std::pair{std::string_view("HTTP/1.1 999 "), 999}, std::pair{std::string_view("HTTP/1.1 099 Low"), 99},
             std::pair{std::string_view("HTTP/1.1 000 "), 0}}) {
        std::string response(line);
        response.append("\r\nContent-Length: 2");
        const auto head = parse_head("GET", response);
        EXPECT_TRUE(head.head().status() == ruvia::http_status::internal_server_error);
        EXPECT_EQ(head.head().received_status().value(), value);
        EXPECT_EQ(require_known_length(head.plan()).content_length(), std::size_t{2});
    }
    for (const std::string_view line : {"HTTP/1.1 -00 Signed", "HTTP/1.1 +99 Signed", "HTTP/1.1 0x1 Hex"}) {
        std::string response(line);
        response.append("\r\nContent-Length: 0");
        EXPECT_TRUE(parse_fails("GET", response));
    }
}

// RFC 9112 section 6.3 rule 1 precedes rule 5: HEAD and 304 responses end at
// the header section, so an invalid Content-Length does not fail them.
TEST(Ruvia, http_client_ignores_invalid_content_length_without_content) {
    for (const auto& [method, response] : {std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: abc"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 1, 2"}}) {
        const auto head = parse_head(method, response);
        EXPECT_TRUE(head.plan().without_content() != nullptr);
    }
}

TEST(Ruvia, http_client_final_after_continue_does_not_cancel_released_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    http1_client_response_parser parser = exchange_parser(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        EXPECT_TRUE(continue_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }

    const auto final_response = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(!final_response.parsed()->plan().request_content_signal());
        EXPECT_TRUE(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }
}

TEST(Ruvia, http_client_closing_final_stops_unfinished_request_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    http1_client_response_parser parser = exchange_parser(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);

    const auto closing_final = parser.parse(
        "HTTP/1.1 413 Content Too Large\r\n"
        "Connection: close\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(closing_final.parsed() != nullptr);
    if (closing_final.parsed() != nullptr) {
        EXPECT_TRUE(closing_final.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
        EXPECT_TRUE(require_known_length(closing_final.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }
}

TEST(Ruvia, http_client_response_preserves_typed_protocol_version) {
    const auto http10 = parse_head("GET", "HTTP/1.0 204 No Content");
    const auto http11 = parse_head("GET", "HTTP/1.1 204 No Content");

    EXPECT_TRUE(http10.head().protocol_version() == http_protocol_version::http10);
    EXPECT_TRUE(http11.head().protocol_version() == http_protocol_version::http11);
}

TEST(Ruvia, http_client_unframed_body_response_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK");
    EXPECT_TRUE(head.plan().close_delimited() != nullptr);
}

TEST(Ruvia, http_client_http10_transfer_encoding_is_faulty_framing_only_with_content) {
    // RFC 9112 section 6.1: the codings are ignored; the message falls back to
    // Content-Length, else close delimiting, and closes after it.
    const auto unframed = parse_head("GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked");
    const auto* close_delimited = unframed.plan().close_delimited();
    EXPECT_TRUE(close_delimited != nullptr);
    if (close_delimited != nullptr) {
        EXPECT_TRUE(close_delimited->transfer_codings().empty());
    }
    const auto framed = parse_head(
        "GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5");
    EXPECT_EQ(require_known_length(framed.plan()).content_length(), std::size_t{5});
    EXPECT_TRUE(require_known_length(framed.plan()).persistence() ==
                http1_close_policy::close_after_response);
    // RFC 9112 section 6.3 rule 1: HEAD, 204 and 304 responses end at the
    // header section regardless of framing fields. RFC 9112 section 6.1 still
    // treats the HTTP/1.0 message as faulty: the connection closes after it,
    // whatever keep-alive it requests.
    for (const auto& [method, response] : {
             std::pair{"HEAD", "HTTP/1.0 200 OK\r\nTransfer-Encoding: gzip\r\nConnection: keep-alive"},
             std::pair{"GET", "HTTP/1.0 304 Not Modified\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive"},
             std::pair{"GET", "HTTP/1.0 204 No Content\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive"},
         }) {
        const auto head = parse_head(method, response);
        EXPECT_TRUE(require_without_content(head.plan()).persistence() == http1_close_policy::close_after_response);
    }
    const auto keep_alive = parse_head("GET", "HTTP/1.0 304 Not Modified\r\nConnection: keep-alive");
    EXPECT_TRUE(require_without_content(keep_alive.plan()).persistence() == http1_close_policy::allow_reuse);
}

TEST(Ruvia, http_client_head_method_is_case_sensitive) {
    const auto head = parse_head("HEAD", "HTTP/1.1 200 OK");
    const auto lowercase = parse_head("head", "HTTP/1.1 200 OK");
    EXPECT_TRUE(head.plan().without_content() != nullptr);
    EXPECT_TRUE(lowercase.plan().close_delimited() != nullptr);
}

using namespace ruvia::test;

// HTTP/1 client responses: the exchange around it: interim responses, Expect, upgrades and tunnels.

TEST(Ruvia, http_client_informational_responses_ignore_framing_fields) {
    for (const auto status :
        {std::string_view("HTTP/1.1 100 Continue"), std::string_view("HTTP/1.1 103 Early Hints")}) {
        const auto head = parse_head("GET", status);
        EXPECT_TRUE(head.plan().informational() != nullptr);
    }

    const auto valid = parse_head("GET",
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </style.css>; rel=preload\r\n"
        "Content-Type: text/html; charset=utf-8");
    EXPECT_TRUE(valid.plan().informational() != nullptr);

    // RFC 9112 section 6.3 rule 1: a 1xx ends at its header section, so the
    // framing fields a sender must not emit are ignored rather than fatal.
    for (const auto fields : {std::string_view("Content-Length: 5"),
             std::string_view("Transfer-Encoding: chunked"),
             std::string_view("Content-Length: invalid\r\nTransfer-Encoding: gzip")}) {
        std::string head("HTTP/1.1 103 Early Hints\r\n");
        head.append(fields);
        const auto ignored = parse_head("GET", head);
        EXPECT_TRUE(ignored.plan().informational() != nullptr);
        EXPECT_EQ(ignored.consumed_bytes(), head.size() + 4);
    }
}

TEST(Ruvia, http_client_long_informational_sequence_preserves_continue_and_final_boundary) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    http1_client_response_parser parser = exchange_parser(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    constexpr std::string_view early_hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </asset>; rel=preload\r\n\r\n";
    for (std::size_t i = 0; i < 300; ++i) {
        const auto interim = parser.parse(early_hints);
        const auto* parsed = interim.parsed();
        EXPECT_TRUE(parsed != nullptr);
        if (parsed == nullptr) {
            return;
        }
        EXPECT_TRUE(parsed->plan().informational() != nullptr);
        EXPECT_TRUE(!parsed->plan().request_content_signal());
        EXPECT_EQ(parsed->consumed_bytes(), early_hints.size());
    }
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() == nullptr) {
        return;
    }
    EXPECT_TRUE(continue_response.parsed()->plan().request_content_signal() ==
                http_client_request_content_signal::continue_value);
    EXPECT_TRUE(parser.complete_request_content() == http1_client_request_content_completion_status::completed);

    constexpr std::string_view final_wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcNEXT";
    const auto final_response = parser.parse(final_wire);
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (const auto* parsed = final_response.parsed()) {
        EXPECT_EQ(parsed->head().status().value(), std::uint16_t{200});
        EXPECT_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{3});
        EXPECT_EQ(final_wire.substr(parsed->consumed_bytes()), "abcNEXT");
        EXPECT_TRUE(!parsed->plan().request_content_signal());
    }
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)parser.parse({}); }));
}

TEST(Ruvia, http_client_expect_continue_is_one_stateful_exchange_contract) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    std::array<char, 512> request_head;
    auto prepared_result = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    auto* prepared = std::get_if<ruvia::prepared_http1_client_request>(&prepared_result);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    EXPECT_TRUE(prepared->content_plan().continue_gated() != nullptr);

    http1_client_response_parser parser(std::move(*prepared).take_exchange_state());
    auto early_hints = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    EXPECT_TRUE(early_hints.parsed() != nullptr);
    if (early_hints.parsed() != nullptr) {
        EXPECT_TRUE(!early_hints.parsed()->plan().request_content_signal());
        EXPECT_TRUE(early_hints.parsed()->plan().informational() != nullptr);
    }

    auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        EXPECT_TRUE(continue_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }
    const auto duplicate_continue = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(duplicate_continue.parsed() != nullptr);
    if (duplicate_continue.parsed() != nullptr) {
        EXPECT_TRUE(!duplicate_continue.parsed()->plan().request_content_signal());
    }
    EXPECT_TRUE(parser.complete_request_content() == http1_client_request_content_completion_status::completed);
    EXPECT_TRUE(parser.complete_request_content() ==
                http1_client_request_content_completion_status::already_complete);

    auto final_response = parser.parse("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(!final_response.parsed()->plan().request_content_signal());
    }

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)parser.parse("HTTP/1.1 204 No Content\r\n\r\n"); }));
    EXPECT_TRUE(parser.complete_request_content() ==
                http1_client_request_content_completion_status::exchange_terminal);
}

TEST(Ruvia, http_client_closing_informational_response_keeps_exchange_open) {
    ruvia::http_client_request_view get_request;
    get_request.method_ = "GET";
    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    http1_client_response_parser get_parser = exchange_parser(origin, get_request);
    const auto early_hints =
        get_parser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(early_hints.parsed() != nullptr);
    if (early_hints.parsed() != nullptr) {
        const auto* const informational = early_hints.parsed()->plan().informational();
        EXPECT_TRUE(informational != nullptr);
        if (informational != nullptr) {
            EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
    // RFC 9110 Section 15.2: any number of 1xx responses precede the final
    // response, whose own persistence decides the connection. A close option on
    // the 1xx is metadata only.
    const auto final_response = get_parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }

    ruvia::http_client_request_view post_request;
    post_request.method_ = "POST";
    post_request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    const http1_client_request_wire_policy continue_policy{
        .expectation_ = ruvia::http_client_request_expectation::continue_value};
    http1_client_response_parser post_parser = exchange_parser(origin, post_request, continue_policy);
    const auto closing_continue =
        post_parser.parse("HTTP/1.1 100 Continue\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(closing_continue.parsed() != nullptr);
    if (closing_continue.parsed() != nullptr) {
        EXPECT_TRUE(closing_continue.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }

    http1_client_response_parser closing_hints_parser = exchange_parser(origin, post_request, continue_policy);
    const auto closing_hints =
        closing_hints_parser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(closing_hints.parsed() != nullptr);
    if (closing_hints.parsed() != nullptr) {
        EXPECT_TRUE(!closing_hints.parsed()->plan().request_content_signal());
    }

    http1_client_response_parser request_close_parser = exchange_parser(origin, get_request,
        http1_client_request_wire_policy{.close_policy_ = http1_close_policy::close_after_response});
    const auto non_closing_hints = request_close_parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    EXPECT_TRUE(non_closing_hints.parsed() != nullptr);
    if (non_closing_hints.parsed() != nullptr) {
        const auto* const informational = non_closing_hints.parsed()->plan().informational();
        EXPECT_TRUE(informational != nullptr);
        if (informational != nullptr) {
            EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
    const auto request_close_final = request_close_parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    EXPECT_TRUE(request_close_final.parsed() != nullptr);
    if (request_close_final.parsed() != nullptr) {
        EXPECT_TRUE(require_without_content(request_close_final.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }
}

TEST(Ruvia, http_client_expect_final_cancels_only_pending_request_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    const http1_client_request_wire_policy continue_policy{
        .expectation_ = ruvia::http_client_request_expectation::continue_value};

    http1_client_response_parser parser = exchange_parser(origin, request, continue_policy);
    const auto final_response =
        parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(final_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
        // The head committed Content-Length framing for content never sent.
        EXPECT_TRUE(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }

    http1_client_response_parser completed_parser = exchange_parser(origin, request, continue_policy);
    EXPECT_TRUE(completed_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto completed_final =
        completed_parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(completed_final.parsed() != nullptr);
    if (completed_final.parsed() != nullptr) {
        EXPECT_TRUE(!completed_final.parsed()->plan().request_content_signal());
        EXPECT_TRUE(require_known_length(completed_final.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }
}

TEST(Ruvia, http_client_timeout_released_content_follows_final_response_persistence) {
    const ruvia::http1_client_request_head_view request{
        .method_ = "POST", .target_ = "/upload", .content_length_ = 6};
    for (const bool closing : {false, true}) {
        http1_client_response_parser parser = exchange_parser(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request,
            {.expectation_ = ruvia::http_client_request_expectation::continue_value});
        parser.release_request_content();
        const auto response = parser.parse(closing
                                               ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                                               : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        EXPECT_TRUE(response.parsed() != nullptr);
        if (response.parsed() != nullptr) {
            const auto& plan = response.parsed()->plan();
            EXPECT_TRUE(require_known_length(plan).persistence() ==
                        (closing ? http1_close_policy::close_after_response : http1_close_policy::allow_reuse));
            if (closing) {
                EXPECT_TRUE(plan.request_content_signal() == http_client_request_content_signal::exchange_complete);
            } else {
                EXPECT_TRUE(!plan.request_content_signal());
            }
        }
    }
}

TEST(Ruvia, http_client_upgrade_accepts_omitted_continue_after_content_complete) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.headers_ = upgrade_headers;
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    const http1_client_request_wire_policy continue_policy{
        .expectation_ = ruvia::http_client_request_expectation::continue_value};

    http1_client_response_parser rejected_parser = exchange_parser(origin, request, continue_policy);
    const auto rejected = rejected_parser.parse(switching);
    EXPECT_TRUE(rejected.failure() != nullptr);
    if (rejected.failure() != nullptr) {
        EXPECT_TRUE(*rejected.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)rejected_parser.parse(switching); }));

    // RFC 9110 Section 10.1.1 lets the server omit 100 once it has received
    // some or all of the content, so a 101 after the client finished writing
    // the body is a valid switch.
    http1_client_response_parser completed_without_continue_parser =
        exchange_parser(origin, request, continue_policy);
    completed_without_continue_parser.release_request_content();
    EXPECT_TRUE(completed_without_continue_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto completed_without_continue = completed_without_continue_parser.parse(switching);
    EXPECT_TRUE(completed_without_continue.parsed() != nullptr);
    if (completed_without_continue.parsed() != nullptr) {
        EXPECT_TRUE(completed_without_continue.parsed()->plan().protocol_upgrade() != nullptr);
    }

    http1_client_response_parser late_continue_parser = exchange_parser(origin, request, continue_policy);
    late_continue_parser.release_request_content();
    EXPECT_TRUE(late_continue_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto late_continue = late_continue_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(late_continue.parsed() != nullptr);
    if (late_continue.parsed() != nullptr) {
        EXPECT_TRUE(!late_continue.parsed()->plan().request_content_signal());
    }
    const auto accepted_after_late_continue = late_continue_parser.parse(switching);
    EXPECT_TRUE(accepted_after_late_continue.parsed() != nullptr);
    if (accepted_after_late_continue.parsed() != nullptr) {
        EXPECT_TRUE(accepted_after_late_continue.parsed()->plan().protocol_upgrade() != nullptr);
    }

    http1_client_response_parser pending_parser = exchange_parser(origin, request, continue_policy);
    const auto pending_continue = pending_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(pending_continue.parsed() != nullptr);
    const auto pending_upgrade = pending_parser.parse(switching);
    EXPECT_TRUE(pending_upgrade.failure() != nullptr);
    if (pending_upgrade.failure() != nullptr) {
        EXPECT_TRUE(*pending_upgrade.failure() ==
                    http1_client_response_parse_error::invalid_protocol_switch);
    }

    http1_client_response_parser accepted_parser = exchange_parser(origin, request, continue_policy);
    accepted_parser.release_request_content();
    const auto continue_response = accepted_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        EXPECT_TRUE(!continue_response.parsed()->plan().request_content_signal());
    }
    EXPECT_TRUE(accepted_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto accepted = accepted_parser.parse(switching);
    EXPECT_TRUE(accepted.parsed() != nullptr);
    if (accepted.parsed() != nullptr) {
        EXPECT_TRUE(accepted.parsed()->plan().protocol_upgrade() != nullptr);
    }
}

TEST(Ruvia, http_client_upgrade_requires_complete_request_content) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.headers_ = upgrade_headers;
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    http1_client_response_parser incomplete_parser = exchange_parser(origin, request);
    const auto incomplete = incomplete_parser.parse(switching);
    EXPECT_TRUE(incomplete.failure() != nullptr);
    if (incomplete.failure() != nullptr) {
        EXPECT_TRUE(*incomplete.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }

    http1_client_response_parser complete_parser = exchange_parser(origin, request);
    EXPECT_TRUE(complete_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto complete_value = complete_parser.parse(switching);
    EXPECT_TRUE(complete_value.parsed() != nullptr);
    if (complete_value.parsed() != nullptr) {
        EXPECT_TRUE(complete_value.parsed()->plan().protocol_upgrade() != nullptr);
    }
}

TEST(Ruvia, http_client_switching_protocols_is_an_exclusive_upgrade_transition) {
    const ruvia::http_header_view request_headers[] = {
        {"Connection", "keep-alive, Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto upgraded = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket",
        http1_close_policy::allow_reuse, request_headers);
    EXPECT_TRUE(upgraded.head().status() == ruvia::http_status::switching_protocols);
    EXPECT_TRUE(upgraded.plan().protocol_upgrade() != nullptr);
    EXPECT_TRUE(upgraded.plan().connect_tunnel() == nullptr);

    // Protocol names compare case-insensitively; versions remain exact tokens.
    const auto versioned = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: irc/6.9",
        http1_close_policy::allow_reuse, request_headers);
    EXPECT_TRUE(versioned.plan().protocol_upgrade() != nullptr);
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: IRC/6.10",
        http1_close_policy::allow_reuse, request_headers));
}

TEST(Ruvia, http_client_connection_fields_use_recipient_list_semantics) {
    const auto reusable = parse_head("GET",
        "HTTP/1.0 200 OK\r\n"
        "Connection: , keep-alive,\r\nContent-Length: 0");
    EXPECT_TRUE(require_known_length(reusable.plan()).persistence() == http1_close_policy::allow_reuse);
    EXPECT_TRUE(reusable.head().protocol_version() == http_protocol_version::http10);

    EXPECT_TRUE(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nConnection: close;invalid\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_connection);
    EXPECT_TRUE(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nUpgrade: websocket/\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_upgrade);
    const auto advertised = parse_head("GET",
        "HTTP/1.1 200 OK\r\nUpgrade: websocket\r\nContent-Length: 0");
    EXPECT_EQ(require_known_length(advertised.plan()).content_length(), std::size_t{0});
    EXPECT_TRUE(require_known_length(advertised.plan()).persistence() == http1_close_policy::allow_reuse);

    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto upgraded = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , websocket,",
        http1_close_policy::allow_reuse, offered);
    EXPECT_TRUE(upgraded.plan().protocol_upgrade() != nullptr);
}

TEST(Ruvia, http_client_rejects_only_protected_connection_options) {
    // Only framing fields are rejected on received responses (RFC 9110 §7.6.1).
    for (const std::string_view option : {"Content-Length", "Transfer-Encoding"}) {
        std::string response = "HTTP/1.1 200 OK\r\nConnection: ";
        response.append(option);
        response.append("\r\nContent-Length: 0");
        EXPECT_TRUE(parse_failure_error("GET", response) ==
                    http1_client_response_parse_error::invalid_connection);
    }

    // RFC 9110 section 7.6.1 forbids sending end-to-end field names in
    // Connection, but recipients accept the message without 400-style rejection.
    for (const std::string_view option :
        {"Set-Cookie", "X-Hop", "DATE", "Range", "Cache-Control", "Host", "Authorization", "Origin", "Cookie"}) {
        std::string response = "HTTP/1.1 200 OK\r\nConnection: ";
        response.append(option);
        response.append("\r\nSet-Cookie: sid=1\r\nX-Hop: local\r\nContent-Length: 0");
        const auto accepted = parse_head("GET", response);
        EXPECT_TRUE(accepted.plan().known_length() != nullptr);
    }
}

TEST(Ruvia, http_client_switching_protocols_requires_wire_agreement) {
    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const ruvia::http_header_view closing_offer[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "websocket"},
    };
    constexpr std::string_view valid_response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket";

    EXPECT_TRUE(parse_fails("GET", valid_response));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, offered));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade",
        http1_close_policy::allow_reuse, offered));
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: IRC",
        http1_close_policy::allow_reuse, offered));
    for (const std::string_view framing : {
             std::string_view("Content-Length: 0\r\n"),
             std::string_view("Content-Length: 5\r\n"),
             std::string_view("Transfer-Encoding: chunked\r\n"),
         }) {
        std::string head(valid_response);
        head.append("\r\n");
        head.append(framing);
        head.append("\r\n");
        const std::string wire = head + "protocol_bytes";
        const auto switched = parse_wire("GET", wire, http1_close_policy::allow_reuse, offered);
        EXPECT_TRUE(switched.parsed() != nullptr);
        if (const auto* parsed = switched.parsed()) {
            EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
        }
    }
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.0 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, offered));
    EXPECT_TRUE(parse_fails("GET", valid_response, http1_close_policy::allow_reuse, closing_offer));
}

TEST(Ruvia, http_client_http10_informational_transfer_encoding_latches_faulty_framing_close) {
    ruvia::http_client_request_view post_request;
    post_request.method_ = "POST";
    post_request.target_ = "/upload";
    post_request.content_ = ruvia::http_client_request_content_view::bytes("hello");
    http1_client_response_parser post_parser = exchange_parser(
        ruvia::http_origin_view::http({.host_ = "example.test"}), post_request,
        {.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto interim = post_parser.parse(
        "HTTP/1.0 100 Continue\r\n"
        "Transfer-Encoding: chunked\r\n\r\n");
    EXPECT_TRUE(interim.parsed() != nullptr);
    EXPECT_TRUE(post_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto final_res = post_parser.parse(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_res.parsed() != nullptr);
    if (final_res.parsed() != nullptr) {
        EXPECT_TRUE(require_known_length(final_res.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }

    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::http_client_request_view ws_request;
    ws_request.method_ = "GET";
    ws_request.target_ = "/ws";
    ws_request.headers_ = offered;
    http1_client_response_parser ws_parser =
        exchange_parser(ruvia::http_origin_view::http({.host_ = "example.test"}), ws_request);
    const auto hints = ws_parser.parse(
        "HTTP/1.0 103 Early Hints\r\n"
        "Transfer-Encoding: chunked\r\n\r\n");
    EXPECT_TRUE(hints.parsed() != nullptr);
    const auto upgrade_res = ws_parser.parse(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n\r\n");
    EXPECT_TRUE(upgrade_res.failure() != nullptr);
    if (upgrade_res.failure() != nullptr) {
        EXPECT_TRUE(*upgrade_res.failure() ==
                    http1_client_response_parse_error::invalid_protocol_switch);
    }
}

TEST(Ruvia, http_client_response_plan_owns_version_and_connection_persistence) {
    const auto http10 = parse_head("GET", "HTTP/1.0 200 OK\r\nContent-Length: 3");
    const auto& http10_body = require_known_length(http10.plan());
    EXPECT_TRUE(http10_body.persistence() == http1_close_policy::close_after_response);

    const auto http10_keep_alive =
        parse_head("GET", "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 3");
    EXPECT_TRUE(require_known_length(http10_keep_alive.plan()).persistence() == http1_close_policy::allow_reuse);

    const auto response_close =
        parse_head("GET", "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3");
    EXPECT_TRUE(require_known_length(response_close.plan()).persistence() ==
                http1_close_policy::close_after_response);

    const auto request_close = parse_head(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 3", http1_close_policy::close_after_response);
    EXPECT_TRUE(require_known_length(request_close.plan()).persistence() ==
                http1_close_policy::close_after_response);
}

TEST(Ruvia, http_client_successful_connect_transitions_to_tunnel) {
    const auto tunnel = parse_head("CONNECT",
        "HTTP/1.1 200 Connection Established\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: chunked;invalid=parameter");
    EXPECT_TRUE(tunnel.plan().connect_tunnel() != nullptr);

    const auto rejected =
        parse_head("CONNECT", "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 3");
    EXPECT_TRUE(rejected.plan().known_length() != nullptr);

    // Methods are case-sensitive. A custom lowercase token is not CONNECT.
    const auto lowercase = parse_head("connect", "HTTP/1.1 200 OK");
    EXPECT_TRUE(lowercase.plan().close_delimited() != nullptr);
}

TEST(Ruvia, http_client_rejected_connect_never_reuses_the_proxy_connection) {
    for (const std::string_view response : {
             "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0",
             "HTTP/1.1 403 Forbidden\r\nContent-Length: 3",
             "HTTP/1.0 407 Proxy Authentication Required\r\nConnection: keep-alive\r\nContent-Length: 0",
         }) {
        const auto rejected = parse_head("CONNECT", response);
        EXPECT_TRUE(require_known_length(rejected.plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }

    const auto chunked = parse_head("CONNECT",
        "HTTP/1.1 502 Bad Gateway\r\nTransfer-Encoding: chunked");
    EXPECT_TRUE(require_chunked(chunked.plan()).persistence() ==
                http1_close_policy::close_after_response);
    const auto without_content = parse_head("CONNECT", "HTTP/1.1 304 Not Modified");
    EXPECT_TRUE(require_without_content(without_content.plan()).persistence() ==
                http1_close_policy::close_after_response);

    // Persistence belongs to the rejected CONNECT, not to these otherwise
    // reusable response fields or the normal non-CONNECT framing alternative.
    const auto ordinary = parse_head("GET",
        "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0");
    EXPECT_TRUE(require_known_length(ordinary.plan()).persistence() ==
                http1_close_policy::allow_reuse);
    for (const std::string_view response : {
             "HTTP/1.1 200 Connection Established",
             "HTTP/1.1 205 Reset Content",
             "HTTP/1.1 299 Tunnel Established",
         }) {
        const auto tunnel = parse_head("CONNECT", response);
        EXPECT_TRUE(tunnel.plan().connect_tunnel() != nullptr);
    }
}

TEST(Ruvia, http1_client_exchange_state_owns_every_long_lived_response_fact) {
    std::optional<ruvia::http1_client_exchange_state> head_exchange;
    std::array<char, 512> head_wire{};
    std::string method = "HEAD";
    {
        ruvia::http_client_request_view request{.method_ = method, .target_ = "/resource"};
        auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, head_wire);
        auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
        if (prepared_req == nullptr) {
            throw std::runtime_error("HEAD request preparation failed");
        }
        head_exchange.emplace(std::move(*prepared_req).take_exchange_state());
    }
    method.assign("GET");
    ruvia::http1_client_response_parser head_parser(std::move(*head_exchange));
    auto head_result = head_parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n");
    if (head_result.parsed() == nullptr) {
        throw std::runtime_error("HEAD response parsing failed");
    }
    EXPECT_TRUE(head_result.parsed()->plan().without_content() != nullptr);

    std::optional<ruvia::http1_client_exchange_state> upgrade_exchange;
    std::array<char, 512> upgrade_wire{};
    std::string connection = "Upgrade";
    std::string protocol = "websocket";
    {
        const std::array headers{
            ruvia::http_header_view{"Connection", connection},
            ruvia::http_header_view{"Upgrade", protocol},
        };
        ruvia::http_client_request_view request{
            .method_ = "GET", .target_ = "/socket", .headers_ = headers};
        auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, upgrade_wire);
        auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
        if (prepared_req == nullptr) {
            throw std::runtime_error("Upgrade request preparation failed");
        }
        upgrade_exchange.emplace(std::move(*prepared_req).take_exchange_state());
    }
    connection.assign("close");
    protocol.assign("not-websocket");
    ruvia::http1_client_response_parser upgrade_parser(std::move(*upgrade_exchange));
    auto upgrade_result = upgrade_parser.parse(
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    if (upgrade_result.parsed() == nullptr) {
        throw std::runtime_error("Upgrade response parsing failed");
    }
    EXPECT_TRUE(upgrade_result.parsed()->plan().protocol_upgrade() != nullptr);
}

using namespace ruvia::test;

// HTTP/1 client responses: Content-Encoding and decoding the body.

TEST(Ruvia, http_client_content_encoding_has_one_authoritative_path) {
    using ruvia::http_content_coding;

    struct case_value final {
        std::string_view headers_;
        std::vector<http_content_coding> expected_;
        bool unsupported_{false};
    };
    const case_value cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: GZIP\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 0",
            {http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: zstd\r\nContent-Length: 0",
            {http_content_coding::zstd}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 0",
            {http_content_coding::deflate}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0", {}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity, identity, gzip, identity\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br\r\nContent-Length: 0",
            {http_content_coding::gzip, http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
         "Content-Encoding: br\r\nContent-Length: 0",
            {http_content_coding::gzip, http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: compress\r\nContent-Length: 0", {}, true},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0", {}},
    };

    for (const auto& test : cases) {
        auto parsed_value = parse_response("GET", test.headers_);
        EXPECT_EQ(parsed_value.head_.status(), ruvia::http_status::ok);
        const auto coding = ruvia::parse_http_content_coding_headers(parsed_value.head_.headers());
        EXPECT_TRUE(coding.invalid() == nullptr);
        EXPECT_TRUE((coding.unsupported() != nullptr) == test.unsupported_);
        EXPECT_EQ(coding.codings().size(), test.expected_.size());
        for (std::size_t i = 0; i < test.expected_.size(); ++i) {
            EXPECT_TRUE(coding.codings()[i] == test.expected_[i]);
        }
    }
}

TEST(Ruvia, http_client_content_encoding_ignores_empty_list_members) {
    const auto tolerant = parse_response("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Encoding: , gzip,,\r\n"
        "Content-Length: 0");
    const auto coding = ruvia::parse_http_content_coding_headers(tolerant.head_.headers());
    EXPECT_TRUE(coding.unsupported() == nullptr);
    EXPECT_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        EXPECT_TRUE(coding.codings().front() == ruvia::http_content_coding::gzip);
    }
}

TEST(Ruvia, http_client_coding_metadata_preserves_framing_without_decoder_fallback) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate", "future-coding"}) {
        std::string wire = "HTTP/1.1 200 OK\r\nContent-Encoding: ";
        wire.append(value);
        wire.append("\r\nContent-Encoding: identity\r\nContent-Length: 3\r\n\r\nabcNEXT");
        const auto result = parse_wire("GET", wire);
        const auto* parsed = result.parsed();
        EXPECT_TRUE(parsed != nullptr);
        if (parsed == nullptr) {
            continue;
        }
        EXPECT_EQ(parsed->head().headers().front().value(), value);
        const auto length = require_known_length(parsed->plan()).content_length();
        EXPECT_EQ(length, std::size_t{3});
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes(), length), "abc");
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes() + length), "NEXT");
        const auto coding = ruvia::parse_http_content_coding_headers(parsed->head().headers());
        const bool unsupported = value == "future-coding";
        EXPECT_TRUE((coding.unsupported() != nullptr) == unsupported);
        EXPECT_TRUE((coding.invalid() != nullptr) == !unsupported);
    }
}

using namespace ruvia::test;

// HTTP/1 client responses: reading a response head off the wire.

namespace {

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

void check_repeated_response_fields(const http_client_response_head& head, std::size_t expected_count) {
    const auto fields = head.headers();
    EXPECT_EQ(fields.size(), expected_count);
    if (fields.size() < repeated_field_count) {
        return;
    }
    for (std::size_t index = 0; index < repeated_field_count; ++index) {
        EXPECT_EQ(fields[index].name(), std::string_view("X-Repeat"));
        EXPECT_EQ(fields[index].value(), std::string("value-") + std::to_string(index).c_str());
    }
}

}  // namespace

TEST(Ruvia, http_client_upgrade_after_many_response_fields_validates_offered_protocol) {
    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto wire = repeated_response_wire("HTTP/1.1 101 Switching Protocols",
        "Connection: Upgrade\r\nUpgrade: irc/6.9\r\n");
    const auto accepted = parse_wire("GET", wire, http1_close_policy::allow_reuse, offered);
    EXPECT_TRUE(accepted.parsed() != nullptr);
    if (const auto* parsed = accepted.parsed()) {
        EXPECT_TRUE(parsed->head().status() == ruvia::http_status::switching_protocols);
        EXPECT_EQ(parsed->consumed_bytes(), wire.size());
        EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
        EXPECT_EQ(active_plan_alternative_count(parsed->plan()), std::size_t{1});
        check_repeated_response_fields(parsed->head(), repeated_field_count + 2);
        const auto fields = parsed->head().headers();
        if (fields.size() == repeated_field_count + 2) {
            EXPECT_EQ(fields[repeated_field_count].name(), std::string_view("Connection"));
            EXPECT_EQ(fields[repeated_field_count].value(), std::string_view("Upgrade"));
            EXPECT_EQ(fields.back().name(), std::string_view("Upgrade"));
            EXPECT_EQ(fields.back().value(), std::string_view("irc/6.9"));
        }
    }
    const auto unoffered_wire = repeated_response_wire("HTTP/1.1 101 Switching Protocols",
        "Connection: Upgrade\r\nUpgrade: IRC/6.10\r\n");
    const auto unoffered = parse_wire("GET", unoffered_wire, http1_close_policy::allow_reuse, offered);
    EXPECT_TRUE(unoffered.failure() != nullptr);
    if (unoffered.failure() != nullptr) {
        EXPECT_TRUE(*unoffered.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
    const auto unsolicited = parse_wire("GET", wire);
    EXPECT_TRUE(unsolicited.failure() != nullptr);
    if (unsolicited.failure() != nullptr) {
        EXPECT_TRUE(*unsolicited.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
}
