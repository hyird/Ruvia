#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

#include "content_decoding_fixture.h"
#include "test_exceptions.h"

namespace {

using ruvia::http1_chunked_body_decoder;
using ruvia::protocol_byte_limit;

// Published trailer fields as "name=value;" in received order.
std::string rendered(std::span<const ruvia::http_header> fields) {
    std::string text;
    for (const auto& field : fields) {
        text.append(field.name()).append("=").append(field.value()).append(";");
    }
    return text;
}

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
            EXPECT_EQ(rendered(complete->trailers()), "x-trace=abc;");
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
                EXPECT_EQ(rendered(complete->trailers()),
                    std::string(length, 'x') + "=first:second;x-next=done;");
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
// section 7.1.2 lets the recipient discard them: the framing stays valid and
// each message direction publishes only the fields its sender may generate.
TEST(Ruvia, chunked_body_decoder_discards_fields_not_permitted_as_trailers) {
    struct case_type final {
        std::string_view trailers_;
        std::string_view request_;
        std::string_view response_;
    };
    for (const auto& [trailers, request, response] : std::array{
             case_type{"Content-Length: 1", "", ""},
             case_type{"Accept-Ranges: bytes", "", "accept-ranges=bytes;"},
             case_type{"Date: Sun, 06 Nov 1994 08:49:37 GMT", "date=Sun, 06 Nov 1994 08:49:37 GMT;", ""},
             case_type{"Cache-Control: no-cache\r\nX-Trace: abc\r\nETag: \"v\"", "x-trace=abc;etag=\"v\";",
                 "x-trace=abc;etag=\"v\";"},
         }) {
        const std::string wire = "0\r\n" + std::string(trailers) + "\r\n\r\nNEXT";
        for (const auto message : {ruvia::http1_trailer_message::request, ruvia::http1_trailer_message::response}) {
            http1_chunked_body_decoder decoder({.trailer_message_ = message});
            const auto result_value = decoder.decode(wire);
            EXPECT_TRUE(result_value.complete() != nullptr);
            if (const auto* complete = result_value.complete()) {
                EXPECT_EQ(rendered(complete->trailers()),
                    message == ruvia::http1_trailer_message::request ? request : response);
                EXPECT_EQ(complete->consumed_bytes(), wire.size() - 4);
            }
        }
    }
}

// RFC 9112 section 5.2: a server may reject obs-fold (the request default),
// a user agent replaces it with SP, so the response policy validates and
// publishes the unfolding.
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
        EXPECT_EQ(rendered(complete->trailers()), "x-t=a b  c;x-u=d;");
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

TEST(Ruvia, chunked_body_decoder_replays_completion_without_retained_trailers) {
    for (const std::string_view wire :
        {std::string_view("1\r\nx\r\n0\r\n\r\n"), std::string_view("1\r\nx\r\n0\r\nContent-Length: 1\r\n\r\n")}) {
        http1_chunked_body_decoder decoder;
        const auto body = decoder.decode(wire);
        EXPECT_TRUE(body.body_chunk() != nullptr);
        if (const auto* chunk = body.body_chunk()) {
            EXPECT_EQ(chunk->bytes(), std::string_view("x"));
            const auto terminal = decoder.decode(wire.substr(chunk->consumed_bytes()));
            EXPECT_TRUE(terminal.complete() != nullptr);
            if (const auto* complete = terminal.complete()) {
                EXPECT_TRUE(complete->trailers().empty());
            }
        }
    }

    http1_chunked_body_decoder decoder;
    const auto terminal = decoder.decode("0\r\nX-Trace: ok\r\n\r\n");
    EXPECT_TRUE(terminal.complete() != nullptr);
    if (const auto* complete = terminal.complete()) {
        EXPECT_EQ(rendered(complete->trailers()), "x-trace=ok;");
    }
    const auto replay = decoder.decode({});
    EXPECT_TRUE(replay.complete() != nullptr);
    if (const auto* complete = replay.complete()) {
        EXPECT_TRUE(complete->trailers().empty());
    }
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
                    EXPECT_EQ(complete->trailers().size(), std::size_t{1});
                    EXPECT_EQ(rendered(complete->trailers()), "x-trace=x;");
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

using namespace ruvia::test;

// Decoding a transfer-coded (chunked, then coded) request body.

TEST(Ruvia, transfer_coding_stack_decodes_reverse_order_with_tiny_scratch) {
    constexpr std::string_view plain = "stacked transfer coding payload";
    const std::string inner = gzip_compress(plain);
    const std::string wire = zlib_deflate_compress(inner);
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    std::string decoded;
    std::array<char, 1> scratch{};
    {
        ruvia::http_transfer_coding_stack_decoder stack(
            codings, protocol_byte_limit::limited(1024));
        std::size_t cursor_value = 0;
        while (cursor_value < wire.size()) {
            const auto result_value = stack.decode(std::string_view(wire).substr(cursor_value, 1), scratch);
            cursor_value += result_value.consumed_bytes();
            if (const auto* output = result_value.output()) {
                decoded.append(output->bytes());
            } else if (result_value.failure() != nullptr || result_value.decoder_failure() != nullptr) {
                EXPECT_TRUE(false);
                return;
            }
        }
        for (;;) {
            const auto result_value = stack.decode({}, scratch);
            if (const auto* output = result_value.output()) {
                decoded.append(output->bytes());
                continue;
            }
            if (result_value.need_input() != nullptr) {
                break;
            }
            if (result_value.failure() != nullptr || result_value.decoder_failure() != nullptr) {
                EXPECT_TRUE(false);
                return;
            }
        }
        const auto finish_value = stack.finish_input();
        EXPECT_TRUE(finish_value.complete() != nullptr);
    }
    EXPECT_EQ(std::string_view(decoded), plain);
}

TEST(Ruvia, transfer_coding_stack_rejects_truncated_inner_stream) {
    constexpr std::string_view plain = "truncated stacked transfer coding";
    auto inner = gzip_compress(plain);
    inner.resize(inner.size() - 4);
    const auto wire = zlib_deflate_compress(inner);
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(
        codings, protocol_byte_limit::limited(1024));
    std::array<char, 3> scratch{};
    std::size_t cursor_value = 0;
    for (std::size_t count = 0; count < 1024; ++count) {
        const auto result_value = stack.decode(std::string_view(wire).substr(cursor_value), scratch);
        cursor_value += result_value.consumed_bytes();
        if (result_value.failure() != nullptr || result_value.decoder_failure() != nullptr) {
            EXPECT_TRUE(false);
            return;
        }
        if (result_value.output() != nullptr) {
            continue;
        }
        if (result_value.need_input() != nullptr && cursor_value == wire.size()) {
            break;
        }
        if (result_value.complete() != nullptr) {
            break;
        }
    }
    const auto finish_value = stack.finish_input();
    EXPECT_TRUE(finish_value.failure() != nullptr);
}

TEST(Ruvia, transfer_coding_stack_rejects_empty_inner_stream_after_non_empty_outer_wire) {
    const auto wire = zlib_deflate_compress("");
    EXPECT_TRUE(!wire.empty());
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(
        codings, protocol_byte_limit::limited(1024));
    std::array<char, 64> scratch{};
    const auto decoded = stack.decode(wire, scratch);
    EXPECT_EQ(decoded.consumed_bytes(), wire.size());
    EXPECT_TRUE(decoded.need_input() != nullptr);
    const auto finish_value = stack.finish_input();
    EXPECT_TRUE(finish_value.failure() != nullptr);
    if (const auto* failure = finish_value.failure()) {
        EXPECT_TRUE(failure->error() == ruvia::http_transfer_coding_decode_error::invalid_content);
    }
}

TEST(Ruvia, transfer_coding_stack_accepts_the_bounded_sequence_and_reclaims_all_stages) {
    constexpr std::string_view plain = "bounded sequence payload";
    std::array<http_transfer_coding, ruvia::max_transfer_codings> codings{};
    std::string wire(plain);
    for (std::size_t index = 0; index < codings.size(); ++index) {
        codings[index] = index % 2 == 0 ? http_transfer_coding::gzip : http_transfer_coding::deflate;
        wire = index % 2 == 0 ? gzip_compress(wire) : zlib_deflate_compress(wire);
    }
    {
        http_transfer_coding_stack_decoder decoder(codings, protocol_byte_limit::limited(4096));
        std::string output;
        EXPECT_TRUE(!append_transfer_decoded(decoder, wire, output).failed_);
        const auto finish_value = decoder.finish_input();
        EXPECT_TRUE(finish_value.complete() != nullptr);
        EXPECT_EQ(std::string_view(output), plain);
    }
}

TEST(Ruvia, transfer_coding_stack_rejects_invalid_lengths_before_constructing_stages) {
    const std::array<http_transfer_coding, ruvia::max_transfer_codings + 1> codings{};
    for (const auto sequence : {std::span<const http_transfer_coding>{}, std::span<const http_transfer_coding>(codings)}) {
        bool rejected = false;
        try {
            http_transfer_coding_stack_decoder decoder(sequence, protocol_byte_limit::unlimited());
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, transfer_coding_stack_owns_sequence_and_decodes_one_deflate_stage) {
    auto codings = std::array{http_transfer_coding::deflate};
    http_transfer_coding_stack_decoder decoder(codings, protocol_byte_limit::limited(1024));
    codings[0] = http_transfer_coding::gzip;
    const auto wire = zlib_deflate_compress("one deflate stage");
    std::string output;
    EXPECT_TRUE(!append_transfer_decoded(decoder, wire, output).failed_);
    const auto finish_value = decoder.finish_input();
    EXPECT_TRUE(finish_value.complete() != nullptr);
    EXPECT_EQ(output, "one deflate stage");
    std::array<char, 1> scratch{};
    const auto replay = decoder.decode({}, scratch);
    EXPECT_TRUE(replay.complete() != nullptr);
}

TEST(Ruvia, transfer_coding_stack_applies_decoded_limit_to_each_layer) {
    const std::string plain(4096, 'x');
    const auto wire = zlib_deflate_compress(gzip_compress(plain));
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(
        codings, protocol_byte_limit::limited(1024));
    std::array<char, 31> scratch{};
    bool exceeded = false;
    std::size_t cursor_value = 0;
    for (std::size_t count = 0; count < 1024 && !exceeded; ++count) {
        const auto input = std::string_view(wire).substr(cursor_value);
        const auto next_value = stack.decode(input, scratch);
        cursor_value += next_value.consumed_bytes();
        if (const auto* failure = next_value.failure()) {
            exceeded = failure->error() == ruvia::http_transfer_coding_decode_error::decoded_size_exceeded;
            break;
        }
        if (next_value.decoder_failure() != nullptr) {
            break;
        }
        if (next_value.need_input() != nullptr && cursor_value == wire.size()) {
            break;
        }
        if (next_value.complete() != nullptr) {
            break;
        }
    }
    EXPECT_TRUE(exceeded);
}

TEST(Ruvia, transfer_coding_stack_rejects_intermediate_limit_when_final_layer_fits) {
    constexpr std::string_view plain = "small decoded body";
    auto inner = gzip_compress(plain);
    inner[3] = static_cast<char>(static_cast<unsigned char>(inner[3]) | 0x04U);
    std::string extra;
    extra.push_back('\0');
    extra.push_back('\x04');  // RFC 1952 XLEN: 1024 extra bytes.
    extra.append(1024, 'x');
    inner.insert(10, extra);
    EXPECT_TRUE(inner.size() > 1024);

    const auto wire = zlib_deflate_compress(inner);
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(
        codings, protocol_byte_limit::limited(1024));
    std::array<char, 31> scratch{};
    bool exceeded = false;
    std::size_t cursor_value = 0;
    for (std::size_t count = 0; count < 1024 && !exceeded; ++count) {
        const auto next_value = stack.decode(std::string_view(wire).substr(cursor_value), scratch);
        cursor_value += next_value.consumed_bytes();
        if (const auto* failure = next_value.failure()) {
            exceeded = failure->error() == ruvia::http_transfer_coding_decode_error::decoded_size_exceeded;
            break;
        }
        if (next_value.decoder_failure() != nullptr || next_value.complete() != nullptr ||
            (next_value.need_input() != nullptr && cursor_value == wire.size())) {
            break;
        }
    }
    EXPECT_TRUE(exceeded);
}

TEST(Ruvia, transfer_coding_decoder_gzip_round_trip) {
    http_transfer_coding_stack_decoder decoder(
        std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(1u << 20));

    const std::string plain = "transfer-encoding gzip body content, repeated repeated repeated";
    const std::string gz = gzip_compress(plain);
    std::string output;
    EXPECT_TRUE(!append_transfer_decoded(decoder, gz, output).failed_);
    const auto finish_result = decoder.finish_input();
    EXPECT_TRUE(finish_result.complete() != nullptr);
    EXPECT_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

TEST(Ruvia, transfer_coding_decoder_gzip_decodes_every_rfc1952_member) {
    const std::string first = gzip_compress("first-");
    const std::string second = gzip_compress("second");
    EXPECT_TRUE(!first.empty());
    EXPECT_TRUE(!second.empty());

    http_transfer_coding_stack_decoder contiguous(
        std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(1024));
    std::string contiguous_output;
    EXPECT_TRUE(!append_transfer_decoded(contiguous, first + second, contiguous_output).failed_);
    const auto contiguous_finish = contiguous.finish_input();
    EXPECT_TRUE(contiguous_finish.complete() != nullptr);
    EXPECT_EQ(std::string_view(contiguous_output.data(), contiguous_output.size()), std::string_view("first-second"));

    http_transfer_coding_stack_decoder fragmented(
        std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(1024));
    std::string fragmented_output;
    EXPECT_TRUE(!append_transfer_decoded(fragmented, first, fragmented_output).failed_);
    EXPECT_TRUE(!append_transfer_decoded(fragmented, second, fragmented_output).failed_);
    const auto fragmented_finish = fragmented.finish_input();
    EXPECT_TRUE(fragmented_finish.complete() != nullptr);
    EXPECT_EQ(std::string_view(fragmented_output.data(), fragmented_output.size()), std::string_view("first-second"));
}

TEST(Ruvia, transfer_coded_chunked_request_plan_drives_decode_order) {
    const std::string plain = "RFC 9112 transfer coding followed by final chunked framing";
    const std::string gz = gzip_compress(plain);
    const std::string wire_body = chunked(gz);
    EXPECT_TRUE(!gz.empty());
    EXPECT_TRUE(!wire_body.empty());
    const std::string raw_request = std::string(
                                        "POST / HTTP/1.1\r\nHost: x\r\n"
                                        "Transfer-Encoding: gzip, chunked\r\n\r\n") +
                                    wire_body;
    const auto parsed_value = parse_http1_request_message(raw_request);
    EXPECT_TRUE(parsed_value.complete());
    const auto* chunked_body = parsed_value.state_.body_plan().chunked();
    EXPECT_TRUE(chunked_body != nullptr);
    if (chunked_body == nullptr) {
        return;
    }
    EXPECT_EQ(chunked_body->transfer_codings().size(), std::size_t{1});
    http1_chunked_body_decoder chunks({.body_limit_ = protocol_byte_limit::limited(1u << 20)});
    http_transfer_coding_stack_decoder transfer(
        chunked_body->transfer_codings(), protocol_byte_limit::limited(1u << 20));
    std::string output;
    std::string_view pending(wire_body);
    bool complete_value = false;
    while (!complete_value) {
        const auto result_value = chunks.decode(pending);
        EXPECT_TRUE(result_value.need_more() == nullptr);
        if (result_value.need_more() != nullptr) {
            break;
        }
        if (const auto* body_chunk = result_value.body_chunk()) {
            EXPECT_TRUE(!append_transfer_decoded(transfer, body_chunk->bytes(), output).failed_);
        } else if (result_value.complete() != nullptr) {
            complete_value = true;
        }
        pending.remove_prefix(result_value.consumed_bytes());
    }
    EXPECT_TRUE(complete_value);
    const auto finish_result = transfer.finish_input();
    EXPECT_TRUE(finish_result.complete() != nullptr);
    EXPECT_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

TEST(Ruvia, transfer_coded_chunked_request_accepts_empty_coded_body) {
    constexpr std::string_view wire_body = "0\r\n\r\n";
    const std::string raw_request = std::string(
                                        "POST / HTTP/1.1\r\nHost: x\r\n"
                                        "Transfer-Encoding: gzip, chunked\r\n\r\n") +
                                    std::string(wire_body);
    const auto parsed_value = parse_http1_request_message(raw_request);
    EXPECT_TRUE(parsed_value.complete());
    const auto* chunked_body = parsed_value.state_.body_plan().chunked();
    EXPECT_TRUE(chunked_body != nullptr);
    if (chunked_body == nullptr) {
        return;
    }
    http1_chunked_body_decoder chunks({.body_limit_ = protocol_byte_limit::limited(1024)});
    const auto terminal = chunks.decode(wire_body);
    EXPECT_TRUE(terminal.complete() != nullptr);
    EXPECT_EQ(terminal.consumed_bytes(), wire_body.size());

    http_transfer_coding_stack_decoder transfer(
        chunked_body->transfer_codings(), protocol_byte_limit::limited(1024));
    const auto finish_result = transfer.finish_input();
    EXPECT_TRUE(finish_result.complete() != nullptr);

    // Every layer of a stacked coding treats absent coded bytes as empty.
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    http_transfer_coding_stack_decoder stacked(codings, protocol_byte_limit::limited(1024));
    std::array<char, 16> scratch{};
    const auto drained = stacked.decode({}, scratch);
    EXPECT_TRUE(drained.need_input() != nullptr);
    const auto stacked_finish = stacked.finish_input();
    EXPECT_TRUE(stacked_finish.complete() != nullptr);
}

TEST(Ruvia, transfer_coding_decoder_rejects_bomb) {
    // A 1 MiB body compresses to a tiny gzip; the decoder must abort the
    // expansion once it passes the small cap, not stage the whole megabyte.
    http_transfer_coding_stack_decoder decoder(
        std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(1024));

    const std::string big(1u << 20, 'a');
    const std::string gz = gzip_compress(big);
    std::string output;
    const auto error = append_transfer_decoded(decoder, gz, output);
    EXPECT_TRUE(error.failed_);
    EXPECT_TRUE(error.protocol_error_.has_value());
    EXPECT_EQ(error.protocol_error_->status(), ruvia::http_status::content_too_large);
    const auto finish_value = decoder.finish_input();
    EXPECT_TRUE(finish_value.failure() != nullptr);
    if (finish_value.failure() != nullptr) {
        EXPECT_TRUE(finish_value.failure()->error() ==
                    ruvia::http_transfer_coding_decode_error::decoded_size_exceeded);
        EXPECT_TRUE(ruvia::http_request_transfer_coding_error(finish_value.failure()->error()).status() ==
                    ruvia::http_status::content_too_large);
    }
}

TEST(Ruvia, transfer_coding_decoder_rebinds_caller_storage_between_steps) {
    const std::string plain(4096, 'a');
    std::string input = gzip_compress(plain);
    http_transfer_coding_stack_decoder decoder(std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(plain.size()));
    std::array<std::array<char, 7>, 2> windows{};
    std::string decoded;
    bool drained = false;
    for (std::size_t turn = 0; turn < 2048; ++turn) {
        auto& window = windows[turn % windows.size()];
        const auto result_value = decoder.decode(input, window);
        EXPECT_TRUE(result_value.failure() == nullptr);
        EXPECT_TRUE(result_value.decoder_failure() == nullptr);
        if (const auto* output = result_value.output()) {
            decoded.append(output->bytes());
        }
        // Replace the old input allocation and poison output immediately after
        // consuming its view. The inflater must only retain its own dictionary.
        std::string next_value(input.substr(result_value.consumed_bytes()));
        input.swap(next_value);
        std::fill(window.begin(), window.end(), '#');
        if (result_value.need_input() || result_value.complete()) {
            drained = true;
            break;
        }
        if (result_value.failure() || result_value.decoder_failure()) {
            break;
        }
    }
    EXPECT_TRUE(drained);
    EXPECT_TRUE(input.empty());
    const auto finish_value = decoder.finish_input();
    EXPECT_TRUE(finish_value.complete() != nullptr);
    EXPECT_EQ(decoded, plain);
}

TEST(Ruvia, transfer_coding_decoder_rejects_unsupported_coding) {
    bool rejected = false;
    try {
        http_transfer_coding_stack_decoder decoder(std::array{static_cast<http_transfer_coding>(255)}, protocol_byte_limit::unlimited());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, transfer_coding_decoder_reports_typed_wire_failures) {
    std::array<char, std::size_t{8} * 1024> window{};

    http_transfer_coding_stack_decoder invalid(
        std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(1024));
    const auto invalid_result = invalid.decode("not-gzip", window);
    EXPECT_TRUE(invalid_result.failure() != nullptr);
    EXPECT_TRUE(invalid_result.decoder_failure() == nullptr);
    EXPECT_TRUE(invalid_result.failure()->error() ==
                ruvia::http_transfer_coding_decode_error::invalid_content);
    EXPECT_TRUE(ruvia::http_request_transfer_coding_error(invalid_result.failure()->error()).status() ==
                ruvia::http_status::bad_request);

    std::string truncated = gzip_compress("truncated");
    truncated.resize(truncated.size() - 4);
    http_transfer_coding_stack_decoder incomplete(
        std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(1024));
    std::string ignored;
    EXPECT_TRUE(!append_transfer_decoded(incomplete, truncated, ignored).failed_);
    const auto incomplete_finish = incomplete.finish_input();
    EXPECT_TRUE(incomplete_finish.failure() != nullptr);
    if (incomplete_finish.failure() != nullptr) {
        EXPECT_TRUE(incomplete_finish.failure()->error() ==
                    ruvia::http_transfer_coding_decode_error::invalid_content);
        EXPECT_TRUE(ruvia::http_request_transfer_coding_error(incomplete_finish.failure()->error()).status() ==
                    ruvia::http_status::bad_request);
    }
    const auto repeated_finish = incomplete.finish_input();
    EXPECT_TRUE(repeated_finish.failure() != nullptr);
    if (repeated_finish.failure() != nullptr) {
        EXPECT_TRUE(repeated_finish.failure()->error() ==
                    ruvia::http_transfer_coding_decode_error::invalid_content);
    }

    http_transfer_coding_stack_decoder internal_failure(
        std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(1024));
    const auto decoder_failure = internal_failure.decode("input", {});
    EXPECT_TRUE(decoder_failure.failure() == nullptr);
    EXPECT_TRUE(decoder_failure.decoder_failure() != nullptr);
    const auto repeated_decoder_failure = internal_failure.finish_input();
    EXPECT_TRUE(repeated_decoder_failure.failure() == nullptr);
    EXPECT_TRUE(repeated_decoder_failure.decoder_failure() != nullptr);

    std::string trailing = gzip_compress("complete");
    trailing.push_back('x');
    http_transfer_coding_stack_decoder extra(
        std::array{http_transfer_coding::gzip}, protocol_byte_limit::limited(1024));
    const auto trailing_error = append_transfer_decoded(extra, trailing, ignored);
    // A short prefix of another member can remain ambiguous until framing EOF.
    // It must not make the first valid member terminal, but EOF must reject it.
    EXPECT_TRUE(!trailing_error.failed_);
    const auto trailing_finish = extra.finish_input();
    EXPECT_TRUE(trailing_finish.failure() != nullptr);
    if (const auto* failure = trailing_finish.failure()) {
        EXPECT_TRUE(failure->error() == ruvia::http_transfer_coding_decode_error::invalid_content);
    }
}

// Decoding a request body: what each coding accepts and what it refuses.

using namespace ruvia::test;

TEST(Ruvia, request_body_failures_own_cross_runtime_http_errors) {
    const auto too_large =
        ruvia::http_request_body_size_failure(5, protocol_byte_limit::limited(4));
    EXPECT_TRUE(too_large.has_value());
    if (too_large) {
        const auto error = ruvia::http_request_body_protocol_error(*too_large);
        EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("request body is too large"));
    }
    EXPECT_TRUE(!ruvia::http_request_body_addition_failure(2, 2, protocol_byte_limit::limited(4)));
    EXPECT_TRUE(ruvia::http_request_body_addition_failure(2, 3, protocol_byte_limit::limited(4))
            .has_value());

    const auto incomplete = ruvia::http_request_body_protocol_error(ruvia::http_request_body_failure::incomplete);
    EXPECT_EQ(incomplete.status(), ruvia::http_status::bad_request);
    EXPECT_EQ(std::string_view(incomplete.what()), std::string_view("incomplete request body"));
}

TEST(Ruvia, http1_parse_message_classifies_cleartext_request_line_failures) {
    constexpr std::string_view invalid_request_line = "GET / NOT-HTTP\r\n\r\n";
    const auto parsed_line = parse_http1_request_message(invalid_request_line);
    const auto* line_failure = parsed_line.state_.failure();
    EXPECT_TRUE(line_failure != nullptr);
    if (line_failure != nullptr) {
        EXPECT_TRUE(line_failure->source() ==
                    ruvia::http1_server_request_parse_failure_source::request_line);
        EXPECT_TRUE(ruvia::should_drop_invalid_cleartext_http1_input(
            invalid_request_line, line_failure->source()));
    }

    const auto malformed_header = parse_http1_request_message("GET / HTTP/1.1\r\nBad Header\r\n\r\n");
    const auto* message_failure = malformed_header.state_.failure();
    EXPECT_TRUE(message_failure != nullptr);
    if (message_failure != nullptr) {
        EXPECT_TRUE(message_failure->source() ==
                    ruvia::http1_server_request_parse_failure_source::message);
        EXPECT_TRUE(!ruvia::should_drop_invalid_cleartext_http1_input(
            "GET / HTTP/1.1\r\nBad Header\r\n\r\n",
            message_failure->source()));
    }
}

TEST(Ruvia, http1_request_body_plan_has_one_framing_truth) {
    const auto none_message = parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto& none = none_message.state_.body_plan();
    EXPECT_TRUE(none.without_body() != nullptr);
    EXPECT_TRUE(none.known_length() == nullptr);
    EXPECT_TRUE(none.chunked() == nullptr);
    EXPECT_TRUE(!none.requires_consumption());

    const auto empty_length_message = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n");
    const auto& empty_length = empty_length_message.state_.body_plan();
    const auto* known_length = empty_length.known_length();
    EXPECT_TRUE(known_length != nullptr);
    EXPECT_TRUE(empty_length.without_body() == nullptr);
    EXPECT_TRUE(empty_length.chunked() == nullptr);
    if (known_length != nullptr) {
        EXPECT_EQ(known_length->content_length(), std::size_t{0});
    }
    EXPECT_TRUE(!empty_length.requires_consumption());
    EXPECT_TRUE(empty_length.expectations().has_continue());
    const auto empty_expectation_plan =
        empty_length.expectation_plan(http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(empty_expectation_plan == ruvia::http_server_expectation_plan::no_action);

    const auto compressed_chunked_message = parse_http1_request_message(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
    const auto& compressed_chunked = compressed_chunked_message.state_.body_plan();
    const auto* chunked_body = compressed_chunked.chunked();
    EXPECT_TRUE(chunked_body != nullptr);
    EXPECT_TRUE(compressed_chunked.without_body() == nullptr);
    EXPECT_TRUE(compressed_chunked.known_length() == nullptr);
    EXPECT_TRUE(compressed_chunked.requires_consumption());
    const auto compressed_expectation_plan =
        compressed_chunked.expectation_plan(http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(compressed_expectation_plan == ruvia::http_server_expectation_plan::send_continue);
    if (chunked_body != nullptr) {
        EXPECT_EQ(chunked_body->transfer_codings().size(), std::size_t{1});
    }
}

TEST(Ruvia, request_body_gzip_round_trip) {
    const std::string plain = "The quick brown fox jumps over the lazy dog";
    const std::string gz = gzip_compress(plain);
    EXPECT_TRUE(!gz.empty());
    EXPECT_EQ(decoded(http_content_coding::gzip, gz, decoded_body_limit), plain);
}

TEST(Ruvia, request_body_gzip_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny gzip
    const std::string gz = gzip_compress(big);
    // A small cap must stop the expansion, not decode the whole megabyte.
    EXPECT_TRUE(decode_error(http_content_coding::gzip, gz, 1024) ==
                http_content_decode_error::decoded_size_exceeded);
}

TEST(Ruvia, request_body_gzip_truncated_rejected) {
    const std::string plain(4096, 'q');
    std::string gz = gzip_compress(plain);
    EXPECT_TRUE(gz.size() > 6);
    gz.resize(gz.size() - 6);  // cut into the gzip trailer -> incomplete stream
    EXPECT_TRUE(decode_error(http_content_coding::gzip, gz) == http_content_decode_error::invalid_content);
}

TEST(Ruvia, request_body_gzip_decodes_every_rfc1952_member) {
    const std::string first = gzip_compress("first-");
    const std::string second = gzip_compress("second");
    EXPECT_TRUE(!first.empty());
    EXPECT_TRUE(!second.empty());
    EXPECT_EQ(decoded(http_content_coding::gzip, first + second, decoded_body_limit), std::string("first-second"));
}

TEST(Ruvia, request_body_gzip_rejects_bytes_after_the_last_member) {
    std::string encoded = gzip_compress("complete");
    encoded.append("not-a-gzip-member");
    EXPECT_TRUE(decode_error(http_content_coding::gzip, encoded) == http_content_decode_error::invalid_content);
}

TEST(Ruvia, request_body_brotli_round_trip) {
    const std::string plain = "permessage brotli body content, repeated repeated repeated";
    const std::string br = brotli_compress(plain);
    EXPECT_TRUE(!br.empty());
    EXPECT_EQ(decoded(http_content_coding::brotli, br, decoded_body_limit), plain);
}

TEST(Ruvia, request_body_brotli_bomb_rejected) {
    const std::string big(1u << 20, 'a');
    const std::string br = brotli_compress(big);
    EXPECT_TRUE(!br.empty());
    EXPECT_TRUE(decode_error(http_content_coding::brotli, br, 1024) ==
                http_content_decode_error::decoded_size_exceeded);
}

TEST(Ruvia, request_body_brotli_rejects_trailing_bytes) {
    std::string encoded = brotli_compress("complete");
    encoded.append("trailing");
    EXPECT_TRUE(decode_error(http_content_coding::brotli, encoded) ==
                http_content_decode_error::invalid_content);
}

TEST(Ruvia, request_body_zstd_round_trip) {
    const std::string plain = "zstd request body content, repeated repeated repeated repeated";
    const std::string zz = zstd_compress(plain);
    EXPECT_TRUE(!zz.empty());
    EXPECT_EQ(decoded(http_content_coding::zstd, zz, decoded_body_limit), plain);
}

TEST(Ruvia, request_body_zstd_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny zstd frame
    const std::string zz = zstd_compress(big);
    EXPECT_TRUE(!zz.empty());
    // A small cap must stop the expansion mid-stream, not decode the whole megabyte.
    EXPECT_TRUE(decode_error(http_content_coding::zstd, zz, 1024) ==
                http_content_decode_error::decoded_size_exceeded);
}

TEST(Ruvia, request_body_zstd_decodes_every_rfc8878_frame) {
    const std::string first = zstd_compress("first-");
    const std::string second = zstd_compress("second");
    EXPECT_TRUE(!first.empty());
    EXPECT_TRUE(!second.empty());
    EXPECT_EQ(decoded(http_content_coding::zstd, first + second, decoded_body_limit), std::string("first-second"));
}

TEST(Ruvia, request_body_zstd_rejects_bytes_after_the_last_frame) {
    std::string encoded = zstd_compress("complete");
    encoded.append("not-a-zstd-frame");
    EXPECT_TRUE(decode_error(http_content_coding::zstd, encoded) == http_content_decode_error::invalid_content);
}

TEST(Ruvia, http_request_content_decode_errors_own_protocol_failure_status) {
    constexpr std::array gzip{http_content_coding::gzip};
    constexpr std::array identity{http_content_coding::identity};
    constexpr std::array invalid_coding{static_cast<http_content_coding>(255)};
    const auto check_protocol_error = [&](std::span<const http_content_coding> codings,
                                          std::string_view body, std::size_t max_bytes,
                                          ruvia::http_status_code expected_status) {
        const auto result_value = decode_http_content(
            codings, body, {.max_decoded_bytes_ = max_bytes});
        const auto* failure = std::get_if<http_content_decode_error>(&result_value);
        EXPECT_TRUE(failure != nullptr);
        if (failure != nullptr) {
            EXPECT_EQ(ruvia::http_request_content_decode_error(*failure).status(), expected_status);
        }
    };
    check_protocol_error(gzip, "not-gzip", 1024, ruvia::http_status::bad_request);
    check_protocol_error(identity, "too large", 4, ruvia::http_status::content_too_large);
    check_protocol_error(invalid_coding, {}, 1024, ruvia::http_status::unsupported_media_type);
}

namespace {

ruvia::http1_request_content_writer chunked_request_writer() {
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST"}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    if (prepared_req == nullptr || prepared_req->content_plan().streaming() == nullptr) {
        throw std::logic_error("cannot prepare test request content");
    }
    return ruvia::http1_request_content_writer(*prepared_req->content_plan().streaming());
}

}  // namespace

TEST(Ruvia, http1_streaming_request_chunked_upload_and_trailers) {
    ruvia::http1_client_request_writer head_writer;
    std::array<char, 4096> head{};
    auto prepared = head_writer.prepare_streaming(ruvia::http_origin_view::https({.host_ = "example.com"}), {.method_ = "POST"}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    auto* plan = prepared_req->content_plan().streaming();
    EXPECT_TRUE(plan != nullptr);
    EXPECT_EQ(prepared_req->head(), std::string_view("POST / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n"));
    ruvia::http1_request_content_writer writer(*plan);
    const std::string payload_value = "hello";
    auto chunk = writer.plan_chunk(payload_value);
    EXPECT_TRUE((chunk.index() == 0));
    EXPECT_EQ(std::get<0>(chunk).prefix(), std::string_view("5\r\n"));
    EXPECT_EQ(std::get<0>(chunk).suffix(), std::string_view("\r\n"));
    writer.commit_chunk();
    std::array<char, 128> finish_value{};
    const std::array trailers{ruvia::http_header_view{"Digest", "value"}};
    auto final = writer.plan_finish(finish_value, trailers);
    EXPECT_TRUE((final.index() == 0));
    EXPECT_EQ(std::get<0>(final), std::string_view("0\r\nDigest: value\r\n\r\n"));
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    writer.commit_finish();
    EXPECT_TRUE(writer.finished());
}

// RFC 9110 section 6.6.2: a sender that will generate trailer fields SHOULD
// declare them in a Trailer field; only chunked content carries trailers.
TEST(Ruvia, http1_streaming_request_declares_trailer_only_for_chunked_content) {
    const std::array declared{ruvia::http_header_view{"Trailer", "Digest"}};
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST", .headers_ = declared}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    if (prepared_req != nullptr) {
        EXPECT_EQ(prepared_req->head(), std::string_view("POST / HTTP/1.1\r\nHost: example.com\r\nTrailer: Digest\r\n"
                                                         "Transfer-Encoding: chunked\r\n\r\n"));
        ruvia::http1_request_content_writer writer(*prepared_req->content_plan().streaming());
        std::array<char, 128> finish_value{};
        const std::array trailers{ruvia::http_header_view{"Digest", "value"}};
        auto final = writer.plan_finish(finish_value, trailers);
        EXPECT_TRUE((final.index() == 0));
    }

    auto known_length = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}),
        {.method_ = "POST", .headers_ = declared, .content_length_ = 3}, head);
    const auto* failure = std::get_if<ruvia::http1_client_request_prepare_error>(&known_length);
    EXPECT_TRUE(failure != nullptr && *failure == ruvia::http1_client_request_prepare_error::trailer_requires_chunked_content);

    const std::array forbidden{ruvia::http_header_view{"Trailer", "Content-Length"}};
    auto rejected = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST", .headers_ = forbidden}, head);
    const auto* invalid = std::get_if<ruvia::http1_client_request_prepare_error>(&rejected);
    EXPECT_TRUE(invalid != nullptr && *invalid == ruvia::http1_client_request_prepare_error::invalid_header);
}
TEST(Ruvia, http1_streaming_request_known_length_and_continue_gate) {
    ruvia::http1_client_request_writer head_writer;
    std::array<char, 4096> head{};
    auto prepared = head_writer.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}),
        {.method_ = "PUT", .content_length_ = 3}, head, {.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    EXPECT_TRUE((prepared_req->head().find("Content-Length: 3\r\n") != std::string_view::npos));
    ruvia::http1_request_content_writer writer(*prepared_req->content_plan().streaming());
    const std::string payload_value = "abc";
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    writer.release_content();
    EXPECT_TRUE(!(writer.plan_finish({}).index() == 0));
    auto chunk = writer.plan_chunk(payload_value);
    EXPECT_TRUE((chunk.index() == 0));
    EXPECT_TRUE(std::get<0>(chunk).prefix().empty());
    EXPECT_TRUE(std::get<0>(chunk).suffix().empty());
    writer.commit_chunk();
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    auto finish_value = writer.plan_finish({});
    EXPECT_TRUE((finish_value.index() == 0));
    EXPECT_TRUE(std::get<0>(finish_value).empty());
    writer.commit_finish();
    EXPECT_TRUE(ruvia::test::throws_on([&] { writer.commit_chunk(); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { writer.commit_finish(); }));
}
TEST(Ruvia, http1_streaming_request_rejects_forbidden_trailer_without_mutating_buffer) {
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST"}, head);
    ruvia::http1_request_content_writer writer(*std::get<0>(prepared).content_plan().streaming());
    std::array<char, 128> finish_value{};
    finish_value.fill('X');
    const std::array trailers{ruvia::http_header_view{"Content-Length", "0"}};
    auto rejected = writer.plan_finish(finish_value, trailers);
    EXPECT_TRUE(!(rejected.index() == 0));
    EXPECT_EQ(finish_value.front(), 'X');
    EXPECT_TRUE((writer.plan_finish(finish_value).index() == 0));
}

TEST(Ruvia, http1_streaming_request_finish_accepts_trailers_borrowed_from_output) {
    enum class alias_kind { name,
        value,
        disjoint,
        overlapping,
        cross_field };
    for (const auto kind : {alias_kind::name, alias_kind::value, alias_kind::disjoint,
             alias_kind::overlapping, alias_kind::cross_field}) {
        auto writer = chunked_request_writer();
        std::array<char, 128> buffer{};
        buffer.fill('#');
        std::array<ruvia::http_header_view, 2> fields{};
        std::size_t count = 1;
        std::string expected = "0\r\nDigest: value\r\n\r\n";
        const auto copy = [&](std::size_t offset, std::string_view text) {
            std::copy(text.begin(), text.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            return std::string_view(buffer.data() + offset, text.size());
        };
        switch (kind) {
            case alias_kind::name:
                fields[0] = {copy(0, "Digest"), "value"};
                break;
            case alias_kind::value:
                fields[0] = {"Digest", copy(0, "value")};
                break;
            case alias_kind::disjoint:
                fields[0] = {copy(0, "Digest"), copy(16, "value")};
                break;
            case alias_kind::overlapping: {
                const auto text = copy(0, "DigestValue");
                fields[0] = {text.substr(0, 6), text.substr(3)};
                expected = "0\r\nDigest: estValue\r\n\r\n";
                break;
            }
            case alias_kind::cross_field:
                fields[0] = {"X-First", copy(32, "first")};
                fields[1] = {copy(0, "Digest"), "second"};
                count = 2;
                expected = "0\r\nX-First: first\r\nDigest: second\r\n\r\n";
                break;
        }
        const auto trailers = std::span(fields).first(count);
        const auto original = buffer;
        const auto too_small = writer.plan_finish(std::span(buffer).first(expected.size() - 1), trailers);
        EXPECT_TRUE(too_small.index() == 1);
        if (too_small.index() == 1) {
            EXPECT_TRUE(std::get<1>(too_small) == ruvia::http1_request_content_write_error::output_too_small);
        }
        EXPECT_TRUE(buffer == original);
        EXPECT_TRUE(!writer.finished());

        const auto planned = writer.plan_finish(buffer, trailers);
        EXPECT_TRUE(planned.index() == 0);
        if (planned.index() == 0) {
            EXPECT_EQ(std::get<0>(planned), std::string_view(expected));
        }
        EXPECT_EQ(std::string_view(buffer.data() + expected.size(), buffer.size() - expected.size()), std::string_view(original.data() + expected.size(), original.size() - expected.size()));
        const auto repeated = writer.plan_finish(buffer, trailers);
        EXPECT_TRUE(repeated.index() == 1);
        if (repeated.index() == 1) {
            EXPECT_TRUE(std::get<1>(repeated) == ruvia::http1_request_content_write_error::write_pending);
        }
        writer.commit_finish();
        EXPECT_TRUE(writer.finished());
        const auto finished = writer.plan_finish(buffer);
        EXPECT_TRUE(finished.index() == 1);
        if (finished.index() == 1) {
            EXPECT_TRUE(std::get<1>(finished) == ruvia::http1_request_content_write_error::stopped);
        }
    }
}

TEST(Ruvia, http1_streaming_request_finish_accepts_output_alias_at_header_byte_limit) {
    auto writer = chunked_request_writer();
    constexpr std::size_t framing_bytes = std::string_view("0\r\nDigest: \r\n\r\n").size();
    const std::string value(ruvia::max_http_header_bytes - framing_bytes, 'v');
    std::string buffer(ruvia::max_http_header_bytes, 'v');
    const std::array trailers{ruvia::http_header_view{"Digest", std::string_view(buffer).substr(0, value.size())}};
    const auto planned = writer.plan_finish(std::span(buffer.data(), buffer.size()), trailers);
    EXPECT_TRUE(planned.index() == 0);
    if (planned.index() == 0) {
        const std::string expected = "0\r\nDigest: " + value + "\r\n\r\n";
        EXPECT_EQ(std::get<0>(planned), std::string_view(expected));
    }
    writer.commit_finish();
    EXPECT_TRUE(writer.finished());
}

TEST(Ruvia, http1_streaming_request_finish_accepts_trailer_descriptors_stored_in_output) {
    auto writer = chunked_request_writer();
    // The descriptor array itself lives where the trailer section is written, so
    // every descriptor must be consumed before the first output byte lands.
    alignas(ruvia::http_header_view) std::array<char, 256> buffer{};
    auto* const descriptors = ::new (static_cast<void*>(buffer.data()))
        ruvia::http_header_view[2]{{"X-One", "first"}, {"X-Two", "second"}};
    const std::span<const ruvia::http_header_view> trailers(descriptors, 2);
    const auto planned = writer.plan_finish(std::span(buffer.data(), buffer.size()), trailers);
    EXPECT_TRUE(planned.index() == 0);
    if (planned.index() == 0) {
        EXPECT_EQ(std::get<0>(planned), std::string_view("0\r\nX-One: first\r\nX-Two: second\r\n\r\n"));
    }
    writer.commit_finish();
}

TEST(Ruvia, http_content_length_rejects_negative_sources_without_mutating_existing_options) {
    ruvia::http1_client_request_head_view request{.method_ = "POST", .target_ = "/upload", .content_length_ = 3};
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(!ruvia::http_content_length::try_from_value(invalid).has_value());
        EXPECT_TRUE(ruvia::test::throws_on([&] { request.content_length_ = invalid; }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::http_request_content_plan::known_length(invalid); }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::http_request_content_plan::streaming(invalid); }));
    }
    std::array<char, 256> head{};
    const auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), request, head);
    const auto* retained = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(retained != nullptr && retained->content_plan().streaming() != nullptr);
    if (retained && retained->content_plan().streaming()) {
        EXPECT_TRUE(retained->content_plan().streaming()->content_length() == 3);
        EXPECT_TRUE(retained->head().find("Content-Length: 3\r\n") != std::string_view::npos);
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto h2_known = ruvia::http_request_content_plan::known_length(maximum);
    const auto h2_streaming = ruvia::http_request_content_plan::streaming(maximum);
    EXPECT_TRUE(h2_known.known_length_content() && h2_known.known_length_content()->length() == maximum);
    EXPECT_TRUE(h2_streaming.streaming_content() && h2_streaming.streaming_content()->expected_length() == maximum);
}

TEST(Ruvia, http1_chunk_header_encodes_lowercase_hex_and_crlf) {
    const ruvia::http1_chunk_header zero(0);
    const ruvia::http1_chunk_header fifteen(15);
    const ruvia::http1_chunk_header sixteen(16);
    const ruvia::http1_chunk_header abc(0xabc);
    EXPECT_EQ(zero.view(), std::string_view("0\r\n"));
    EXPECT_EQ(fifteen.view(), std::string_view("f\r\n"));
    EXPECT_EQ(sixteen.view(), std::string_view("10\r\n"));
    EXPECT_EQ(abc.view(), std::string_view("abc\r\n"));
}

TEST(Ruvia, http1_chunk_header_buffer_covers_size_t_max) {
    const ruvia::http1_chunk_header header_value((std::numeric_limits<std::size_t>::max)());
    const auto encoded = header_value.view();
    EXPECT_EQ(encoded.size(), sizeof(std::size_t) * 2 + 2);
    EXPECT_TRUE(encoded.ends_with("\r\n"));
}

TEST(Ruvia, http_authority_host_public_parse_preserves_ip_literal_brackets) {
    EXPECT_EQ(ruvia::parse_http_authority_host("[::1]:8080").value(), std::string_view("[::1]"));
    EXPECT_EQ(ruvia::parse_http_authority_host("example.test:443").value(), std::string_view("example.test"));
    EXPECT_TRUE(!ruvia::parse_http_authority_host("user@example.test"));
    EXPECT_TRUE(ruvia::is_valid_http_ipv4_literal("127.0.0.1"));
    EXPECT_TRUE(!ruvia::is_valid_http_ipv4_literal("999.0.0.1"));
    EXPECT_TRUE(ruvia::is_valid_http_ipv6_literal("::1"));
    EXPECT_TRUE(!ruvia::is_valid_http_ipv6_literal("not-an-ip"));
}

TEST(Ruvia, http_authority_host_public_parse_distinguishes_empty_host_from_invalid_authority) {
    const auto empty_host = ruvia::parse_http_authority_host("");
    EXPECT_TRUE(empty_host.has_value());
    if (empty_host) {
        EXPECT_TRUE(empty_host->empty());
    }

    EXPECT_TRUE(!ruvia::parse_http_authority(""));
    EXPECT_TRUE(!ruvia::is_valid_http_connect_authority(""));
    EXPECT_TRUE(!ruvia::is_valid_http_connect_authority(":443"));
    for (const std::string_view invalid : {":", ":80", "user@example.test", " ", "[::1", "example.test:65536", "example.test:-1"}) {
        EXPECT_TRUE(!ruvia::parse_http_authority_host(invalid));
    }
}

TEST(Ruvia, http_ip_literal_validation_preserves_compression_and_tail_bounds) {
    for (const auto valid : {"::", "1::", "::1", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7::",
             "1::2:3:4:5:6:7", "::ffff:192.0.2.1", "1:2:3:4:5:6:192.0.2.1"}) {
        EXPECT_TRUE(ruvia::is_valid_http_ipv6_literal(valid));
    }
    for (const auto invalid : {"", ":", ":::1", "1:::2", "1::2::3", "1:2:3:4:5:6:7",
             "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8::", "1:2:3:4:5:6::7:8",
             "1:2:3:4:5:6:7:192.0.2.1", "1:2:3:4:5::6:192.0.2.1",
             "::ffff:192.00.2.1", "::ffff:192.0.2.256", "::ffff:192.0.2.1:",
             "::ffff:192.0.2", "::ffff:192.0.2.1suffix", "12345::", "::1%zone", "[::1]"}) {
        EXPECT_TRUE(!ruvia::is_valid_http_ipv6_literal(invalid));
    }
    for (const auto invalid : {"", "1.2.3", "1.2.3.4.5", "01.2.3.4", "1.2.3.256", "1.2.3.4suffix"}) {
        EXPECT_TRUE(!ruvia::is_valid_http_ipv4_literal(invalid));
    }
    EXPECT_TRUE(ruvia::is_valid_http_ipv4_literal("0.0.0.0"));
    EXPECT_TRUE(ruvia::is_valid_http_ipv4_literal("255.255.255.255"));
}

namespace {

template <typename fn_type>
[[nodiscard]] bool throws_head_error(fn_type&& fn) {
    try {
        fn();
    } catch (const ruvia::http1_response_head_error&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(Ruvia, http1_connect_head_transfers_to_tunnel_without_message_framing_for_every_successful_status) {
    for (const auto version : {ruvia::http_protocol_version::http10, ruvia::http_protocol_version::http11}) {
        for (unsigned status = 200; status != 300; ++status) {
            ruvia::http_response response;
            response.status(ruvia::http_status_code::from_value(status));
            response.header("x-tunnel", "established");
            const auto plan = ruvia::prepare_http1_connect_response_head(response, version);
            ruvia::http_response_head_buffer head;
            ruvia::append_http1_response_head(response, head, plan);
            EXPECT_TRUE(head.view().starts_with("HTTP/1.1 "));
            EXPECT_TRUE(head.view().find("x-tunnel: established\r\n") != std::string_view::npos);
            EXPECT_TRUE(head.view().find("Content-Length:") == std::string_view::npos);
            EXPECT_TRUE(head.view().find("Transfer-Encoding:") == std::string_view::npos);
            EXPECT_TRUE(head.view().ends_with("\r\n\r\n"));
        }
    }
}

TEST(Ruvia, http1_connect_head_rejects_failed_status_payload_and_framing_fields_before_commit) {
    for (const auto name : {"Content-Length", "Transfer-Encoding"}) {
        ruvia::http_response response;
        response.header(name, name == std::string_view("Content-Length") ? "0" : "chunked");
        EXPECT_TRUE(throws_head_error([&] {
            (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
        }));
    }
    ruvia::http_response response;
    response.status(ruvia::http_status::forbidden);
    EXPECT_TRUE(throws_head_error([&] {
        (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
    }));
    response.status(ruvia::http_status::ok);
    response.body("HTTP payload");
    EXPECT_TRUE(throws_head_error([&] {
        (void)ruvia::prepare_http1_connect_response_head(response, ruvia::http_protocol_version::http11);
    }));
}

namespace {

using ruvia::test::parse_http1_request_message;

[[nodiscard]] bool offers_upgrade(std::string_view wire) {
    const auto parsed = parse_http1_request_message(wire);
    EXPECT_TRUE(parsed.complete()) << wire;
    return parsed.complete() && ruvia::http1_request_offers_upgrade(parsed.state_.request());
}

[[nodiscard]] std::size_t field_count(const ruvia::http_response& response, std::string_view name) {
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        count += ruvia::http_ascii_equals_ignore_case(header.name(), name) ? 1 : 0;
    }
    return count;
}

// The 101 head `response` becomes for `request_wire`, or nothing when
// preparing it throws http1_response_head_error.
[[nodiscard]] std::string upgrade_head(ruvia::http_response& response, std::string_view request_wire) {
    const auto parsed = parse_http1_request_message(request_wire);
    EXPECT_TRUE(parsed.complete()) << request_wire;
    try {
        const auto plan = ruvia::prepare_http1_upgrade_response_head(response, parsed.state_.request());
        EXPECT_TRUE(plan.close_delimited_stream() != nullptr);
        ruvia::http_response_head_buffer head;
        ruvia::append_http1_response_head(response, head, plan);
        return std::string(head.view());
    } catch (const ruvia::http1_response_head_error&) {
        return {};
    }
}

constexpr std::string_view offering_request =
    "GET /chat HTTP/1.1\r\nHost: example.test\r\nConnection: keep-alive, Upgrade\r\nUpgrade: echo/1, other\r\n\r\n";

}  // namespace

// RFC 9110 §7.8: only an HTTP/1.1 request that lists the Upgrade connection
// option beside an Upgrade field naming a protocol asks to switch; CONNECT
// establishes a tunnel through its 2xx instead.
TEST(Ruvia, http1_upgrade_offer_needs_http11_the_upgrade_option_and_a_protocol) {
    EXPECT_TRUE(offers_upgrade(offering_request));
    EXPECT_TRUE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nconnection: UPGRADE\r\nupgrade: Echo\r\n\r\n"));
    EXPECT_TRUE(offers_upgrade(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nConnection: upgrade\r\n"
        "Upgrade: , echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nConnection: upgrade\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nConnection: upgrade\r\nUpgrade: ,\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.0\r\nHost: x\r\nConnection: upgrade\r\nUpgrade: echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade(
        "CONNECT example.test:443 HTTP/1.1\r\nHost: example.test:443\r\n"
        "Connection: upgrade\r\nUpgrade: echo\r\n\r\n"));
}

// Without an application Upgrade field the 101 names the first offered
// protocol; the head carries no framing and the connection option is added
// once, beside the application's own options.
TEST(Ruvia, http1_upgrade_head_switches_to_an_offered_protocol_without_framing) {
    {
        ruvia::http_response response;
        response.header("x-tunnel", "echo");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_EQ(response.status().value(), 101);
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n")) << head;
        EXPECT_NE(head.find("\r\nUpgrade: echo/1\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nConnection: Upgrade\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nx-tunnel: echo\r\n"), std::string::npos) << head;
        EXPECT_EQ(head.find("Content-Length:"), std::string::npos) << head;
        EXPECT_EQ(head.find("Transfer-Encoding:"), std::string::npos) << head;
        EXPECT_TRUE(head.ends_with("\r\n\r\n")) << head;
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "OTHER");
        response.header("Connection", "upgrade");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_NE(head.find("\r\nUpgrade: OTHER\r\n"), std::string::npos) << head;
        EXPECT_EQ(head.find("echo/1"), std::string::npos) << head;
        EXPECT_EQ(field_count(response, "Upgrade"), 1U);
        EXPECT_EQ(field_count(response, "Connection"), 1U);
    }
    {
        ruvia::http_response response;
        response.header("Connection", "x-hop");
        response.header("x-hop", "1");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n")) << head;
        EXPECT_NE(head.find("\r\nConnection: x-hop\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nConnection: Upgrade\r\n"), std::string::npos) << head;
    }
}

// A request without an offer, a protocol the client did not offer, content
// and framing fields are refused before the response changes.
TEST(Ruvia, http1_upgrade_head_rejects_unoffered_switches_content_and_framing_unchanged) {
    const auto refused_unchanged = [&](ruvia::http_response& response, std::string_view request_wire) {
        const auto fields = response.headers().size();
        const auto refused = upgrade_head(response, request_wire).empty();
        return refused && response.status().value() == 200 && response.headers().size() == fields;
    };
    {
        ruvia::http_response response;
        EXPECT_TRUE(refused_unchanged(response, "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\n\r\n"));
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "echo/2");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "other, unoffered");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    {
        ruvia::http_response response;
        response.body("payload");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    for (const auto& [name, value] : {std::pair<std::string_view, std::string_view>{"Content-Length", "0"},
             std::pair<std::string_view, std::string_view>{"Transfer-Encoding", "chunked"}}) {
        ruvia::http_response response;
        response.header(name, value);
        EXPECT_TRUE(refused_unchanged(response, offering_request)) << name;
    }
}

namespace {

using ruvia::http1_interim_connection_disposition;
using ruvia::http1_interim_response_buffer_too_small;
using ruvia::http1_response_head_error;
using ruvia::http_header_view;
using ruvia::http_interim_response_head;
using ruvia::prepare_http1_interim_response_head;
using ruvia::prepared_http1_interim_response;

[[nodiscard]] bool unchanged(const std::array<char, 64>& buffer, char sentinel) {
    return std::ranges::all_of(buffer, [sentinel](char value) { return value == sentinel; });
}

// Rejection throws before the head buffer is touched.
[[nodiscard]] bool rejects_without_writing(std::span<const http_header_view> fields_value, char sentinel) {
    std::array<char, 64> buffer;
    buffer.fill(sentinel);
    try {
        (void)prepare_http1_interim_response_head(
            http_interim_response_head(ruvia::http_status::early_hints, fields_value), buffer);
    } catch (const http1_response_head_error&) {
        return unchanged(buffer, sentinel);
    }
    return false;
}

}  // namespace

TEST(Ruvia, http1_interim_response_writer_emits_exact_typed_head) {
    std::array<char, 64> buffer{};
    const auto result_value = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::continue_value), buffer);
    const auto* const prepared = std::get_if<prepared_http1_interim_response>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_EQ(prepared->head(), std::string_view("HTTP/1.1 100 Continue\r\n\r\n"));
        EXPECT_EQ(prepared->connection_disposition(), http1_interim_connection_disposition::unchanged);
    }

    const http_header_view hints[] = {
        {"Link", "</style.css>; rel=preload"},
        {"Content-Type", "text/html; charset=utf-8"},
        {"X-Hint", "warm"},
    };
    std::array<char, 256> hints_buffer{};
    const auto hints_result = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::early_hints, hints), hints_buffer);
    const auto* const prepared_hints = std::get_if<prepared_http1_interim_response>(&hints_result);
    EXPECT_TRUE(prepared_hints != nullptr);
    if (prepared_hints != nullptr) {
        EXPECT_EQ(prepared_hints->head(), std::string_view("HTTP/1.1 103 Early Hints\r\n"
                                                           "Link: </style.css>; rel=preload\r\n"
                                                           "Content-Type: text/html; charset=utf-8\r\n"
                                                           "X-Hint: warm\r\n\r\n"));
        EXPECT_TRUE(!(prepared_hints->head().find("Server:") != std::string_view::npos));
        EXPECT_TRUE(!(prepared_hints->head().find("Date:") != std::string_view::npos));
    }
}

TEST(Ruvia, http1_interim_response_writer_preserves_required_status_line_space) {
    std::array<char, 32> buffer{};
    const auto result_value = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status_code::from_value(199)), buffer);
    const auto* const prepared = std::get_if<prepared_http1_interim_response>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        // RFC 9112 section 4 requires the SP after status-code even when the
        // optional reason phrase is absent.
        EXPECT_EQ(prepared->head(), std::string_view("HTTP/1.1 199 \r\n\r\n"));
    }
}

TEST(Ruvia, http1_interim_response_writer_closes_after_containing_response) {
    const http_header_view fields_value[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "example/1"},
    };
    std::array<char, 128> buffer{};
    const auto result_value = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::early_hints, fields_value), buffer);
    const auto* const prepared = std::get_if<prepared_http1_interim_response>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_EQ(prepared->connection_disposition(), http1_interim_connection_disposition::close_after_interim_response);
        EXPECT_TRUE((prepared->head().find("Upgrade: example/1\r\n") != std::string_view::npos));
    }
}

TEST(Ruvia, http1_interim_response_writer_buffer_too_small_is_transactional) {
    std::array<char, 8> buffer;
    buffer.fill('#');
    const auto result_value = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::continue_value), buffer);
    const auto* const too_small = std::get_if<http1_interim_response_buffer_too_small>(&result_value);
    EXPECT_TRUE(too_small != nullptr);
    if (too_small != nullptr) {
        EXPECT_EQ(too_small->required_head_bytes(), std::size_t{25});
    }
    EXPECT_TRUE(std::ranges::all_of(buffer, [](char value) { return value == '#'; }));
}

TEST(Ruvia, http1_interim_response_writer_preserves_fields_borrowed_from_output) {
    constexpr std::string_view expected = "HTTP/1.1 103 Early Hints\r\nX-Hint: warm\r\nX-Next: ready\r\n\r\n";
    for (const std::size_t offset : {std::size_t{0}, std::size_t{24}, std::size_t{32}}) {
        std::array<char, 128> buffer;
        buffer.fill('#');
        constexpr std::string_view borrowed = "X-HintwarmX-Nextready";
        std::ranges::copy(borrowed, buffer.begin() + offset);
        const auto input = std::string_view(buffer.data() + offset, borrowed.size());
        const std::array fields_value{
            http_header_view(input.substr(0, 6), input.substr(6, 4)),
            http_header_view(input.substr(10, 6), input.substr(16, 5)),
        };
        const auto result_value = prepare_http1_interim_response_head(
            http_interim_response_head(ruvia::http_status::early_hints, fields_value), buffer);
        const auto* const prepared = std::get_if<prepared_http1_interim_response>(&result_value);
        EXPECT_TRUE(prepared != nullptr);
        if (prepared != nullptr) {
            EXPECT_EQ(prepared->head(), expected);
            EXPECT_EQ(prepared->head().data(), buffer.data());
            EXPECT_EQ(buffer[expected.size()], '#');
        }
    }
}

TEST(Ruvia, http1_interim_response_writer_rejects_invalid_fields_transactionally) {
    const http_header_view malformed_name[] = {{"Bad Name", "value"}};
    const http_header_view malformed_value[] = {{"X-Test", "a\r\nb"}};
    const http_header_view malformed_content_encoding[] = {{"Content-Encoding", "gzip;level=9"}};
    const http_header_view empty_content_encoding[] = {{"Content-Encoding", ""}};
    const http_header_view malformed_content_type[] = {{"Content-Type", "not a media type"}};
    const http_header_view empty_content_type[] = {{"Content-Type", ""}};
    const http_header_view content_length[] = {{"Content-Length", "0"}};
    const http_header_view transfer_encoding[] = {{"Transfer-Encoding", "chunked"}};
    const http_header_view trailer[] = {{"Trailer", "X-Checksum"}};
    const http_header_view te[] = {{"TE", "trailers"}};
    const http_header_view duplicate_server[] = {
        {"Server", "one"},
        {"server", "two"},
    };
    const http_header_view invalid_connection[] = {{"Connection", ","}};
    const http_header_view managed_connection[] = {
        {"Connection", "close, date"},
    };
    const http_header_view invalid_upgrade[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "bad protocol"},
    };
    const http_header_view unlisted_upgrade[] = {{"Upgrade", "example/1"}};

    for (const std::span<const http_header_view> fields_value :
        {std::span<const http_header_view>(malformed_name), std::span<const http_header_view>(malformed_value),
            std::span<const http_header_view>(malformed_content_encoding),
            std::span<const http_header_view>(empty_content_encoding),
            std::span<const http_header_view>(malformed_content_type),
            std::span<const http_header_view>(empty_content_type), std::span<const http_header_view>(content_length),
            std::span<const http_header_view>(transfer_encoding), std::span<const http_header_view>(trailer),
            std::span<const http_header_view>(te), std::span<const http_header_view>(duplicate_server),
            std::span<const http_header_view>(invalid_connection),
            std::span<const http_header_view>(managed_connection),
            std::span<const http_header_view>(invalid_upgrade), std::span<const http_header_view>(unlisted_upgrade)}) {
        EXPECT_TRUE(rejects_without_writing(fields_value, '@'));
    }
}

TEST(Ruvia, http1_interim_response_writer_enforces_size_limit) {
    std::vector<http_header_view> many(80, http_header_view("X-Hint", "value"));
    std::array<char, 2048> many_buffer{};
    const auto many_result = prepare_http1_interim_response_head(
        http_interim_response_head(ruvia::http_status::early_hints, many),
        many_buffer);
    EXPECT_TRUE(std::holds_alternative<prepared_http1_interim_response>(many_result));

    const std::string oversized_value(ruvia::max_http_header_bytes, 'x');
    const http_header_view oversized[] = {{"X-Hint", oversized_value}};
    EXPECT_TRUE(rejects_without_writing(oversized, '!'));
}

static_assert(std::is_constructible_v<ruvia::borrowed_http_headers, std::vector<http_header_view>&>);
static_assert(std::is_constructible_v<ruvia::borrowed_http_headers, const std::vector<http_header_view>&>);
static_assert(std::is_constructible_v<ruvia::borrowed_http_headers, const std::array<http_header_view, 1>&>);
static_assert(!std::is_constructible_v<ruvia::borrowed_http_headers, std::vector<http_header_view>>);
static_assert(!std::is_constructible_v<ruvia::borrowed_http_headers, std::array<http_header_view, 1>>);
static_assert(!std::is_constructible_v<ruvia::borrowed_http_headers, std::initializer_list<http_header_view>>);
static_assert(std::is_constructible_v<http_interim_response_head, ruvia::http_status_code, const std::vector<http_header_view>&>);
static_assert(!std::is_constructible_v<http_interim_response_head, ruvia::http_status_code, std::vector<http_header_view>>);
static_assert(!std::is_constructible_v<http_interim_response_head, ruvia::http_status_code, std::array<http_header_view, 1>>);
