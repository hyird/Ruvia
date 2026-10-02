#pragma once
#include <optional>
#include <span>
#include <system_error>

#include "ruvia/core/Bytes.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
namespace ruvia::detail {
// Streaming request-body reader for the sans-I/O session; the BodyReader facade wraps
// it for handler consumption. Chunk-for-chunk port of the coroutine readBodyChunk.
// Admission always binds the runtime-owned signal before this facade can exist.
class Http2SansIoRequestBodyReader final {
public:
    Http2SansIoRequestBodyReader(ruvia::Http2Connection& connection, std::uint32_t streamId,
        Http2SansIoBodyQueue& bodyQueue, Http2SansIoStreamSignal& signal) noexcept
        : connection_(connection),
          streamId_(streamId),
          bodyQueue_(bodyQueue),
          signal_(signal) {}

    [[nodiscard]] Task<std::optional<std::span<const std::byte>>> read() {
        for (;;) {
            if (signal_.terminated()) {
                throw std::system_error(signal_.terminalError());
            }
            const auto receiveStatus = connection_.streamReceiveStatus(streamId_);
            if (receiveStatus == Http2StreamReceiveStatus::kClosed) {
                throw std::system_error(std::make_error_code(std::errc::connection_reset));
            }
            if (const auto chunk = bodyQueue_.pop(); !chunk.empty()) {
                co_return ::ruvia::asBytes(chunk);
            }
            if (!bodyQueue_.empty()) {
                continue;
            }
            if (receiveStatus == Http2StreamReceiveStatus::kEnded) {
                co_return std::nullopt;
            }
            if (signal_.terminated()) {
                throw std::system_error(signal_.terminalError());
            }
            co_await signal_.wait();
        }
    }

private:
    ruvia::Http2Connection& connection_;
    std::uint32_t streamId_;
    Http2SansIoBodyQueue& bodyQueue_;
    Http2SansIoStreamSignal& signal_;
};

}  // namespace ruvia::detail
