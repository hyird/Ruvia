#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>

#include <asio.hpp>
#include <zlib.h>

#include "ConnectionScanner.h"
#include "HttpContinueWriter.h"
#include "../http/HttpBodyFramer.h"
#include "../AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

inline constexpr std::size_t kBodyReadChunkBytes = 8 * 1024;
inline constexpr std::size_t kChunkedEncodedBufferBytes = kMaxHttpHeaderBytes;

[[noreturn]] inline void throwRequestBodyTooLarge() {
    throw HttpError(413, "payload_too_large", "request body is too large");
}

class TransferCodingDecoder final {
public:
    TransferCodingDecoder(
        HttpTransferCodings codings,
        std::pmr::polymorphic_allocator<char> allocator,
        std::size_t maxBodyBytes)
        : codings_(codings),
          output_(allocator),
          resource_(allocator.resource()),
          maxBodyBytes_(maxBodyBytes) {
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
            output_.resize(kBodyReadChunkBytes);
        }
    }

    ~TransferCodingDecoder() {
        cleanup();
    }

    TransferCodingDecoder(const TransferCodingDecoder&) = delete;
    TransferCodingDecoder& operator=(const TransferCodingDecoder&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return codings_.count == 0;
    }

    [[nodiscard]] bool finished() const noexcept {
        if (codings_.count == 0) {
            return true;
        }
        return ended_;
    }

    // Feeds the next encoded chunk. The view must stay valid until produce()
    // has consumed it (i.e. until produce() returns an empty view).
    void setInput(std::string_view input) {
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

    // Inflates into a fixed-size window and returns the decoded view, valid
    // until the next produce()/setInput() call. An empty view means the
    // decoder needs more input (or the stream already ended), so decoded
    // output stays bounded per call regardless of compression ratio.
    [[nodiscard]] std::string_view produce() {
        if (!initialized_ || ended_) {
            return {};
        }
        const auto step = inflateStep(output_.data(), output_.size());
        applyStatus(step);
        return std::string_view(output_.data(), step.produced);
    }

    void decodeAppend(std::string_view input, std::pmr::string& target) {
        if (codings_.count == 0) {
            if (maxBodyBytes_ != 0 && (input.size() > maxBodyBytes_ || target.size() > maxBodyBytes_ - input.size())) {
                throwRequestBodyTooLarge();
            }
            target.append(input.data(), input.size());
            decodedBytes_ += input.size();
            return;
        }
        setInput(input);
        // Inflate straight into the target tail; resize_and_overwrite skips
        // the zero-fill a plain resize() would pay for each window.
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

    void finish() {
        if (codings_.count == 0) {
            return;
        }
        if (!ended_) {
            throw std::invalid_argument("incomplete transfer-coding body");
        }
    }

private:
    struct InflateStep {
        std::size_t produced{0};
        int status{Z_OK};
    };

    [[nodiscard]] InflateStep inflateStep(char* out, std::size_t capacity) noexcept {
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

    void applyStatus(const InflateStep& step) {
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

    struct alignas(std::max_align_t) ZlibAllocationHeader {
        std::pmr::memory_resource* resource;
        std::size_t bytes;
    };

    static voidpf zallocThunk(voidpf opaque, uInt items, uInt size) noexcept {
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

    static void zfreeThunk(voidpf, voidpf address) noexcept {
        if (address == nullptr) {
            return;
        }
        auto* raw = static_cast<std::byte*>(address) - sizeof(ZlibAllocationHeader);
        auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
        header->resource->deallocate(raw, header->bytes, alignof(ZlibAllocationHeader));
    }

    void cleanup() noexcept {
        if (initialized_) {
            (void)inflateEnd(&stream_);
            initialized_ = false;
        }
    }

    void checkProducedLimit(std::size_t produced) const {
        if (produced == 0) {
            return;
        }
        if (maxBodyBytes_ != 0 && (produced > maxBodyBytes_ || decodedBytes_ > maxBodyBytes_ - produced)) {
            throwRequestBodyTooLarge();
        }
        if (decodedBytes_ > (std::numeric_limits<std::size_t>::max)() - produced) {
            throwRequestBodyTooLarge();
        }
    }

    HttpTransferCodings codings_;
    z_stream stream_{};
    bool initialized_{false};
    bool ended_{false};
    std::pmr::string output_;
    std::string_view pendingInput_;
    std::size_t pendingOffset_{0};
    std::pmr::memory_resource* resource_{nullptr};
    std::size_t maxBodyBytes_{0};
    std::size_t decodedBytes_{0};
};

template <typename Stream>
class StreamBodyReader final {
public:
    StreamBodyReader(
        Stream& stream,
        std::pmr::polymorphic_allocator<char> allocator,
        std::string_view initialBodyAndPipeline,
        std::size_t contentLength,
        bool chunked,
        HttpTransferCodings transferCodings,
        std::size_t maxBodyBytes,
        ConnectionScanner::Entry& scannerEntry,
        bool sendContinue = false)
        : stream_(stream),
          buffer_(allocator),
          transferDecoderAllocator_(allocator.resource()),
          initialBodyAndPipeline_(initialBodyAndPipeline),
          contentLength_(contentLength),
          chunked_(chunked),
          maxBodyBytes_(maxBodyBytes),
          chunkDecoder_(maxBodyBytes),
          scannerEntry_(scannerEntry),
          sendContinue_(sendContinue) {
        if (!transferCodings.empty()) {
            transferDecoder_ = transferDecoderAllocator_.allocate(1);
            try {
                std::construct_at(transferDecoder_, transferCodings, allocator, maxBodyBytes);
            } catch (...) {
                transferDecoderAllocator_.deallocate(transferDecoder_, 1);
                transferDecoder_ = nullptr;
                throw;
            }
        }
    }

    ~StreamBodyReader() {
        if (transferDecoder_ != nullptr) {
            std::destroy_at(transferDecoder_);
            transferDecoderAllocator_.deallocate(transferDecoder_, 1);
        }
    }

    StreamBodyReader(const StreamBodyReader&) = delete;
    StreamBodyReader& operator=(const StreamBodyReader&) = delete;

    [[nodiscard]] bool finished() const noexcept {
        return finished_ && (transferDecoder_ == nullptr || transferDecoder_->finished());
    }

    void restorePipeline(std::pmr::string& readBuffer, std::size_t& usedBytes) {
        if (!chunked_) {
            compactPending();
            const auto initialBodyBytes = std::min(contentLength_, initialBodyAndPipeline_.size());
            const auto initialPipeline = initialBodyAndPipeline_.substr(initialBodyBytes);
            const auto bufferedBytes = readCursor_ < buffer_.size() ? buffer_.size() - readCursor_ : 0;
            usedBytes = initialPipeline.size() + bufferedBytes;
            if (usedBytes > 0) {
                if (usedBytes > readBuffer.capacity()) {
                    std::pmr::string pipeline(readBuffer.get_allocator());
                    pipeline.reserve(usedBytes);
                    pipeline.append(initialPipeline);
                    if (bufferedBytes > 0) {
                        pipeline.append(buffer_.data() + readCursor_, bufferedBytes);
                    }
                    readBuffer = std::move(pipeline);
                } else {
                    // Move the aliasing view first: initialPipeline points into
                    // readBuffer, and a shrinking resize writes its terminator
                    // inside the source range before the bytes are copied.
                    if (!initialPipeline.empty()) {
                        std::memmove(readBuffer.data(), initialPipeline.data(), initialPipeline.size());
                    }
                    readBuffer.resize(usedBytes);
                    if (bufferedBytes > 0) {
                        std::memcpy(readBuffer.data() + initialPipeline.size(), buffer_.data() + readCursor_, bufferedBytes);
                    }
                }
            }
            buffer_.clear();
            initialBodyAndPipeline_ = {};
            readCursor_ = 0;
            pendingCompactUntil_ = 0;
            trailerSearchOffset_ = 0;
            readingTrailers_ = false;
            return;
        }

        compactPending();
        const auto initialPipeline = initialBodyAndPipeline_.empty()
            ? std::string_view{}
            : initialBodyAndPipeline_.substr(std::min(readCursor_, initialBodyAndPipeline_.size()));
        const auto bufferedBytes = readCursor_ < buffer_.size() ? buffer_.size() - readCursor_ : 0;
        usedBytes = initialPipeline.size() + bufferedBytes;
        if (usedBytes > 0) {
            if (usedBytes > readBuffer.capacity()) {
                std::pmr::string pipeline(readBuffer.get_allocator());
                pipeline.reserve(usedBytes);
                pipeline.append(initialPipeline);
                if (bufferedBytes > 0) {
                    pipeline.append(buffer_.data() + readCursor_, bufferedBytes);
                }
                readBuffer = std::move(pipeline);
            } else {
                // Move the aliasing view first: initialPipeline points into
                // readBuffer, and a shrinking resize writes its terminator
                // inside the source range before the bytes are copied.
                if (!initialPipeline.empty()) {
                    std::memmove(readBuffer.data(), initialPipeline.data(), initialPipeline.size());
                }
                readBuffer.resize(usedBytes);
                if (bufferedBytes > 0) {
                    std::memcpy(readBuffer.data() + initialPipeline.size(), buffer_.data() + readCursor_, bufferedBytes);
                }
            }
        }
        buffer_.clear();
        initialBodyAndPipeline_ = {};
        readCursor_ = 0;
        pendingCompactUntil_ = 0;
        trailerSearchOffset_ = 0;
        readingTrailers_ = false;
        chunkDecoder_.resetDelimiter();
    }

    [[nodiscard]] static Task<std::optional<std::string_view>> readThunk(void* target) {
        return static_cast<StreamBodyReader*>(target)->read();
    }

    [[nodiscard]] Task<std::optional<std::string_view>> read() {
        if (chunked_) {
            co_await ensureContinue();
            co_return co_await readTransferDecodedChunked();
        }
        if (exceedsLimit(contentLength_)) {
            throwRequestBodyTooLarge();
        }

        co_await ensureContinue();
        co_return co_await readContentLength();
    }

    Task<std::string_view> readContentLengthAll(std::pmr::string& body) {
        if (chunked_) {
            throw std::logic_error("chunked request body cannot use Content-Length reader");
        }
        if (transferDecoder_ != nullptr) {
            throw std::logic_error("transfer-coded request body cannot use Content-Length reader");
        }

        compactPending();
        if (finished_) {
            co_return std::string_view(body.data(), body.size());
        }
        if (exceedsLimit(contentLength_)) {
            throwRequestBodyTooLarge();
        }

        const auto initialBodyBytes = std::min(contentLength_, initialBodyAndPipeline_.size());
        if (initialBodyBytes == contentLength_) {
            markFinished();
            co_return initialBodyAndPipeline_.substr(0, contentLength_);
        }

        co_await ensureContinue();

        body.resize(contentLength_);
        if (initialBodyBytes > 0) {
            std::memcpy(body.data(), initialBodyAndPipeline_.data(), initialBodyBytes);
        }

        std::size_t offset = initialBodyBytes;
        while (offset < contentLength_) {
            scannerEntry_.setPhase(ConnectionScanner::Phase::kReadingBody);
            const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
                [this, &body, offset](auto handler) mutable {
                    stream_.async_read_some(
                        asio::buffer(body.data() + offset, body.size() - offset),
                        std::move(handler));
                });
            if (ec || bytesRead == 0) {
                throw std::invalid_argument("incomplete request body");
            }
            offset += bytesRead;
            scannerEntry_.touch();
        }

        deliveredBytes_ = contentLength_;
        markFinished();
        co_return std::string_view(body.data(), body.size());
    }

    Task<std::string_view> readAll(std::pmr::string& body) {
        if (!chunked_) {
            co_return co_await readContentLengthAll(body);
        }

        co_await ensureContinue();
        while (auto chunk = co_await readChunked()) {
            if (transferDecoder_ != nullptr) {
                transferDecoder_->decodeAppend(*chunk, body);
            } else {
                if (maxBodyBytes_ != 0 && (chunk->size() > maxBodyBytes_ || body.size() > maxBodyBytes_ - chunk->size())) {
                    throwRequestBodyTooLarge();
                }
                body.append(chunk->data(), chunk->size());
            }
        }
        if (transferDecoder_ != nullptr) {
            transferDecoder_->finish();
        }
        co_return std::string_view(body.data(), body.size());
    }

private:
    Task<void> ensureContinue() {
        if (sendContinue_ && !continueSent_) {
            if (!(co_await writeContinue(stream_))) {
                throw std::invalid_argument("failed to write 100 Continue");
            }
            continueSent_ = true;
        }
    }

    void compactPending() {
        if (pendingCompactUntil_ == 0) {
            return;
        }
        if (!initialBodyAndPipeline_.empty()) {
            readCursor_ = std::min(pendingCompactUntil_, initialBodyAndPipeline_.size());
            pendingCompactUntil_ = 0;
            if (readCursor_ == initialBodyAndPipeline_.size()) {
                initialBodyAndPipeline_ = {};
                readCursor_ = 0;
            }
            return;
        }
        if (pendingCompactUntil_ > buffer_.size()) {
            pendingCompactUntil_ = 0;
            readCursor_ = 0;
            return;
        }

        const auto removed = pendingCompactUntil_;
        const auto remaining = buffer_.size() - pendingCompactUntil_;
        if (remaining > 0) {
            std::memmove(
                buffer_.data(),
                buffer_.data() + pendingCompactUntil_,
                remaining);
        }
        buffer_.resize(buffer_.size() - removed);
        pendingCompactUntil_ = 0;
        readCursor_ = 0;
    }

    void materializeInitialRemainder() {
        if (initialBodyAndPipeline_.empty()) {
            return;
        }
        const auto oldReadCursor = readCursor_;
        buffer_.assign(initialBodyAndPipeline_.data() + readCursor_, initialBodyAndPipeline_.size() - readCursor_);
        initialBodyAndPipeline_ = {};
        readCursor_ = 0;
        pendingCompactUntil_ = 0;
        if (readingTrailers_) {
            trailerSearchOffset_ = trailerSearchOffset_ > oldReadCursor
                ? trailerSearchOffset_ - oldReadCursor
                : 0;
        }
    }

    Task<void> readMore() {
        compactPending();
        const auto oldSize = buffer_.size();
        const auto hardLimit = chunked_
            ? kChunkedEncodedBufferBytes
            : (maxBodyBytes_ == 0 ? (std::numeric_limits<std::size_t>::max)() : maxBodyBytes_);
        if (oldSize >= hardLimit) {
            throwRequestBodyTooLarge();
        }
        if (oldSize == buffer_.capacity()) {
            const auto nextCapacity = std::min<std::size_t>(
                std::max<std::size_t>(buffer_.capacity() * 2, oldSize + kBodyReadChunkBytes),
                hardLimit);
            buffer_.reserve(nextCapacity);
        }
        const auto writable = std::min<std::size_t>(
            kBodyReadChunkBytes,
            hardLimit - oldSize);
        buffer_.resize(oldSize + writable);

        scannerEntry_.setPhase(ConnectionScanner::Phase::kReadingBody);
        const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [this, oldSize, writable](auto handler) mutable {
                stream_.async_read_some(
                    asio::buffer(buffer_.data() + oldSize, writable),
                    std::move(handler));
            });
        if (ec || bytesRead == 0) {
            throw std::invalid_argument("incomplete request body");
        }

        buffer_.resize(oldSize + bytesRead);
        scannerEntry_.touch();
    }

    Task<std::optional<std::string_view>> readContentLength() {
        compactPending();
        if (finished_) {
            co_return std::nullopt;
        }
        if (exceedsLimit(contentLength_)) {
            throwRequestBodyTooLarge();
        }
        if (contentLength_ == 0 || deliveredBytes_ == contentLength_) {
            markFinished();
            co_return std::nullopt;
        }

        const auto initialBodyBytes = std::min(contentLength_, initialBodyAndPipeline_.size());
        if (deliveredBytes_ < initialBodyBytes) {
            const auto remainingBody = contentLength_ - deliveredBytes_;
            const auto available = initialBodyBytes - deliveredBytes_;
            const auto chunkBytes = std::min(available, remainingBody);
            auto chunk = initialBodyAndPipeline_.substr(deliveredBytes_, chunkBytes);
            deliveredBytes_ += chunkBytes;
            if (deliveredBytes_ == contentLength_) {
                markFinished();
            }
            co_return chunk;
        }

        while (buffer_.size() <= readCursor_) {
            co_await readMore();
        }

        const auto remainingBody = contentLength_ - deliveredBytes_;
        const auto available = buffer_.size() - readCursor_;
        const auto chunkBytes = std::min(available, remainingBody);
        auto chunk = std::string_view(buffer_.data() + readCursor_, chunkBytes);
        pendingCompactUntil_ = readCursor_ + chunkBytes;
        deliveredBytes_ += chunkBytes;
        if (deliveredBytes_ == contentLength_) {
            markFinished();
        }

        co_return chunk;
    }

    Task<std::optional<std::string_view>> readChunked() {
        compactPending();
        if (finished_) {
            co_return std::nullopt;
        }

        for (;;) {
            const auto source = !initialBodyAndPipeline_.empty()
                ? std::string_view(initialBodyAndPipeline_.data(), initialBodyAndPipeline_.size())
                : std::string_view(buffer_.data(), buffer_.size());

            if (readingTrailers_) {
                if (source.size() >= readCursor_ + 2 && source.substr(readCursor_, 2) == "\r\n") {
                    pendingCompactUntil_ = readCursor_ + 2;
                    chunkDecoder_.consumeTrailers(2);
                    readingTrailers_ = false;
                    trailerSearchOffset_ = 0;
                    markFinished();
                    compactPending();
                    co_return std::nullopt;
                }

                const auto searchOffset = std::max(readCursor_, trailerSearchOffset_);
                const auto trailerEnd = source.find("\r\n\r\n", searchOffset);
                if (trailerEnd == std::string_view::npos) {
                    trailerSearchOffset_ = source.size() > 3
                        ? std::max(readCursor_, source.size() - 3)
                        : readCursor_;
                    materializeInitialRemainder();
                    co_await readMore();
                    continue;
                }

                if (validateHttpChunkTrailers(source.substr(readCursor_, trailerEnd - readCursor_)) !=
                    HttpChunkScanStatus::kComplete) {
                    throw std::invalid_argument("invalid chunked request body");
                }

                pendingCompactUntil_ = trailerEnd + 4;
                chunkDecoder_.consumeTrailers(trailerEnd - readCursor_ + 4);
                readingTrailers_ = false;
                trailerSearchOffset_ = 0;
                markFinished();
                compactPending();
                co_return std::nullopt;
            }

            if (chunkDecoder_.awaitingDelimiter()) {
                const auto available = source.substr(readCursor_);
                switch (chunkDecoder_.checkDelimiter(available)) {
                    case ChunkDelimiterStatus::kNeedMore:
                        materializeInitialRemainder();
                        co_await readMore();
                        continue;
                    case ChunkDelimiterStatus::kInvalid:
                        throw std::invalid_argument("invalid chunked request body");
                    case ChunkDelimiterStatus::kOk:
                        break;
                }

                pendingCompactUntil_ = readCursor_ + 2;
                chunkDecoder_.consumeDelimiter();
                compactPending();
                continue;
            }

            if (chunkDecoder_.remaining() > 0) {
                if (readCursor_ >= source.size()) {
                    materializeInitialRemainder();
                    co_await readMore();
                    continue;
                }

                const auto availableBodyBytes = source.size() - readCursor_;
                const auto remaining = chunkDecoder_.remaining();
                if (remaining <= availableBodyBytes) {
                    if (availableBodyBytes - remaining < 2) {
                        auto chunk = std::string_view(source.data() + readCursor_, remaining);
                        pendingCompactUntil_ = readCursor_ + remaining;
                        chunkDecoder_.consumeBodyBytes(remaining);
                        co_return chunk;
                    }
                    if (source.substr(readCursor_ + remaining, 2) != "\r\n") {
                        throw std::invalid_argument("invalid chunked request body");
                    }

                    auto chunk = std::string_view(source.data() + readCursor_, remaining);
                    pendingCompactUntil_ = readCursor_ + remaining + 2;
                    chunkDecoder_.consumeBodyBytes(remaining);
                    chunkDecoder_.consumeDelimiter();
                    co_return chunk;
                }

                auto chunk = std::string_view(source.data() + readCursor_, availableBodyBytes);
                chunkDecoder_.consumeBodyBytes(availableBodyBytes);
                pendingCompactUntil_ = source.size();
                co_return chunk;
            }

            const auto available = source.substr(readCursor_);
            const auto lineEnd = available.find("\r\n");
            if (lineEnd == std::string_view::npos) {
                materializeInitialRemainder();
                co_await readMore();
                continue;
            }

            std::size_t chunkSize = 0;
            const auto hasChunkBody = chunkDecoder_.parseSizeLine(available.substr(0, lineEnd), chunkSize);

            const auto chunkDataStart = readCursor_ + lineEnd + 2;
            if (!hasChunkBody) {
                readCursor_ = chunkDataStart;
                readingTrailers_ = true;
                trailerSearchOffset_ = readCursor_;
                continue;
            }

            if (chunkDataStart > source.size()) {
                throwRequestBodyTooLarge();
            }
            readCursor_ = chunkDataStart;
        }
    }

    Task<std::optional<std::string_view>> readTransferDecodedChunked() {
        if (transferDecoder_ == nullptr) {
            co_return co_await readChunked();
        }

        // Drain pending decoder output before pulling more encoded bytes:
        // readChunked() compacts the buffer the decoder input view points at,
        // and a single encoded chunk may inflate into many output windows.
        for (;;) {
            const auto decoded = transferDecoder_->produce();
            if (!decoded.empty()) {
                co_return decoded;
            }
            auto chunk = co_await readChunked();
            if (!chunk) {
                transferDecoder_->finish();
                co_return std::nullopt;
            }
            transferDecoder_->setInput(*chunk);
        }
    }

    Stream& stream_;
    std::pmr::string buffer_;
    std::pmr::polymorphic_allocator<TransferCodingDecoder> transferDecoderAllocator_;
    TransferCodingDecoder* transferDecoder_{nullptr};
    std::string_view initialBodyAndPipeline_;
    std::size_t contentLength_;
    bool chunked_;
    std::size_t maxBodyBytes_;
    HttpChunkDecoder chunkDecoder_;
    ConnectionScanner::Entry& scannerEntry_;
    std::size_t readCursor_{0};
    std::size_t pendingCompactUntil_{0};
    std::size_t trailerSearchOffset_{0};
    std::size_t deliveredBytes_{0};
    bool finished_{false};
    bool readingTrailers_{false};
    bool sendContinue_{false};
    bool continueSent_{false};

    [[nodiscard]] bool exceedsLimit(std::size_t bytes) const noexcept {
        return maxBodyBytes_ != 0 && bytes > maxBodyBytes_;
    }

    // Marks the body fully consumed and resets the connection phase out of
    // kReadingBody. Without this, bodyTimeout would keep counting against
    // post-read dispatch time (DB queries, business logic) since
    // phaseStartedMs only resets on phase change.
    void markFinished() noexcept {
        finished_ = true;
        scannerEntry_.setPhase(ConnectionScanner::Phase::kIdle);
    }
};

template <typename Stream>
class LazyBufferedBody final {
public:
    LazyBufferedBody(
        Stream& stream,
        std::pmr::polymorphic_allocator<char> workerAllocator,
        std::pmr::memory_resource* requestResource,
        std::string_view initialBodyAndPipeline,
        std::size_t contentLength,
        bool chunked,
        HttpTransferCodings transferCodings,
        std::size_t maxBodyBytes,
        ConnectionScanner::Entry& scannerEntry,
        bool sendContinue)
        : reader_(
              stream,
              workerAllocator,
              initialBodyAndPipeline,
              contentLength,
              chunked,
              transferCodings,
              maxBodyBytes,
              scannerEntry,
              sendContinue),
          body_(requestResource == nullptr ? ProcessMemory::instance().upstreamResource() : requestResource),
          hasBody_(contentLength > 0 || chunked || !transferCodings.empty()) {}

    [[nodiscard]] bool consumed() const noexcept {
        return !hasBody_ || reader_.finished();
    }

    void restorePipeline(std::pmr::string& readBuffer, std::size_t& usedBytes) {
        reader_.restorePipeline(readBuffer, usedBytes);
    }

    [[nodiscard]] static Task<std::string_view> readAllThunk(void* target) {
        return static_cast<LazyBufferedBody*>(target)->readAll();
    }

    static Task<void> discardThunk(void* target) {
        return static_cast<LazyBufferedBody*>(target)->discard();
    }

    [[nodiscard]] Task<std::string_view> readAll() {
        if (read_) {
            co_return bodyView_;
        }
        bodyView_ = co_await reader_.readAll(body_);
        read_ = true;
        co_return bodyView_;
    }

    Task<void> discard() {
        if (read_) {
            co_return;
        }
        while (co_await reader_.read()) {}
    }

private:
    StreamBodyReader<Stream> reader_;
    std::pmr::string body_;
    std::string_view bodyView_;
    bool hasBody_{false};
    bool read_{false};
};

}  // namespace ruvia::detail
