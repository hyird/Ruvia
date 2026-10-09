#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpRequestBodyFailure.h"
#include "ruvia/http/ProtocolByteLimit.h"

#include "test_harness.h"

namespace {

using ruvia::Http1ChunkedBodyDecoder;
using ruvia::Http1ChunkTrailerRole;
using ruvia::ProtocolByteLimit;

class CountingNoAllocResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }

private:
    void* do_allocate(std::size_t, std::size_t) override {
        ++allocations_;
        throw std::bad_alloc();
    }
    void do_deallocate(void*, std::size_t, std::size_t) override {}
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t allocations_{0};
};

class ScopedDefaultResource final {
public:
    explicit ScopedDefaultResource(std::pmr::memory_resource* resource) noexcept
        : previous_(std::pmr::set_default_resource(resource)) {}
    ~ScopedDefaultResource() {
        std::pmr::set_default_resource(previous_);
    }

private:
    std::pmr::memory_resource* previous_;
};

}  // namespace

RUVIA_TEST(protocol_byte_limit_has_no_numeric_sentinel) {
    const auto unlimited = ProtocolByteLimit::unlimited();
    RUVIA_CHECK(!unlimited.isLimited());
    RUVIA_CHECK(!unlimited.maximum().has_value());
    RUVIA_CHECK(!unlimited.exceeds((std::numeric_limits<std::size_t>::max)()));
    RUVIA_CHECK(unlimited.additionExceeds((std::numeric_limits<std::size_t>::max)(), 1));

    const auto limited = ProtocolByteLimit::limited(8);
    RUVIA_CHECK(limited.isLimited());
    RUVIA_CHECK(limited.maximum().has_value());
    RUVIA_CHECK_EQ(limited.maximum().value(), std::size_t{8});
    RUVIA_CHECK(!limited.exceeds(8));
    RUVIA_CHECK(limited.exceeds(9));
    RUVIA_CHECK(!limited.additionExceeds(3, 5));
    RUVIA_CHECK(limited.additionExceeds(3, 6));

    bool rejectedZero = false;
    try {
        (void)ProtocolByteLimit::limited(0);
    } catch (const std::invalid_argument&) {
        rejectedZero = true;
    }
    RUVIA_CHECK(rejectedZero);
}

RUVIA_TEST(chunked_body_decoder_reports_typed_size_and_limit_failures) {
    Http1ChunkedBodyDecoder invalid({.bodyLimit = ProtocolByteLimit::unlimited()});
    const auto invalidResult = invalid.decode("xyz\r\n");
    RUVIA_CHECK(invalidResult.failure() != nullptr);
    RUVIA_CHECK(invalidResult.failure()->error() == ruvia::Http1ChunkDecodeError::kInvalidFraming);
    RUVIA_CHECK_EQ(ruvia::httpRequestChunkDecodeError(invalidResult.failure()->error()).status(),
        ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(std::string_view(
                       ruvia::httpRequestChunkDecodeError(invalidResult.failure()->error()).what()),
        std::string_view("invalid chunked request body"));
    const auto repeatedInvalid = invalid.decode("0\r\n\r\n");
    RUVIA_CHECK(repeatedInvalid.failure() != nullptr);
    RUVIA_CHECK(repeatedInvalid.failure()->error() ==
                ruvia::Http1ChunkDecodeError::kInvalidFraming);
    RUVIA_CHECK_EQ(repeatedInvalid.consumedBytes(), std::size_t{0});

    Http1ChunkedBodyDecoder singleLimit({.bodyLimit = ProtocolByteLimit::limited(10)});
    const auto singleLimitResult = singleLimit.decode("b\r\n");
    RUVIA_CHECK(singleLimitResult.failure() != nullptr);
    RUVIA_CHECK(singleLimitResult.failure()->error() ==
                ruvia::Http1ChunkDecodeError::kBodyLimitExceeded);
    RUVIA_CHECK_EQ(ruvia::httpRequestChunkDecodeError(singleLimitResult.failure()->error()).status(),
        ruvia::http_status::kContentTooLarge);
    RUVIA_CHECK_EQ(singleLimitResult.consumedBytes(), std::size_t{3});

    Http1ChunkedBodyDecoder accumulated({.bodyLimit = ProtocolByteLimit::limited(10)});
    const std::string_view wire = "8\r\n12345678\r\n5\r\nabcde\r\n0\r\n\r\n";
    const auto first = accumulated.decode(wire);
    RUVIA_CHECK(first.bodyChunk() != nullptr);
    const auto second = accumulated.decode(wire.substr(first.consumedBytes()));
    RUVIA_CHECK(second.failure() != nullptr);
    RUVIA_CHECK(second.failure()->error() == ruvia::Http1ChunkDecodeError::kBodyLimitExceeded);
    RUVIA_CHECK_EQ(ruvia::httpRequestChunkDecodeError(second.failure()->error()).status(),
        ruvia::http_status::kContentTooLarge);
}

RUVIA_TEST(chunked_body_decoder_separates_body_and_framing_budgets) {
    Http1ChunkedBodyDecoder tinyBody({.bodyLimit = ProtocolByteLimit::limited(1)});
    constexpr std::string_view tinyWire = "1\r\nx\r\n0\r\n\r\n";
    const auto body = tinyBody.decode(tinyWire);
    RUVIA_CHECK(body.bodyChunk() != nullptr);
    if (const auto* chunk = body.bodyChunk()) {
        RUVIA_CHECK_EQ(chunk->bytes(), std::string_view("x"));
        const auto terminal = tinyBody.decode(tinyWire.substr(chunk->consumedBytes()));
        RUVIA_CHECK(terminal.complete() != nullptr);
    }

    Http1ChunkedBodyDecoder framingFlood({.bodyLimit = ProtocolByteLimit::unlimited()});
    std::string sizeLine = "1;x=";
    sizeLine.append(ruvia::kMaxHttpHeaderBytes / 2 - sizeLine.size() - 2, 'a');
    sizeLine.append("\r\n");
    const std::string floodWire = sizeLine + "x\r\n" + sizeLine + "y\r\n0\r\n\r\n";

    const auto first = framingFlood.decode(floodWire);
    RUVIA_CHECK(first.bodyChunk() != nullptr);
    if (const auto* chunk = first.bodyChunk()) {
        const auto excessive =
            framingFlood.decode(std::string_view(floodWire).substr(chunk->consumedBytes()));
        RUVIA_CHECK(excessive.failure() != nullptr);
        if (const auto* failure = excessive.failure()) {
            RUVIA_CHECK(failure->error() == ruvia::Http1ChunkDecodeError::kFramingLimitExceeded);
        }
    }
}

RUVIA_TEST(chunked_body_decoder_emits_zero_copy_chunks_and_preserves_pipeline) {
    Http1ChunkedBodyDecoder decoder({.bodyLimit = ProtocolByteLimit::limited(1024)});
    const std::string_view wire =
        "5\r\nhello\r\n"
        "6;ext=yes\r\n world\r\n"
        "0\r\nX-Trace: abc\r\n\r\nNEXT";

    std::size_t consumed = 0;
    std::string body;
    for (;;) {
        const auto result = decoder.decode(wire.substr(consumed));
        consumed += result.consumedBytes();
        if (const auto* bodyChunk = result.bodyChunk()) {
            body.append(bodyChunk->bytes());
            continue;
        }
        if (const auto* complete = result.complete()) {
            RUVIA_CHECK_EQ(complete->trailers(), std::string_view("X-Trace: abc"));
            break;
        }
        RUVIA_CHECK(result.needMore() == nullptr);
        break;
    }

    RUVIA_CHECK_EQ(body, std::string("hello world"));
    RUVIA_CHECK_EQ(wire.substr(consumed), std::string_view("NEXT"));
}

RUVIA_TEST(chunked_body_decoder_handles_single_byte_input_fragmentation) {
    Http1ChunkedBodyDecoder decoder({.bodyLimit = ProtocolByteLimit::limited(1024)});
    const std::string wire = "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
    std::string pending;
    std::string body;
    bool complete = false;

    for (const char byte : wire) {
        pending.push_back(byte);
        for (;;) {
            const auto result = decoder.decode(pending);
            if (const auto* bodyChunk = result.bodyChunk()) {
                body.append(bodyChunk->bytes());
            }
            if (result.consumedBytes() != 0) {
                pending.erase(0, result.consumedBytes());
            }
            if (result.complete() != nullptr) {
                complete = true;
                break;
            }
            if (result.needMore() != nullptr) {
                break;
            }
        }
    }

    RUVIA_CHECK(complete);
    RUVIA_CHECK_EQ(body, std::string("abcde"));
    RUVIA_CHECK(pending.empty());
}

RUVIA_TEST(chunked_request_trailer_names_preserve_fragment_and_pipeline_boundaries) {
    for (const std::size_t length : {1U, 3U, 4U, 9U, 128U}) {
        const std::string name(length, 'X');
        const std::string trailers = name + ": \tfirst:second\t \r\nX-Next: done";
        const std::string message = "0\r\n" + trailers + "\r\n\r\n";
        const std::string wire = message + "NEXT";
        for (std::size_t split = 0; split < message.size(); ++split) {
            Http1ChunkedBodyDecoder decoder;
            const auto first = decoder.decode(std::string_view(wire).substr(0, split));
            RUVIA_CHECK(first.needMore() != nullptr);
            RUVIA_CHECK(first.consumedBytes() <= split);
            const auto last = decoder.decode(std::string_view(wire).substr(first.consumedBytes()));
            RUVIA_CHECK(last.complete() != nullptr);
            if (const auto* complete = last.complete()) {
                RUVIA_CHECK_EQ(complete->trailers(), std::string_view(trailers));
                RUVIA_CHECK_EQ(complete->trailers().data(), wire.data() + 3);
                const auto consumed = first.consumedBytes() + complete->consumedBytes();
                RUVIA_CHECK_EQ(consumed, message.size());
                RUVIA_CHECK_EQ(std::string_view(wire).substr(consumed), std::string_view("NEXT"));
            }
        }
    }
}

RUVIA_TEST(chunked_body_decoder_handles_deterministic_arbitrary_fragmented_bytes) {
    std::uint64_t state = 0x4348'554E'4B45'4455ULL;
    const auto next = [&state]() {
        state ^= state << 7U;
        state ^= state >> 9U;
        return state;
    };

    for (std::size_t sample = 0; sample < 2048; ++sample) {
        std::string input(static_cast<std::size_t>(next() % 513U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next());
        }

        Http1ChunkedBodyDecoder decoder({.bodyLimit = ProtocolByteLimit::limited(1024)});
        std::string pending;
        bool terminal = false;
        for (const auto byte : input) {
            pending.push_back(byte);
            for (std::size_t step = 0; step <= pending.size(); ++step) {
                const auto result = decoder.decode(pending);
                const auto alternatives = static_cast<unsigned int>(result.needMore() != nullptr) +
                                          static_cast<unsigned int>(result.bodyChunk() != nullptr) +
                                          static_cast<unsigned int>(result.complete() != nullptr) +
                                          static_cast<unsigned int>(result.failure() != nullptr);
                RUVIA_CHECK_EQ(alternatives, 1U);
                RUVIA_CHECK(result.consumedBytes() <= pending.size());

                if (const auto* body = result.bodyChunk()) {
                    RUVIA_CHECK(!body->bytes().empty());
                    RUVIA_CHECK(pending.find(body->bytes()) != std::string::npos);
                }
                if (result.consumedBytes() != 0) {
                    pending.erase(0, result.consumedBytes());
                }
                if (result.complete() != nullptr || result.failure() != nullptr) {
                    terminal = true;
                    break;
                }
                if (result.needMore() != nullptr) {
                    break;
                }
                RUVIA_CHECK(result.consumedBytes() != 0);
            }
            if (terminal) {
                break;
            }
        }
    }
}

RUVIA_TEST(chunked_body_decoder_rejects_bad_delimiter_and_trailer) {
    Http1ChunkedBodyDecoder delimiter({.bodyLimit = ProtocolByteLimit::limited(1024)});
    const auto badDelimiter = delimiter.decode("1\r\nxXY");
    RUVIA_CHECK(badDelimiter.failure() != nullptr);
    RUVIA_CHECK(badDelimiter.failure()->error() == ruvia::Http1ChunkDecodeError::kInvalidFraming);
    RUVIA_CHECK_EQ(badDelimiter.consumedBytes(), std::size_t{4});

    Http1ChunkedBodyDecoder trailer({.bodyLimit = ProtocolByteLimit::limited(1024)});
    const auto badTrailer = trailer.decode("0\r\nContent-Length: 1\r\n\r\n");
    RUVIA_CHECK(badTrailer.failure() != nullptr);
    RUVIA_CHECK(badTrailer.failure()->error() == ruvia::Http1ChunkDecodeError::kInvalidFraming);
}

RUVIA_TEST(chunked_body_decoder_validates_trailers_by_message_role) {
    Http1ChunkedBodyDecoder response({.bodyLimit = ProtocolByteLimit::limited(1024),
        .trailerRole = Http1ChunkTrailerRole::kResponse});
    const auto acceptedResponse = response.decode("0\r\nAccept-Ranges: bytes\r\n\r\n");
    RUVIA_CHECK(acceptedResponse.complete() != nullptr);
    if (const auto* complete = acceptedResponse.complete()) {
        RUVIA_CHECK_EQ(complete->trailers(), std::string_view("Accept-Ranges: bytes"));
    }

    Http1ChunkedBodyDecoder request({.bodyLimit = ProtocolByteLimit::limited(1024)});
    const auto rejectedRequest = request.decode("0\r\nAccept-Ranges: bytes\r\n\r\n");
    RUVIA_CHECK(rejectedRequest.failure() != nullptr);
    RUVIA_CHECK(rejectedRequest.failure()->error() == ruvia::Http1ChunkDecodeError::kInvalidFraming);

    Http1ChunkedBodyDecoder forbiddenResponse({.bodyLimit = ProtocolByteLimit::limited(1024),
        .trailerRole = Http1ChunkTrailerRole::kResponse});
    const auto rejectedResponse =
        forbiddenResponse.decode("0\r\nDate: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n");
    RUVIA_CHECK(rejectedResponse.failure() != nullptr);
    RUVIA_CHECK(rejectedResponse.failure()->error() == ruvia::Http1ChunkDecodeError::kInvalidFraming);
}

RUVIA_TEST(chunked_body_decoder_validates_config_quota_and_neutral_errors) {
    bool rejectedRole = false;
    try {
        Http1ChunkedBodyDecoder decoder({.trailerRole = static_cast<Http1ChunkTrailerRole>(255)});
    } catch (const std::invalid_argument&) {
        rejectedRole = true;
    }
    RUVIA_CHECK(rejectedRole);

    Http1ChunkedBodyDecoder decoder({.bodyLimit = ProtocolByteLimit::unlimited()});
    constexpr std::string_view wire = "3\r\nabc\r\n0\r\n\r\n";
    bool rejectedQuota = false;
    try {
        (void)decoder.decode(wire, 0);
    } catch (const std::invalid_argument&) {
        rejectedQuota = true;
    }
    RUVIA_CHECK(rejectedQuota);
    const auto decoded = decoder.decode(wire, 2);
    RUVIA_CHECK(decoded.bodyChunk() != nullptr);
    if (const auto* chunk = decoded.bodyChunk()) {
        RUVIA_CHECK_EQ(chunk->bytes(), std::string_view("ab"));
        RUVIA_CHECK_EQ(chunk->consumedBytes(), std::size_t{5});
        const auto remainder = decoder.decode(wire.substr(chunk->consumedBytes()), 1);
        RUVIA_CHECK(remainder.bodyChunk() != nullptr);
        if (const auto* finalChunk = remainder.bodyChunk()) {
            RUVIA_CHECK_EQ(finalChunk->bytes(), std::string_view("c"));
            RUVIA_CHECK_EQ(finalChunk->consumedBytes(), std::size_t{3});
        }
    }

    RUVIA_CHECK_EQ(ruvia::httpRequestChunkDecodeError(
                       ruvia::Http1ChunkDecodeError::kFramingLimitExceeded)
                       .status(),
        ruvia::http_status::kContentTooLarge);
}

RUVIA_TEST(chunked_body_decoder_decodes_without_pmr_allocation) {
    CountingNoAllocResource resource;
    {
        ScopedDefaultResource scoped(&resource);
        Http1ChunkedBodyDecoder decoder;
        constexpr std::string_view wire = "1\r\nx\r\n0\r\nX-Trace: ok\r\n\r\n";
        const auto body = decoder.decode(wire);
        RUVIA_CHECK(body.bodyChunk() != nullptr);
        if (const auto* chunk = body.bodyChunk()) {
            RUVIA_CHECK_EQ(chunk->bytes(), std::string_view("x"));
            const auto terminal = decoder.decode(wire.substr(chunk->consumedBytes()));
            RUVIA_CHECK(terminal.complete() != nullptr);
            if (const auto* complete = terminal.complete()) {
                RUVIA_CHECK_EQ(complete->trailers(), std::string_view("X-Trace: ok"));
            }
        }
    }
    RUVIA_CHECK_EQ(resource.allocations(), std::size_t{0});
}

RUVIA_TEST(chunked_body_decoder_enforces_cumulative_framing_at_exact_boundary) {
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        std::string wire = "1;x=";
        wire.append(ruvia::kMaxHttpHeaderBytes - 7 + excess - wire.size() - 2, 'a');
        wire.append("\r\nx\r\n0\r\n\r\nNEXT");
        Http1ChunkedBodyDecoder decoder;
        const auto body = decoder.decode(wire, 1);
        RUVIA_CHECK(body.bodyChunk() != nullptr);
        if (!body.bodyChunk()) {
            continue;
        }
        RUVIA_CHECK_EQ(body.bodyChunk()->bytes(), "x");
        auto suffix = std::string_view(wire).substr(body.consumedBytes());
        const auto terminal = decoder.decode(suffix, 1);
        if (excess == 0) {
            RUVIA_CHECK(terminal.complete() != nullptr);
            suffix.remove_prefix(terminal.consumedBytes());
            RUVIA_CHECK_EQ(suffix, "NEXT");
            const auto replay = decoder.decode(suffix);
            RUVIA_CHECK(replay.complete() != nullptr);
            RUVIA_CHECK_EQ(replay.consumedBytes(), std::size_t{0});
        } else {
            RUVIA_CHECK(terminal.failure() != nullptr);
            if (const auto* failure = terminal.failure()) {
                RUVIA_CHECK(failure->error() == ruvia::Http1ChunkDecodeError::kFramingLimitExceeded);
            }
            const auto replay = decoder.decode(suffix);
            RUVIA_CHECK(replay.failure() != nullptr);
            RUVIA_CHECK_EQ(replay.consumedBytes(), std::size_t{0});
            if (const auto* failure = replay.failure()) {
                RUVIA_CHECK(failure->error() == ruvia::Http1ChunkDecodeError::kFramingLimitExceeded);
            }
        }
    }
}

RUVIA_TEST(chunked_body_decoder_caps_each_size_line) {
    Http1ChunkedBodyDecoder decoder(
        {.bodyLimit = ProtocolByteLimit::limited(ruvia::kDefaultMaxBufferedBodyBytes)});
    std::string oversized = "1;x=";
    oversized.append(ruvia::kMaxHttpHeaderBytes, 'a');
    oversized.append("\r\n");

    const auto result = decoder.decode(oversized);
    RUVIA_CHECK(result.failure() != nullptr);
    if (const auto* failure = result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::Http1ChunkDecodeError::kFramingLimitExceeded);
    }

    Http1ChunkedBodyDecoder boundary(
        {.bodyLimit = ProtocolByteLimit::limited(ruvia::kDefaultMaxBufferedBodyBytes)});
    std::string accepted = "1;x=";
    // Reserve two bytes for the data delimiter and five for the terminal chunk.
    accepted.append(ruvia::kMaxHttpHeaderBytes - 7 - accepted.size() - 2, 'a');
    accepted.append("\r\nx\r\n0\r\n\r\n");
    const auto boundaryResult = boundary.decode(accepted);
    RUVIA_CHECK(boundaryResult.bodyChunk() != nullptr);
    if (const auto* body = boundaryResult.bodyChunk()) {
        const auto terminal =
            boundary.decode(std::string_view(accepted).substr(body->consumedBytes()));
        RUVIA_CHECK(terminal.complete() != nullptr);
    }
}

RUVIA_TEST(chunked_body_decoder_preserves_incomplete_delimiter_consumption) {
    Http1ChunkedBodyDecoder decoder;
    constexpr std::string_view wire = "1\r\nx\r";
    const auto body = decoder.decode(wire);
    RUVIA_CHECK(body.bodyChunk() != nullptr);
    RUVIA_CHECK_EQ(body.consumedBytes(), std::size_t{4});
    if (const auto* chunk = body.bodyChunk()) {
        RUVIA_CHECK_EQ(chunk->bytes().data(), wire.data() + 3);
    }
    const auto incomplete = decoder.decode(wire.substr(body.consumedBytes()));
    RUVIA_CHECK(incomplete.needMore() != nullptr);
    RUVIA_CHECK_EQ(incomplete.consumedBytes(), std::size_t{0});
    constexpr std::string_view suffix = "\r\n0\r\n\r\nNEXT";
    const auto complete = decoder.decode(suffix);
    RUVIA_CHECK(complete.complete() != nullptr);
    RUVIA_CHECK_EQ(suffix.substr(complete.consumedBytes()), "NEXT");
}

RUVIA_TEST(chunked_body_decoder_preserves_trailer_failure_precedence) {
    std::string oversized = "0\r\nX-Trace: ";
    oversized.append(ruvia::kMaxHttpHeaderBytes, 'a');
    Http1ChunkedBodyDecoder incomplete;
    const auto incompleteResult = incomplete.decode(oversized);
    RUVIA_CHECK(incompleteResult.failure() != nullptr);
    RUVIA_CHECK_EQ(incompleteResult.consumedBytes(), std::size_t{3});
    if (const auto* failure = incompleteResult.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::Http1ChunkDecodeError::kFramingLimitExceeded);
    }

    oversized.append("\r\n\r\n");
    Http1ChunkedBodyDecoder terminated;
    const auto terminatedResult = terminated.decode(oversized);
    RUVIA_CHECK(terminatedResult.failure() != nullptr);
    RUVIA_CHECK_EQ(terminatedResult.consumedBytes(), std::size_t{3});
    if (const auto* failure = terminatedResult.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::Http1ChunkDecodeError::kInvalidFraming);
    }

    std::string fields = "0\r\n";
    for (std::size_t index = 0; index <= ruvia::kMaxHttpHeaderFields; ++index) {
        fields.append("X-Trace: ok\r\n");
    }
    fields.append("\r\n");
    Http1ChunkedBodyDecoder fieldLimit;
    const auto fieldResult = fieldLimit.decode(fields);
    RUVIA_CHECK(fieldResult.failure() != nullptr);
    if (const auto* failure = fieldResult.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::Http1ChunkDecodeError::kInvalidFraming);
    }
}
