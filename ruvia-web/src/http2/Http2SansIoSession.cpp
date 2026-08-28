#include "ruvia/web/detail/http2/Http2SansIoSession.h"

#include <array>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/write.hpp>

#include "http2/Http2SansIoSessionEngine.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/web/detail/server/response/HttpResponseWriter.h"

namespace ruvia::detail {
namespace {

template <typename Stream>
Task<void> runHttp2SansIoWriter(Stream& stream, Http2SansIoSessionEngine& engine) {
    std::pmr::string writeScratch(engine.workerResource());
    for (;;) {
        while (engine.wantsWrite()) {
            engine.takeOutput(writeScratch);
            if (engine.writeFailed()) {
                continue;
            }

            std::error_code writeError;
            std::size_t writtenBytes = 0;
            bool writeDone = false;
            if constexpr (std::is_same_v<std::remove_cvref_t<Stream>, asio::ip::tcp::socket>) {
                writeDone = tryPlainTcpSyncWrite(stream, asio::buffer(writeScratch.data(), writeScratch.size()), writeScratch.size(), writeError, writtenBytes);
            }
            if (!writeDone) {
                const auto writeCompletion = co_await asyncAsio([&stream, &writeScratch, writtenBytes](auto handler) mutable { asio::async_write(stream, asio::buffer(writeScratch.data() + writtenBytes, writeScratch.size() - writtenBytes), std::move(handler)); });
                writeError = writeCompletion.errorCode();
            }
            if (writeError) {
                engine.writerWriteFailed(writeError);
                continue;
            }
            engine.touchActivity();
            clearPmrStringRetainingSmall(writeScratch, 64 * 1024);
        }
        if (engine.writerShouldExit()) {
            co_return;
        }
        co_await engine.waitForWrite();
    }
}

template <typename Stream>
Task<void> runHttp2SansIoSessionImpl(Stream& stream, asio::ip::tcp::socket& socket, const RouteTable& routes, WorkerMemory& worker, Http2SansIoSessionContext session, std::string_view initialBytes) {
    auto executor = asio::any_io_executor(stream.get_executor());
    Http2SansIoSessionEngine engine(executor, socket, routes, worker, std::move(session));

    engine.beginConnection();
    try {
        asio::co_spawn(executor, taskAsAwaitable(runHttp2SansIoWriter(stream, engine)), [&engine](std::exception_ptr exception) noexcept { engine.writerCompleted(exception); });
    } catch (...) {
        engine.terminate(std::make_error_code(std::errc::operation_canceled));
        throw;
    }

    bool initialInputRetained = false;
    std::error_code readerTerminalError;
    std::exception_ptr readerFailure;
    try {
        engine.drainEvents();
        if (!engine.connectionFailed() && !initialBytes.empty()) {
            const auto result = engine.feedAndDrain(initialBytes);
            initialInputRetained = result == Http2FeedResult::kConnectionNotStarted;
        }
        engine.wakeWriter();

        if (!engine.connectionFailed() && !initialInputRetained && !engine.terminated()) {
            std::array<char, 4096> readBuffer;
            for (;;) {
                engine.setInactivityPhase();
                auto readCompletion = co_await asyncAsio<std::size_t>([&stream, &readBuffer](auto handler) mutable { stream.async_read_some(asio::buffer(readBuffer.data(), readBuffer.size()), std::move(handler)); });
                const auto error = readCompletion.errorCode();
                const auto bytesRead = readCompletion.result();
                const bool workerStopped = !engine.workerRunning();
                if (error || bytesRead == 0 || workerStopped) {
                    readerTerminalError = error ? error : std::make_error_code(workerStopped ? std::errc::operation_canceled : std::errc::connection_reset);
                    break;
                }
                engine.touchActivity();
                const auto result = engine.feedAndDrain(std::string_view(readBuffer.data(), bytesRead));
                engine.wakeWriter();
                if (result == Http2FeedResult::kConnectionNotStarted || result == Http2FeedResult::kProtocolFailure || engine.writeFailed()) {
                    if (result == Http2FeedResult::kConnectionNotStarted || result == Http2FeedResult::kProtocolFailure) {
                        readerTerminalError = std::make_error_code(std::errc::protocol_error);
                    }
                    break;
                }
            }
        }
    } catch (...) {
        readerFailure = std::current_exception();
        readerTerminalError = std::make_error_code(std::errc::operation_canceled);
    }

    if (!engine.terminated()) {
        if (!readerTerminalError) {
            readerTerminalError = engine.connectionFailed() || initialInputRetained ? std::make_error_code(std::errc::protocol_error) : std::make_error_code(std::errc::connection_aborted);
        }
        engine.terminate(readerTerminalError);
    }

    std::exception_ptr finishFailure;
    try {
        co_await engine.finish();
    } catch (...) {
        finishFailure = std::current_exception();
    }
    if (readerFailure != nullptr) {
        std::rethrow_exception(readerFailure);
    }
    if (finishFailure != nullptr) {
        std::rethrow_exception(finishFailure);
    }
}

}  // namespace

Task<void> runHttp2SansIoSession(asio::ip::tcp::socket& stream, const RouteTable& routes, WorkerMemory& worker, Http2SansIoSessionContext session, std::string_view initialBytes) {
    return runHttp2SansIoSessionImpl(stream, stream, routes, worker, std::move(session), initialBytes);
}

Task<void> runHttp2SansIoSession(asio::ssl::stream<asio::ip::tcp::socket&>& stream, const RouteTable& routes, WorkerMemory& worker, Http2SansIoSessionContext session, std::string_view initialBytes) {
    return runHttp2SansIoSessionImpl(stream, stream.next_layer(), routes, worker, std::move(session), initialBytes);
}

}  // namespace ruvia::detail
