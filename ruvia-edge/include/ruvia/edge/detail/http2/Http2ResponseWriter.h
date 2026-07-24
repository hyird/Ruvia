#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/edge/detail/ResponseWriter.h"
#include "ruvia/http/HttpKnownMethod.h"

namespace ruvia::detail {
class Http2Connection;
}  // namespace ruvia::detail

namespace ruvia::edge {

// Per-connection coordination shared by the HTTP/2 reader loop, the single writer
// coroutine, and each stream's response handler. Everything runs on one io_context
// thread, so these references need no locking; the timers are pure wakeup channels.
struct Http2SessionShared final {
    ruvia::detail::Http2Connection& connection;
    // cancel() to wake the writer coroutine to flush the connection's output.
    asio::steady_timer& writeWake;
    // Streams whose handler is parked waiting for its flow-control window to
    // reopen, keyed by stream id -> its wakeup timer. The reader cancels the
    // matching timer when the window drains or the stream is aborted.
    std::pmr::unordered_map<std::uint32_t, asio::steady_timer*>& drainWaiters;
    // Set once the session is tearing down (client gone / write failed) so a
    // parked handler unwinds instead of waiting for a window that will never open.
    bool& shuttingDown;
};

// HTTP/2 response writer. A buffered response (cache hit or fixed-length body) is
// submitted as one HEADERS+DATA. A streamed response (an unknown-length origin
// body) is written incrementally: a streaming HEADERS, then DATA chunks, then a
// terminal END_STREAM -- parking on the stream's flow-control window between
// chunks so a slow client never forces unbounded buffering. This writer only
// submits frames and pokes the session's writer coroutine, which owns all I/O.
class Http2ResponseWriter final : public ResponseWriter {
public:
    Http2ResponseWriter(Http2SessionShared& shared, std::uint32_t streamId, HttpKnownMethod method, std::pmr::memory_resource* resource)
        : shared_(shared),
          streamId_(streamId),
          method_(method),
          resource_(resource),
          drainTimer_(shared.writeWake.get_executor()) {}

    asio::awaitable<bool> respond(std::uint16_t status, const Headers& headers, std::string_view body, std::string_view cacheResult, std::optional<std::uint64_t> age, bool omitBody, bool keepAlive) override;

    asio::awaitable<bool> respondHead(std::uint16_t status, const Headers& headers, std::string_view cacheResult, bool hasBody, std::optional<std::size_t> contentLength, bool keepAlive) override;

    asio::awaitable<bool> respondChunk(std::string_view chunk) override;

    asio::awaitable<bool> respondEnd() override;

    [[nodiscard]] std::size_t bytesWritten() const override {
        return bytes_;
    }

    [[nodiscard]] bool connectionReusable() const noexcept override {
        return true;
    }

    // Whether the response was fully submitted (so the driver need not reset a
    // dangling stream after the serve core returns).
    [[nodiscard]] bool ended() const noexcept {
        return ended_;
    }

private:
    void poke() noexcept;

    // Park until the reader signals this stream's flow-control window reopened, the
    // stream is aborted, or the session is shutting down. Returns false if the
    // stream can no longer be written.
    asio::awaitable<bool> waitForWindow();

    void submitBuffered(std::uint16_t status, const Headers& headers, std::string_view body, std::string_view cacheResult, std::optional<std::uint64_t> age, bool omitBody);

    Http2SessionShared& shared_;
    std::uint32_t streamId_;
    HttpKnownMethod method_;
    std::pmr::memory_resource* resource_;
    asio::steady_timer drainTimer_;
    std::size_t bytes_{0};
    bool bodyOpen_{false};
    bool ended_{false};
};

}  // namespace ruvia::edge
