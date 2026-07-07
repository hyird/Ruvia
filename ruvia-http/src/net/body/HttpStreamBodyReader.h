#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "../server/ConnectionScanner.h"
#include "HttpTransferCodingDecoder.h"
#include "HttpBodyFramer.h"
#include "ruvia/app/Task.h"

namespace ruvia::detail {

inline constexpr std::size_t kChunkedEncodedBufferBytes = kMaxHttpHeaderBytes;

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
        bool sendContinue = false);
    ~StreamBodyReader();

    StreamBodyReader(const StreamBodyReader&) = delete;
    StreamBodyReader& operator=(const StreamBodyReader&) = delete;

    [[nodiscard]] bool finished() const noexcept;
    void restorePipeline(std::pmr::string& readBuffer, std::size_t& usedBytes);

    [[nodiscard]] Task<std::optional<std::string_view>> read();
    Task<std::string_view> readContentLengthAll(std::pmr::string& body);
    Task<std::string_view> readAll(std::pmr::string& body);

private:
    Task<void> ensureContinue();
    void compactPending();
    [[nodiscard]] std::string_view initialPipelineRemainder() const noexcept;
    [[nodiscard]] std::string_view bufferedPipelineRemainder() const noexcept;
    static void restorePipelineBytes(
        std::pmr::string& readBuffer,
        std::size_t& usedBytes,
        std::string_view initialPipeline,
        std::string_view bufferedPipeline);
    void resetPipelineState() noexcept;
    void materializeInitialRemainder();
    Task<void> readMore();
    Task<std::optional<std::string_view>> readContentLength();
    Task<std::optional<std::string_view>> readChunked();
    Task<std::optional<std::string_view>> readTransferDecodedChunked();
    [[nodiscard]] bool exceedsLimit(std::size_t bytes) const noexcept;
    void markFinished() noexcept;

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
};

}  // namespace ruvia::detail

#include "HttpStreamBodyReader.inl"
