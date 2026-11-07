#include <gtest/gtest.h>

#include <array>
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

#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/protocol_byte_limit.h"

namespace {

using ruvia::http1_chunked_body_decoder;
using ruvia::protocol_byte_limit;

class counting_no_alloc_resource final : public std::pmr::memory_resource {
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

class scoped_default_resource final {
public:
    explicit scoped_default_resource(std::pmr::memory_resource* resource) noexcept
        : previous_(std::pmr::set_default_resource(resource)) {}
    ~scoped_default_resource() {
        std::pmr::set_default_resource(previous_);
    }

private:
    std::pmr::memory_resource* previous_;
};

}  // namespace

TEST(Ruvia, protocol_byte_limit_has_no_numeric_sentinel) {
    const auto unlimited = protocol_byte_limit::unlimited();
    EXPECT_TRUE(!unlimited.is_limited());
    EXPECT_TRUE(!unlimited.exceeds((std::numeric_limits<std::size_t>::max)()));
    EXPECT_TRUE(unlimited.addition_exceeds((std::numeric_limits<std::size_t>::max)(), 1));

    const auto limited = protocol_byte_limit::limited(8);
    EXPECT_TRUE(limited.is_limited());
    EXPECT_TRUE(!limited.exceeds(8));
    EXPECT_TRUE(limited.exceeds(9));
    EXPECT_TRUE(!limited.addition_exceeds(3, 5));
    EXPECT_TRUE(limited.addition_exceeds(3, 6));

    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    const auto finite_maximum = protocol_byte_limit::limited(maximum);
    EXPECT_TRUE(finite_maximum.is_limited());
    EXPECT_TRUE(!finite_maximum.exceeds(maximum));
    EXPECT_TRUE(finite_maximum.addition_exceeds(maximum, 1));

    bool rejected_zero = false;
    try {
        (void)protocol_byte_limit::limited(0);
    } catch (const std::invalid_argument&) {
        rejected_zero = true;
    }
    EXPECT_TRUE(rejected_zero);
}

TEST(Ruvia, protocol_byte_limit_rejects_negative_sources_without_weakening_body_admission) {
    auto retained = protocol_byte_limit::limited(2);
    for (const auto source : std::array<std::int64_t, 2>{
             (std::numeric_limits<std::int64_t>::min)(), -1}) {
        bool rejected = false;
        try {
            retained = protocol_byte_limit::limited(source);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    http1_chunked_body_decoder exact({.body_limit_ = retained});
    constexpr std::string_view wire = "2\r\nok\r\n0\r\n\r\n";
    const auto admitted = exact.decode(wire);
    EXPECT_TRUE(admitted.body_chunk() != nullptr);
    if (const auto* chunk = admitted.body_chunk()) {
        EXPECT_EQ(chunk->bytes(), "ok");
        const auto terminal = exact.decode(wire.substr(chunk->consumed_bytes()));
        EXPECT_TRUE(terminal.complete() != nullptr);
    }
    http1_chunked_body_decoder excess({.body_limit_ = retained});
    const auto refused = excess.decode("3\r\nbad\r\n0\r\n\r\n");
    EXPECT_TRUE(refused.failure() != nullptr);
    if (const auto* failure = refused.failure()) {
        EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::body_limit_exceeded);
    }
}

TEST(Ruvia, chunked_body_decoder_reports_typed_size_and_limit_failures) {
    http1_chunked_body_decoder invalid({.body_limit_ = protocol_byte_limit::unlimited()});
    const auto invalid_result = invalid.decode("xyz\r\n");
    EXPECT_TRUE(invalid_result.failure() != nullptr);
    EXPECT_TRUE(invalid_result.failure()->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    EXPECT_EQ(ruvia::http_request_chunk_decode_error(invalid_result.failure()->error()).status(), ruvia::http_status::bad_request);
    const auto repeated_invalid = invalid.decode("0\r\n\r\n");
    EXPECT_TRUE(repeated_invalid.failure() != nullptr);
    EXPECT_TRUE(repeated_invalid.failure()->error() ==
                ruvia::http1_chunk_decode_error::invalid_framing);
    EXPECT_EQ(repeated_invalid.consumed_bytes(), std::size_t{0});

    http1_chunked_body_decoder single_limit({.body_limit_ = protocol_byte_limit::limited(10)});
    const auto single_limit_result = single_limit.decode("b\r\n");
    EXPECT_TRUE(single_limit_result.failure() != nullptr);
    EXPECT_TRUE(single_limit_result.failure()->error() ==
                ruvia::http1_chunk_decode_error::body_limit_exceeded);
    EXPECT_EQ(ruvia::http_request_chunk_decode_error(single_limit_result.failure()->error()).status(), ruvia::http_status::content_too_large);
    EXPECT_EQ(single_limit_result.consumed_bytes(), std::size_t{3});

    http1_chunked_body_decoder accumulated({.body_limit_ = protocol_byte_limit::limited(10)});
    const std::string_view wire = "8\r\n12345678\r\n5\r\nabcde\r\n0\r\n\r\n";
    const auto first = accumulated.decode(wire);
    EXPECT_TRUE(first.body_chunk() != nullptr);
    const auto second = accumulated.decode(wire.substr(first.consumed_bytes()));
    EXPECT_TRUE(second.failure() != nullptr);
    EXPECT_TRUE(second.failure()->error() == ruvia::http1_chunk_decode_error::body_limit_exceeded);
    EXPECT_EQ(ruvia::http_request_chunk_decode_error(second.failure()->error()).status(), ruvia::http_status::content_too_large);
}

TEST(Ruvia, chunked_body_decoder_separates_body_and_framing_limits) {
    http1_chunked_body_decoder tiny_body({.body_limit_ = protocol_byte_limit::limited(1)});
    constexpr std::string_view tiny_wire = "1\r\nx\r\n0\r\n\r\n";
    const auto body = tiny_body.decode(tiny_wire);
    EXPECT_TRUE(body.body_chunk() != nullptr);
    if (const auto* chunk = body.body_chunk()) {
        EXPECT_EQ(chunk->bytes(), std::string_view("x"));
        const auto terminal = tiny_body.decode(tiny_wire.substr(chunk->consumed_bytes()));
        EXPECT_TRUE(terminal.complete() != nullptr);
    }

    // Size-line overhead has its own per-message budget, independent of the
    // body limit: three large lines that together stay within it all decode.
    std::string size_line = "1;x=";
    size_line.append(ruvia::max_http_header_bytes / 4 - size_line.size() - 2, 'a');
    size_line.append("\r\n");
    std::string wire;
    for (const char payload : std::string_view("xyz")) {
        wire.append(size_line);
        wire.push_back(payload);
        wire.append("\r\n");
    }
    wire.append("0\r\n\r\nNEXT");

    http1_chunked_body_decoder decoder({.body_limit_ = protocol_byte_limit::limited(3)});
    std::string decoded;
    std::string_view remaining = wire;
    for (std::size_t step = 0; step < 8; ++step) {
        const auto result_value = decoder.decode(remaining);
        remaining.remove_prefix(result_value.consumed_bytes());
        if (const auto* chunk = result_value.body_chunk()) {
            decoded.append(chunk->bytes());
            continue;
        }
        EXPECT_TRUE(result_value.complete() != nullptr);
        break;
    }
    EXPECT_EQ(decoded, "xyz");
    EXPECT_EQ(remaining, "NEXT");
}

TEST(Ruvia, chunked_body_decoder_accepts_unbounded_chunk_count) {
    constexpr std::size_t chunk_count = 20000;
    std::string wire;
    wire.reserve(chunk_count * 6 + 9);
    for (std::size_t index = 0; index < chunk_count; ++index) {
        wire.append("1\r\n");
        wire.push_back(static_cast<char>('a' + index % 26));
        wire.append("\r\n");
    }
    wire.append("0\r\n\r\nNEXT");

    http1_chunked_body_decoder decoder({.body_limit_ = protocol_byte_limit::limited(chunk_count)});
    std::size_t decoded_bytes = 0;
    bool ordered = true;
    std::string_view remaining = wire;
    for (;;) {
        const auto result_value = decoder.decode(remaining);
        remaining.remove_prefix(result_value.consumed_bytes());
        if (const auto* chunk = result_value.body_chunk()) {
            for (const char byte : chunk->bytes()) {
                ordered = ordered && byte == static_cast<char>('a' + decoded_bytes % 26);
                ++decoded_bytes;
            }
            continue;
        }
        EXPECT_TRUE(result_value.complete() != nullptr);
        break;
    }
    EXPECT_TRUE(ordered);
    EXPECT_EQ(decoded_bytes, chunk_count);
    EXPECT_EQ(remaining, "NEXT");
}

TEST(Ruvia, chunked_body_decoder_emits_zero_copy_chunks_and_preserves_pipeline) {
    http1_chunked_body_decoder decoder({.body_limit_ = protocol_byte_limit::limited(1024)});
    const std::string_view wire =
        "5\r\nhello\r\n"
        "6;ext=yes\r\n world\r\n"
        "0\r\nX-Trace: abc\r\n\r\nNEXT";

    std::size_t consumed = 0;
    std::string body;
    for (;;) {
        const auto result_value = decoder.decode(wire.substr(consumed));
        consumed += result_value.consumed_bytes();
        if (const auto* body_chunk = result_value.body_chunk()) {
            body.append(body_chunk->bytes());
            continue;
        }
        if (const auto* complete = result_value.complete()) {
            EXPECT_EQ(complete->trailers(), std::string_view("X-Trace: abc"));
            break;
        }
        EXPECT_TRUE(result_value.need_more() == nullptr);
        break;
    }

    EXPECT_EQ(body, std::string("hello world"));
    EXPECT_EQ(wire.substr(consumed), std::string_view("NEXT"));
}

TEST(Ruvia, chunked_body_decoder_handles_single_byte_input_fragmentation) {
    http1_chunked_body_decoder decoder({.body_limit_ = protocol_byte_limit::limited(1024)});
    const std::string wire = "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
    std::string pending;
    std::string body;
    bool complete_value = false;

    for (const char byte : wire) {
        pending.push_back(byte);
        for (;;) {
            const auto result_value = decoder.decode(pending);
            if (const auto* body_chunk = result_value.body_chunk()) {
                body.append(body_chunk->bytes());
            }
            if (result_value.consumed_bytes() != 0) {
                pending.erase(0, result_value.consumed_bytes());
            }
            if (result_value.complete() != nullptr) {
                complete_value = true;
                break;
            }
            if (result_value.need_more() != nullptr) {
                break;
            }
        }
    }

    EXPECT_TRUE(complete_value);
    EXPECT_EQ(body, std::string("abcde"));
    EXPECT_TRUE(pending.empty());
}

TEST(Ruvia, chunked_request_trailer_names_preserve_fragment_and_pipeline_boundaries) {
    for (const std::size_t length : {1U, 3U, 4U, 9U, 128U}) {
        const std::string name(length, 'X');
        const std::string trailers = name + ": \tfirst:second\t \r\nX-Next: done";
        const std::string message = "0\r\n" + trailers + "\r\n\r\n";
        const std::string wire = message + "NEXT";
        for (std::size_t split = 0; split < message.size(); ++split) {
            http1_chunked_body_decoder decoder;
            const auto first = decoder.decode(std::string_view(wire).substr(0, split));
            EXPECT_TRUE(first.need_more() != nullptr);
            EXPECT_TRUE(first.consumed_bytes() <= split);
            const auto last = decoder.decode(std::string_view(wire).substr(first.consumed_bytes()));
            EXPECT_TRUE(last.complete() != nullptr);
            if (const auto* complete = last.complete()) {
                EXPECT_EQ(complete->trailers(), std::string_view(trailers));
                EXPECT_EQ(complete->trailers().data(), wire.data() + 3);
                const auto consumed = first.consumed_bytes() + complete->consumed_bytes();
                EXPECT_EQ(consumed, message.size());
                EXPECT_EQ(std::string_view(wire).substr(consumed), std::string_view("NEXT"));
            }
        }
    }
}

TEST(Ruvia, chunked_body_decoder_handles_deterministic_arbitrary_fragmented_bytes) {
    std::uint64_t state_value = 0x4348'554E'4B45'4455ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 2048; ++sample) {
        std::string input(static_cast<std::size_t>(next_value() % 513U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        http1_chunked_body_decoder decoder({.body_limit_ = protocol_byte_limit::limited(1024)});
        std::string pending;
        bool terminal = false;
        for (const auto byte : input) {
            pending.push_back(byte);
            for (std::size_t step = 0; step <= pending.size(); ++step) {
                const auto result_value = decoder.decode(pending);
                const auto alternatives = static_cast<unsigned int>(result_value.need_more() != nullptr) +
                                          static_cast<unsigned int>(result_value.body_chunk() != nullptr) +
                                          static_cast<unsigned int>(result_value.complete() != nullptr) +
                                          static_cast<unsigned int>(result_value.failure() != nullptr);
                EXPECT_EQ(alternatives, 1U);
                EXPECT_TRUE(result_value.consumed_bytes() <= pending.size());

                if (const auto* body = result_value.body_chunk()) {
                    EXPECT_TRUE(!body->bytes().empty());
                    EXPECT_TRUE(pending.find(body->bytes()) != std::string::npos);
                }
                if (result_value.consumed_bytes() != 0) {
                    pending.erase(0, result_value.consumed_bytes());
                }
                if (result_value.complete() != nullptr || result_value.failure() != nullptr) {
                    terminal = true;
                    break;
                }
                if (result_value.need_more() != nullptr) {
                    break;
                }
                EXPECT_TRUE(result_value.consumed_bytes() != 0);
            }
            if (terminal) {
                break;
            }
        }
    }
}

TEST(Ruvia, chunked_body_decoder_rejects_bad_delimiter_and_trailer) {
    http1_chunked_body_decoder delimiter({.body_limit_ = protocol_byte_limit::limited(1024)});
    const auto bad_delimiter = delimiter.decode("1\r\nxXY");
    EXPECT_TRUE(bad_delimiter.failure() != nullptr);
    EXPECT_TRUE(bad_delimiter.failure()->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    EXPECT_EQ(bad_delimiter.consumed_bytes(), std::size_t{4});

    http1_chunked_body_decoder trailer({.body_limit_ = protocol_byte_limit::limited(1024)});
    const auto bad_trailer = trailer.decode("0\r\nX-Trace abc\r\n\r\n");
    EXPECT_TRUE(bad_trailer.failure() != nullptr);
    EXPECT_TRUE(bad_trailer.failure()->error() == ruvia::http1_chunk_decode_error::invalid_framing);
}

// RFC 9110 section 6.5.1 forbids generating these trailers, but RFC 9112
// section 7.1.2 lets the recipient discard them: the framing stays valid.
TEST(Ruvia, chunked_body_decoder_completes_with_fields_not_permitted_as_trailers) {
    for (const std::string_view trailers : {std::string_view("Content-Length: 1"),
             std::string_view("Accept-Ranges: bytes"), std::string_view("Date: Sun, 06 Nov 1994 08:49:37 GMT"),
             std::string_view("Cache-Control: no-cache\r\nX-Trace: abc")}) {
        http1_chunked_body_decoder decoder({.body_limit_ = protocol_byte_limit::limited(1024)});
        const std::string wire = "0\r\n" + std::string(trailers) + "\r\n\r\nNEXT";
        const auto result_value = decoder.decode(wire);
        EXPECT_TRUE(result_value.complete() != nullptr);
        if (const auto* complete = result_value.complete()) {
            EXPECT_EQ(complete->trailers(), trailers);
            EXPECT_EQ(complete->consumed_bytes(), wire.size() - 4);
        }
    }
}

// RFC 9112 section 5.2: a server may reject obs-fold (the request default),
// a user agent replaces it with SP, so the response policy validates the
// unfolding and publishes the received bytes for its consumer to unfold.
TEST(Ruvia, chunked_body_decoder_trailer_obs_fold_follows_the_configured_policy) {
    constexpr std::string_view folded = "0\r\nX-T: a\r\n \t b\r\n\t\r\n c\r\nX-U: d\r\n\r\nNEXT";
    http1_chunked_body_decoder request;
    const auto rejected = request.decode(folded);
    EXPECT_TRUE(rejected.failure() != nullptr);
    if (const auto* failure = rejected.failure()) {
        EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }

    http1_chunked_body_decoder response({.obs_fold_ = ruvia::http1_trailer_obs_fold::accept});
    const auto accepted = response.decode(folded);
    EXPECT_TRUE(accepted.complete() != nullptr);
    if (const auto* complete = accepted.complete()) {
        EXPECT_EQ(complete->trailers(), "X-T: a\r\n \t b\r\n\t\r\n c\r\nX-U: d");
        EXPECT_EQ(complete->consumed_bytes(), folded.size() - 4);
    }

    // A fold is a continuation, never a field line of its own, and its bytes
    // are still field-value bytes.
    for (const std::string_view invalid : {std::string_view("0\r\n x: a\r\n\r\n"),
             std::string_view("0\r\nX-T: a\r\n b\x01\r\n\r\n"), std::string_view("0\r\nX-T: a\r\nbad\r\n\r\n")}) {
        http1_chunked_body_decoder decoder({.obs_fold_ = ruvia::http1_trailer_obs_fold::accept});
        const auto result_value = decoder.decode(invalid);
        EXPECT_TRUE(result_value.failure() != nullptr);
    }
}

// A fold continues its field rather than adding one, so the field-count rule
// counts folded fields once while the request policy still rejects them.
TEST(Ruvia, chunked_body_decoder_counts_each_folded_trailer_as_one_field) {
    const auto wire_with = [](std::size_t fields) {
        std::string wire = "0\r\n";
        for (std::size_t field = 0; field < fields; ++field) {
            wire.append("X: a\r\n b\r\n");
        }
        wire.append("\r\n");
        return wire;
    };
    const auto at_limit = wire_with(ruvia::max_http_header_fields);
    http1_chunked_body_decoder response({.obs_fold_ = ruvia::http1_trailer_obs_fold::accept});
    const auto accepted = response.decode(at_limit);
    EXPECT_TRUE(accepted.complete() != nullptr);
    if (const auto* complete = accepted.complete()) {
        EXPECT_EQ(complete->consumed_bytes(), at_limit.size());
    }
    http1_chunked_body_decoder request;
    const auto folded_request = request.decode(at_limit);
    EXPECT_TRUE(folded_request.failure() != nullptr);

    const auto over_limit = wire_with(ruvia::max_http_header_fields + 1);
    http1_chunked_body_decoder over({.obs_fold_ = ruvia::http1_trailer_obs_fold::accept});
    const auto rejected = over.decode(over_limit);
    EXPECT_TRUE(rejected.failure() != nullptr);
    if (const auto* failure = rejected.failure()) {
        EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
}

TEST(Ruvia, chunked_body_decoder_validates_config_quota_and_neutral_errors) {
    http1_chunked_body_decoder decoder({.body_limit_ = protocol_byte_limit::unlimited()});
    constexpr std::string_view wire = "3\r\nabc\r\n0\r\n\r\n";
    bool rejected_quota = false;
    try {
        (void)decoder.decode(wire, 0);
    } catch (const std::invalid_argument&) {
        rejected_quota = true;
    }
    EXPECT_TRUE(rejected_quota);
    const auto decoded = decoder.decode(wire, 2);
    EXPECT_TRUE(decoded.body_chunk() != nullptr);
    if (const auto* chunk = decoded.body_chunk()) {
        EXPECT_EQ(chunk->bytes(), std::string_view("ab"));
        EXPECT_EQ(chunk->consumed_bytes(), std::size_t{5});
        const auto remainder = decoder.decode(wire.substr(chunk->consumed_bytes()), 1);
        EXPECT_TRUE(remainder.body_chunk() != nullptr);
        if (const auto* final_chunk = remainder.body_chunk()) {
            EXPECT_EQ(final_chunk->bytes(), std::string_view("c"));
            EXPECT_EQ(final_chunk->consumed_bytes(), std::size_t{3});
        }
    }

    EXPECT_EQ(ruvia::http_request_chunk_decode_error(
                  ruvia::http1_chunk_decode_error::framing_limit_exceeded)
                  .status(),
        ruvia::http_status::content_too_large);
}

TEST(Ruvia, chunked_body_decoder_decodes_without_pmr_allocation) {
    counting_no_alloc_resource resource;
    {
        scoped_default_resource scoped(&resource);
        http1_chunked_body_decoder decoder;
        constexpr std::string_view wire = "1\r\nx\r\n0\r\nX-Trace: ok\r\n\r\n";
        const auto body = decoder.decode(wire);
        EXPECT_TRUE(body.body_chunk() != nullptr);
        if (const auto* chunk = body.body_chunk()) {
            EXPECT_EQ(chunk->bytes(), std::string_view("x"));
            const auto terminal = decoder.decode(wire.substr(chunk->consumed_bytes()));
            EXPECT_TRUE(terminal.complete() != nullptr);
            if (const auto* complete = terminal.complete()) {
                EXPECT_EQ(complete->trailers(), std::string_view("X-Trace: ok"));
            }
        }
    }
    EXPECT_EQ(resource.allocations(), std::size_t{0});
}

TEST(Ruvia, chunked_body_decoder_caps_each_size_line_at_exact_boundary) {
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        std::string wire = "1;x=";
        // The size line including its CRLF is exactly max_http_header_bytes.
        wire.append(ruvia::max_http_header_bytes + excess - wire.size() - 2, 'a');
        wire.append("\r\nx\r\n0\r\n\r\nNEXT");
        http1_chunked_body_decoder decoder(
            {.body_limit_ = protocol_byte_limit::limited(ruvia::default_max_buffered_body_bytes)});
        const auto body = decoder.decode(wire, 1);
        if (excess == 0) {
            EXPECT_TRUE(body.body_chunk() != nullptr);
            if (!body.body_chunk()) {
                continue;
            }
            EXPECT_EQ(body.body_chunk()->bytes(), "x");
            auto suffix = std::string_view(wire).substr(body.consumed_bytes());
            const auto terminal = decoder.decode(suffix, 1);
            EXPECT_TRUE(terminal.complete() != nullptr);
            suffix.remove_prefix(terminal.consumed_bytes());
            EXPECT_EQ(suffix, "NEXT");
            const auto replay = decoder.decode(suffix);
            EXPECT_TRUE(replay.complete() != nullptr);
            EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
        } else {
            EXPECT_TRUE(body.failure() != nullptr);
            EXPECT_EQ(body.consumed_bytes(), std::size_t{0});
            if (const auto* failure = body.failure()) {
                EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
            }
            const auto replay = decoder.decode(wire);
            EXPECT_TRUE(replay.failure() != nullptr);
            EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
            if (const auto* failure = replay.failure()) {
                EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
            }
        }
    }

    http1_chunked_body_decoder unterminated;
    std::string oversized = "1;x=";
    oversized.append(ruvia::max_http_header_bytes, 'a');
    const auto result_value = unterminated.decode(oversized);
    EXPECT_TRUE(result_value.failure() != nullptr);
    if (const auto* failure = result_value.failure()) {
        EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
    }
}

TEST(Ruvia, chunked_body_decoder_bounds_cumulative_size_line_overhead_per_message) {
    // Leading zeros and chunk-ext beyond the shortest hex size share one
    // max_http_header_bytes budget across all size lines of a message.
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        constexpr auto leading_zeros = ruvia::max_http_header_bytes / 2;
        std::string wire(leading_zeros, '0');
        wire.append("1\r\nx\r\n");
        const auto second_line_offset = wire.size();
        wire.append("1;x=");
        // This line's overhead is ";x=" plus the extension value.
        wire.append(ruvia::max_http_header_bytes - leading_zeros - 3 + excess, 'a');
        wire.append("\r\ny\r\n0\r\n\r\nNEXT");

        http1_chunked_body_decoder decoder;
        std::string_view rest(wire);
        std::size_t consumed = 0;
        std::string body;
        for (;;) {
            const auto result = decoder.decode(rest);
            consumed += result.consumed_bytes();
            rest.remove_prefix(result.consumed_bytes());
            if (const auto* chunk = result.body_chunk()) {
                body.append(chunk->bytes());
                continue;
            }
            if (excess == 0) {
                EXPECT_TRUE(result.complete() != nullptr);
                EXPECT_EQ(body, "xy");
                EXPECT_EQ(rest, "NEXT");
            } else {
                EXPECT_TRUE(result.failure() != nullptr);
                if (const auto* failure = result.failure()) {
                    EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
                }
                EXPECT_EQ(consumed, second_line_offset);
                EXPECT_EQ(body, "x");
            }
            break;
        }
    }
}

TEST(Ruvia, chunked_body_decoder_preserves_incomplete_delimiter_consumption) {
    http1_chunked_body_decoder decoder;
    constexpr std::string_view wire = "1\r\nx\r";
    const auto body = decoder.decode(wire);
    EXPECT_TRUE(body.body_chunk() != nullptr);
    EXPECT_EQ(body.consumed_bytes(), std::size_t{4});
    if (const auto* chunk = body.body_chunk()) {
        EXPECT_EQ(chunk->bytes().data(), wire.data() + 3);
    }
    const auto incomplete = decoder.decode(wire.substr(body.consumed_bytes()));
    EXPECT_TRUE(incomplete.need_more() != nullptr);
    EXPECT_EQ(incomplete.consumed_bytes(), std::size_t{0});
    constexpr std::string_view suffix = "\r\n0\r\n\r\nNEXT";
    const auto complete_value = decoder.decode(suffix);
    EXPECT_TRUE(complete_value.complete() != nullptr);
    EXPECT_EQ(suffix.substr(complete_value.consumed_bytes()), "NEXT");
}

TEST(Ruvia, chunked_body_decoder_rejects_bare_lf_without_waiting_for_crlf) {
    for (const std::string_view wire : {std::string_view("5\nhello\n0\n\n"), std::string_view("5;a=b\n"),
             std::string_view("\n"), std::string_view("0\r\nX-Trace: ok\n\n"), std::string_view("0\r\n\n"),
             std::string_view("0\r\nX-Trace: ok\r\n\n")}) {
        http1_chunked_body_decoder decoder;
        const auto result_value = decoder.decode(wire);
        EXPECT_TRUE(result_value.failure() != nullptr);
        if (const auto* failure = result_value.failure()) {
            EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
        }
    }

    // A bare LF arriving in a later fragment of a partial size line is rejected too.
    http1_chunked_body_decoder fragmented;
    const auto partial = fragmented.decode("5;a=");
    EXPECT_TRUE(partial.need_more() != nullptr);
    EXPECT_EQ(partial.consumed_bytes(), std::size_t{0});
    const auto rejected = fragmented.decode("5;a=b\nhello\r\n");
    EXPECT_TRUE(rejected.failure() != nullptr);
    if (const auto* failure = rejected.failure()) {
        EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
}

TEST(Ruvia, chunked_body_decoder_accepts_many_trailer_fields_within_byte_budget) {
    std::string fields_value = "0\r\n";
    for (std::size_t index = 0; index < 80; ++index) {
        fields_value.append("X-Trace: ok\r\n");
    }
    fields_value.append("\r\n");
    http1_chunked_body_decoder field_limit;
    const auto field_result = field_limit.decode(fields_value);
    EXPECT_TRUE(field_result.complete() != nullptr);
}

TEST(Ruvia, chunked_body_decoder_bounds_trailers_independently_of_input_segmentation) {
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        std::string trailers = "X-Trace: ";
        trailers.append(ruvia::max_http_header_bytes + excess - trailers.size() - 5, ' ');
        trailers.append("x\r\n\r\n");
        const std::string wire = "0\r\n" + trailers + "NEXT";
        const auto message_end = 3 + trailers.size();
        for (const auto split : {wire.size(), message_end - 1}) {
            http1_chunked_body_decoder decoder;
            auto result = decoder.decode(std::string_view(wire).substr(0, split));
            auto consumed = result.consumed_bytes();
            if (result.need_more()) {
                result = decoder.decode(std::string_view(wire).substr(consumed));
                consumed += result.consumed_bytes();
            }
            if (excess == 0) {
                EXPECT_TRUE(result.complete() != nullptr);
                if (const auto* complete = result.complete()) {
                    EXPECT_EQ(complete->trailers(), std::string_view(trailers).substr(0, trailers.size() - 4));
                }
                EXPECT_EQ(consumed, message_end);
                EXPECT_EQ(std::string_view(wire).substr(consumed), "NEXT");
            } else {
                EXPECT_TRUE(result.failure() != nullptr);
                EXPECT_EQ(consumed, std::size_t{3});
                if (const auto* failure = result.failure()) {
                    EXPECT_TRUE(failure->error() ==
                                ruvia::http1_chunk_decode_error::framing_limit_exceeded);
                }
                const auto replay = decoder.decode({});
                EXPECT_TRUE(replay.failure() != nullptr);
                EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
            }
        }
    }
}

TEST(Ruvia, chunked_body_decoder_reports_byte_cap_before_later_invalid_framing) {
    for (const bool trailer : {false, true}) {
        std::string wire = trailer ? "0\r\nX-Trace: " : "1;x=";
        wire.append(ruvia::max_http_header_bytes, 'a');
        wire.append("\n\r\n\r\n");
        http1_chunked_body_decoder decoder;
        const auto result = decoder.decode(wire);
        EXPECT_TRUE(result.failure() != nullptr);
        if (const auto* failure = result.failure()) {
            EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
        }
    }
}
