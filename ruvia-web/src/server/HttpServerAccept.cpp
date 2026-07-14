#include "ruvia/web/detail/server/HttpServer.h"

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/SocketUtils.h"
#include "ruvia/core/Timer.h"

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>
#include <chrono>
#include <system_error>
#include <utility>

namespace ruvia::detail {

Task<void> HttpServer::acceptLoop() {
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
            co_await sleepFor(workerHandle_, std::chrono::milliseconds(50));
            if (!workerRunning_) {
                co_return;
            }
            continue;
        }

        if (!workerRunning_) {
            closeSocket(socket);
            co_return;
        }
        if (options_.maxConnections.has_value() &&
            activeConnectionCount_ >= *options_.maxConnections) {
            closeSocket(socket);
            continue;
        }

        configureAcceptedSocket(socket);
        ++activeConnectionCount_;

        asio::co_spawn(
            ioContext_,
            taskAsAwaitable(handleSession(std::move(socket))),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    }
}

}  // namespace ruvia::detail
