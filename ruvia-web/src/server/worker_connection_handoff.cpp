#include <utility>

#include "ruvia/core/Socket.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"
#include "ruvia/web/detail/server/session/HttpServerConnectionGuards.h"
#include "ruvia/web/detail/server/session/HttpServerSessionEntry.h"

namespace ruvia::detail {

void WebWorkerRuntime::acceptSocketOnContext(std::size_t listenerIndex, TcpSocket socket) {
    if (runtime_.state() != RuntimeLifecycle::State::kRunning ||
        !httpServerWorkerRunning(workerState_)) {
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
        backgroundTasks_.spawn(handleSession(*listeners_[listenerIndex], std::move(connection)));
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
    if (runtime_.state() != RuntimeLifecycle::State::kRunning ||
        !httpServerWorkerRunning(workerState_) || listenerIndex >= listeners_.size()) {
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
