#include "ruvia/web/detail/server/HttpServer.h"

#include "ruvia/web/detail/server/HttpResponseWriter.h"
#include "ruvia/web/detail/server/HttpServerSessionUtils.h"
#include "ruvia/core/detail/AsioAwait.h"

#include "ruvia/web/Error.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/web/detail/http/HttpErrorResponse.h"

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/steady_timer.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

namespace ruvia::detail {

Task<void> HttpServer::acceptLoop() {
    asio::steady_timer retryTimer(ioContext_);
    for (;;) {
        auto [ec, socket] = co_await asyncResult<asio::ip::tcp::socket>([this](auto handler) mutable {
            acceptor_.async_accept(std::move(handler));
        });

        if (ec) {
            // Fatal: acceptor was cancelled (stop()) or closed. Exit cleanly.
            if (ec == asio::error::operation_aborted ||
                ec == asio::error::bad_descriptor ||
                ec == asio::error::invalid_argument) {
                co_return;
            }
            // Transient: fd exhaustion, ECONNABORTED, EINTR, ENOBUFS, ENOMEM,
            // etc. A single bad accept must not stop the worker forever.
            retryTimer.expires_after(std::chrono::milliseconds(50));
            const auto waitEc = co_await asyncError([&retryTimer](auto handler) mutable {
                retryTimer.async_wait(std::move(handler));
            });
            if (waitEc || !started_.load(std::memory_order_relaxed)) {
                co_return;
            }
            continue;
        }

        if (!started_.load(std::memory_order_relaxed)) {
            closeSocket(socket);
            co_return;
        }
        configureAcceptedSocket(socket);

        if (options_.maxConnections > 0 && activeConnectionCount_ >= options_.maxConnections) {
            if (options_.tls.enabled) {
                closeSocket(socket);
                continue;
            }
            std::array<std::byte, kRequestArenaStackBytes> limitArenaBuffer;
            std::optional<RequestMemory> limitMemoryStorage;
            auto& limitMemory = emplaceRequestMemory(
                limitMemoryStorage,
                memory_,
                std::span<std::byte>(limitArenaBuffer.data(), limitArenaBuffer.size()));
            auto response = makeDefaultErrorResponse(
                limitMemory.resource(),
                HttpErrorInfo(429));
            http1MarkConnectionClose(response);
            std::error_code writeEc;
            const auto writePlan = httpBufferedResponseWritePlan(HttpMethod::kGet, response);
            co_await writeResponse(
                socket, memory_, nullptr, nullptr, response, writePlan, writeEc);
            closeSocket(socket);
            continue;
        }

        ++activeConnectionCount_;

        asio::co_spawn(
            ioContext_,
            taskAsAwaitable(handleSession(std::move(socket))),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    }
}

}  // namespace ruvia::detail
