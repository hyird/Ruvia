#include "ruvia/http/detail/body/HttpTransferCodingDecoder.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "ruvia/http/HttpProtocolError.h"

namespace ruvia::detail {

void throwRequestBodyTooLarge() {
    throw HttpProtocolError(413, "request body is too large");
}

TransferCodingDecoder::TransferCodingDecoder(
    HttpTransferCodings codings,
    std::pmr::polymorphic_allocator<char> allocator,
    ProtocolByteLimit bodyLimit)
    : codings_(codings),
      output_(allocator),
      resource_(allocator.resource()),
      bodyLimit_(bodyLimit) {
    if (codings_.count > kMaxTransferCodings) {
        throw std::invalid_argument("invalid Transfer-Encoding header");
    }
    if (!codings_.empty()) {
        stream_.zalloc = &TransferCodingDecoder::zallocThunk;
        stream_.zfree = &TransferCodingDecoder::zfreeThunk;
        stream_.opaque = this;
        const auto coding = codings_.values[0];
        const int rc = coding == HttpTransferCoding::kGzip
            ? inflateInit2(&stream_, 15 + 16)
            : inflateInit(&stream_);
        if (rc != Z_OK) {
            throw std::invalid_argument("invalid transfer-coding state");
        }
        initialized_ = true;
    }
}

TransferCodingDecoder::~TransferCodingDecoder() {
    cleanup();
}

bool TransferCodingDecoder::empty() const noexcept {
    return codings_.count == 0;
}

bool TransferCodingDecoder::finished() const noexcept {
    if (codings_.count == 0) {
        return true;
    }
    return ended_;
}

void TransferCodingDecoder::setInput(std::string_view input) {
    if (ended_) {
        if (!input.empty()) {
            throw std::invalid_argument("invalid transfer-coding body");
        }
        return;
    }
    if (pendingOffset_ < pendingInput_.size()) {
        throw std::logic_error("transfer-coding input not fully consumed");
    }
    pendingInput_ = input;
    pendingOffset_ = 0;
}

std::string_view TransferCodingDecoder::produce() {
    if (!initialized_ || ended_) {
        return {};
    }
    if (output_.empty()) {
        resizePmrStringForOverwrite(output_, kBodyReadChunkBytes);
    }
    const auto step = inflateStep(output_.data(), output_.size());
    applyStatus(step);
    return std::string_view(output_.data(), step.produced);
}

void TransferCodingDecoder::decodeAppend(std::string_view input, std::pmr::string& target) {
    if (codings_.count == 0) {
        if (bodyLimit_.additionExceeds(target.size(), input.size()) ||
            bodyLimit_.additionExceeds(decodedBytes_, input.size())) {
            throwRequestBodyTooLarge();
        }
        target.append(input.data(), input.size());
        decodedBytes_ += input.size();
        return;
    }
    setInput(input);
    // Inflate straight into the target tail; resize_and_overwrite skips the
    // zero-fill a plain resize() would pay for each window.
    while (!ended_) {
        const auto oldSize = target.size();
        InflateStep step;
        target.resize_and_overwrite(
            oldSize + kBodyReadChunkBytes,
            [this, oldSize, &step](char* data, std::size_t) noexcept {
                step = inflateStep(data + oldSize, kBodyReadChunkBytes);
                return oldSize + step.produced;
            });
        applyStatus(step);
        if (step.produced == 0) {
            return;
        }
    }
}

void TransferCodingDecoder::finish() {
    if (codings_.count == 0) {
        return;
    }
    if (!ended_) {
        throw std::invalid_argument("incomplete transfer-coding body");
    }
}

TransferCodingDecoder::InflateStep TransferCodingDecoder::inflateStep(char* out, std::size_t capacity) noexcept {
    const auto inputBytes = std::min<std::size_t>(
        pendingInput_.size() - pendingOffset_,
        (std::numeric_limits<uInt>::max)());
    stream_.next_in = inputBytes == 0
        ? Z_NULL
        : reinterpret_cast<Bytef*>(const_cast<char*>(pendingInput_.data() + pendingOffset_));
    stream_.avail_in = static_cast<uInt>(inputBytes);
    stream_.next_out = reinterpret_cast<Bytef*>(out);
    stream_.avail_out = static_cast<uInt>(capacity);

    const auto status = inflate(&stream_, Z_NO_FLUSH);
    pendingOffset_ += inputBytes - stream_.avail_in;
    return InflateStep{capacity - stream_.avail_out, status};
}

void TransferCodingDecoder::applyStatus(const InflateStep& step) {
    checkProducedLimit(step.produced);
    decodedBytes_ += step.produced;
    if (step.status == Z_STREAM_END) {
        ended_ = true;
        if (pendingOffset_ != pendingInput_.size()) {
            throw std::invalid_argument("invalid transfer-coding body");
        }
    } else if (step.status == Z_BUF_ERROR) {
        // Benign "no progress" signal: legal only while waiting for more
        // input. With unconsumed input it means the stream is corrupt.
        if (step.produced == 0 && pendingOffset_ < pendingInput_.size()) {
            throw std::invalid_argument("invalid transfer-coding body");
        }
    } else if (step.status != Z_OK) {
        throw std::invalid_argument("invalid transfer-coding body");
    }
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

void TransferCodingDecoder::checkProducedLimit(std::size_t produced) const {
    if (produced == 0) {
        return;
    }
    if (bodyLimit_.additionExceeds(decodedBytes_, produced)) {
        throwRequestBodyTooLarge();
    }
}

}  // namespace ruvia::detail
