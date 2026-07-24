#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/http1/Http1RequestBodyPlan.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/core/Task.h"

namespace ruvia::detail {

inline constexpr std::size_t kChunkedEncodedBufferBytes = kMaxHttpHeaderBytes;

template <typename Stream>
class StreamBodyReader final {
public:
    StreamBodyReader(Stream& stream, std::pmr::polymorphic_allocator<char> allocator, std::string_view initialBodyAndPipeline, Http1RequestBodyPlan bodyPlan, ProtocolByteLimit bodyLimit, ConnectionScanner::Entry& scannerEntry);
    ~StreamBodyReader() = default;

    StreamBodyReader(const StreamBodyReader&) = delete;
    StreamBodyReader& operator=(const StreamBodyReader&) = delete;

    [[nodiscard]] Http1RequestBodyConsumption consumption() const noexcept;
    // Hands the pipelined suffix -- the bytes of the next request that arrived
    // in the same segment -- to `stash`, and drops this reader's claim on them.
    // The connection read buffer is deliberately untouched: every view in the
    // request being served still borrows it, so the session installs these bytes
    // itself once those views are dead.
    void takePipeline(std::pmr::string& stash);

    [[nodiscard]] Task<std::optional<std::string_view>> read();
    Task<std::string_view> readAll(std::pmr::string& body);

private:
    Task<void> ensureContinue();
    void compactPending();
    [[nodiscard]] std::string_view initialPipelineRemainder() const noexcept;
    [[nodiscard]] std::string_view bufferedPipelineRemainder() const noexcept;
    void resetPipelineState() noexcept;
    void materializeInitialRemainder();
    Task<void> readMore();
    Task<std::string_view> readKnownLengthAll(std::pmr::string& body, std::size_t contentLength);
    Task<std::optional<std::string_view>> readKnownLength(std::size_t contentLength);
    Task<std::optional<std::string_view>> readChunked();
    Task<std::optional<std::string_view>> readTransferDecodedChunked();
    void decodeTransferAppend(std::string_view input, std::pmr::string& target);
    [[nodiscard]] bool exceedsLimit(std::size_t bytes) const noexcept;
    void markFinished() noexcept;

    Stream& stream_;
    std::pmr::string buffer_;
    std::pmr::string transferOutput_;
    std::unique_ptr<TransferCodingDecoder, PmrObjectDeleter<TransferCodingDecoder>> transferDecoder_;
    std::string_view transferInput_;
    std::string_view initialBodyAndPipeline_;
    Http1RequestBodyPlan bodyPlan_;
    ProtocolByteLimit bodyLimit_;
    Http1ChunkedBodyDecoder chunkDecoder_;
    ConnectionScanner::Entry& scannerEntry_;
    std::size_t readCursor_{0};
    std::size_t pendingCompactUntil_{0};
    std::size_t deliveredBytes_{0};
    bool finished_{false};
    bool continueSent_{false};
};

}  // namespace ruvia::detail

#include "ruvia/web/detail/body/HttpStreamBodyReader.inl"
