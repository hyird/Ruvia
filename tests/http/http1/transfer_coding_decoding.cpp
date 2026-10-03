#include "content_decoding_fixture.h"

namespace {
class DecoderMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations(bool reject) noexcept {
        rejectAllocations_ = reject;
    }
    void rejectAfter(std::size_t successfulAllocations) noexcept {
        rejectAfter_ = true;
        successfulAllocationsBeforeFailure_ = successfulAllocations;
    }
    [[nodiscard]] std::size_t liveAllocations() const noexcept {
        return live_;
    }
    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::size_t deallocationCount() const noexcept {
        return deallocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (rejectAllocations_ || (rejectAfter_ && successfulAllocationsBeforeFailure_ == 0)) {
            rejectAfter_ = false;
            throw std::bad_alloc();
        }
        if (rejectAfter_) {
            --successfulAllocationsBeforeFailure_;
        }
        auto* result = std::pmr::get_default_resource()->allocate(bytes, alignment);
        ++allocations_;
        ++live_;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::get_default_resource()->deallocate(pointer, bytes, alignment);
        ++deallocations_;
        --live_;
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    bool rejectAllocations_{false};
    bool rejectAfter_{false};
    std::size_t successfulAllocationsBeforeFailure_{0};
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
    std::size_t live_{0};
};
}  // namespace

// Decoding a transfer-coded (chunked, then coded) request body.

RUVIA_TEST(transfer_coding_stack_decodes_reverse_order_with_tiny_scratch) {
    constexpr std::string_view plain = "stacked transfer coding payload";
    const std::string inner = gzipCompress(plain);
    const std::string wire = zlib_deflate_compress(inner);
    constexpr std::array codings{HttpTransferCoding::kGzip, HttpTransferCoding::kDeflate};
    DecoderMemoryResource memory;
    std::string decoded;
    std::array<char, 1> scratch{};
    {
        ruvia::http_transfer_coding_stack_decoder stack(
            codings, &memory, ProtocolByteLimit::limited(1024));
        std::size_t cursor = 0;
        while (cursor < wire.size()) {
            const auto result = stack.decode(std::string_view(wire).substr(cursor, 1), scratch);
            cursor += result.consumedBytes();
            if (const auto* output = result.output()) {
                decoded.append(output->bytes());
            } else if (result.failure() != nullptr || result.decoderFailure() != nullptr) {
                RUVIA_CHECK(false);
                return;
            }
        }
        for (;;) {
            const auto result = stack.decode({}, scratch);
            if (const auto* output = result.output()) {
                decoded.append(output->bytes());
                continue;
            }
            if (result.needInput() != nullptr) {
                break;
            }
            if (result.failure() != nullptr || result.decoderFailure() != nullptr) {
                RUVIA_CHECK(false);
                return;
            }
        }
        const auto finish = stack.finish_input();
        RUVIA_CHECK(finish.complete() != nullptr);
    }
    RUVIA_CHECK_EQ(std::string_view(decoded), plain);
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(transfer_coding_stack_rejects_truncated_inner_stream) {
    constexpr std::string_view plain = "truncated stacked transfer coding";
    auto inner = gzipCompress(plain);
    inner.resize(inner.size() - 4);
    const auto wire = zlib_deflate_compress(inner);
    constexpr std::array codings{HttpTransferCoding::kGzip, HttpTransferCoding::kDeflate};
    ruvia::http_transfer_coding_stack_decoder stack(
        codings, std::pmr::get_default_resource(), ProtocolByteLimit::limited(1024));
    std::array<char, 3> scratch{};
    std::size_t cursor = 0;
    for (std::size_t count = 0; count < 1024; ++count) {
        const auto result = stack.decode(std::string_view(wire).substr(cursor), scratch);
        cursor += result.consumedBytes();
        if (result.failure() != nullptr || result.decoderFailure() != nullptr) {
            RUVIA_CHECK(false);
            return;
        }
        if (result.output() != nullptr) {
            continue;
        }
        if (result.needInput() != nullptr && cursor == wire.size()) {
            break;
        }
        if (result.complete() != nullptr) {
            break;
        }
    }
    const auto finish = stack.finish_input();
    RUVIA_CHECK(finish.failure() != nullptr);
}

RUVIA_TEST(transfer_coding_stack_construction_failure_releases_partial_state) {
    constexpr std::array codings{HttpTransferCoding::kGzip, HttpTransferCoding::kDeflate};
    DecoderMemoryResource baseline;
    {
        http_transfer_coding_stack_decoder decoder(
            codings, &baseline, ProtocolByteLimit::unlimited());
    }
    RUVIA_CHECK_EQ(baseline.liveAllocations(), std::size_t{0});
    for (std::size_t index = 0; index < baseline.allocationCount(); ++index) {
        DecoderMemoryResource memory;
        memory.rejectAfter(index);
        bool failed = false;
        try {
            http_transfer_coding_stack_decoder decoder(
                codings, &memory, ProtocolByteLimit::unlimited());
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
    }
}

RUVIA_TEST(transfer_coding_stack_accepts_the_bounded_sequence_and_reclaims_all_stages) {
    constexpr std::string_view plain = "bounded sequence payload";
    std::array<HttpTransferCoding, ruvia::kMaxTransferCodings> codings{};
    std::string wire(plain);
    for (std::size_t index = 0; index < codings.size(); ++index) {
        codings[index] = index % 2 == 0 ? HttpTransferCoding::kGzip : HttpTransferCoding::kDeflate;
        wire = index % 2 == 0 ? gzipCompress(wire) : zlib_deflate_compress(wire);
    }
    DecoderMemoryResource resource;
    {
        http_transfer_coding_stack_decoder decoder(codings, &resource, ProtocolByteLimit::limited(4096));
        std::pmr::string output(&resource);
        RUVIA_CHECK(!appendTransferDecoded(decoder, wire, output).failed);
        const auto finish = decoder.finish_input();
        RUVIA_CHECK(finish.complete() != nullptr);
        RUVIA_CHECK_EQ(std::string_view(output), plain);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(transfer_coding_stack_rejects_invalid_lengths_before_constructing_stages) {
    DecoderMemoryResource resource;
    const std::array<HttpTransferCoding, ruvia::kMaxTransferCodings + 1> codings{};
    for (const auto sequence : {std::span<const HttpTransferCoding>{}, std::span<const HttpTransferCoding>(codings)}) {
        bool rejected = false;
        try {
            http_transfer_coding_stack_decoder decoder(sequence, &resource, ProtocolByteLimit::unlimited());
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
}

RUVIA_TEST(transfer_coding_stack_owns_sequence_and_decodes_one_deflate_stage) {
    auto* resource = std::pmr::get_default_resource();
    auto codings = std::array{HttpTransferCoding::kDeflate};
    http_transfer_coding_stack_decoder decoder(codings, resource, ProtocolByteLimit::limited(1024));
    codings[0] = HttpTransferCoding::kGzip;
    const auto wire = zlib_deflate_compress("one deflate stage");
    std::pmr::string output(resource);
    RUVIA_CHECK(!appendTransferDecoded(decoder, wire, output).failed);
    const auto finish = decoder.finish_input();
    RUVIA_CHECK(finish.complete() != nullptr);
    RUVIA_CHECK_EQ(output, "one deflate stage");
    std::array<char, 1> scratch{};
    const auto replay = decoder.decode({}, scratch);
    RUVIA_CHECK(replay.complete() != nullptr);
}

RUVIA_TEST(transfer_coding_stack_applies_decoded_limit_to_each_layer) {
    const std::string plain(4096, 'x');
    const auto wire = zlib_deflate_compress(gzipCompress(plain));
    constexpr std::array codings{HttpTransferCoding::kGzip, HttpTransferCoding::kDeflate};
    ruvia::http_transfer_coding_stack_decoder stack(
        codings, std::pmr::get_default_resource(), ProtocolByteLimit::limited(1024));
    std::array<char, 31> scratch{};
    bool exceeded = false;
    std::size_t cursor = 0;
    for (std::size_t count = 0; count < 1024 && !exceeded; ++count) {
        const auto input = std::string_view(wire).substr(cursor);
        const auto next = stack.decode(input, scratch);
        cursor += next.consumedBytes();
        if (const auto* failure = next.failure()) {
            exceeded = failure->error() == ruvia::HttpTransferCodingDecodeError::kDecodedSizeExceeded;
            break;
        }
        if (next.decoderFailure() != nullptr) {
            break;
        }
        if (next.needInput() != nullptr && cursor == wire.size()) {
            break;
        }
        if (next.complete() != nullptr) {
            break;
        }
    }
    RUVIA_CHECK(exceeded);
}

RUVIA_TEST(transfer_coding_stack_rejects_intermediate_limit_when_final_layer_fits) {
    constexpr std::string_view plain = "small decoded body";
    auto inner = gzipCompress(plain);
    inner[3] = static_cast<char>(static_cast<unsigned char>(inner[3]) | 0x04U);
    std::string extra;
    extra.push_back('\0');
    extra.push_back('\x04');  // RFC 1952 XLEN: 1024 extra bytes.
    extra.append(1024, 'x');
    inner.insert(10, extra);
    RUVIA_CHECK(inner.size() > 1024);

    const auto wire = zlib_deflate_compress(inner);
    constexpr std::array codings{HttpTransferCoding::kGzip, HttpTransferCoding::kDeflate};
    ruvia::http_transfer_coding_stack_decoder stack(
        codings, std::pmr::get_default_resource(), ProtocolByteLimit::limited(1024));
    std::array<char, 31> scratch{};
    bool exceeded = false;
    std::size_t cursor = 0;
    for (std::size_t count = 0; count < 1024 && !exceeded; ++count) {
        const auto next = stack.decode(std::string_view(wire).substr(cursor), scratch);
        cursor += next.consumedBytes();
        if (const auto* failure = next.failure()) {
            exceeded = failure->error() == ruvia::HttpTransferCodingDecodeError::kDecodedSizeExceeded;
            break;
        }
        if (next.decoderFailure() != nullptr || next.complete() != nullptr ||
            (next.needInput() != nullptr && cursor == wire.size())) {
            break;
        }
    }
    RUVIA_CHECK(exceeded);
}

RUVIA_TEST(transfer_coding_decoder_gzip_round_trip) {
    auto* resource = std::pmr::get_default_resource();
    http_transfer_coding_stack_decoder decoder(
        std::array{HttpTransferCoding::kGzip}, resource, ProtocolByteLimit::limited(1u << 20));

    const std::string plain = "transfer-encoding gzip body content, repeated repeated repeated";
    const std::string gz = gzipCompress(plain);
    std::pmr::string output(resource);
    RUVIA_CHECK(!appendTransferDecoded(decoder, gz, output).failed);
    const auto finishResult = decoder.finish_input();
    RUVIA_CHECK(finishResult.complete() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

RUVIA_TEST(transfer_coding_decoder_gzip_decodes_every_rfc1952_member) {
    auto* resource = std::pmr::get_default_resource();
    const std::string first = gzipCompress("first-");
    const std::string second = gzipCompress("second");
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());

    http_transfer_coding_stack_decoder contiguous(
        std::array{HttpTransferCoding::kGzip}, resource, ProtocolByteLimit::limited(1024));
    std::pmr::string contiguousOutput(resource);
    RUVIA_CHECK(!appendTransferDecoded(contiguous, first + second, contiguousOutput).failed);
    const auto contiguousFinish = contiguous.finish_input();
    RUVIA_CHECK(contiguousFinish.complete() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(contiguousOutput.data(), contiguousOutput.size()),
        std::string_view("first-second"));

    http_transfer_coding_stack_decoder fragmented(
        std::array{HttpTransferCoding::kGzip}, resource, ProtocolByteLimit::limited(1024));
    std::pmr::string fragmentedOutput(resource);
    RUVIA_CHECK(!appendTransferDecoded(fragmented, first, fragmentedOutput).failed);
    RUVIA_CHECK(!appendTransferDecoded(fragmented, second, fragmentedOutput).failed);
    const auto fragmentedFinish = fragmented.finish_input();
    RUVIA_CHECK(fragmentedFinish.complete() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(fragmentedOutput.data(), fragmentedOutput.size()),
        std::string_view("first-second"));
}

RUVIA_TEST(transfer_coded_chunked_request_plan_drives_decode_order) {
    const std::string plain = "RFC 9112 transfer coding followed by final chunked framing";
    const std::string gz = gzipCompress(plain);
    const std::string wireBody = chunked(gz);
    RUVIA_CHECK(!gz.empty());
    RUVIA_CHECK(!wireBody.empty());

    Http1ServerRequestParser parser;
    const std::string rawRequest = std::string(
                                       "POST / HTTP/1.1\r\nHost: x\r\n"
                                       "Transfer-Encoding: gzip, chunked\r\n\r\n") +
                                   wireBody;
    const auto parsed = parser.parseMessage(rawRequest);
    RUVIA_CHECK(parsed.messageReady());
    const auto* chunkedBody = parsed.bodyPlan.chunked();
    RUVIA_CHECK(chunkedBody != nullptr);
    if (chunkedBody == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(chunkedBody->transferCodings().values.size(), std::size_t{1});

    auto* resource = std::pmr::get_default_resource();
    Http1ChunkedBodyDecoder chunks({.bodyLimit = ProtocolByteLimit::limited(1u << 20)});
    http_transfer_coding_stack_decoder transfer(
        chunkedBody->transferCodings().values, resource, ProtocolByteLimit::limited(1u << 20));
    std::pmr::string output(resource);
    std::string_view pending(wireBody);
    bool complete = false;
    while (!complete) {
        const auto result = chunks.decode(pending);
        RUVIA_CHECK(result.needMore() == nullptr);
        if (result.needMore() != nullptr) {
            break;
        }
        if (const auto* bodyChunk = result.bodyChunk()) {
            RUVIA_CHECK(!appendTransferDecoded(transfer, bodyChunk->bytes(), output).failed);
        } else if (result.complete() != nullptr) {
            complete = true;
        }
        pending.remove_prefix(result.consumedBytes());
    }
    RUVIA_CHECK(complete);
    const auto finishResult = transfer.finish_input();
    RUVIA_CHECK(finishResult.complete() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

RUVIA_TEST(transfer_coding_decoder_rejects_bomb) {
    auto* resource = std::pmr::get_default_resource();
    // A 1 MiB body compresses to a tiny gzip; the decoder must abort the
    // expansion once it passes the small cap, not stage the whole megabyte.
    http_transfer_coding_stack_decoder decoder(
        std::array{HttpTransferCoding::kGzip}, resource, ProtocolByteLimit::limited(1024));

    const std::string big(1u << 20, 'a');
    const std::string gz = gzipCompress(big);
    std::pmr::string output(resource);
    const auto error = appendTransferDecoded(decoder, gz, output);
    RUVIA_CHECK(error.failed);
    RUVIA_CHECK(error.protocolError.has_value());
    RUVIA_CHECK_EQ(error.protocolError->status(), ruvia::http_status::kContentTooLarge);
    const auto finish = decoder.finish_input();
    RUVIA_CHECK(finish.failure() != nullptr);
    if (finish.failure() != nullptr) {
        RUVIA_CHECK(finish.failure()->error() ==
                    ruvia::HttpTransferCodingDecodeError::kDecodedSizeExceeded);
        RUVIA_CHECK(ruvia::httpRequestTransferCodingError(finish.failure()->error()).status() ==
                    ruvia::http_status::kContentTooLarge);
    }
}

RUVIA_TEST(transfer_coding_decoder_rebinds_caller_storage_between_steps) {
    const std::string plain(4096, 'a');
    std::string input = gzipCompress(plain);
    http_transfer_coding_stack_decoder decoder(std::array{HttpTransferCoding::kGzip},
        std::pmr::get_default_resource(), ProtocolByteLimit::limited(plain.size()));
    std::array<std::array<char, 7>, 2> windows{};
    std::string decoded;
    bool drained = false;
    for (std::size_t turn = 0; turn < 2048; ++turn) {
        auto& window = windows[turn % windows.size()];
        const auto result = decoder.decode(input, window);
        RUVIA_CHECK(result.failure() == nullptr);
        RUVIA_CHECK(result.decoderFailure() == nullptr);
        if (const auto* output = result.output()) {
            decoded.append(output->bytes());
        }
        // Replace the old input allocation and poison output immediately after
        // consuming its view. The inflater must only retain its own dictionary.
        std::string next(input.substr(result.consumedBytes()));
        input.swap(next);
        std::fill(window.begin(), window.end(), '#');
        if (result.needInput() || result.complete()) {
            drained = true;
            break;
        }
        if (result.failure() || result.decoderFailure()) {
            break;
        }
    }
    RUVIA_CHECK(drained);
    RUVIA_CHECK(input.empty());
    const auto finish = decoder.finish_input();
    RUVIA_CHECK(finish.complete() != nullptr);
    RUVIA_CHECK_EQ(decoded, plain);
}

RUVIA_TEST(transfer_coding_decoder_reclaims_allocations_after_constructor_and_decode_failure) {
    DecoderMemoryResource memory;
    memory.rejectAllocations(true);
    bool constructorFailed = false;
    try {
        http_transfer_coding_stack_decoder decoder(std::array{HttpTransferCoding::kGzip}, &memory,
            ProtocolByteLimit::unlimited());
    } catch (const std::bad_alloc&) {
        constructorFailed = true;
    }
    RUVIA_CHECK(constructorFailed);
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});

    memory.rejectAllocations(false);
    {
        http_transfer_coding_stack_decoder decoder(std::array{HttpTransferCoding::kGzip}, &memory,
            ProtocolByteLimit::unlimited());
        RUVIA_CHECK(memory.liveAllocations() != 0);
        memory.rejectAllocations(true);
        const auto input = gzipCompress(std::string(4096, 'a'));
        std::array<char, 1> output{};
        const auto failed = decoder.decode(input, output);
        RUVIA_CHECK(failed.decoderFailure() != nullptr);
        RUVIA_CHECK(failed.failure() == nullptr);
        const auto replay = decoder.decode({}, output);
        RUVIA_CHECK(replay.decoderFailure() != nullptr);
        RUVIA_CHECK_EQ(replay.consumedBytes(), std::size_t{0});
        const auto finish = decoder.finish_input();
        RUVIA_CHECK(finish.decoderFailure() != nullptr);
        RUVIA_CHECK_EQ(finish.consumedBytes(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(transfer_coding_decoder_rejects_unsupported_coding) {
    bool rejected = false;
    try {
        http_transfer_coding_stack_decoder decoder(std::array{static_cast<HttpTransferCoding>(255)},
            std::pmr::get_default_resource(), ProtocolByteLimit::unlimited());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(transfer_coding_decoder_reports_typed_wire_failures) {
    auto* resource = std::pmr::get_default_resource();
    std::array<char, std::size_t{8} * 1024> window{};

    http_transfer_coding_stack_decoder invalid(
        std::array{HttpTransferCoding::kGzip}, resource, ProtocolByteLimit::limited(1024));
    const auto invalidResult = invalid.decode("not-gzip", window);
    RUVIA_CHECK(invalidResult.failure() != nullptr);
    RUVIA_CHECK(invalidResult.decoderFailure() == nullptr);
    RUVIA_CHECK(invalidResult.failure()->error() ==
                ruvia::HttpTransferCodingDecodeError::kInvalidContent);
    RUVIA_CHECK(ruvia::httpRequestTransferCodingError(invalidResult.failure()->error()).status() ==
                ruvia::http_status::kBadRequest);

    std::string truncated = gzipCompress("truncated");
    truncated.resize(truncated.size() - 4);
    http_transfer_coding_stack_decoder incomplete(
        std::array{HttpTransferCoding::kGzip}, resource, ProtocolByteLimit::limited(1024));
    std::pmr::string ignored(resource);
    RUVIA_CHECK(!appendTransferDecoded(incomplete, truncated, ignored).failed);
    const auto incompleteFinish = incomplete.finish_input();
    RUVIA_CHECK(incompleteFinish.failure() != nullptr);
    if (incompleteFinish.failure() != nullptr) {
        RUVIA_CHECK(incompleteFinish.failure()->error() ==
                    ruvia::HttpTransferCodingDecodeError::kInvalidContent);
        RUVIA_CHECK(ruvia::httpRequestTransferCodingError(incompleteFinish.failure()->error()).status() ==
                    ruvia::http_status::kBadRequest);
    }
    const auto repeatedFinish = incomplete.finish_input();
    RUVIA_CHECK(repeatedFinish.failure() != nullptr);
    if (repeatedFinish.failure() != nullptr) {
        RUVIA_CHECK(repeatedFinish.failure()->error() ==
                    ruvia::HttpTransferCodingDecodeError::kInvalidContent);
    }

    http_transfer_coding_stack_decoder internalFailure(
        std::array{HttpTransferCoding::kGzip}, resource, ProtocolByteLimit::limited(1024));
    const auto decoderFailure = internalFailure.decode("input", {});
    RUVIA_CHECK(decoderFailure.failure() == nullptr);
    RUVIA_CHECK(decoderFailure.decoderFailure() != nullptr);
    const auto repeatedDecoderFailure = internalFailure.finish_input();
    RUVIA_CHECK(repeatedDecoderFailure.failure() == nullptr);
    RUVIA_CHECK(repeatedDecoderFailure.decoderFailure() != nullptr);

    std::string trailing = gzipCompress("complete");
    trailing.push_back('x');
    http_transfer_coding_stack_decoder extra(
        std::array{HttpTransferCoding::kGzip}, resource, ProtocolByteLimit::limited(1024));
    const auto trailingError = appendTransferDecoded(extra, trailing, ignored);
    // A short prefix of another member can remain ambiguous until framing EOF.
    // It must not make the first valid member terminal, but EOF must reject it.
    RUVIA_CHECK(!trailingError.failed);
    const auto trailingFinish = extra.finish_input();
    RUVIA_CHECK(trailingFinish.failure() != nullptr);
    if (const auto* failure = trailingFinish.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::HttpTransferCodingDecodeError::kInvalidContent);
    }
}
