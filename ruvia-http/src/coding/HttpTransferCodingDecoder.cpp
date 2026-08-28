#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"

#include "ruvia/http/detail/coding/ZlibPmrAllocation.h"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

TransferCodingDecoder::TransferCodingDecoder(HttpTransferCoding coding, std::pmr::memory_resource* resource, ProtocolByteLimit bodyLimit)
    : resource_(httpPmrResourceOrDefault(resource)),
      bodyLimit_(bodyLimit),
      coding_(coding) {
    stream_.zalloc = &TransferCodingDecoder::zallocThunk;
    stream_.zfree = &TransferCodingDecoder::zfreeThunk;
    stream_.opaque = this;
    const int rc = coding == HttpTransferCoding::kGzip ? inflateInit2(&stream_, 15 + 16) : inflateInit(&stream_);
    if (rc == Z_MEM_ERROR) {
        throw std::bad_alloc();
    }
    if (rc != Z_OK) {
        throw std::runtime_error("failed to initialize transfer-coding decoder");
    }
}

TransferCodingDecoder::~TransferCodingDecoder() {
    (void)inflateEnd(&stream_);
}

TransferCodingDecodeResult TransferCodingDecoder::decode(std::string_view input, std::span<char> outputBuffer) noexcept {
    if (const auto* failure = std::get_if<TransferCodingDecodeError>(&state_)) {
        return fail(0, *failure);
    }
    if (std::holds_alternative<Complete>(state_)) {
        return input.empty() ? complete(0) : fail(0, TransferCodingDecodeError::kInvalidContent);
    }
    if (outputBuffer.empty()) {
        return fail(0, TransferCodingDecodeError::kDecoderFailure);
    }

    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (;;) {
        if (std::holds_alternative<GzipMemberBoundary>(state_)) {
            if (consumed == input.size()) {
                return produced != 0 ? output(consumed, std::string_view(outputBuffer.data(), produced)) : needInput(consumed);
            }
            if (inflateReset(&stream_) != Z_OK) {
                return fail(consumed, TransferCodingDecodeError::kDecoderFailure);
            }
            state_.emplace<Active>();
        }

        const auto step = inflateStep(input.substr(consumed), outputBuffer.subspan(produced));
        consumed += step.consumed;
        if (bodyLimit_.additionExceeds(decodedBytes_, step.produced)) {
            return fail(consumed, TransferCodingDecodeError::kDecodedSizeExceeded);
        }
        decodedBytes_ += step.produced;
        produced += step.produced;

        if (step.status == Z_STREAM_END) {
            if (coding_ != HttpTransferCoding::kGzip) {
                if (consumed != input.size()) {
                    return fail(consumed, TransferCodingDecodeError::kInvalidContent);
                }
                state_.emplace<Complete>();
                return produced != 0 ? output(consumed, std::string_view(outputBuffer.data(), produced)) : complete(consumed);
            }

            // RFC 1952 gzip data is a series of members. A member boundary is
            // not the end of the transfer coding: another member can arrive in
            // the same HTTP chunk or in a later one. Only framing EOF, reported
            // through finishInput(), commits this boundary as complete.
            state_.emplace<GzipMemberBoundary>();
            if (produced == outputBuffer.size()) {
                return output(consumed, std::string_view(outputBuffer.data(), produced));
            }
            continue;
        }

        if (step.status != Z_OK && step.status != Z_BUF_ERROR) {
            const auto error = step.status == Z_DATA_ERROR || step.status == Z_NEED_DICT ? TransferCodingDecodeError::kInvalidContent : TransferCodingDecodeError::kDecoderFailure;
            return fail(consumed, error);
        }

        if (produced != 0) {
            return output(consumed, std::string_view(outputBuffer.data(), produced));
        }
        if (consumed != input.size()) {
            return fail(consumed, TransferCodingDecodeError::kInvalidContent);
        }
        return needInput(consumed);
    }
}

TransferCodingDecodeResult TransferCodingDecoder::finishInput() noexcept {
    if (std::holds_alternative<Complete>(state_) || std::holds_alternative<GzipMemberBoundary>(state_)) {
        state_.emplace<Complete>();
        return complete(0);
    }
    if (const auto* failure = std::get_if<TransferCodingDecodeError>(&state_)) {
        return fail(0, *failure);
    }
    return fail(0, TransferCodingDecodeError::kInvalidContent);
}

TransferCodingDecoder::InflateStep TransferCodingDecoder::inflateStep(std::string_view input, std::span<char> output) noexcept {
    const auto inputBytes = std::min<std::size_t>(input.size(), (std::numeric_limits<uInt>::max)());
    stream_.next_in = inputBytes == 0 ? Z_NULL : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream_.avail_in = static_cast<uInt>(inputBytes);
    const auto outputBytes = std::min<std::size_t>(output.size(), (std::numeric_limits<uInt>::max)());
    stream_.next_out = reinterpret_cast<Bytef*>(output.data());
    stream_.avail_out = static_cast<uInt>(outputBytes);

    const auto status = inflate(&stream_, Z_NO_FLUSH);
    return InflateStep{inputBytes - stream_.avail_in, outputBytes - stream_.avail_out, status};
}

TransferCodingDecodeResult TransferCodingDecoder::needInput(std::size_t consumed) noexcept {
    return TransferCodingDecodeResult(TransferCodingDecodeNeedInput(consumed));
}

TransferCodingDecodeResult TransferCodingDecoder::output(std::size_t consumed, std::string_view bytes) noexcept {
    return TransferCodingDecodeResult(TransferCodingDecodeOutput(consumed, bytes));
}

TransferCodingDecodeResult TransferCodingDecoder::complete(std::size_t consumed) noexcept {
    return TransferCodingDecodeResult(TransferCodingDecodeComplete(consumed));
}

TransferCodingDecodeResult TransferCodingDecoder::fail(std::size_t consumed, TransferCodingDecodeError error) noexcept {
    state_.emplace<TransferCodingDecodeError>(error);
    if (error == TransferCodingDecodeError::kDecoderFailure) {
        return TransferCodingDecodeResult(TransferCodingDecoderFailure(consumed));
    }
    return TransferCodingDecodeResult(TransferCodingDecodeProtocolFailure(consumed, error));
}

voidpf TransferCodingDecoder::zallocThunk(voidpf opaque, uInt items, uInt size) noexcept {
    auto* self = static_cast<TransferCodingDecoder*>(opaque);
    return self == nullptr ? nullptr : zlibPmrAllocate(self->resource_, items, size);
}

void TransferCodingDecoder::zfreeThunk(voidpf, voidpf address) noexcept {
    zlibPmrFree(address);
}

}  // namespace ruvia::detail
