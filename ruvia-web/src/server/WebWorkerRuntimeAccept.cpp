#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/Socket.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"
#include "ruvia/web/detail/server/session/HttpServerConnectionGuards.h"
#include "ruvia/web/detail/server/session/HttpServerSessionEntry.h"

namespace ruvia::detail {

Task<void> WebWorkerRuntime::superviseListener(std::size_t listenerIndex,
    HttpServerAcceptor& acceptor, HttpServerSessionConfig& session) {
    try {
        co_await acceptLoop(listenerIndex, acceptor, session);
        if (httpServerWorkerRunning(workerState_)) {
            throw std::runtime_error("HTTP listener stopped unexpectedly");
        }
    } catch (...) {
        failWorker(std::current_exception());
    }
}

Task<void> WebWorkerRuntime::acceptLoop(std::size_t listenerIndex,
    HttpServerAcceptor& acceptor, HttpServerSessionConfig& session) {
    for (;;) {
        auto acceptCompletion =
            co_await ruvia::asyncAsio<asio::ip::tcp::socket>([&acceptor](auto handler) mutable {
                acceptor.acceptor.async_accept(std::move(handler));
            });
        const auto ec = acceptCompletion.errorCode();
        auto socket = std::move(acceptCompletion).takeResult();

        if (ec) {
            // Fatal: acceptor was cancelled (stop()) or closed. Exit cleanly.
            if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor ||
                ec == asio::error::invalid_argument) {
                co_return;
            }
            // Transient: fd exhaustion, ECONNABORTED, EINTR, ENOBUFS, ENOMEM,
            // etc. A single bad accept must not stop the worker forever.
            acceptFailures_.fetch_add(1, std::memory_order_relaxed);
            static_cast<void>(
                co_await sleepFor(workerRuntime_.handle(), std::chrono::milliseconds(50)));
            if (!httpServerWorkerRunning(workerState_)) {
                co_return;
            }
            continue;
        }

        acceptSocketOnContext(listenerIndex, std::move(socket));
    }
}

void WebWorkerRuntime::acceptSocketOnContext(std::size_t listenerIndex, TcpSocket socket) {
    if (!httpServerWorkerRunning(workerState_)) {
        return;
    }
    if (listenerIndex >= listeners_.size()) {
        return;
    }
    if (options_.maxConnections.has_value() &&
        activeConnectionCount_.load(std::memory_order_relaxed) >= *options_.maxConnections) {
        connectionsRefused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    try {
        ruvia::configureAcceptedSocket(socket);
        AcceptedConnectionLease connection(std::move(socket), activeConnectionCount_);
        asio::co_spawn(ioContext_,
            ruvia::asAwaitable(handleSession(*listeners_[listenerIndex], std::move(connection))),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    } catch (...) {
        acceptFailures_.fetch_add(1, std::memory_order_relaxed);
        options_.connectionFailure.invoke({}, std::current_exception());
    }
}

void WebWorkerRuntime::acceptTransferredConnection(NativeAcceptedSocketTicket&& ticket) noexcept {
    if (!ticket.valid()) {
        return;
    }
    const auto listenerIndex = ticket.listenerIndex();
    if (!httpServerWorkerRunning(workerState_) || listenerIndex >= listeners_.size()) {
        return;
    }

    try {
        TcpSocket socket(ioContext_);
        asio::error_code error;
        try {
            socket.assign(ticket.protocol(), ticket.nativeHandle(), error);
        } catch (...) {
            // Some Asio implementations can take the handle before reporting an
            // exception. Disarm the ticket before socket's RAII cleanup in that case.
            if (socket.is_open() && socket.native_handle() == ticket.nativeHandle()) {
                static_cast<void>(ticket.release());
            }
            acceptFailures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (error) {
            if (socket.is_open() && socket.native_handle() == ticket.nativeHandle()) {
                static_cast<void>(ticket.release());
            }
            acceptFailures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        static_cast<void>(ticket.release());  // ownership now belongs to socket.
        acceptSocketOnContext(listenerIndex, std::move(socket));
    } catch (...) {
        // Includes TcpSocket construction and any unexpected accept-path failure.
        // Until assign transfers ownership the ticket remains responsible for close.
        acceptFailures_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace ruvia::detail
