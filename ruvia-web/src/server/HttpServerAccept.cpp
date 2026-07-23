#include "ruvia/web/detail/server/HttpServer.h"
#include "ruvia/web/detail/server/session/HttpServerSessionEntry.h"

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/SocketUtils.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/detail/server/session/HttpServerConnectionGuards.h"

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
        auto acceptCompletion =
            co_await asyncAsio<asio::ip::tcp::socket>(
                [this](auto handler) mutable {
                    acceptor_.async_accept(std::move(handler));
                });
        const auto ec = acceptCompletion.errorCode();
        auto socket = std::move(acceptCompletion).takeResult();

        if (ec) {
            // Fatal: acceptor was cancelled (stop()) or closed. Exit cleanly.
            if (ec == asio::error::operation_aborted ||
                ec == asio::error::bad_descriptor ||
                ec == asio::error::invalid_argument) {
                co_return;
            }
            // Transient: fd exhaustion, ECONNABORTED, EINTR, ENOBUFS, ENOMEM,
            // etc. A single bad accept must not stop the worker forever.
            acceptFailures_.fetch_add(1, std::memory_order_relaxed);
            static_cast<void>(
                co_await sleepFor(
                    workerHandle_, std::chrono::milliseconds(50)));
            if (!httpServerWorkerRunning(workerState_)) {
                co_return;
            }
            continue;
        }

        if (!httpServerWorkerRunning(workerState_)) {
            closeSocket(socket);
            co_return;
        }
        if (options_.maxConnections.has_value() &&
            activeConnectionCount_.load(std::memory_order_relaxed) >=
                *options_.maxConnections) {
            closeSocket(socket);
            connectionsRefused_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        configureAcceptedSocket(socket);
        // Starting the session is the one part of accepting that can throw
        // (coroutine frame allocation). Letting it escape would reach
        // asio::detached, which rethrows out of io_context::run() and fails the
        // whole worker -- a transient allocation failure would take down the
        // application. Treat it like the transient accept errors above: report,
        // drop this connection, pause, and keep accepting. Destroying the
        // unspawned lease closes the socket and returns its slot.
        try {
            AcceptedConnectionLease connection(
                std::move(socket), activeConnectionCount_);
            asio::co_spawn(
                ioContext_,
                taskAsAwaitable(handleSession(std::move(connection))),
                asio::bind_allocator(
                    asio::recycling_allocator<void>(), asio::detached));
            continue;
        } catch (...) {
            acceptFailures_.fetch_add(1, std::memory_order_relaxed);
            options_.connectionFailure.invoke({}, std::current_exception());
        }
        static_cast<void>(
            co_await sleepFor(workerHandle_, std::chrono::milliseconds(50)));
        if (!httpServerWorkerRunning(workerState_)) {
            co_return;
        }
    }
}

}  // namespace ruvia::detail
