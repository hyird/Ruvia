#include <array>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/Http1ClientResponseBodyDecoder.h"

#include "http_client_response_fixture.h"

using http_client_response_test::parseHead;

namespace {
class AllocationCounter final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t outstanding() const noexcept {
        return outstanding_;
    }
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations_;
        ++outstanding_;
        return std::pmr::get_default_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        --outstanding_;
        std::pmr::get_default_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
    std::size_t outstanding_{0};
};
}  // namespace

RUVIA_TEST(http1_response_body_decoder_fixed_length_preserves_tail_and_eof) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 8> scratch{};
    auto first = decoder.decode("ab", scratch);
    RUVIA_CHECK(first.output() != nullptr);
    RUVIA_CHECK_EQ(first.consumedBytes(), std::size_t{2});
    RUVIA_CHECK_EQ(first.output()->bytes(), "ab");
    auto second = decoder.finishInput("cNEXT", scratch);
    RUVIA_CHECK(second.output() != nullptr);
    RUVIA_CHECK_EQ(second.consumedBytes(), std::size_t{1});
    auto complete = decoder.finishInput("NEXT", scratch);
    RUVIA_CHECK(complete.complete() != nullptr);
    RUVIA_CHECK_EQ(complete.consumedBytes(), std::size_t{0});
}

RUVIA_TEST(http1_response_body_decoder_head_and_no_content_complete_without_consuming_tail) {
    for (const auto& [method, status] : {std::pair{"HEAD", "200 OK"},
             std::pair{"GET", "204 No Content"}, std::pair{"GET", "304 Not Modified"}}) {
        const auto head = parseHead(method, std::string("HTTP/1.1 ") + status);
        ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
        std::array<char, 4> scratch{};
        const auto result = decoder.decode("TAIL", scratch);
        RUVIA_CHECK(result.complete() != nullptr);
        RUVIA_CHECK_EQ(result.consumedBytes(), std::size_t{0});
    }
}

RUVIA_TEST(http1_response_body_decoder_chunked_trailers_and_tail) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 8> scratch{};
    auto body = decoder.decode("3\r\nabc\r\n0\r\nX-Test: yes\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(body.output() != nullptr);
    RUVIA_CHECK_EQ(body.output()->bytes(), "abc");
    auto trailer = decoder.decode("0\r\nX-Test: yes\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(trailer.trailers() != nullptr);
    RUVIA_CHECK_EQ(trailer.trailers()->bytes(), "X-Test: yes");
    RUVIA_CHECK_EQ(trailer.consumedBytes(), std::size_t{18});
    auto complete = decoder.decode("TAIL", scratch);
    RUVIA_CHECK(complete.complete() != nullptr);
    RUVIA_CHECK_EQ(complete.consumedBytes(), std::size_t{0});
}

RUVIA_TEST(http1_response_body_decoder_close_delimited_completes_only_at_eof) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 3> scratch{};
    auto body = decoder.decode("abcdef", scratch);
    RUVIA_CHECK(body.output() != nullptr);
    RUVIA_CHECK_EQ(body.consumedBytes(), std::size_t{3});
    RUVIA_CHECK_EQ(body.output()->bytes(), "abc");
    auto rest = decoder.finishInput("def", scratch);
    RUVIA_CHECK(rest.output() != nullptr);
    RUVIA_CHECK_EQ(rest.consumedBytes(), std::size_t{3});
    auto complete = decoder.finishInput({}, scratch);
    RUVIA_CHECK(complete.complete() != nullptr);
}

RUVIA_TEST(http1_response_body_decoder_transfer_gzip_and_deflate_stream_with_tiny_scratch) {
    constexpr std::string_view gzipHello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    constexpr std::string_view deflateHello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    for (const auto& [coding, bytes] : {std::pair{"gzip", gzipHello},
             std::pair{"deflate", deflateHello}}) {
        const auto head = parseHead("GET", std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: ") + coding);
        ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
        std::array<char, 1> scratch{};
        std::string output;
        std::string input(bytes);
        auto result = decoder.finishInput(input, scratch);
        for (std::size_t step = 0; step < 100 && !result.complete() &&
                                   !result.protocolFailure() && !result.decoderFailure();
            ++step) {
            input.erase(0, result.consumedBytes());
            if (result.output()) {
                output.append(result.output()->bytes());
            }
            result = decoder.finishInput(input, scratch);
        }
        if (result.output()) {
            output.append(result.output()->bytes());
        }
        RUVIA_CHECK_EQ(output, "hello");
        RUVIA_CHECK(result.complete() != nullptr);
    }
}

RUVIA_TEST(http1_response_body_decoder_transfer_allocations_are_released_on_repeated_operations) {
    constexpr std::string_view gzipHello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    AllocationCounter resource;
    std::array<char, 8> scratch{};
    for (int index = 0; index < 4; ++index) {
        {
            ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), &resource);
            auto result = decoder.finishInput(gzipHello, scratch);
            RUVIA_CHECK(result.output() != nullptr);
            result = decoder.finishInput({}, scratch);
            RUVIA_CHECK(result.complete() != nullptr);
        }
        RUVIA_CHECK_EQ(resource.outstanding(), std::size_t{0});
    }
    RUVIA_CHECK(resource.allocations() > 0);
}

RUVIA_TEST(http1_response_body_decoder_gzip_concatenation_and_chunked_transfer) {
    constexpr std::string_view gzipHello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    std::string concatenated(gzipHello);
    concatenated.append(gzipHello);
    const auto closeHead = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::Http1ClientResponseBodyDecoder closeDecoder(
        closeHead.plan(), std::pmr::get_default_resource());
    std::array<char, 2> scratch{};
    std::string decoded;
    auto result = closeDecoder.finishInput(concatenated, scratch);
    for (std::size_t step = 0; step < 100 && !result.complete() &&
                               !result.protocolFailure() && !result.decoderFailure();
        ++step) {
        concatenated.erase(0, result.consumedBytes());
        if (result.output()) {
            decoded.append(result.output()->bytes());
        }
        result = closeDecoder.finishInput(concatenated, scratch);
    }
    if (result.output()) {
        decoded.append(result.output()->bytes());
    }
    RUVIA_CHECK_EQ(decoded, "hellohello");
    RUVIA_CHECK(result.complete() != nullptr);

    const auto chunkedHead =
        parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    ruvia::Http1ClientResponseBodyDecoder chunkedDecoder(
        chunkedHead.plan(), std::pmr::get_default_resource());
    char size[3]{};
    std::snprintf(size, sizeof(size), "%zx", gzipHello.size());
    std::string wire(size);
    wire.append("\r\n");
    wire.append(gzipHello);
    wire.append("\r\n0\r\n\r\nTAIL");
    decoded.clear();
    bool trailersSeen = false;
    for (;;) {
        result = chunkedDecoder.decode(wire, scratch);
        wire.erase(0, result.consumedBytes());
        if (result.output() != nullptr) {
            decoded.append(result.output()->bytes());
        } else if (result.trailers() != nullptr) {
            trailersSeen = true;
        } else if (result.complete() != nullptr) {
            break;
        } else {
            RUVIA_CHECK(result.needInput() != nullptr);
        }
    }
    RUVIA_CHECK_EQ(decoded, "hello");
    RUVIA_CHECK(trailersSeen);
    RUVIA_CHECK_EQ(wire, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_transfer_completion_waits_for_http_framing_and_rejects_tail) {
    constexpr std::string_view deflateHello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 16> scratch{};
    const auto output = decoder.decode(deflateHello, scratch);
    RUVIA_CHECK(output.output() != nullptr);
    RUVIA_CHECK_EQ(output.output()->bytes(), "hello");
    const auto stillWaiting = decoder.decode({}, scratch);
    RUVIA_CHECK(stillWaiting.needInput() != nullptr);
    const auto illegalTail = decoder.decode("X", scratch);
    RUVIA_CHECK(illegalTail.protocolFailure() != nullptr);
    RUVIA_CHECK(illegalTail.protocolFailure()->error() ==
                ruvia::Http1ClientResponseBodyError::kInvalidTransferCoding);
    RUVIA_CHECK_EQ(illegalTail.consumedBytes(), std::size_t{0});

    const auto eofHead = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
    ruvia::Http1ClientResponseBodyDecoder eofDecoder(
        eofHead.plan(), std::pmr::get_default_resource());
    auto eofResult = eofDecoder.finishInput(deflateHello, scratch);
    while (eofResult.output() || eofResult.needInput()) {
        eofResult = eofDecoder.finishInput({}, scratch);
    }
    RUVIA_CHECK(eofResult.complete() != nullptr);
}

RUVIA_TEST(http1_response_body_decoder_chunked_deflate_waits_for_valid_trailers) {
    constexpr std::string_view deflateHello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate, chunked");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::string input = "d\r\n";
    input.append(deflateHello);
    input.append("\r\n0\r\nX-End: yes\r\n\r\nTAIL");
    std::array<char, 8> scratch{};
    std::string body;
    bool sawTrailers = false;
    auto result = decoder.decode(input, scratch);
    for (int step = 0; step < 8 && !result.complete() && !result.protocolFailure(); ++step) {
        input.erase(0, result.consumedBytes());
        if (result.output()) {
            body.append(result.output()->bytes());
        }
        sawTrailers = sawTrailers || result.trailers() != nullptr;
        result = decoder.decode(input, scratch);
    }
    if (result.output()) {
        body.append(result.output()->bytes());
    }
    RUVIA_CHECK(result.complete() != nullptr);
    RUVIA_CHECK_EQ(body, "hello");
    RUVIA_CHECK(sawTrailers);
    RUVIA_CHECK_EQ(input, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_terminal_result_is_stable_and_replayed) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 4> scratch{};
    const auto failed = decoder.decode("1\r\nx\r\n0\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(failed.protocolFailure() != nullptr);
    RUVIA_CHECK(failed.protocolFailure()->error() == ruvia::Http1ClientResponseBodyError::kNonEmpty205);
    const auto replayed = decoder.decode("ignored", scratch);
    RUVIA_CHECK(replayed.protocolFailure() != nullptr);
    RUVIA_CHECK(replayed.protocolFailure()->error() == failed.protocolFailure()->error());
    RUVIA_CHECK_EQ(replayed.consumedBytes(), std::size_t{0});
    const auto eofReplay = decoder.finishInput({}, scratch);
    RUVIA_CHECK(eofReplay.protocolFailure() != nullptr);
    RUVIA_CHECK(eofReplay.protocolFailure()->error() == failed.protocolFailure()->error());

    const auto noBody = parseHead("GET", "HTTP/1.1 204 No Content");
    ruvia::Http1ClientResponseBodyDecoder completeDecoder(
        noBody.plan(), std::pmr::get_default_resource());
    const auto firstComplete = completeDecoder.decode("TAIL", scratch);
    RUVIA_CHECK(firstComplete.complete() != nullptr);
    const auto completeReplay = completeDecoder.decode("ignored", scratch);
    RUVIA_CHECK(completeReplay.complete() != nullptr);
    RUVIA_CHECK_EQ(completeReplay.consumedBytes(), std::size_t{0});
    const auto completeEofReplay = completeDecoder.finishInput({}, scratch);
    RUVIA_CHECK(completeEofReplay.complete() != nullptr);
    RUVIA_CHECK_EQ(completeEofReplay.consumedBytes(), std::size_t{0});
}

RUVIA_TEST(http1_response_body_decoder_205_gzip_chunked_eof_drains_buffered_terminator) {
    constexpr std::string_view gzipEmpty{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x03"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        20};
    const auto head = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip, chunked");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::string input = "14\r\n";
    input.append(gzipEmpty);
    input.append("\r\n0\r\n\r\nTAIL");
    std::array<char, 8> scratch{};
    bool trailersSeen = false;
    auto result = decoder.finishInput(input, scratch);
    for (std::size_t step = 0; step < 16 && !result.complete() &&
                               !result.protocolFailure() && !result.decoderFailure();
        ++step) {
        input.erase(0, result.consumedBytes());
        trailersSeen = trailersSeen || result.trailers() != nullptr;
        result = decoder.finishInput(input, scratch);
    }
    RUVIA_CHECK(result.complete() != nullptr);
    RUVIA_CHECK(trailersSeen);
    RUVIA_CHECK_EQ(input, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_205_accepts_empty_gzip_and_rejects_decoded_content) {
    constexpr std::string_view gzipEmpty{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x03"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        20};
    const auto emptyHead = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    ruvia::Http1ClientResponseBodyDecoder empty(emptyHead.plan(), std::pmr::get_default_resource());
    std::array<char, 4> scratch{};
    auto accepted = empty.finishInput(gzipEmpty, scratch);
    while (accepted.needInput() != nullptr) {
        accepted = empty.finishInput({}, scratch);
    }
    RUVIA_CHECK(accepted.protocolFailure() == nullptr);
    RUVIA_CHECK(accepted.decoderFailure() == nullptr);
    RUVIA_CHECK(accepted.complete() != nullptr);

    const auto nonemptyHead = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    ruvia::Http1ClientResponseBodyDecoder nonempty(
        nonemptyHead.plan(), std::pmr::get_default_resource());
    constexpr std::string_view gzipHello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    const auto rejected = nonempty.finishInput(gzipHello, scratch);
    RUVIA_CHECK(rejected.protocolFailure() != nullptr);
    RUVIA_CHECK(rejected.protocolFailure()->error() == ruvia::Http1ClientResponseBodyError::kNonEmpty205);
}

RUVIA_TEST(http1_response_body_decoder_chunk_output_is_limited_to_scratch_and_keeps_tail) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::null_memory_resource());
    std::array<char, 3> scratch{};
    std::string wire = "a\r\n0123456789\r\n0\r\n\r\nTAIL";
    std::string body;
    for (;;) {
        const auto result = decoder.decode(wire, scratch);
        RUVIA_CHECK(result.protocolFailure() == nullptr);
        if (result.output()) {
            body.append(result.output()->bytes());
        }
        wire.erase(0, result.consumedBytes());
        if (result.trailers() != nullptr) {
            break;
        }
        RUVIA_CHECK(result.output() != nullptr);
    }
    RUVIA_CHECK_EQ(body, "0123456789");
    RUVIA_CHECK_EQ(wire, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_no_transfer_coding_does_not_allocate) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 4");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::null_memory_resource());
    std::array<char, 2> scratch{};
    auto first = decoder.decode("data", scratch);
    RUVIA_CHECK_EQ(first.output()->bytes(), "da");
    auto second = decoder.decode("ta", scratch);
    RUVIA_CHECK_EQ(second.output()->bytes(), "ta");
    const auto complete = decoder.decode({}, scratch);
    RUVIA_CHECK(complete.complete() != nullptr);
}

RUVIA_TEST(http1_response_body_decoder_rejects_truncated_transfer_coding) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    constexpr std::string_view truncatedGzip{"\x1f\x8b\x08", 3};
    std::array<char, 8> scratch{};
    auto result = decoder.finishInput(truncatedGzip, scratch);
    while (result.output() != nullptr ||
           result.needInput() != nullptr) {
        result = decoder.finishInput({}, scratch);
    }
    RUVIA_CHECK(result.protocolFailure() != nullptr);
    RUVIA_CHECK(result.protocolFailure()->error() == ruvia::Http1ClientResponseBodyError::kIncompleteBody);
}

RUVIA_TEST(http1_response_body_decoder_handles_chunk_extension_and_trailer_over_8k) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::string wire = "1;";
    wire.append(9000, 'a');
    wire.append("\r\nz\r\n0\r\nX-Large: ");
    wire.append(9000, 'b');
    wire.append("\r\n\r\nTAIL");
    std::array<char, 4> scratch{};
    bool sawBody = false;
    bool sawTrailers = false;
    for (int step = 0; step < 8; ++step) {
        const auto result = decoder.decode(wire, scratch);
        RUVIA_CHECK(result.protocolFailure() == nullptr);
        if (result.output()) {
            sawBody = result.output()->bytes() == "z";
        }
        if (result.trailers()) {
            sawTrailers = result.trailers()->bytes().size() > 9000;
        }
        wire.erase(0, result.consumedBytes());
        if (sawTrailers) {
            break;
        }
        RUVIA_CHECK(result.output() != nullptr || result.needInput() != nullptr);
    }
    RUVIA_CHECK(sawBody);
    RUVIA_CHECK(sawTrailers);
    RUVIA_CHECK_EQ(wire, "TAIL");
}

RUVIA_TEST(http1_response_body_decoder_fragmented_chunk_and_205_empty_chunk_complete) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 4> scratch{};
    auto first = decoder.decode("3\r\nab", scratch);
    RUVIA_CHECK(first.output() != nullptr);
    RUVIA_CHECK_EQ(first.output()->bytes(), "ab");
    RUVIA_CHECK_EQ(first.consumedBytes(), std::size_t{5});
    auto second = decoder.decode("c\r\n0\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(second.output() != nullptr);
    RUVIA_CHECK_EQ(second.output()->bytes(), "c");
    auto trailers = decoder.decode("0\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(trailers.trailers() != nullptr);
    RUVIA_CHECK_EQ(trailers.consumedBytes(), std::size_t{5});
    RUVIA_CHECK_EQ(trailers.trailers()->bytes(), "");
    const auto complete = decoder.decode("TAIL", scratch);
    RUVIA_CHECK(complete.complete() != nullptr);

    const auto resetHead = parseHead("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    ruvia::Http1ClientResponseBodyDecoder reset(
        resetHead.plan(), std::pmr::get_default_resource());
    auto emptyChunk = reset.decode("0\r\n\r\nTAIL", scratch);
    RUVIA_CHECK(emptyChunk.trailers() != nullptr);
    RUVIA_CHECK(emptyChunk.trailers()->bytes().empty());
    const auto resetComplete = reset.decode("TAIL", scratch);
    RUVIA_CHECK(resetComplete.complete() != nullptr);
}

RUVIA_TEST(http1_response_body_decoder_fixed_length_eof_is_incomplete) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    std::array<char, 4> scratch{};
    const auto body = decoder.finishInput("ab", scratch);
    RUVIA_CHECK(body.output() != nullptr);
    const auto failure = decoder.finishInput({}, scratch);
    RUVIA_CHECK(failure.protocolFailure() != nullptr);
    RUVIA_CHECK(failure.protocolFailure()->error() == ruvia::Http1ClientResponseBodyError::kIncompleteBody);
}

RUVIA_TEST(http1_response_body_decoder_drains_pending_transfer_output_without_wire_input) {
    constexpr std::string_view deflateAaaa{"\x78\x9c\x73\x04\x02\x00\x02\x8e\x01\x05", 10};
    for (const bool chunked : {false, true}) {
        const auto head = parseHead("GET", chunked
                                               ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate, chunked"
                                               : "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
        ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
        std::array<char, 1> scratch{};
        std::string input(deflateAaaa.substr(0, 5));
        if (chunked) {
            input = "5\r\n" + input + "\r\n";
        }
        std::string content;
        const auto first = decoder.decode(input, scratch);
        RUVIA_CHECK(first.output() != nullptr);
        if (first.output()) {
            content.append(first.output()->bytes());
        }
        input.erase(0, first.consumedBytes());
        RUVIA_CHECK(input.empty());
        for (int count = 0; count < 3; ++count) {
            const auto pending = decoder.decode(input, scratch);
            RUVIA_CHECK(pending.output() != nullptr);
            RUVIA_CHECK_EQ(pending.consumedBytes(), std::size_t{0});
            if (!pending.output()) {
                break;
            }
            content.append(pending.output()->bytes());
        }
        // All four bytes must be observable before the peer supplies the rest
        // of the encoding or any subsequent chunk framing.
        RUVIA_CHECK_EQ(content, "AAAA");
        const auto needsInput = decoder.decode(input, scratch);
        RUVIA_CHECK(needsInput.needInput() != nullptr);

        input.assign(deflateAaaa.substr(5));
        if (chunked) {
            input = "5\r\n" + input + "\r\n0\r\n\r\nTAIL";
        }
        bool complete = false;
        for (int count = 0; count < 16 && !complete; ++count) {
            const auto result = decoder.finishInput(input, scratch);
            RUVIA_CHECK(result.protocolFailure() == nullptr);
            RUVIA_CHECK(result.decoderFailure() == nullptr);
            RUVIA_CHECK(result.needInput() == nullptr);
            if (result.output()) {
                content.append(result.output()->bytes());
            }
            input.erase(0, result.consumedBytes());
            complete = result.complete() != nullptr;
            if (result.protocolFailure() || result.decoderFailure()) {
                break;
            }
        }
        RUVIA_CHECK(complete);
        RUVIA_CHECK_EQ(content, "AAAA");
        RUVIA_CHECK_EQ(input, chunked ? "TAIL" : "");
    }
}

RUVIA_TEST(http1_response_body_decoder_requires_nonempty_scratch) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    ruvia::Http1ClientResponseBodyDecoder decoder(head.plan(), std::pmr::get_default_resource());
    bool rejected = false;
    try {
        (void)decoder.decode({}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
