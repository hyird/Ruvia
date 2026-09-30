#include "ruvia/http/HttpTransferCodingDecoder.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

#include "ruvia/http/detail/coding/ZlibPmrAllocation.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia {

HttpTransferCodingDecoder::HttpTransferCodingDecoder(
    HttpTransferCoding coding, std::pmr::memory_resource* resource, ProtocolByteLimit bodyLimit)
    : resource_(detail::httpPmrResourceOrDefault(resource)),
      bodyLimit_(bodyLimit),
      coding_(coding) {
    int windowBits = 0;
    switch (coding) {
        case HttpTransferCoding::kGzip:
            windowBits = 15 + 16;
            break;
        case HttpTransferCoding::kDeflate:
            windowBits = 15;
            break;
        default:
            throw std::invalid_argument("unsupported HTTP transfer coding");
    }
    stream_.zalloc = &HttpTransferCodingDecoder::zallocThunk;
    stream_.zfree = &HttpTransferCodingDecoder::zfreeThunk;
    stream_.opaque = this;
    const int rc = inflateInit2(&stream_, windowBits);
    if (rc == Z_MEM_ERROR) {
        throw std::bad_alloc();
    }
    if (rc != Z_OK) {
        throw std::runtime_error("failed to initialize transfer-coding decoder");
    }
}

HttpTransferCodingDecoder::~HttpTransferCodingDecoder() {
    (void)inflateEnd(&stream_);
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::decode(
    std::string_view input, std::span<char> outputBuffer) noexcept {
    if (std::holds_alternative<DecoderFailed>(state_)) {
        return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
    }
    if (const auto* failure = std::get_if<HttpTransferCodingDecodeError>(&state_)) {
        return fail(0, *failure);
    }
    if (std::holds_alternative<Complete>(state_)) {
        return input.empty() ? complete(0) : fail(0, HttpTransferCodingDecodeError::kInvalidContent);
    }
    if (outputBuffer.empty()) {
        return failDecoder(0);
    }

    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (;;) {
        if (std::holds_alternative<GzipMemberBoundary>(state_)) {
            if (consumed == input.size()) {
                return produced != 0
                           ? output(consumed, std::string_view(outputBuffer.data(), produced))
                           : needInput(consumed);
            }
            if (inflateReset(&stream_) != Z_OK) {
                return failDecoder(consumed);
            }
            state_.emplace<Active>();
        }

        const auto step = inflateStep(input.substr(consumed), outputBuffer.subspan(produced));
        consumed += step.consumed;
        if (bodyLimit_.additionExceeds(decodedBytes_, step.produced)) {
            return fail(consumed, HttpTransferCodingDecodeError::kDecodedSizeExceeded);
        }
        decodedBytes_ += step.produced;
        produced += step.produced;

        if (step.status == Z_STREAM_END) {
            if (coding_ != HttpTransferCoding::kGzip) {
                if (consumed != input.size()) {
                    return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
                }
                state_.emplace<Complete>();
                return produced != 0
                           ? output(consumed, std::string_view(outputBuffer.data(), produced))
                           : complete(consumed);
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
            if (step.status == Z_DATA_ERROR || step.status == Z_NEED_DICT) {
                return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
            }
            return failDecoder(consumed);
        }

        if (produced != 0) {
            return output(consumed, std::string_view(outputBuffer.data(), produced));
        }
        if (consumed != input.size()) {
            return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
        }
        return needInput(consumed);
    }
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::finishInput() noexcept {
    if (std::holds_alternative<DecoderFailed>(state_)) {
        return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
    }
    if (std::holds_alternative<Complete>(state_) ||
        std::holds_alternative<GzipMemberBoundary>(state_)) {
        state_.emplace<Complete>();
        return complete(0);
    }
    if (const auto* failure = std::get_if<HttpTransferCodingDecodeError>(&state_)) {
        return fail(0, *failure);
    }
    return fail(0, HttpTransferCodingDecodeError::kInvalidContent);
}

HttpTransferCodingDecoder::InflateStep HttpTransferCodingDecoder::inflateStep(
    std::string_view input, std::span<char> output) noexcept {
    const auto inputBytes = std::min<std::size_t>(input.size(), (std::numeric_limits<uInt>::max)());
    stream_.next_in =
        inputBytes == 0 ? Z_NULL : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream_.avail_in = static_cast<uInt>(inputBytes);
    const auto outputBytes =
        std::min<std::size_t>(output.size(), (std::numeric_limits<uInt>::max)());
    stream_.next_out = reinterpret_cast<Bytef*>(output.data());
    stream_.avail_out = static_cast<uInt>(outputBytes);

    const auto status = inflate(&stream_, Z_NO_FLUSH);
    const auto consumed = inputBytes - stream_.avail_in;
    const auto produced = outputBytes - stream_.avail_out;
    stream_.next_in = Z_NULL;
    stream_.avail_in = 0;
    stream_.next_out = Z_NULL;
    stream_.avail_out = 0;
    return InflateStep{consumed, produced, status};
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::needInput(std::size_t consumed) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeNeedInput(consumed));
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::output(
    std::size_t consumed, std::string_view bytes) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeOutputView(consumed, bytes));
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::complete(std::size_t consumed) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeComplete(consumed));
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::fail(
    std::size_t consumed, HttpTransferCodingDecodeError error) noexcept {
    state_.emplace<HttpTransferCodingDecodeError>(error);
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeFailure(consumed, error));
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::failDecoder(std::size_t consumed) noexcept {
    state_.emplace<DecoderFailed>();
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(consumed));
}

voidpf HttpTransferCodingDecoder::zallocThunk(voidpf opaque, uInt items, uInt size) noexcept {
    auto* self = static_cast<HttpTransferCodingDecoder*>(opaque);
    return self == nullptr ? nullptr : detail::zlibPmrAllocate(self->resource_, items, size);
}

void HttpTransferCodingDecoder::zfreeThunk(voidpf, voidpf address) noexcept {
    detail::zlibPmrFree(address);
}

}  // namespace ruvia
