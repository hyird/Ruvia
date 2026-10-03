#pragma once

#include "ruvia/web/detail/body/HttpStreamBodyReaderErrors.h"

namespace ruvia::detail {

template <typename Stream>
Task<std::optional<std::span<const std::byte>>> StreamBodyReader<Stream>::readChunked() {
    compactPending();
    if (finished_) {
        co_return std::nullopt;
    }

    for (;;) {
        const auto source = !initialBodyAndPipeline_.empty()
                                ? std::string_view(initialBodyAndPipeline_)
                                : std::string_view(buffer_);
        const auto result = chunkDecoder_.decode(source.substr(readCursor_));
        if (result.consumedBytes() != 0) {
            pendingCompactUntil_ = readCursor_ + result.consumedBytes();
        }
        if (const auto* bodyChunk = result.bodyChunk()) {
            co_return ::ruvia::asBytes(bodyChunk->bytes());
        }
        if (const auto* complete = result.complete()) {
            if (!trailers_.appendHttp1(complete->trailers())) {
                throw std::logic_error("HTTP/1 decoder published invalid request trailers");
            }
            compactPending();
            co_return std::nullopt;
        }
        if (const auto* failure = result.failure()) {
            throw httpRequestChunkDecodeError(failure->error());
        }
        if (result.needMore() == nullptr) {
            throw std::logic_error("unexpected HTTP/1 chunk decode result");
        }
        compactPending();
        materializeInitialRemainder();
        co_await readMore();
    }
}

template <typename Stream>
Task<std::optional<std::span<const std::byte>>> StreamBodyReader<Stream>::read_transfer_decoded_chunked() {
    if (transfer_decoder_ == nullptr) {
        auto chunk = co_await readChunked();
        if (!chunk) {
            markFinished();
        }
        co_return chunk;
    }

    if (transfer_output_.empty()) {
        ::ruvia::resizePmrStringForOverwrite(transfer_output_, kHttpBodyBufferBytes);
    }

    // Keep the borrowed encoded chunk until the decoder reports its consumed
    // prefix. readChunked() is called only after that view is empty, so its
    // compaction cannot invalidate decoder input across application reads.
    for (;;) {
        const auto result =
            transfer_decoder_->decode(transfer_input_, std::span<char>(transfer_output_));
        transfer_input_.remove_prefix(std::min(transfer_input_.size(), result.consumedBytes()));
        if (const auto* output = result.output()) {
            co_return ::ruvia::asBytes(output->bytes());
        }
        if (const auto* failure = result.failure()) {
            throwTransferCodingProtocolFailure(*failure);
        }
        if (result.decoderFailure() != nullptr) {
            throwHttpTransferCodingDecoderFailure();
        }
        if (result.complete() == nullptr && result.needInput() == nullptr) {
            throw std::logic_error("unexpected transfer-coding decode result");
        }

        auto chunk = co_await readChunked();
        if (!chunk) {
            require_complete_transfer_coding(*transfer_decoder_);
            markFinished();
            co_return std::nullopt;
        }
        transfer_input_ = ::ruvia::asChars(*chunk);
    }
}

}  // namespace ruvia::detail
