#include <gtest/gtest.h>

#include "content_decoding_fixture.h"
#include "http1_request_message_fixture.h"

namespace {
using ruvia::testing::parse_http1_request_message;

class decoder_memory_resource final : public std::pmr::memory_resource {
public:
    void reject_allocations(bool reject) noexcept {
        reject_allocations_ = reject;
    }
    void reject_after(std::size_t successful_allocations) noexcept {
        reject_after_ = true;
        successful_allocations_before_failure_ = successful_allocations;
    }
    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_;
    }
    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::size_t deallocation_count() const noexcept {
        return deallocations_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_allocations_ || (reject_after_ && successful_allocations_before_failure_ == 0)) {
            reject_after_ = false;
            throw std::bad_alloc();
        }
        if (reject_after_) {
            --successful_allocations_before_failure_;
        }
        auto* result_value = std::pmr::get_default_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        ++live_;
        return result_value;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::get_default_resource()->deallocate(pointer, bytes_value, alignment);
        ++deallocations_;
        --live_;
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    bool reject_allocations_{false};
    bool reject_after_{false};
    std::size_t successful_allocations_before_failure_{0};
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
    std::size_t live_{0};
};
}  // namespace

// Decoding a transfer-coded (chunked, then coded) request body.

TEST(Ruvia, transfer_coding_stack_decodes_reverse_order_with_tiny_scratch) {
    constexpr std::string_view plain = "stacked transfer coding payload";
    const std::string inner = gzip_compress(plain);
    const std::string wire = zlib_deflate_compress(inner);
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    decoder_memory_resource memory;
    std::string decoded;
    std::array<char, 1> scratch{};
    {
        ruvia::http_transfer_coding_stack_decoder stack(
            codings, &memory, protocol_byte_limit::limited(1024));
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
    EXPECT_EQ(memory.live_allocations(), std::size_t{0});
}

TEST(Ruvia, transfer_coding_stack_rejects_truncated_inner_stream) {
    constexpr std::string_view plain = "truncated stacked transfer coding";
    auto inner = gzip_compress(plain);
    inner.resize(inner.size() - 4);
    const auto wire = zlib_deflate_compress(inner);
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    ruvia::http_transfer_coding_stack_decoder stack(
        codings, std::pmr::get_default_resource(), protocol_byte_limit::limited(1024));
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
        codings, std::pmr::get_default_resource(), protocol_byte_limit::limited(1024));
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

TEST(Ruvia, transfer_coding_stack_construction_failure_releases_partial_state) {
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    decoder_memory_resource baseline;
    {
        http_transfer_coding_stack_decoder decoder(
            codings, &baseline, protocol_byte_limit::unlimited());
    }
    EXPECT_EQ(baseline.live_allocations(), std::size_t{0});
    for (std::size_t index = 0; index < baseline.allocation_count(); ++index) {
        decoder_memory_resource memory;
        memory.reject_after(index);
        bool failed = false;
        try {
            http_transfer_coding_stack_decoder decoder(
                codings, &memory, protocol_byte_limit::unlimited());
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(memory.live_allocations(), std::size_t{0});
        EXPECT_EQ(memory.allocation_count(), memory.deallocation_count());
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
    decoder_memory_resource resource;
    {
        http_transfer_coding_stack_decoder decoder(codings, &resource, protocol_byte_limit::limited(4096));
        std::pmr::string output(&resource);
        EXPECT_TRUE(!append_transfer_decoded(decoder, wire, output).failed_);
        const auto finish_value = decoder.finish_input();
        EXPECT_TRUE(finish_value.complete() != nullptr);
        EXPECT_EQ(std::string_view(output), plain);
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(resource.allocation_count(), resource.deallocation_count());
}

TEST(Ruvia, transfer_coding_stack_rejects_invalid_lengths_before_constructing_stages) {
    decoder_memory_resource resource;
    const std::array<http_transfer_coding, ruvia::max_transfer_codings + 1> codings{};
    for (const auto sequence : {std::span<const http_transfer_coding>{}, std::span<const http_transfer_coding>(codings)}) {
        bool rejected = false;
        try {
            http_transfer_coding_stack_decoder decoder(sequence, &resource, protocol_byte_limit::unlimited());
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    }
}

TEST(Ruvia, transfer_coding_stack_owns_sequence_and_decodes_one_deflate_stage) {
    auto* resource = std::pmr::get_default_resource();
    auto codings = std::array{http_transfer_coding::deflate};
    http_transfer_coding_stack_decoder decoder(codings, resource, protocol_byte_limit::limited(1024));
    codings[0] = http_transfer_coding::gzip;
    const auto wire = zlib_deflate_compress("one deflate stage");
    std::pmr::string output(resource);
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
        codings, std::pmr::get_default_resource(), protocol_byte_limit::limited(1024));
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
        codings, std::pmr::get_default_resource(), protocol_byte_limit::limited(1024));
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
    auto* resource = std::pmr::get_default_resource();
    http_transfer_coding_stack_decoder decoder(
        std::array{http_transfer_coding::gzip}, resource, protocol_byte_limit::limited(1u << 20));

    const std::string plain = "transfer-encoding gzip body content, repeated repeated repeated";
    const std::string gz = gzip_compress(plain);
    std::pmr::string output(resource);
    EXPECT_TRUE(!append_transfer_decoded(decoder, gz, output).failed_);
    const auto finish_result = decoder.finish_input();
    EXPECT_TRUE(finish_result.complete() != nullptr);
    EXPECT_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

TEST(Ruvia, transfer_coding_decoder_gzip_decodes_every_rfc1952_member) {
    auto* resource = std::pmr::get_default_resource();
    const std::string first = gzip_compress("first-");
    const std::string second = gzip_compress("second");
    EXPECT_TRUE(!first.empty());
    EXPECT_TRUE(!second.empty());

    http_transfer_coding_stack_decoder contiguous(
        std::array{http_transfer_coding::gzip}, resource, protocol_byte_limit::limited(1024));
    std::pmr::string contiguous_output(resource);
    EXPECT_TRUE(!append_transfer_decoded(contiguous, first + second, contiguous_output).failed_);
    const auto contiguous_finish = contiguous.finish_input();
    EXPECT_TRUE(contiguous_finish.complete() != nullptr);
    EXPECT_EQ(std::string_view(contiguous_output.data(), contiguous_output.size()), std::string_view("first-second"));

    http_transfer_coding_stack_decoder fragmented(
        std::array{http_transfer_coding::gzip}, resource, protocol_byte_limit::limited(1024));
    std::pmr::string fragmented_output(resource);
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
    const auto* chunked_body = parsed_value.state_.body_plan_.chunked();
    EXPECT_TRUE(chunked_body != nullptr);
    if (chunked_body == nullptr) {
        return;
    }
    EXPECT_EQ(chunked_body->transfer_codings().size(), std::size_t{1});

    auto* resource = std::pmr::get_default_resource();
    http1_chunked_body_decoder chunks({.body_limit_ = protocol_byte_limit::limited(1u << 20)});
    http_transfer_coding_stack_decoder transfer(
        chunked_body->transfer_codings(), resource, protocol_byte_limit::limited(1u << 20));
    std::pmr::string output(resource);
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
    const auto* chunked_body = parsed_value.state_.body_plan_.chunked();
    EXPECT_TRUE(chunked_body != nullptr);
    if (chunked_body == nullptr) {
        return;
    }

    auto* resource = std::pmr::get_default_resource();
    http1_chunked_body_decoder chunks({.body_limit_ = protocol_byte_limit::limited(1024)});
    const auto terminal = chunks.decode(wire_body);
    EXPECT_TRUE(terminal.complete() != nullptr);
    EXPECT_EQ(terminal.consumed_bytes(), wire_body.size());

    http_transfer_coding_stack_decoder transfer(
        chunked_body->transfer_codings(), resource, protocol_byte_limit::limited(1024));
    const auto finish_result = transfer.finish_input();
    EXPECT_TRUE(finish_result.complete() != nullptr);

    // Every layer of a stacked coding treats absent coded bytes as empty.
    constexpr std::array codings{http_transfer_coding::gzip, http_transfer_coding::deflate};
    http_transfer_coding_stack_decoder stacked(codings, resource, protocol_byte_limit::limited(1024));
    std::array<char, 16> scratch{};
    const auto drained = stacked.decode({}, scratch);
    EXPECT_TRUE(drained.need_input() != nullptr);
    const auto stacked_finish = stacked.finish_input();
    EXPECT_TRUE(stacked_finish.complete() != nullptr);
}

TEST(Ruvia, transfer_coding_decoder_rejects_bomb) {
    auto* resource = std::pmr::get_default_resource();
    // A 1 MiB body compresses to a tiny gzip; the decoder must abort the
    // expansion once it passes the small cap, not stage the whole megabyte.
    http_transfer_coding_stack_decoder decoder(
        std::array{http_transfer_coding::gzip}, resource, protocol_byte_limit::limited(1024));

    const std::string big(1u << 20, 'a');
    const std::string gz = gzip_compress(big);
    std::pmr::string output(resource);
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
    http_transfer_coding_stack_decoder decoder(std::array{http_transfer_coding::gzip},
        std::pmr::get_default_resource(), protocol_byte_limit::limited(plain.size()));
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

TEST(Ruvia, transfer_coding_decoder_reclaims_allocations_after_constructor_and_decode_failure) {
    decoder_memory_resource memory;
    memory.reject_allocations(true);
    bool constructor_failed = false;
    try {
        http_transfer_coding_stack_decoder decoder(std::array{http_transfer_coding::gzip}, &memory,
            protocol_byte_limit::unlimited());
    } catch (const std::bad_alloc&) {
        constructor_failed = true;
    }
    EXPECT_TRUE(constructor_failed);
    EXPECT_EQ(memory.live_allocations(), std::size_t{0});

    memory.reject_allocations(false);
    {
        http_transfer_coding_stack_decoder decoder(std::array{http_transfer_coding::gzip}, &memory,
            protocol_byte_limit::unlimited());
        EXPECT_TRUE(memory.live_allocations() != 0);
        memory.reject_allocations(true);
        const auto input = gzip_compress(std::string(4096, 'a'));
        std::array<char, 1> output{};
        const auto failed = decoder.decode(input, output);
        EXPECT_TRUE(failed.decoder_failure() != nullptr);
        EXPECT_TRUE(failed.failure() == nullptr);
        const auto replay = decoder.decode({}, output);
        EXPECT_TRUE(replay.decoder_failure() != nullptr);
        EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
        const auto finish_value = decoder.finish_input();
        EXPECT_TRUE(finish_value.decoder_failure() != nullptr);
        EXPECT_EQ(finish_value.consumed_bytes(), std::size_t{0});
    }
    EXPECT_EQ(memory.live_allocations(), std::size_t{0});
    EXPECT_EQ(memory.allocation_count(), memory.deallocation_count());
}

TEST(Ruvia, transfer_coding_decoder_rejects_unsupported_coding) {
    bool rejected = false;
    try {
        http_transfer_coding_stack_decoder decoder(std::array{static_cast<http_transfer_coding>(255)},
            std::pmr::get_default_resource(), protocol_byte_limit::unlimited());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, transfer_coding_decoder_reports_typed_wire_failures) {
    auto* resource = std::pmr::get_default_resource();
    std::array<char, std::size_t{8} * 1024> window{};

    http_transfer_coding_stack_decoder invalid(
        std::array{http_transfer_coding::gzip}, resource, protocol_byte_limit::limited(1024));
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
        std::array{http_transfer_coding::gzip}, resource, protocol_byte_limit::limited(1024));
    std::pmr::string ignored(resource);
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
        std::array{http_transfer_coding::gzip}, resource, protocol_byte_limit::limited(1024));
    const auto decoder_failure = internal_failure.decode("input", {});
    EXPECT_TRUE(decoder_failure.failure() == nullptr);
    EXPECT_TRUE(decoder_failure.decoder_failure() != nullptr);
    const auto repeated_decoder_failure = internal_failure.finish_input();
    EXPECT_TRUE(repeated_decoder_failure.failure() == nullptr);
    EXPECT_TRUE(repeated_decoder_failure.decoder_failure() != nullptr);

    std::string trailing = gzip_compress("complete");
    trailing.push_back('x');
    http_transfer_coding_stack_decoder extra(
        std::array{http_transfer_coding::gzip}, resource, protocol_byte_limit::limited(1024));
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
