#include "content_decoding_fixture.h"

// Decoding a transfer-coded (chunked, then coded) request body.

RUVIA_TEST(transfer_coding_decoder_gzip_round_trip) {
    auto* resource = std::pmr::get_default_resource();
    TransferCodingDecoder decoder(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1u << 20));

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

    TransferCodingDecoder contiguous(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    std::pmr::string contiguousOutput(resource);
    RUVIA_CHECK(!appendTransferDecoded(
        contiguous, first + second, contiguousOutput).failed);
    const auto contiguousFinish = contiguous.finishInput();
    RUVIA_CHECK(contiguousFinish.complete() != nullptr);
    RUVIA_CHECK_EQ(
        std::string_view(contiguousOutput.data(), contiguousOutput.size()),
        std::string_view("first-second"));

    TransferCodingDecoder fragmented(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    std::pmr::string fragmentedOutput(resource);
    RUVIA_CHECK(!appendTransferDecoded(
        fragmented, first, fragmentedOutput).failed);
    RUVIA_CHECK(!appendTransferDecoded(
        fragmented, second, fragmentedOutput).failed);
    const auto fragmentedFinish = fragmented.finishInput();
    RUVIA_CHECK(fragmentedFinish.complete() != nullptr);
    RUVIA_CHECK_EQ(
        std::string_view(fragmentedOutput.data(), fragmentedOutput.size()),
        std::string_view("first-second"));
}

RUVIA_TEST(transfer_coded_chunked_request_plan_drives_decode_order) {
    const std::string plain =
        "RFC 9112 transfer coding followed by final chunked framing";
    const std::string gz = gzipCompress(plain);
    const std::string wireBody = chunked(gz);
    RUVIA_CHECK(!gz.empty());
    RUVIA_CHECK(!wireBody.empty());

    Http1ServerRequestParser parser;
    const std::string rawRequest =
        std::string(
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
    Http1ChunkedBodyDecoder chunks(ProtocolByteLimit::limited(1u << 20));
    TransferCodingDecoder transfer(
        chunkedBody->transferCodings().values[0],
        resource,
        ProtocolByteLimit::limited(1u << 20));
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
            RUVIA_CHECK(!appendTransferDecoded(
                transfer, bodyChunk->bytes(), output).failed);
        } else if (result.complete() != nullptr) {
            complete = true;
        }
        pending.remove_prefix(result.consumedBytes());
    }
    RUVIA_CHECK(complete);
    const auto finishResult = transfer.finishInput();
    RUVIA_CHECK(finishResult.complete() != nullptr);
    RUVIA_CHECK_EQ(
        std::string_view(output.data(), output.size()),
        std::string_view(plain));
}

RUVIA_TEST(transfer_coding_decoder_rejects_bomb) {
    auto* resource = std::pmr::get_default_resource();
    // A 1 MiB body compresses to a tiny gzip; the decoder must abort the
    // expansion once it passes the small cap, not stage the whole megabyte.
    TransferCodingDecoder decoder(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));

    const std::string big(1u << 20, 'a');
    const std::string gz = gzipCompress(big);
    std::pmr::string output(resource);
    const auto error = appendTransferDecoded(decoder, gz, output);
    RUVIA_CHECK(error.failed);
    RUVIA_CHECK(error.protocolError.has_value());
    RUVIA_CHECK_EQ(error.protocolError->status(), ruvia::http_status::kContentTooLarge);
    const auto finish = decoder.finishInput();
    RUVIA_CHECK(finish.protocolFailure() != nullptr);
    if (finish.protocolFailure() != nullptr) {
        RUVIA_CHECK(
            finish.protocolFailure()->protocolError().status() == ruvia::http_status::kContentTooLarge);
    }
}

RUVIA_TEST(transfer_coding_decoder_reports_typed_wire_failures) {
    auto* resource = std::pmr::get_default_resource();
    std::array<char, ruvia::detail::kBodyReadChunkBytes> window{};

    TransferCodingDecoder invalid(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    const auto invalidResult = invalid.decode("not-gzip", window);
    RUVIA_CHECK(invalidResult.protocolFailure() != nullptr);
    RUVIA_CHECK(invalidResult.decoderFailure() == nullptr);
    RUVIA_CHECK_EQ(
        invalidResult.protocolFailure()->protocolError().status(), ruvia::http_status::kBadRequest);

    std::string truncated = gzipCompress("truncated");
    truncated.resize(truncated.size() - 4);
    TransferCodingDecoder incomplete(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    std::pmr::string ignored(resource);
    RUVIA_CHECK(!appendTransferDecoded(
        incomplete, truncated, ignored).failed);
    const auto incompleteFinish = incomplete.finishInput();
    RUVIA_CHECK(incompleteFinish.protocolFailure() != nullptr);
    if (incompleteFinish.protocolFailure() != nullptr) {
        RUVIA_CHECK(
            incompleteFinish.protocolFailure()->protocolError().status() == ruvia::http_status::kBadRequest);
    }
    const auto repeatedFinish = incomplete.finishInput();
    RUVIA_CHECK(repeatedFinish.protocolFailure() != nullptr);
    if (repeatedFinish.protocolFailure() != nullptr) {
        RUVIA_CHECK(
            repeatedFinish.protocolFailure()->protocolError().status() == ruvia::http_status::kBadRequest);
    }

    TransferCodingDecoder internalFailure(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    const auto decoderFailure = internalFailure.decode("input", {});
    RUVIA_CHECK(decoderFailure.protocolFailure() == nullptr);
    RUVIA_CHECK(decoderFailure.decoderFailure() != nullptr);
    const auto repeatedDecoderFailure = internalFailure.finishInput();
    RUVIA_CHECK(repeatedDecoderFailure.protocolFailure() == nullptr);
    RUVIA_CHECK(repeatedDecoderFailure.decoderFailure() != nullptr);

    std::string trailing = gzipCompress("complete");
    trailing.push_back('x');
    TransferCodingDecoder extra(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    const auto trailingError = appendTransferDecoded(
        extra, trailing, ignored);
    // A short prefix of another member can remain ambiguous until framing EOF.
    // It must not make the first valid member terminal, but EOF must reject it.
    RUVIA_CHECK(!trailingError.failed);
    const auto trailingFinish = extra.finishInput();
    RUVIA_CHECK(trailingFinish.protocolFailure() != nullptr);
    if (const auto* failure = trailingFinish.protocolFailure()) {
        RUVIA_CHECK_EQ(failure->protocolError().status(), ruvia::http_status::kBadRequest);
    }
}
