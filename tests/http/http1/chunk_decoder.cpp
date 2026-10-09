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
#include "ruvia/http/http_request_body_failure.h"
#include "ruvia/http/protocol_byte_limit.h"

#include "test_harness.h"

namespace {

using ruvia::http1_chunk_trailer_role;
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

RUVIA_TEST(protocol_byte_limit_has_no_numeric_sentinel) {
    const auto unlimited = protocol_byte_limit::unlimited();
    RUVIA_CHECK(!unlimited.is_limited());
    RUVIA_CHECK(!unlimited.maximum().has_value());
    RUVIA_CHECK(!unlimited.exceeds((std::numeric_limits<std::size_t>::max)()));
    RUVIA_CHECK(unlimited.addition_exceeds((std::numeric_limits<std::size_t>::max)(), 1));

    const auto limited = protocol_byte_limit::limited(8);
    RUVIA_CHECK(limited.is_limited());
    RUVIA_CHECK(limited.maximum().has_value());
    RUVIA_CHECK_EQ(limited.maximum().value(), std::size_t{8});
    RUVIA_CHECK(!limited.exceeds(8));
    RUVIA_CHECK(limited.exceeds(9));
    RUVIA_CHECK(!limited.addition_exceeds(3, 5));
    RUVIA_CHECK(limited.addition_exceeds(3, 6));

    bool rejected_zero = false;
    try {
        (void)protocol_byte_limit::limited(0);
    } catch (const std::invalid_argument&) {
        rejected_zero = true;
    }
    RUVIA_CHECK(rejected_zero);
}

RUVIA_TEST(chunked_body_decoder_reports_typed_size_and_limit_failures) {
    http1_chunked_body_decoder invalid({.body_limit_ = protocol_byte_limit::unlimited()});
    const auto invalid_result = invalid.decode("xyz\r\n");
    RUVIA_CHECK(invalid_result.failure() != nullptr);
    RUVIA_CHECK(invalid_result.failure()->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    RUVIA_CHECK_EQ(ruvia::http_request_chunk_decode_error(invalid_result.failure()->error()).status(),
        ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(std::string_view(
                       ruvia::http_request_chunk_decode_error(invalid_result.failure()->error()).what()),
        std::string_view("invalid chunked request body"));
    const auto repeated_invalid = invalid.decode("0\r\n\r\n");
    RUVIA_CHECK(repeated_invalid.failure() != nullptr);
    RUVIA_CHECK(repeated_invalid.failure()->error() ==
                ruvia::http1_chunk_decode_error::invalid_framing);
    RUVIA_CHECK_EQ(repeated_invalid.consumed_bytes(), std::size_t{0});

    http1_chunked_body_decoder single_limit({.body_limit_ = protocol_byte_limit::limited(10)});
    const auto single_limit_result = single_limit.decode("b\r\n");
    RUVIA_CHECK(single_limit_result.failure() != nullptr);
    RUVIA_CHECK(single_limit_result.failure()->error() ==
                ruvia::http1_chunk_decode_error::body_limit_exceeded);
    RUVIA_CHECK_EQ(ruvia::http_request_chunk_decode_error(single_limit_result.failure()->error()).status(),
        ruvia::http_status::content_too_large);
    RUVIA_CHECK_EQ(single_limit_result.consumed_bytes(), std::size_t{3});

    http1_chunked_body_decoder accumulated({.body_limit_ = protocol_byte_limit::limited(10)});
    const std::string_view wire = "8\r\n12345678\r\n5\r\nabcde\r\n0\r\n\r\n";
    const auto first = accumulated.decode(wire);
    RUVIA_CHECK(first.body_chunk() != nullptr);
    const auto second = accumulated.decode(wire.substr(first.consumed_bytes()));
    RUVIA_CHECK(second.failure() != nullptr);
    RUVIA_CHECK(second.failure()->error() == ruvia::http1_chunk_decode_error::body_limit_exceeded);
    RUVIA_CHECK_EQ(ruvia::http_request_chunk_decode_error(second.failure()->error()).status(),
        ruvia::http_status::content_too_large);
}

RUVIA_TEST(chunked_body_decoder_separates_body_and_framing_budgets) {
    http1_chunked_body_decoder tiny_body({.body_limit_ = protocol_byte_limit::limited(1)});
    constexpr std::string_view tiny_wire = "1\r\nx\r\n0\r\n\r\n";
    const auto body = tiny_body.decode(tiny_wire);
    RUVIA_CHECK(body.body_chunk() != nullptr);
    if (const auto* chunk = body.body_chunk()) {
        RUVIA_CHECK_EQ(chunk->bytes(), std::string_view("x"));
        const auto terminal = tiny_body.decode(tiny_wire.substr(chunk->consumed_bytes()));
        RUVIA_CHECK(terminal.complete() != nullptr);
    }

    http1_chunked_body_decoder framing_flood({.body_limit_ = protocol_byte_limit::unlimited()});
    std::string size_line = "1;x=";
    size_line.append(ruvia::max_http_header_bytes / 2 - size_line.size() - 2, 'a');
    size_line.append("\r\n");
    const std::string flood_wire = size_line + "x\r\n" + size_line + "y\r\n0\r\n\r\n";

    const auto first = framing_flood.decode(flood_wire);
    RUVIA_CHECK(first.body_chunk() != nullptr);
    if (const auto* chunk = first.body_chunk()) {
        const auto excessive =
            framing_flood.decode(std::string_view(flood_wire).substr(chunk->consumed_bytes()));
        RUVIA_CHECK(excessive.failure() != nullptr);
        if (const auto* failure = excessive.failure()) {
            RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
        }
    }
}

RUVIA_TEST(chunked_body_decoder_emits_zero_copy_chunks_and_preserves_pipeline) {
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
            RUVIA_CHECK_EQ(complete->trailers(), std::string_view("X-Trace: abc"));
            break;
        }
        RUVIA_CHECK(result_value.need_more() == nullptr);
        break;
    }

    RUVIA_CHECK_EQ(body, std::string("hello world"));
    RUVIA_CHECK_EQ(wire.substr(consumed), std::string_view("NEXT"));
}

RUVIA_TEST(chunked_body_decoder_handles_single_byte_input_fragmentation) {
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

    RUVIA_CHECK(complete_value);
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
            http1_chunked_body_decoder decoder;
            const auto first = decoder.decode(std::string_view(wire).substr(0, split));
            RUVIA_CHECK(first.need_more() != nullptr);
            RUVIA_CHECK(first.consumed_bytes() <= split);
            const auto last = decoder.decode(std::string_view(wire).substr(first.consumed_bytes()));
            RUVIA_CHECK(last.complete() != nullptr);
            if (const auto* complete = last.complete()) {
                RUVIA_CHECK_EQ(complete->trailers(), std::string_view(trailers));
                RUVIA_CHECK_EQ(complete->trailers().data(), wire.data() + 3);
                const auto consumed = first.consumed_bytes() + complete->consumed_bytes();
                RUVIA_CHECK_EQ(consumed, message.size());
                RUVIA_CHECK_EQ(std::string_view(wire).substr(consumed), std::string_view("NEXT"));
            }
        }
    }
}

RUVIA_TEST(chunked_body_decoder_handles_deterministic_arbitrary_fragmented_bytes) {
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
                RUVIA_CHECK_EQ(alternatives, 1U);
                RUVIA_CHECK(result_value.consumed_bytes() <= pending.size());

                if (const auto* body = result_value.body_chunk()) {
                    RUVIA_CHECK(!body->bytes().empty());
                    RUVIA_CHECK(pending.find(body->bytes()) != std::string::npos);
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
                RUVIA_CHECK(result_value.consumed_bytes() != 0);
            }
            if (terminal) {
                break;
            }
        }
    }
}

RUVIA_TEST(chunked_body_decoder_rejects_bad_delimiter_and_trailer) {
    http1_chunked_body_decoder delimiter({.body_limit_ = protocol_byte_limit::limited(1024)});
    const auto bad_delimiter = delimiter.decode("1\r\nxXY");
    RUVIA_CHECK(bad_delimiter.failure() != nullptr);
    RUVIA_CHECK(bad_delimiter.failure()->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    RUVIA_CHECK_EQ(bad_delimiter.consumed_bytes(), std::size_t{4});

    http1_chunked_body_decoder trailer({.body_limit_ = protocol_byte_limit::limited(1024)});
    const auto bad_trailer = trailer.decode("0\r\nContent-Length: 1\r\n\r\n");
    RUVIA_CHECK(bad_trailer.failure() != nullptr);
    RUVIA_CHECK(bad_trailer.failure()->error() == ruvia::http1_chunk_decode_error::invalid_framing);
}

RUVIA_TEST(chunked_body_decoder_validates_trailers_by_message_role) {
    http1_chunked_body_decoder response({.body_limit_ = protocol_byte_limit::limited(1024),
        .trailer_role_ = http1_chunk_trailer_role::response});
    const auto accepted_response = response.decode("0\r\nAccept-Ranges: bytes\r\n\r\n");
    RUVIA_CHECK(accepted_response.complete() != nullptr);
    if (const auto* complete = accepted_response.complete()) {
        RUVIA_CHECK_EQ(complete->trailers(), std::string_view("Accept-Ranges: bytes"));
    }

    http1_chunked_body_decoder request({.body_limit_ = protocol_byte_limit::limited(1024)});
    const auto rejected_request = request.decode("0\r\nAccept-Ranges: bytes\r\n\r\n");
    RUVIA_CHECK(rejected_request.failure() != nullptr);
    RUVIA_CHECK(rejected_request.failure()->error() == ruvia::http1_chunk_decode_error::invalid_framing);

    http1_chunked_body_decoder forbidden_response({.body_limit_ = protocol_byte_limit::limited(1024),
        .trailer_role_ = http1_chunk_trailer_role::response});
    const auto rejected_response =
        forbidden_response.decode("0\r\nDate: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n");
    RUVIA_CHECK(rejected_response.failure() != nullptr);
    RUVIA_CHECK(rejected_response.failure()->error() == ruvia::http1_chunk_decode_error::invalid_framing);
}

RUVIA_TEST(chunked_body_decoder_validates_config_quota_and_neutral_errors) {
    bool rejected_role = false;
    try {
        http1_chunked_body_decoder decoder({.trailer_role_ = static_cast<http1_chunk_trailer_role>(255)});
    } catch (const std::invalid_argument&) {
        rejected_role = true;
    }
    RUVIA_CHECK(rejected_role);

    http1_chunked_body_decoder decoder({.body_limit_ = protocol_byte_limit::unlimited()});
    constexpr std::string_view wire = "3\r\nabc\r\n0\r\n\r\n";
    bool rejected_quota = false;
    try {
        (void)decoder.decode(wire, 0);
    } catch (const std::invalid_argument&) {
        rejected_quota = true;
    }
    RUVIA_CHECK(rejected_quota);
    const auto decoded = decoder.decode(wire, 2);
    RUVIA_CHECK(decoded.body_chunk() != nullptr);
    if (const auto* chunk = decoded.body_chunk()) {
        RUVIA_CHECK_EQ(chunk->bytes(), std::string_view("ab"));
        RUVIA_CHECK_EQ(chunk->consumed_bytes(), std::size_t{5});
        const auto remainder = decoder.decode(wire.substr(chunk->consumed_bytes()), 1);
        RUVIA_CHECK(remainder.body_chunk() != nullptr);
        if (const auto* final_chunk = remainder.body_chunk()) {
            RUVIA_CHECK_EQ(final_chunk->bytes(), std::string_view("c"));
            RUVIA_CHECK_EQ(final_chunk->consumed_bytes(), std::size_t{3});
        }
    }

    RUVIA_CHECK_EQ(ruvia::http_request_chunk_decode_error(
                       ruvia::http1_chunk_decode_error::framing_limit_exceeded)
                       .status(),
        ruvia::http_status::content_too_large);
}

RUVIA_TEST(chunked_body_decoder_decodes_without_pmr_allocation) {
    counting_no_alloc_resource resource;
    {
        scoped_default_resource scoped(&resource);
        http1_chunked_body_decoder decoder;
        constexpr std::string_view wire = "1\r\nx\r\n0\r\nX-Trace: ok\r\n\r\n";
        const auto body = decoder.decode(wire);
        RUVIA_CHECK(body.body_chunk() != nullptr);
        if (const auto* chunk = body.body_chunk()) {
            RUVIA_CHECK_EQ(chunk->bytes(), std::string_view("x"));
            const auto terminal = decoder.decode(wire.substr(chunk->consumed_bytes()));
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
        wire.append(ruvia::max_http_header_bytes - 7 + excess - wire.size() - 2, 'a');
        wire.append("\r\nx\r\n0\r\n\r\nNEXT");
        http1_chunked_body_decoder decoder;
        const auto body = decoder.decode(wire, 1);
        RUVIA_CHECK(body.body_chunk() != nullptr);
        if (!body.body_chunk()) {
            continue;
        }
        RUVIA_CHECK_EQ(body.body_chunk()->bytes(), "x");
        auto suffix = std::string_view(wire).substr(body.consumed_bytes());
        const auto terminal = decoder.decode(suffix, 1);
        if (excess == 0) {
            RUVIA_CHECK(terminal.complete() != nullptr);
            suffix.remove_prefix(terminal.consumed_bytes());
            RUVIA_CHECK_EQ(suffix, "NEXT");
            const auto replay = decoder.decode(suffix);
            RUVIA_CHECK(replay.complete() != nullptr);
            RUVIA_CHECK_EQ(replay.consumed_bytes(), std::size_t{0});
        } else {
            RUVIA_CHECK(terminal.failure() != nullptr);
            if (const auto* failure = terminal.failure()) {
                RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
            }
            const auto replay = decoder.decode(suffix);
            RUVIA_CHECK(replay.failure() != nullptr);
            RUVIA_CHECK_EQ(replay.consumed_bytes(), std::size_t{0});
            if (const auto* failure = replay.failure()) {
                RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
            }
        }
    }
}

RUVIA_TEST(chunked_body_decoder_caps_each_size_line) {
    http1_chunked_body_decoder decoder(
        {.body_limit_ = protocol_byte_limit::limited(ruvia::default_max_buffered_body_bytes)});
    std::string oversized = "1;x=";
    oversized.append(ruvia::max_http_header_bytes, 'a');
    oversized.append("\r\n");

    const auto result_value = decoder.decode(oversized);
    RUVIA_CHECK(result_value.failure() != nullptr);
    if (const auto* failure = result_value.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
    }

    http1_chunked_body_decoder boundary(
        {.body_limit_ = protocol_byte_limit::limited(ruvia::default_max_buffered_body_bytes)});
    std::string accepted = "1;x=";
    // Reserve two bytes for the data delimiter and five for the terminal chunk.
    accepted.append(ruvia::max_http_header_bytes - 7 - accepted.size() - 2, 'a');
    accepted.append("\r\nx\r\n0\r\n\r\n");
    const auto boundary_result = boundary.decode(accepted);
    RUVIA_CHECK(boundary_result.body_chunk() != nullptr);
    if (const auto* body = boundary_result.body_chunk()) {
        const auto terminal =
            boundary.decode(std::string_view(accepted).substr(body->consumed_bytes()));
        RUVIA_CHECK(terminal.complete() != nullptr);
    }
}

RUVIA_TEST(chunked_body_decoder_preserves_incomplete_delimiter_consumption) {
    http1_chunked_body_decoder decoder;
    constexpr std::string_view wire = "1\r\nx\r";
    const auto body = decoder.decode(wire);
    RUVIA_CHECK(body.body_chunk() != nullptr);
    RUVIA_CHECK_EQ(body.consumed_bytes(), std::size_t{4});
    if (const auto* chunk = body.body_chunk()) {
        RUVIA_CHECK_EQ(chunk->bytes().data(), wire.data() + 3);
    }
    const auto incomplete = decoder.decode(wire.substr(body.consumed_bytes()));
    RUVIA_CHECK(incomplete.need_more() != nullptr);
    RUVIA_CHECK_EQ(incomplete.consumed_bytes(), std::size_t{0});
    constexpr std::string_view suffix = "\r\n0\r\n\r\nNEXT";
    const auto complete_value = decoder.decode(suffix);
    RUVIA_CHECK(complete_value.complete() != nullptr);
    RUVIA_CHECK_EQ(suffix.substr(complete_value.consumed_bytes()), "NEXT");
}

RUVIA_TEST(chunked_body_decoder_preserves_trailer_failure_precedence) {
    std::string oversized = "0\r\nX-Trace: ";
    oversized.append(ruvia::max_http_header_bytes, 'a');
    http1_chunked_body_decoder incomplete;
    const auto incomplete_result = incomplete.decode(oversized);
    RUVIA_CHECK(incomplete_result.failure() != nullptr);
    RUVIA_CHECK_EQ(incomplete_result.consumed_bytes(), std::size_t{3});
    if (const auto* failure = incomplete_result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
    }

    oversized.append("\r\n\r\n");
    http1_chunked_body_decoder terminated;
    const auto terminated_result = terminated.decode(oversized);
    RUVIA_CHECK(terminated_result.failure() != nullptr);
    RUVIA_CHECK_EQ(terminated_result.consumed_bytes(), std::size_t{3});
    if (const auto* failure = terminated_result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }

    std::string fields_value = "0\r\n";
    for (std::size_t index = 0; index <= ruvia::max_http_header_fields; ++index) {
        fields_value.append("X-Trace: ok\r\n");
    }
    fields_value.append("\r\n");
    http1_chunked_body_decoder field_limit;
    const auto field_result = field_limit.decode(fields_value);
    RUVIA_CHECK(field_result.failure() != nullptr);
    if (const auto* failure = field_result.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
}
