#include "content_decoding_fixture.h"

namespace {
class DecoderMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations(bool reject) noexcept {
        rejectAllocations_ = reject;
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
        if (rejectAllocations_) {
            throw std::bad_alloc();
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
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
    std::size_t live_{0};
};
}  // namespace

// Decoding a transfer-coded (chunked, then coded) request body.

RUVIA_TEST(transfer_coding_decoder_gzip_round_trip) {
    auto* resource = std::pmr::get_default_resource();
    HttpTransferCodingDecoder decoder(
        HttpTransferCoding::kGzip, resource, ProtocolByteLimit::limited(1u << 20));

    const std::string plain = "transfer-encoding gzip body content, repeated repeated repeated";
    const std::string gz = gzipCompress(plain);
    std::pmr::string output(resource);
    RUVIA_CHECK(!appendTransferDecoded(decoder, gz, output).failed);
    const auto finishResult = decoder.finishInput();
    RUVIA_CHECK(finishResult.complete() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

RUVIA_TEST(transfer_coding_decoder_gzip_decodes_every_rfc1952_member) {
    auto* resource = std::pmr::get_default_resource();
    const std::string first = gzipCompress("first-");
    const std::string second = gzipCompress("second");
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());

    HttpTransferCodingDecoder contiguous(
        HttpTransferCoding::kGzip, resource, ProtocolByteLimit::limited(1024));
    std::pmr::string contiguousOutput(resource);
    RUVIA_CHECK(!appendTransferDecoded(contiguous, first + second, contiguousOutput).failed);
    const auto contiguousFinish = contiguous.finishInput();
    RUVIA_CHECK(contiguousFinish.complete() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(contiguousOutput.data(), contiguousOutput.size()),
        std::string_view("first-second"));

    HttpTransferCodingDecoder fragmented(
        HttpTransferCoding::kGzip, resource, ProtocolByteLimit::limited(1024));
    std::pmr::string fragmentedOutput(resource);
    RUVIA_CHECK(!appendTransferDecoded(fragmented, first, fragmentedOutput).failed);
    RUVIA_CHECK(!appendTransferDecoded(fragmented, second, fragmentedOutput).failed);
    const auto fragmentedFinish = fragmented.finishInput();
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
    RUVIA_CHECK_EQ(chunkedBody->transferCodings().count, std::size_t{1});

    auto* resource = std::pmr::get_default_resource();
    Http1ChunkedBodyDecoder chunks({.bodyLimit = ProtocolByteLimit::limited(1u << 20)});
    HttpTransferCodingDecoder transfer(
        chunkedBody->transferCodings().values[0], resource, ProtocolByteLimit::limited(1u << 20));
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
    const auto finishResult = transfer.finishInput();
    RUVIA_CHECK(finishResult.complete() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

RUVIA_TEST(transfer_coding_decoder_rejects_bomb) {
    auto* resource = std::pmr::get_default_resource();
    // A 1 MiB body compresses to a tiny gzip; the decoder must abort the
    // expansion once it passes the small cap, not stage the whole megabyte.
    HttpTransferCodingDecoder decoder(
        HttpTransferCoding::kGzip, resource, ProtocolByteLimit::limited(1024));

    const std::string big(1u << 20, 'a');
    const std::string gz = gzipCompress(big);
    std::pmr::string output(resource);
    const auto error = appendTransferDecoded(decoder, gz, output);
    RUVIA_CHECK(error.failed);
    RUVIA_CHECK(error.protocolError.has_value());
    RUVIA_CHECK_EQ(error.protocolError->status(), ruvia::http_status::kContentTooLarge);
    const auto finish = decoder.finishInput();
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
    HttpTransferCodingDecoder decoder(HttpTransferCoding::kGzip,
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
    const auto finish = decoder.finishInput();
    RUVIA_CHECK(finish.complete() != nullptr);
    RUVIA_CHECK_EQ(decoded, plain);
}

RUVIA_TEST(transfer_coding_decoder_reclaims_allocations_after_constructor_and_decode_failure) {
    DecoderMemoryResource memory;
    memory.rejectAllocations(true);
    bool constructorFailed = false;
    try {
        HttpTransferCodingDecoder decoder(HttpTransferCoding::kGzip, &memory,
            ProtocolByteLimit::unlimited());
    } catch (const std::bad_alloc&) {
        constructorFailed = true;
    }
    RUVIA_CHECK(constructorFailed);
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});

    memory.rejectAllocations(false);
    {
        HttpTransferCodingDecoder decoder(HttpTransferCoding::kGzip, &memory,
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
        const auto finish = decoder.finishInput();
        RUVIA_CHECK(finish.decoderFailure() != nullptr);
        RUVIA_CHECK_EQ(finish.consumedBytes(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(transfer_coding_decoder_rejects_unsupported_coding) {
    bool rejected = false;
    try {
        HttpTransferCodingDecoder decoder(static_cast<HttpTransferCoding>(255),
            std::pmr::get_default_resource(), ProtocolByteLimit::unlimited());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(transfer_coding_decoder_reports_typed_wire_failures) {
    auto* resource = std::pmr::get_default_resource();
    std::array<char, std::size_t{8} * 1024> window{};

    HttpTransferCodingDecoder invalid(
        HttpTransferCoding::kGzip, resource, ProtocolByteLimit::limited(1024));
    const auto invalidResult = invalid.decode("not-gzip", window);
    RUVIA_CHECK(invalidResult.failure() != nullptr);
    RUVIA_CHECK(invalidResult.decoderFailure() == nullptr);
    RUVIA_CHECK(invalidResult.failure()->error() ==
                ruvia::HttpTransferCodingDecodeError::kInvalidContent);
    RUVIA_CHECK(ruvia::httpRequestTransferCodingError(invalidResult.failure()->error()).status() ==
                ruvia::http_status::kBadRequest);

    std::string truncated = gzipCompress("truncated");
    truncated.resize(truncated.size() - 4);
    HttpTransferCodingDecoder incomplete(
        HttpTransferCoding::kGzip, resource, ProtocolByteLimit::limited(1024));
    std::pmr::string ignored(resource);
    RUVIA_CHECK(!appendTransferDecoded(incomplete, truncated, ignored).failed);
    const auto incompleteFinish = incomplete.finishInput();
    RUVIA_CHECK(incompleteFinish.failure() != nullptr);
    if (incompleteFinish.failure() != nullptr) {
        RUVIA_CHECK(incompleteFinish.failure()->error() ==
                    ruvia::HttpTransferCodingDecodeError::kInvalidContent);
        RUVIA_CHECK(ruvia::httpRequestTransferCodingError(incompleteFinish.failure()->error()).status() ==
                    ruvia::http_status::kBadRequest);
    }
    const auto repeatedFinish = incomplete.finishInput();
    RUVIA_CHECK(repeatedFinish.failure() != nullptr);
    if (repeatedFinish.failure() != nullptr) {
        RUVIA_CHECK(repeatedFinish.failure()->error() ==
                    ruvia::HttpTransferCodingDecodeError::kInvalidContent);
    }

    HttpTransferCodingDecoder internalFailure(
        HttpTransferCoding::kGzip, resource, ProtocolByteLimit::limited(1024));
    const auto decoderFailure = internalFailure.decode("input", {});
    RUVIA_CHECK(decoderFailure.failure() == nullptr);
    RUVIA_CHECK(decoderFailure.decoderFailure() != nullptr);
    const auto repeatedDecoderFailure = internalFailure.finishInput();
    RUVIA_CHECK(repeatedDecoderFailure.failure() == nullptr);
    RUVIA_CHECK(repeatedDecoderFailure.decoderFailure() != nullptr);

    std::string trailing = gzipCompress("complete");
    trailing.push_back('x');
    HttpTransferCodingDecoder extra(
        HttpTransferCoding::kGzip, resource, ProtocolByteLimit::limited(1024));
    const auto trailingError = appendTransferDecoded(extra, trailing, ignored);
    // A short prefix of another member can remain ambiguous until framing EOF.
    // It must not make the first valid member terminal, but EOF must reject it.
    RUVIA_CHECK(!trailingError.failed);
    const auto trailingFinish = extra.finishInput();
    RUVIA_CHECK(trailingFinish.failure() != nullptr);
    if (const auto* failure = trailingFinish.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::HttpTransferCodingDecodeError::kInvalidContent);
    }
}
