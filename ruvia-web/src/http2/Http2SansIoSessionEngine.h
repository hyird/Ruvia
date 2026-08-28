#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string_view>
#include <system_error>

#include <asio/any_io_executor.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/web/detail/http2/Http2BufferedResponseWrite.h"
#include "ruvia/web/detail/http2/Http2SansIoSessionContext.h"
#include "ruvia/web/detail/http2/Http2SansIoSessionLifecycle.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/http2/Http2SansIoTermination.h"

namespace ruvia {
class WorkerMemory;
}

namespace ruvia::detail {

class RouteTable;

// Transport-independent application driver for one HTTP/2 connection. The
// transport wrapper owns only socket/TLS reads and writes; protocol event
// dispatch, route execution, stream lifetime, and joins compile once.
class Http2SansIoSessionEngine final {
public:
    Http2SansIoSessionEngine(asio::any_io_executor executor, asio::ip::tcp::socket& socket, const RouteTable& routes, WorkerMemory& worker, Http2SansIoSessionContext session);

    Http2SansIoSessionEngine(const Http2SansIoSessionEngine&) = delete;
    Http2SansIoSessionEngine& operator=(const Http2SansIoSessionEngine&) = delete;
    Http2SansIoSessionEngine(Http2SansIoSessionEngine&&) = delete;
    Http2SansIoSessionEngine& operator=(Http2SansIoSessionEngine&&) = delete;

    void beginConnection();
    void drainEvents();
    [[nodiscard]] Http2FeedResult feedAndDrain(std::string_view bytes);

    [[nodiscard]] bool wantsWrite() const noexcept;
    void takeOutput(std::pmr::string& output);
    [[nodiscard]] bool writeFailed() const noexcept;
    [[nodiscard]] bool writerShouldExit() const noexcept;
    [[nodiscard]] Task<void> waitForWrite();
    void writerWriteFailed(std::error_code error) noexcept;
    void writerCompleted(std::exception_ptr exception) noexcept;

    [[nodiscard]] bool connectionFailed() const noexcept;
    [[nodiscard]] bool terminated() const noexcept;
    [[nodiscard]] bool headerBlockInProgress() const noexcept;
    [[nodiscard]] std::size_t activeRuntimeCount() const noexcept;
    [[nodiscard]] bool workerRunning() const noexcept;
    void setInactivityPhase() noexcept;
    void touchActivity() noexcept;
    void wakeWriter() noexcept;
    void terminate(std::error_code error) noexcept;
    [[nodiscard]] Task<void> finish();

    [[nodiscard]] std::pmr::memory_resource* workerResource() const noexcept;

private:
    [[nodiscard]] Task<void> dispatchOneInner(std::uint32_t streamId);
    [[nodiscard]] Task<void> dispatchOne(std::uint32_t streamId);
    [[nodiscard]] bool admitStream(std::uint32_t streamId);
    void resetStreamNoThrow(std::uint32_t streamId, Http2ErrorCode error) noexcept;
    void unpinStreamNoThrow(std::uint32_t streamId) noexcept;

    asio::any_io_executor executor_;
    asio::ip::tcp::socket& socket_;
    const RouteTable& routes_;
    WorkerMemory& worker_;
    Http2SansIoSessionContext session_;
    std::string_view remoteAddress_;
    Http2Connection connection_;
    WorkerSignal writeSignal_;
    WorkerSignal handlerFinished_;
    WorkerSignal writerFinished_;
    Http2SansIoSessionLifecycle lifecycle_;
    Http2SansIoTermination termination_;
    Http2SansIoStreamRuntimeTable streamRuntimes_;
    Http2BufferedResponseWriter bufferedResponseWriter_;
    std::size_t activeHandlerTasks_{0};
    std::size_t acceptedRequestHeads_{0};
    bool writerTaskDone_{false};
};

}  // namespace ruvia::detail
