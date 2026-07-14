#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "ruvia/core/detail/WorkerSignal.h"

#include "ruvia/core/Task.h"
#include "ruvia/http/detail/http2/Http2Connection.h"

namespace ruvia {
class HttpResponse;
class WorkerMemory;
}

namespace ruvia::detail {
class Http2SansIoStreamRuntimeTable;
class HttpBufferedResponseWritePlan;

// The writer completes all transport recovery (including RESET_STREAM) before
// returning. Its owner only needs the status proven by a committed final HEADERS
// transaction; no status exists when the stream disappeared or submission failed
// before that commit.
class Http2BufferedResponseWriteResult final {
public:
    [[nodiscard]] static constexpr Http2BufferedResponseWriteResult
    committed(std::uint16_t status) noexcept {
        return Http2BufferedResponseWriteResult(status);
    }

    [[nodiscard]] static constexpr Http2BufferedResponseWriteResult
    uncommitted() noexcept {
        return Http2BufferedResponseWriteResult(std::nullopt);
    }

    [[nodiscard]] constexpr std::optional<std::uint16_t>
    committedStatus() const noexcept {
        return committedStatus_;
    }

private:
    explicit constexpr Http2BufferedResponseWriteResult(
        std::optional<std::uint16_t> committedStatus) noexcept
        : committedStatus_(committedStatus) {}

    std::optional<std::uint16_t> committedStatus_;
};

static_assert(std::is_trivially_copyable_v<Http2BufferedResponseWriteResult>);
static_assert(sizeof(Http2BufferedResponseWriteResult) <= 4);

// Non-transport HTTP/2 buffered/file response driver. The session template owns
// socket reads/writes and event dispatch; this object owns the single response
// commit, DATA backpressure, file I/O, and typed terminal result chain. Keeping it
// non-template compiles that policy once for plain and TLS sessions.
class Http2BufferedResponseWriter final {
public:
    Http2BufferedResponseWriter(
        Http2Connection& connection,
        Http2SansIoStreamRuntimeTable& streamRuntimes,
        WorkerMemory& worker,
        WorkerSignal& writeSignal) noexcept;

    [[nodiscard]] Task<Http2BufferedResponseWriteResult> write(
        std::uint32_t streamId,
        const HttpResponse& response,
        HttpBufferedResponseWritePlan writePlan);

private:
    enum class DataWriteResult : std::uint8_t {
        kCompleted,
        kPeerAborted,
        kFailed
    };

    [[nodiscard]] Task<bool> awaitSendWindow(std::uint32_t streamId);
    [[nodiscard]] Task<DataWriteResult> writeData(
        std::uint32_t streamId,
        std::string_view chunk,
        Http2EndStream endStream);
    void wakeWriter() noexcept;

    Http2Connection* connection_;
    Http2SansIoStreamRuntimeTable* streamRuntimes_;
    WorkerMemory* worker_;
    WorkerSignal* writeSignal_;
};

}  // namespace ruvia::detail
