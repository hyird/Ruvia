#include "ruvia/http/detail/body/HttpTransferCodingDecoder.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {

TransferCodingDecoder::TransferCodingDecoder(
    HttpTransferCoding coding,
    std::pmr::memory_resource* resource,
    ProtocolByteLimit bodyLimit)
    : resource_(httpPmrResourceOrDefault(resource)),
      bodyLimit_(bodyLimit) {
    stream_.zalloc = &TransferCodingDecoder::zallocThunk;
    stream_.zfree = &TransferCodingDecoder::zfreeThunk;
    stream_.opaque = this;
    const int rc = coding == HttpTransferCoding::kGzip
        ? inflateInit2(&stream_, 15 + 16)
        : inflateInit(&stream_);
    if (rc == Z_MEM_ERROR) {
        throw std::bad_alloc();
    }
    if (rc != Z_OK) {
        throw std::runtime_error("failed to initialize transfer-coding decoder");
    }
    initialized_ = true;
}

TransferCodingDecoder::~TransferCodingDecoder() {
    cleanup();
}

TransferCodingDecodeResult TransferCodingDecoder::decode(
    std::string_view input,
    std::span<char> outputBuffer) noexcept {
    if (failed_) {
        return TransferCodingDecodeResult(
            TransferCodingDecodeFailure(0, failure_));
    }
    if (ended_) {
        return input.empty()
            ? complete(0)
            : fail(0, TransferCodingDecodeError::kInvalidContent);
    }
    if (outputBuffer.empty()) {
        return fail(0, TransferCodingDecodeError::kDecoderFailure);
    }

    const auto step = inflateStep(input, outputBuffer);
    if (bodyLimit_.additionExceeds(decodedBytes_, step.produced)) {
        return fail(
            step.consumed,
            TransferCodingDecodeError::kDecodedSizeExceeded);
    }

    if (step.status == Z_STREAM_END) {
        if (step.consumed != input.size()) {
            return fail(
                step.consumed,
                TransferCodingDecodeError::kInvalidContent);
        }
        decodedBytes_ += step.produced;
        ended_ = true;
        return step.produced != 0
            ? output(
                  step.consumed,
                  std::string_view(outputBuffer.data(), step.produced))
            : complete(step.consumed);
    }

    if (step.status != Z_OK && step.status != Z_BUF_ERROR) {
        const auto error = step.status == Z_DATA_ERROR ||
                step.status == Z_NEED_DICT
            ? TransferCodingDecodeError::kInvalidContent
            : TransferCodingDecodeError::kDecoderFailure;
        return fail(step.consumed, error);
    }

    decodedBytes_ += step.produced;
    if (step.produced != 0) {
        return output(
            step.consumed,
            std::string_view(outputBuffer.data(), step.produced));
    }
    if (step.consumed != input.size()) {
        return fail(
            step.consumed,
            TransferCodingDecodeError::kInvalidContent);
    }
    return needInput(step.consumed);
}

TransferCodingFinishStatus TransferCodingDecoder::finishInput() noexcept {
    if (ended_ && !failed_) {
        return TransferCodingFinishStatus::kComplete;
    }
    failed_ = true;
    failure_ = TransferCodingDecodeError::kInvalidContent;
    return TransferCodingFinishStatus::kIncomplete;
}

TransferCodingDecoder::InflateStep TransferCodingDecoder::inflateStep(
    std::string_view input,
    std::span<char> output) noexcept {
    const auto inputBytes = std::min<std::size_t>(
        input.size(),
        (std::numeric_limits<uInt>::max)());
    stream_.next_in = inputBytes == 0
        ? Z_NULL
        : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream_.avail_in = static_cast<uInt>(inputBytes);
    const auto outputBytes = std::min<std::size_t>(
        output.size(),
        (std::numeric_limits<uInt>::max)());
    stream_.next_out = reinterpret_cast<Bytef*>(output.data());
    stream_.avail_out = static_cast<uInt>(outputBytes);

    const auto status = inflate(&stream_, Z_NO_FLUSH);
    return InflateStep{
        inputBytes - stream_.avail_in,
        outputBytes - stream_.avail_out,
        status};
}

TransferCodingDecodeResult TransferCodingDecoder::needInput(
    std::size_t consumed) noexcept {
    return TransferCodingDecodeResult(
        TransferCodingDecodeNeedInput(consumed));
}

TransferCodingDecodeResult TransferCodingDecoder::output(
    std::size_t consumed,
    std::string_view bytes) noexcept {
    return TransferCodingDecodeResult(
        TransferCodingDecodeOutput(consumed, bytes));
}

TransferCodingDecodeResult TransferCodingDecoder::complete(
    std::size_t consumed) noexcept {
    return TransferCodingDecodeResult(
        TransferCodingDecodeComplete(consumed));
}

TransferCodingDecodeResult TransferCodingDecoder::fail(
    std::size_t consumed,
    TransferCodingDecodeError error) noexcept {
    failed_ = true;
    failure_ = error;
    return TransferCodingDecodeResult(
        TransferCodingDecodeFailure(consumed, error));
}

voidpf TransferCodingDecoder::zallocThunk(voidpf opaque, uInt items, uInt size) noexcept {
    auto* self = static_cast<TransferCodingDecoder*>(opaque);
    if (self == nullptr || items == 0 || size == 0) {
        return nullptr;
    }
    const auto itemBytes = static_cast<std::size_t>(items);
    const auto sizeBytes = static_cast<std::size_t>(size);
    if (itemBytes > (std::numeric_limits<std::size_t>::max)() / sizeBytes) {
        return nullptr;
    }
    const auto payloadBytes = itemBytes * sizeBytes;
    if (payloadBytes > (std::numeric_limits<std::size_t>::max)() - sizeof(ZlibAllocationHeader)) {
        return nullptr;
    }
    const auto totalBytes = sizeof(ZlibAllocationHeader) + payloadBytes;
    try {
        auto* raw = static_cast<std::byte*>(self->resource_->allocate(totalBytes, alignof(ZlibAllocationHeader)));
        auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
        header->resource = self->resource_;
        header->bytes = totalBytes;
        return raw + sizeof(ZlibAllocationHeader);
    } catch (...) {
        return nullptr;
    }
}

void TransferCodingDecoder::zfreeThunk(voidpf, voidpf address) noexcept {
    if (address == nullptr) {
        return;
    }
    auto* raw = static_cast<std::byte*>(address) - sizeof(ZlibAllocationHeader);
    auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
    header->resource->deallocate(raw, header->bytes, alignof(ZlibAllocationHeader));
}

void TransferCodingDecoder::cleanup() noexcept {
    if (initialized_) {
        (void)inflateEnd(&stream_);
        initialized_ = false;
    }
}

}  // namespace ruvia::detail
