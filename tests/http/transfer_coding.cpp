#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "content_decoding_fixture.h"
#include "http1_chunked_body.h"
#include "http1_message_body.h"
#include "http_request.h"
#include "http_request_body_failure.h"
#include "http_transfer_coding.h"

using namespace ruvia::test;

TEST(RuviaHttpPublicApi, transfer_codec_stages_share_request_native_budget_and_unwind_failed_construction) {
    std::size_t stage_bytes = 0;
    {
        ruvia::http_codec_memory measuring;
        http_transfer_coding_stack_decoder decoder(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024), &measuring);
        stage_bytes = measuring.live_bytes();
    }
    ASSERT_GT(stage_bytes, 0U);
    ruvia::http_codec_memory memory(stage_bytes);
    EXPECT_THROW((void)http_transfer_coding_stack_decoder((std::array{http_content_coding::gzip, http_content_coding::gzip}), protocol_byte_limit::limited(1024), &memory), std::bad_alloc);
    EXPECT_EQ(memory.live_bytes(), 0U);
}

TEST(RuviaHttpPublicApi, transfer_decoder_enters_failed_state_on_native_allocation_failure) {
    constexpr std::array codings{http_content_coding::gzip};
    const auto limit = protocol_byte_limit::limited(32 * 1024);
    std::size_t initial_bytes = 0;
    {
        ruvia::http_codec_memory measuring;
        http_transfer_coding_stack_decoder decoder(codings, limit, &measuring);
        initial_bytes = measuring.live_bytes();
    }
    const auto wire = gzip_compress(std::string(16 * 1024, 'x'));
    ruvia::http_codec_memory memory(initial_bytes);
    http_transfer_coding_stack_decoder decoder(codings, limit, &memory);
    std::array<char, 1> scratch{};

    ASSERT_THROW((void)decoder.decode(wire, scratch), std::bad_alloc);
    EXPECT_EQ(decoder.state(), ruvia::decoder_state::failed);
    const auto replayed = decoder.decode(wire, {});
    ASSERT_TRUE(replayed.holds<ruvia::http_transfer_coding_decode_failure>());
    EXPECT_EQ(replayed.consumed_bytes(), 0U);
    EXPECT_TRUE(decoder.finish_input().holds<ruvia::http_transfer_coding_decode_failure>());

    decoder.reset();
    std::string output;
    EXPECT_FALSE(append_transfer_decoded(decoder, gzip_compress("ok"), output).failed_);
    EXPECT_EQ(output, "ok");
    EXPECT_TRUE(decoder.finish_input().holds<ruvia::decode_complete>());
}

// Decoding a transfer-coded (chunked, then coded) request body.

TEST(Ruvia, transfer_coding_stack_decodes_reverse_order_with_tiny_scratch) {
    constexpr std::string_view plain = "stacked transfer coding payload";
    const std::string inner = gzip_compress(plain);
    const std::string wire = zlib_deflate_compress(inner);
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate};
    std::string decoded;
    std::array<char, 1> scratch{};
    {
        ruvia::http_transfer_coding_stack_decoder stack(codings, protocol_byte_limit::limited(1024));
        std::size_t cursor_value = 0;
        while (cursor_value < wire.size()) {
            const auto result_value = stack.decode(std::string_view(wire).substr(cursor_value, 1), scratch);
            cursor_value += result_value.consumed_bytes();
            if (const auto* output = result_value.get_if<ruvia::decode_output>()) {
                decoded.append(output->bytes());
            } else if (result_value.holds<ruvia::http_transfer_coding_decode_failure>()) {
                EXPECT_TRUE(false);
                return;
            }
        }
        for (;;) {
            const auto result_value = stack.decode({}, scratch);
            if (const auto* output = result_value.get_if<ruvia::decode_output>()) {
                decoded.append(output->bytes());
                continue;
            }
            if (result_value.holds<ruvia::decode_need_input>()) {
                break;
            }
            if (result_value.holds<ruvia::http_transfer_coding_decode_failure>()) {
                EXPECT_TRUE(false);
                return;
            }
        }
        const auto finish_value = stack.finish_input();
        EXPECT_TRUE(finish_value.holds<ruvia::decode_complete>());
    }
    EXPECT_EQ(std::string_view(decoded), plain);
}

TEST(Ruvia, transfer_coding_stack_rejects_overlapping_buffers_without_consuming_input) {
    constexpr std::string_view plain = "stacked transfer coding payload";
    const auto wire = zlib_deflate_compress(gzip_compress(plain));
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(codings, protocol_byte_limit::limited(1024));

    auto aliased = wire;
    EXPECT_THROW((void)stack.decode(aliased, std::span<char>(aliased).subspan(1)), std::invalid_argument);
    EXPECT_EQ(aliased, wire);

    std::array<char, 64> scratch{};
    const auto retried = stack.decode(wire, scratch);
    EXPECT_EQ(retried.consumed_bytes(), wire.size());
    const auto* output = retried.get_if<ruvia::decode_output>();
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->bytes(), plain);
}

TEST(Ruvia, transfer_coding_stack_rejects_truncated_inner_stream) {
    constexpr std::string_view plain = "truncated stacked transfer coding";
    auto inner = gzip_compress(plain);
    inner.resize(inner.size() - 4);
    const auto wire = zlib_deflate_compress(inner);
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(codings, protocol_byte_limit::limited(1024));
    std::array<char, 3> scratch{};
    std::size_t cursor_value = 0;
    for (std::size_t count = 0; count < 1024; ++count) {
        const auto result_value = stack.decode(std::string_view(wire).substr(cursor_value), scratch);
        cursor_value += result_value.consumed_bytes();
        if (result_value.holds<ruvia::http_transfer_coding_decode_failure>()) {
            EXPECT_TRUE(false);
            return;
        }
        if (result_value.holds<ruvia::decode_output>()) {
            continue;
        }
        if (result_value.holds<ruvia::decode_need_input>() && cursor_value == wire.size()) {
            break;
        }
        if (result_value.holds<ruvia::decode_complete>()) {
            break;
        }
    }
    const auto finish_value = stack.finish_input();
    EXPECT_TRUE(finish_value.holds<ruvia::http_transfer_coding_decode_failure>());
}

TEST(Ruvia, transfer_coding_stack_rejects_empty_inner_stream_after_non_empty_outer_wire) {
    const auto wire = zlib_deflate_compress("");
    EXPECT_TRUE(!wire.empty());
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(codings, protocol_byte_limit::limited(1024));
    std::array<char, 64> scratch{};
    const auto decoded = stack.decode(wire, scratch);
    EXPECT_EQ(decoded.consumed_bytes(), wire.size());
    EXPECT_TRUE(decoded.holds<ruvia::decode_need_input>());
    const auto finish_value = stack.finish_input();
    EXPECT_TRUE(finish_value.holds<ruvia::http_transfer_coding_decode_failure>());
    if (const auto* failure = finish_value.get_if<ruvia::http_transfer_coding_decode_failure>()) {
        EXPECT_TRUE(failure->error() == http_content_decode_error::invalid_content);
    }
}

TEST(Ruvia, transfer_coding_stack_accepts_the_bounded_sequence_and_reclaims_all_stages) {
    constexpr std::string_view plain = "bounded sequence payload";
    std::array<http_content_coding, ruvia::max_content_codings> codings{};
    std::string wire(plain);
    for (std::size_t index = 0; index < codings.size(); ++index) {
        codings[index] = index % 2 == 0 ? http_content_coding::gzip : http_content_coding::deflate;
        wire = index % 2 == 0 ? gzip_compress(wire) : zlib_deflate_compress(wire);
    }
    {
        http_transfer_coding_stack_decoder decoder(codings, protocol_byte_limit::limited(4096));
        std::string output;
        EXPECT_TRUE(!append_transfer_decoded(decoder, wire, output).failed_);
        const auto finish_value = decoder.finish_input();
        EXPECT_TRUE(finish_value.holds<ruvia::decode_complete>());
        EXPECT_EQ(std::string_view(output), plain);
    }
}

TEST(Ruvia, transfer_coding_stack_rejects_invalid_lengths_before_constructing_stages) {
    const std::array<http_content_coding, ruvia::max_content_codings + 1> codings{};
    for (const auto sequence : {std::span<const http_content_coding>{}, std::span<const http_content_coding>(codings)}) {
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
    auto codings = std::array{http_content_coding::deflate};
    http_transfer_coding_stack_decoder decoder(codings, protocol_byte_limit::limited(1024));
    codings[0] = http_content_coding::gzip;
    const auto wire = zlib_deflate_compress("one deflate stage");
    std::string output;
    EXPECT_TRUE(!append_transfer_decoded(decoder, wire, output).failed_);
    const auto finish_value = decoder.finish_input();
    EXPECT_TRUE(finish_value.holds<ruvia::decode_complete>());
    EXPECT_EQ(output, "one deflate stage");
    std::array<char, 1> scratch{};
    const auto replay = decoder.decode({}, scratch);
    EXPECT_TRUE(replay.holds<ruvia::decode_complete>());
}

TEST(Ruvia, transfer_coding_stack_reports_state_and_decodes_again_after_reset) {
    http_transfer_coding_stack_decoder decoder(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024));
    EXPECT_EQ(decoder.state(), ruvia::decoder_state::active);
    for (const std::string_view plain : {std::string_view("first body"), std::string_view("second body")}) {
        std::string output;
        EXPECT_TRUE(!append_transfer_decoded(decoder, gzip_compress(plain), output).failed_);
        const auto finish_value = decoder.finish_input();
        EXPECT_TRUE(finish_value.holds<ruvia::decode_complete>());
        EXPECT_EQ(decoder.state(), ruvia::decoder_state::finished);
        EXPECT_EQ(std::string_view(output), plain);
        decoder.reset();
        EXPECT_EQ(decoder.state(), ruvia::decoder_state::active);
    }

    std::array<char, 64> scratch{};
    const auto invalid = decoder.decode("not-gzip", scratch);
    EXPECT_TRUE(invalid.holds<ruvia::http_transfer_coding_decode_failure>());
    EXPECT_EQ(decoder.state(), ruvia::decoder_state::failed);
    decoder.reset();
    EXPECT_EQ(decoder.state(), ruvia::decoder_state::active);
}

TEST(Ruvia, transfer_coding_stack_finished_replays_its_end_without_consuming_input) {
    http_transfer_coding_stack_decoder decoder(std::array{http_content_coding::deflate}, protocol_byte_limit::limited(1024));
    std::string output;
    EXPECT_TRUE(!append_transfer_decoded(decoder, zlib_deflate_compress("done"), output).failed_);
    EXPECT_EQ(std::string_view(output), std::string_view("done"));
    EXPECT_EQ(decoder.state(), ruvia::decoder_state::finished);

    std::array<char, 64> scratch{};
    const auto replay = decoder.decode("more", scratch);
    EXPECT_TRUE(replay.holds<ruvia::decode_complete>());
    EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
    EXPECT_EQ(decoder.state(), ruvia::decoder_state::finished);
}

TEST(Ruvia, http1_body_decoder_rejects_chunk_data_after_the_transfer_coding_ended) {
    const std::string coded = zlib_deflate_compress("done");
    const ruvia::http1_chunk_header size_line(coded.size());
    std::string wire(size_line.view());
    wire.append(coded);
    wire.append("\r\n");
    const auto first_chunk_bytes = wire.size();
    wire.append("1\r\nx\r\n0\r\n\r\n");

    ruvia::http_content_codings codings;
    codings.push_back(http_content_coding::deflate);
    ruvia::http1_body_decoder decoder(ruvia::http1_message_framing::chunked(codings));
    std::array<char, 64> scratch{};
    std::string output;
    std::size_t consumed = 0;
    bool failed = false;
    for (int step = 0; step < 16 && !failed; ++step) {
        const auto result_value = decoder.decode(std::string_view(wire).substr(consumed), scratch);
        consumed += result_value.consumed_bytes();
        if (const auto* decoded = result_value.get_if<ruvia::decode_output>()) {
            output.append(decoded->bytes());
        } else if (const auto* failure = result_value.get_if<ruvia::http1_body_decode_failure>()) {
            EXPECT_TRUE(failure->error() == ruvia::http1_body_decode_error::invalid_transfer_coding);
            failed = true;
        } else {
            EXPECT_TRUE(result_value.holds<ruvia::decode_need_input>());
        }
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(std::string_view(output), std::string_view("done"));
    // The second chunk's size line is consumed; its data is not.
    EXPECT_EQ(consumed, first_chunk_bytes + 3);
    EXPECT_EQ(decoder.state(), ruvia::decoder_state::failed);
}

TEST(Ruvia, transfer_coding_stack_applies_decoded_limit_to_each_layer) {
    const std::string plain(4096, 'x');
    const auto wire = zlib_deflate_compress(gzip_compress(plain));
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(codings, protocol_byte_limit::limited(1024));
    std::array<char, 31> scratch{};
    bool exceeded = false;
    std::size_t cursor_value = 0;
    for (std::size_t count = 0; count < 1024 && !exceeded; ++count) {
        const auto input = std::string_view(wire).substr(cursor_value);
        const auto next_value = stack.decode(input, scratch);
        cursor_value += next_value.consumed_bytes();
        if (const auto* failure = next_value.get_if<ruvia::http_transfer_coding_decode_failure>()) {
            exceeded = failure->error() == http_content_decode_error::decoded_size_exceeded;
            break;
        }
        if (next_value.holds<ruvia::decode_need_input>() && cursor_value == wire.size()) {
            break;
        }
        if (next_value.holds<ruvia::decode_complete>()) {
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
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(codings, protocol_byte_limit::limited(1024));
    std::array<char, 31> scratch{};
    bool exceeded = false;
    std::size_t cursor_value = 0;
    for (std::size_t count = 0; count < 1024 && !exceeded; ++count) {
        const auto next_value = stack.decode(std::string_view(wire).substr(cursor_value), scratch);
        cursor_value += next_value.consumed_bytes();
        if (const auto* failure = next_value.get_if<ruvia::http_transfer_coding_decode_failure>()) {
            exceeded = failure->error() == http_content_decode_error::decoded_size_exceeded;
            break;
        }
        if (next_value.holds<ruvia::decode_complete>() || (next_value.holds<ruvia::decode_need_input>() && cursor_value == wire.size())) {
            break;
        }
    }
    EXPECT_TRUE(exceeded);
}

TEST(Ruvia, transfer_coding_decoder_gzip_round_trip) {
    http_transfer_coding_stack_decoder decoder(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1u << 20));

    const std::string plain = "transfer-encoding gzip body content, repeated repeated repeated";
    const std::string gz = gzip_compress(plain);
    std::string output;
    EXPECT_TRUE(!append_transfer_decoded(decoder, gz, output).failed_);
    const auto finish_result = decoder.finish_input();
    EXPECT_TRUE(finish_result.holds<ruvia::decode_complete>());
    EXPECT_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

TEST(Ruvia, transfer_coding_decoder_gzip_decodes_every_rfc1952_member) {
    const std::string first = gzip_compress("first-");
    const std::string second = gzip_compress("second");
    EXPECT_TRUE(!first.empty());
    EXPECT_TRUE(!second.empty());

    http_transfer_coding_stack_decoder contiguous(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024));
    std::string contiguous_output;
    EXPECT_TRUE(!append_transfer_decoded(contiguous, first + second, contiguous_output).failed_);
    const auto contiguous_finish = contiguous.finish_input();
    EXPECT_TRUE(contiguous_finish.holds<ruvia::decode_complete>());
    EXPECT_EQ(std::string_view(contiguous_output.data(), contiguous_output.size()), std::string_view("first-second"));

    http_transfer_coding_stack_decoder fragmented(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024));
    std::string fragmented_output;
    EXPECT_TRUE(!append_transfer_decoded(fragmented, first, fragmented_output).failed_);
    EXPECT_TRUE(!append_transfer_decoded(fragmented, second, fragmented_output).failed_);
    const auto fragmented_finish = fragmented.finish_input();
    EXPECT_TRUE(fragmented_finish.holds<ruvia::decode_complete>());
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
    const auto& framing = parsed_value.state_.body_plan().framing();
    EXPECT_TRUE(framing.kind() == ruvia::http1_framing_kind::chunked);
    if (framing.kind() != ruvia::http1_framing_kind::chunked) {
        return;
    }
    EXPECT_EQ(framing.transfer_codings().size(), std::size_t{1});
    http1_chunked_body_decoder chunks({.body_limit_ = protocol_byte_limit::limited(1u << 20)});
    http_transfer_coding_stack_decoder transfer(framing.transfer_codings(), protocol_byte_limit::limited(1u << 20));
    std::string output;
    std::string_view pending(wire_body);
    bool complete_value = false;
    while (!complete_value) {
        const auto result_value = chunks.decode(pending);
        EXPECT_TRUE(!result_value.holds<ruvia::decode_need_input>());
        if (result_value.holds<ruvia::decode_need_input>()) {
            break;
        }
        if (const auto* body_chunk = result_value.get_if<ruvia::decode_output>()) {
            EXPECT_TRUE(!append_transfer_decoded(transfer, body_chunk->bytes(), output).failed_);
        } else if (result_value.holds<ruvia::http1_chunk_decode_complete>()) {
            complete_value = true;
        }
        pending.remove_prefix(result_value.consumed_bytes());
    }
    EXPECT_TRUE(complete_value);
    const auto finish_result = transfer.finish_input();
    EXPECT_TRUE(finish_result.holds<ruvia::decode_complete>());
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
    const auto& framing = parsed_value.state_.body_plan().framing();
    EXPECT_TRUE(framing.kind() == ruvia::http1_framing_kind::chunked);
    if (framing.kind() != ruvia::http1_framing_kind::chunked) {
        return;
    }
    http1_chunked_body_decoder chunks({.body_limit_ = protocol_byte_limit::limited(1024)});
    const auto terminal = chunks.decode(wire_body);
    EXPECT_TRUE(terminal.holds<ruvia::http1_chunk_decode_complete>());
    EXPECT_EQ(terminal.consumed_bytes(), wire_body.size());

    http_transfer_coding_stack_decoder transfer(framing.transfer_codings(), protocol_byte_limit::limited(1024));
    const auto finish_result = transfer.finish_input();
    EXPECT_TRUE(finish_result.holds<ruvia::decode_complete>());

    // Every layer of a stacked coding treats absent coded bytes as empty.
    constexpr std::array codings{http_content_coding::gzip, http_content_coding::deflate};
    http_transfer_coding_stack_decoder stacked(codings, protocol_byte_limit::limited(1024));
    std::array<char, 16> scratch{};
    const auto drained = stacked.decode({}, scratch);
    EXPECT_TRUE(drained.holds<ruvia::decode_need_input>());
    const auto stacked_finish = stacked.finish_input();
    EXPECT_TRUE(stacked_finish.holds<ruvia::decode_complete>());
}

TEST(Ruvia, transfer_coding_decoder_rejects_bomb) {
    // A 1 MiB body compresses to a tiny gzip; the decoder must abort the
    // expansion once it passes the small cap, not stage the whole megabyte.
    http_transfer_coding_stack_decoder decoder(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024));

    const std::string big(1u << 20, 'a');
    const std::string gz = gzip_compress(big);
    std::string output;
    const auto error = append_transfer_decoded(decoder, gz, output);
    EXPECT_TRUE(error.failed_);
    EXPECT_TRUE(error.protocol_error_.has_value());
    EXPECT_EQ(error.protocol_error_->status(), ruvia::http_status::content_too_large);
    const auto finish_value = decoder.finish_input();
    EXPECT_TRUE(finish_value.holds<ruvia::http_transfer_coding_decode_failure>());
    if (const auto* failure = finish_value.get_if<ruvia::http_transfer_coding_decode_failure>()) {
        EXPECT_TRUE(failure->error() == http_content_decode_error::decoded_size_exceeded);
        EXPECT_TRUE(ruvia::make_http_protocol_error(failure->error(), ruvia::http_coding_field::transfer_encoding).status() == ruvia::http_status::content_too_large);
    }
}

TEST(Ruvia, transfer_coding_decoder_rebinds_caller_storage_between_steps) {
    const std::string plain(4096, 'a');
    std::string input = gzip_compress(plain);
    http_transfer_coding_stack_decoder decoder(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(plain.size()));
    std::array<std::array<char, 7>, 2> windows{};
    std::string decoded;
    bool drained = false;
    for (std::size_t turn = 0; turn < 2048; ++turn) {
        auto& window = windows[turn % windows.size()];
        const auto result_value = decoder.decode(input, window);
        EXPECT_TRUE(!result_value.holds<ruvia::http_transfer_coding_decode_failure>());
        if (const auto* output = result_value.get_if<ruvia::decode_output>()) {
            decoded.append(output->bytes());
        }
        // Replace the old input allocation and poison output immediately after
        // consuming its view. The inflater must only retain its own dictionary.
        std::string next_value(input.substr(result_value.consumed_bytes()));
        input.swap(next_value);
        std::fill(window.begin(), window.end(), '#');
        if (result_value.holds<ruvia::decode_need_input>() || result_value.holds<ruvia::decode_complete>()) {
            drained = true;
            break;
        }
        if (result_value.holds<ruvia::http_transfer_coding_decode_failure>()) {
            break;
        }
    }
    EXPECT_TRUE(drained);
    EXPECT_TRUE(input.empty());
    const auto finish_value = decoder.finish_input();
    EXPECT_TRUE(finish_value.holds<ruvia::decode_complete>());
    EXPECT_EQ(decoded, plain);
}

// RFC 9112 section 7.2: only gzip and deflate are transfer codings.
TEST(Ruvia, transfer_coding_decoder_rejects_non_transfer_codings) {
    for (const auto coding : {http_content_coding::identity, http_content_coding::brotli, http_content_coding::zstd, static_cast<http_content_coding>(255)}) {
        EXPECT_THROW(http_transfer_coding_stack_decoder(std::array{coding}, protocol_byte_limit::unlimited()), std::invalid_argument);
    }
}

TEST(Ruvia, transfer_coding_decoder_reports_typed_wire_failures) {
    std::array<char, std::size_t{8} * 1024> window{};

    http_transfer_coding_stack_decoder invalid(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024));
    const auto invalid_result = invalid.decode("not-gzip", window);
    ASSERT_TRUE(invalid_result.holds<ruvia::http_transfer_coding_decode_failure>());
    EXPECT_TRUE(invalid_result.get_if<ruvia::http_transfer_coding_decode_failure>()->error() == http_content_decode_error::invalid_content);
    EXPECT_TRUE(ruvia::make_http_protocol_error(invalid_result.get_if<ruvia::http_transfer_coding_decode_failure>()->error(), ruvia::http_coding_field::transfer_encoding).status() == ruvia::http_status::bad_request);

    std::string truncated = gzip_compress("truncated");
    truncated.resize(truncated.size() - 4);
    http_transfer_coding_stack_decoder incomplete(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024));
    std::string ignored;
    EXPECT_TRUE(!append_transfer_decoded(incomplete, truncated, ignored).failed_);
    const auto incomplete_finish = incomplete.finish_input();
    EXPECT_TRUE(incomplete_finish.holds<ruvia::http_transfer_coding_decode_failure>());
    if (const auto* failure = incomplete_finish.get_if<ruvia::http_transfer_coding_decode_failure>()) {
        EXPECT_TRUE(failure->error() == http_content_decode_error::invalid_content);
        EXPECT_TRUE(ruvia::make_http_protocol_error(failure->error(), ruvia::http_coding_field::transfer_encoding).status() == ruvia::http_status::bad_request);
    }
    const auto repeated_finish = incomplete.finish_input();
    EXPECT_TRUE(repeated_finish.holds<ruvia::http_transfer_coding_decode_failure>());
    if (const auto* failure = repeated_finish.get_if<ruvia::http_transfer_coding_decode_failure>()) {
        EXPECT_TRUE(failure->error() == http_content_decode_error::invalid_content);
    }

    http_transfer_coding_stack_decoder no_output(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024));
    EXPECT_THROW((void)no_output.decode("input", {}), std::invalid_argument);

    std::string trailing = gzip_compress("complete");
    trailing.push_back('x');
    http_transfer_coding_stack_decoder extra(std::array{http_content_coding::gzip}, protocol_byte_limit::limited(1024));
    const auto trailing_error = append_transfer_decoded(extra, trailing, ignored);
    // A short prefix of another member can remain ambiguous until framing EOF.
    // It must not make the first valid member terminal, but EOF must reject it.
    EXPECT_TRUE(!trailing_error.failed_);
    const auto trailing_finish = extra.finish_input();
    EXPECT_TRUE(trailing_finish.holds<ruvia::http_transfer_coding_decode_failure>());
    if (const auto* failure = trailing_finish.get_if<ruvia::http_transfer_coding_decode_failure>()) {
        EXPECT_TRUE(failure->error() == http_content_decode_error::invalid_content);
    }
}
