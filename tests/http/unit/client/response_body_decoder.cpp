#include <array>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_client_response_body_decoder.h"

#include "content_decoding_fixture.h"
#include "http_client_response_fixture.h"

using http_client_response_test::parse_head;

namespace {
class allocation_counter final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t outstanding() const noexcept {
        return outstanding_;
    }
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        ++outstanding_;
        return std::pmr::get_default_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        --outstanding_;
        std::pmr::get_default_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
    std::size_t outstanding_{0};
};
}  // namespace

RUVIA_TEST(http1_response_body_decoder_fixed_length_preserves_tail_and_eof) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 8> scratch{};
    auto first = decoder.decode("ab", scratch);
    RUVIA_CHECK(first.output() != nullptr);
    RUVIA_CHECK_EQ(first.consumed_bytes(), std::size_t{2});
    RUVIA_CHECK_EQ(first.output()->bytes(), "ab");
    auto second = decoder.finish_input("cNEXT", scratch);
    RUVIA_CHECK(second.output() != nullptr);
    RUVIA_CHECK_EQ(second.consumed_bytes(), std::size_t{1});
    auto complete_value = decoder.finish_input("NEXT", scratch);
    RUVIA_CHECK(complete_value.complete() != nullptr);
    RUVIA_CHECK_EQ(complete_value.consumed_bytes(), std::size_t{0});
}

RUVIA_TEST(http1_response_body_decoder_head_and_no_content_complete_without_consuming_tail) {
    for (const auto& [method, status] : {std::pair{"HEAD", "200 OK"},
             std::pair{"GET", "204 No Content"}, std::pair{"GET", "304 Not Modified"}}) {
        const auto head = parse_head(method, std::string("HTTP/1.1 ") + status);
        ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
        std::array<char, 4> scratch{};
        const auto result_value = decoder.decode("TAIL", scratch);
        RUVIA_CHECK(result_value.complete() != nullptr);
        RUVIA_CHECK_EQ(result_value.consumed_bytes(), std::size_t{0});
    }
}

RUVIA_TEST(http1_response_body_decoder_chunked_trailers_and_tail) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 8> scratch{};
    auto body = decoder.decode("3\r\nabc\r\n0\r\nX-Test: yes\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(body.output() != nullptr);
    RUVIA_CHECK_EQ(body.output()->bytes(), "abc");
    auto trailer = decoder.decode("0\r\nX-Test: yes\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(trailer.trailers() != nullptr);
    RUVIA_CHECK_EQ(trailer.trailers()->bytes(), "X-Test: yes");
    RUVIA_CHECK_EQ(trailer.consumed_bytes(), std::size_t{18});
    auto complete_value = decoder.decode("TAIL", scratch);
    RUVIA_CHECK(complete_value.complete() != nullptr);
    RUVIA_CHECK_EQ(complete_value.consumed_bytes(), std::size_t{0});
}

RUVIA_TEST(http1_response_body_decoder_close_delimited_completes_only_at_eof) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 3> scratch{};
    auto body = decoder.decode("abcdef", scratch);
    RUVIA_CHECK(body.output() != nullptr);
    RUVIA_CHECK_EQ(body.consumed_bytes(), std::size_t{3});
    RUVIA_CHECK_EQ(body.output()->bytes(), "abc");
    auto rest = decoder.finish_input("def", scratch);
    RUVIA_CHECK(rest.output() != nullptr);
    RUVIA_CHECK_EQ(rest.consumed_bytes(), std::size_t{3});
    auto complete_value = decoder.finish_input({}, scratch);
    RUVIA_CHECK(complete_value.complete() != nullptr);
}

RUVIA_TEST(http1_response_body_decoder_transfer_gzip_and_deflate_stream_with_tiny_scratch) {
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    for (const auto& [coding, bytes] : {std::pair{"gzip", gzip_hello},
             std::pair{"deflate", deflate_hello}}) {
        const auto head = parse_head("GET", std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: ") + coding);
        ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
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
        RUVIA_CHECK_EQ(output, "hello");
        RUVIA_CHECK(result_value.complete() != nullptr);
    }
}

RUVIA_TEST(http1_response_body_decoder_transfer_allocations_are_released_on_repeated_operations) {
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    allocation_counter resource;
    std::array<char, 8> scratch{};
    for (int index = 0; index < 4; ++index) {
        {
            ruvia::http1_client_response_body_decoder decoder(head.plan(), &resource);
            auto result_value = decoder.finish_input(gzip_hello, scratch);
            RUVIA_CHECK(result_value.output() != nullptr);
            result_value = decoder.finish_input({}, scratch);
            RUVIA_CHECK(result_value.complete() != nullptr);
        }
        RUVIA_CHECK_EQ(resource.outstanding(), std::size_t{0});
    }
    RUVIA_CHECK(resource.allocations() > 0);
}

RUVIA_TEST(http1_response_body_decoder_gzip_concatenation_and_chunked_transfer) {
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    std::string concatenated(gzip_hello);
    concatenated.append(gzip_hello);
    const auto close_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder close_decoder(
        close_head.plan(), std::pmr::get_default_resource());
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
    RUVIA_CHECK_EQ(decoded, "hellohello");
    RUVIA_CHECK(result_value.complete() != nullptr);

    const auto chunked_head =
        parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    ruvia::http1_client_response_body_decoder chunked_decoder(
        chunked_head.plan(), std::pmr::get_default_resource());
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
            RUVIA_CHECK(result_value.need_input() != nullptr);
        }
    }
    RUVIA_CHECK_EQ(decoded, "hello");
    RUVIA_CHECK(trailers_seen);
    RUVIA_CHECK_EQ(wire, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_transfer_completion_waits_for_http_framing_and_rejects_tail) {
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 16> scratch{};
    const auto output = decoder.decode(deflate_hello, scratch);
    RUVIA_CHECK(output.output() != nullptr);
    RUVIA_CHECK_EQ(output.output()->bytes(), "hello");
    const auto still_waiting = decoder.decode({}, scratch);
    RUVIA_CHECK(still_waiting.need_input() != nullptr);
    const auto illegal_tail = decoder.decode("X", scratch);
    RUVIA_CHECK(illegal_tail.protocol_failure() != nullptr);
    RUVIA_CHECK(illegal_tail.protocol_failure()->error() ==
                ruvia::http1_client_response_body_error::invalid_transfer_coding);
    RUVIA_CHECK_EQ(illegal_tail.consumed_bytes(), std::size_t{0});

    const auto eof_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
    ruvia::http1_client_response_body_decoder eof_decoder(
        eof_head.plan(), std::pmr::get_default_resource());
    auto eof_result = eof_decoder.finish_input(deflate_hello, scratch);
    while (eof_result.output() || eof_result.need_input()) {
        eof_result = eof_decoder.finish_input({}, scratch);
    }
    RUVIA_CHECK(eof_result.complete() != nullptr);
}

RUVIA_TEST(http1_response_body_decoder_chunked_deflate_waits_for_valid_trailers) {
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate, chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
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
    RUVIA_CHECK(result_value.complete() != nullptr);
    RUVIA_CHECK_EQ(body, "hello");
    RUVIA_CHECK(saw_trailers);
    RUVIA_CHECK_EQ(input, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_terminal_result_is_stable_and_replayed) {
    const auto head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 4> scratch{};
    const auto failed = decoder.decode("1\r\nx\r\n0\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(failed.protocol_failure() != nullptr);
    RUVIA_CHECK(failed.protocol_failure()->error() == ruvia::http1_client_response_body_error::non_empty205);
    const auto replayed = decoder.decode("ignored", scratch);
    RUVIA_CHECK(replayed.protocol_failure() != nullptr);
    RUVIA_CHECK(replayed.protocol_failure()->error() == failed.protocol_failure()->error());
    RUVIA_CHECK_EQ(replayed.consumed_bytes(), std::size_t{0});
    const auto eof_replay = decoder.finish_input({}, scratch);
    RUVIA_CHECK(eof_replay.protocol_failure() != nullptr);
    RUVIA_CHECK(eof_replay.protocol_failure()->error() == failed.protocol_failure()->error());

    const auto no_body = parse_head("GET", "HTTP/1.1 204 No Content");
    ruvia::http1_client_response_body_decoder complete_decoder(
        no_body.plan(), std::pmr::get_default_resource());
    const auto first_complete = complete_decoder.decode("TAIL", scratch);
    RUVIA_CHECK(first_complete.complete() != nullptr);
    const auto complete_replay = complete_decoder.decode("ignored", scratch);
    RUVIA_CHECK(complete_replay.complete() != nullptr);
    RUVIA_CHECK_EQ(complete_replay.consumed_bytes(), std::size_t{0});
    const auto complete_eof_replay = complete_decoder.finish_input({}, scratch);
    RUVIA_CHECK(complete_eof_replay.complete() != nullptr);
    RUVIA_CHECK_EQ(complete_eof_replay.consumed_bytes(), std::size_t{0});
}

RUVIA_TEST(http1_response_body_decoder_205_gzip_chunked_eof_drains_buffered_terminator) {
    constexpr std::string_view gzip_empty{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x03"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        20};
    const auto head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip, chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
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
    RUVIA_CHECK(result_value.complete() != nullptr);
    RUVIA_CHECK(trailers_seen);
    RUVIA_CHECK_EQ(input, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_205_accepts_empty_gzip_and_rejects_decoded_content) {
    constexpr std::string_view gzip_empty{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x03"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        20};
    const auto empty_head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder empty(empty_head.plan(), std::pmr::get_default_resource());
    std::array<char, 4> scratch{};
    auto accepted = empty.finish_input(gzip_empty, scratch);
    while (accepted.need_input() != nullptr) {
        accepted = empty.finish_input({}, scratch);
    }
    RUVIA_CHECK(accepted.protocol_failure() == nullptr);
    RUVIA_CHECK(accepted.decoder_failure() == nullptr);
    RUVIA_CHECK(accepted.complete() != nullptr);

    const auto nonempty_head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder nonempty(
        nonempty_head.plan(), std::pmr::get_default_resource());
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    const auto rejected = nonempty.finish_input(gzip_hello, scratch);
    RUVIA_CHECK(rejected.protocol_failure() != nullptr);
    RUVIA_CHECK(rejected.protocol_failure()->error() == ruvia::http1_client_response_body_error::non_empty205);
}

RUVIA_TEST(http1_response_body_decoder_chunk_output_is_limited_to_scratch_and_keeps_tail) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::null_memory_resource());
    std::array<char, 3> scratch{};
    std::string wire = "a\r\n0123456789\r\n0\r\n\r\nTAIL";
    std::string body;
    for (;;) {
        const auto result_value = decoder.decode(wire, scratch);
        RUVIA_CHECK(result_value.protocol_failure() == nullptr);
        if (result_value.output()) {
            body.append(result_value.output()->bytes());
        }
        wire.erase(0, result_value.consumed_bytes());
        if (result_value.trailers() != nullptr) {
            break;
        }
        RUVIA_CHECK(result_value.output() != nullptr);
    }
    RUVIA_CHECK_EQ(body, "0123456789");
    RUVIA_CHECK_EQ(wire, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_no_transfer_coding_does_not_allocate) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 4");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::null_memory_resource());
    std::array<char, 2> scratch{};
    auto first = decoder.decode("data", scratch);
    RUVIA_CHECK_EQ(first.output()->bytes(), "da");
    auto second = decoder.decode("ta", scratch);
    RUVIA_CHECK_EQ(second.output()->bytes(), "ta");
    const auto complete_value = decoder.decode({}, scratch);
    RUVIA_CHECK(complete_value.complete() != nullptr);
}

RUVIA_TEST(http1_response_body_decoder_rejects_truncated_transfer_coding) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    constexpr std::string_view truncated_gzip{"\x1f\x8b\x08", 3};
    std::array<char, 8> scratch{};
    auto result_value = decoder.finish_input(truncated_gzip, scratch);
    while (result_value.output() != nullptr ||
           result_value.need_input() != nullptr) {
        result_value = decoder.finish_input({}, scratch);
    }
    RUVIA_CHECK(result_value.protocol_failure() != nullptr);
    RUVIA_CHECK(result_value.protocol_failure()->error() == ruvia::http1_client_response_body_error::incomplete_body);
}

RUVIA_TEST(http1_response_body_decoder_handles_chunk_extension_and_trailer_over_8k) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
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
        RUVIA_CHECK(result_value.protocol_failure() == nullptr);
        if (result_value.output()) {
            saw_body = result_value.output()->bytes() == "z";
        }
        if (result_value.trailers()) {
            saw_trailers = result_value.trailers()->bytes().size() > 9000;
        }
        wire.erase(0, result_value.consumed_bytes());
        if (saw_trailers) {
            break;
        }
        RUVIA_CHECK(result_value.output() != nullptr || result_value.need_input() != nullptr);
    }
    RUVIA_CHECK(saw_body);
    RUVIA_CHECK(saw_trailers);
    RUVIA_CHECK_EQ(wire, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_fragmented_chunk_and_205_empty_chunk_complete) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 4> scratch{};
    auto first = decoder.decode("3\r\nab", scratch);
    RUVIA_CHECK(first.output() != nullptr);
    RUVIA_CHECK_EQ(first.output()->bytes(), "ab");
    RUVIA_CHECK_EQ(first.consumed_bytes(), std::size_t{5});
    auto second = decoder.decode("c\r\n0\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(second.output() != nullptr);
    RUVIA_CHECK_EQ(second.output()->bytes(), "c");
    auto trailers = decoder.decode("0\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(trailers.trailers() != nullptr);
    RUVIA_CHECK_EQ(trailers.consumed_bytes(), std::size_t{5});
    RUVIA_CHECK_EQ(trailers.trailers()->bytes(), "");
    const auto complete_value = decoder.decode("TAIL", scratch);
    RUVIA_CHECK(complete_value.complete() != nullptr);

    const auto reset_head = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder reset(
        reset_head.plan(), std::pmr::get_default_resource());
    auto empty_chunk = reset.decode("0\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(empty_chunk.trailers() != nullptr);
    RUVIA_CHECK(empty_chunk.trailers()->bytes().empty());
    const auto reset_complete = reset.decode("TAIL", scratch);
    RUVIA_CHECK(reset_complete.complete() != nullptr);
}

RUVIA_TEST(http1_response_body_decoder_fixed_length_eof_is_incomplete) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 4> scratch{};
    const auto body = decoder.finish_input("ab", scratch);
    RUVIA_CHECK(body.output() != nullptr);
    const auto failure = decoder.finish_input({}, scratch);
    RUVIA_CHECK(failure.protocol_failure() != nullptr);
    RUVIA_CHECK(failure.protocol_failure()->error() == ruvia::http1_client_response_body_error::incomplete_body);
}

RUVIA_TEST(http1_response_body_decoder_decodes_transfer_coding_stack_in_reverse_order) {
    constexpr std::string_view plain = "HTTP/1 transfer coding stack";
    const auto wire = zlib_deflate_compress(gzip_compress(plain));
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
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
            RUVIA_CHECK(false);
            return;
        }
        complete_value = result_value.complete() != nullptr;
    }
    RUVIA_CHECK(complete_value);
    RUVIA_CHECK(input.empty());
    RUVIA_CHECK_EQ(output, std::string(plain));
}

RUVIA_TEST(http1_response_body_decoder_drains_pending_transfer_output_without_wire_input) {
    constexpr std::string_view deflate_aaaa{"\x78\x9c\x73\x04\x02\x00\x02\x8e\x01\x05", 10};
    for (const bool chunked : {false, true}) {
        const auto head = parse_head("GET", chunked
                                                ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate, chunked"
                                                : "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
        ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
        std::array<char, 1> scratch{};
        std::string input(deflate_aaaa.substr(0, 5));
        if (chunked) {
            input = "5\r\n" + input + "\r\n";
        }
        std::string content;
        const auto first = decoder.decode(input, scratch);
        RUVIA_CHECK(first.output() != nullptr);
        if (first.output()) {
            content.append(first.output()->bytes());
        }
        input.erase(0, first.consumed_bytes());
        RUVIA_CHECK(input.empty());
        for (int count = 0; count < 3; ++count) {
            const auto pending = decoder.decode(input, scratch);
            RUVIA_CHECK(pending.output() != nullptr);
            RUVIA_CHECK_EQ(pending.consumed_bytes(), std::size_t{0});
            if (!pending.output()) {
                break;
            }
            content.append(pending.output()->bytes());
        }
        // All four bytes must be observable before the peer supplies the rest
        // of the encoding or any subsequent chunk framing.
        RUVIA_CHECK_EQ(content, "AAAA");
        const auto needs_input = decoder.decode(input, scratch);
        RUVIA_CHECK(needs_input.need_input() != nullptr);

        input.assign(deflate_aaaa.substr(5));
        if (chunked) {
            input = "5\r\n" + input + "\r\n0\r\n\r\nTAIL";
        }
        bool complete_value = false;
        for (int count = 0; count < 16 && !complete_value; ++count) {
            const auto result_value = decoder.finish_input(input, scratch);
            RUVIA_CHECK(result_value.protocol_failure() == nullptr);
            RUVIA_CHECK(result_value.decoder_failure() == nullptr);
            RUVIA_CHECK(result_value.need_input() == nullptr);
            if (result_value.output()) {
                content.append(result_value.output()->bytes());
            }
            input.erase(0, result_value.consumed_bytes());
            complete_value = result_value.complete() != nullptr;
            if (result_value.protocol_failure() || result_value.decoder_failure()) {
                break;
            }
        }
        RUVIA_CHECK(complete_value);
        RUVIA_CHECK_EQ(content, "AAAA");
        RUVIA_CHECK_EQ(input, chunked ? "TAIL" : "");
    }
}

RUVIA_TEST(http1_response_chunk_adapter_preserves_typed_views_role_and_wire_prefix) {
    constexpr std::string_view wire = "1\r\nx\r\n0\r\nAccept-Ranges: bytes\r\n\r\nNEXT";
    ruvia::http_response_chunked_body_decoder decoder(ruvia::protocol_byte_limit::unlimited());
    bool zero_budget_rejected = false;
    try {
        (void)decoder.decode(wire, 0);
    } catch (const std::invalid_argument&) {
        zero_budget_rejected = true;
    }
    RUVIA_CHECK(zero_budget_rejected);
    const auto body = decoder.decode(wire, 1);
    RUVIA_CHECK(body.body_chunk() != nullptr);
    RUVIA_CHECK_EQ(body.consumed_bytes(), std::size_t{6});
    if (const auto* chunk = body.body_chunk()) {
        RUVIA_CHECK_EQ(chunk->bytes(), "x");
        RUVIA_CHECK(chunk->bytes().data() == wire.data() + 3);
    }
    auto pending = wire.substr(body.consumed_bytes());
    const auto terminal = decoder.decode(pending, 1);
    RUVIA_CHECK(terminal.complete() != nullptr);
    if (const auto* complete = terminal.complete()) {
        RUVIA_CHECK_EQ(complete->trailers(), "Accept-Ranges: bytes");
    }
    pending.remove_prefix(terminal.consumed_bytes());
    RUVIA_CHECK_EQ(pending, "NEXT");
    const auto replay = decoder.decode(pending);
    RUVIA_CHECK(replay.complete() != nullptr);
    RUVIA_CHECK_EQ(replay.consumed_bytes(), std::size_t{0});

    ruvia::http_response_chunked_body_decoder invalid(ruvia::protocol_byte_limit::unlimited());
    const auto failure = invalid.decode("1\r\nxXX");
    RUVIA_CHECK(failure.failure() != nullptr);
    if (const auto* error = failure.failure()) {
        RUVIA_CHECK(error->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
    const auto repeated_failure = invalid.decode("NEXT", 1);
    RUVIA_CHECK(repeated_failure.failure() != nullptr);
    RUVIA_CHECK_EQ(repeated_failure.consumed_bytes(), std::size_t{0});
    if (const auto* error = repeated_failure.failure()) {
        RUVIA_CHECK(error->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
}

RUVIA_TEST(http1_response_body_decoder_requires_nonempty_scratch) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    ruvia::http1_client_response_body_decoder decoder(head.plan(), std::pmr::get_default_resource());
    bool rejected = false;
    try {
        (void)decoder.decode({}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
