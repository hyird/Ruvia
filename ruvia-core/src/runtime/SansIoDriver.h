#pragma once

// Generic sans-I/O connection pump (ruvia-core).
//
// Drives ANY protocol core that exposes the sans-I/O contract over an asio stream,
// delegating all protocol/framework work (draining events, dispatching handlers,
// submitting responses) to an `onReadable` callback invoked after each feed. It is
// templated on the connection, stream and callback types so ruvia-core never depends
// on ruvia-http -- the sans-I/O cores (Http2Connection, WsConnection) and the
// framework dispatch plug in from the layers above.
//
// Contract required of Connection:
//   <any>            feed(std::string_view);          // advance the protocol
//   std::string_view pendingOutput() const noexcept;  // bytes to write out
//   void             consumeOutput(std::size_t) noexcept;
//   bool             wantsWrite() const noexcept;
//   bool             closing() const noexcept;
// and OnReadable is any callable returning Task<void>: co_await onReadable(connection)
// after each feed drains the connection's events and submits responses (which append
// to the connection's outbound buffer, flushed on the next loop iteration).

#include <array>
#include <cstddef>
#include <string_view>

#include <asio/buffer.hpp>
#include <asio/write.hpp>

#include "runtime/AsioAwait.h"
#include "ruvia/app/Task.h"

namespace ruvia::detail {

template <typename Connection, typename Stream, typename OnReadable>
Task<void> pumpSansIoConnection(Connection& connection, Stream& stream, OnReadable onReadable) {
    std::array<char, 16384> readBuffer;
    for (;;) {
        // Flush everything the core has queued before blocking on the next read, so a
        // peer waiting on our reply (an HTTP/2 stream response, a WebSocket message) is
        // never starved. submit* appends synchronously, so this drains it all.
        while (connection.wantsWrite()) {
            const auto out = connection.pendingOutput();
            const auto ec = co_await asyncError([&stream, out](auto handler) mutable {
                asio::async_write(
                    stream, asio::buffer(out.data(), out.size()), std::move(handler));
            });
            if (ec) {
                co_return;
            }
            connection.consumeOutput(out.size());
        }
        if (connection.closing()) {
            co_return;
        }
        const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [&stream, &readBuffer](auto handler) mutable {
                stream.async_read_some(
                    asio::buffer(readBuffer.data(), readBuffer.size()), std::move(handler));
            });
        if (ec || bytesRead == 0) {
            co_return;
        }
        (void)connection.feed(std::string_view(readBuffer.data(), bytesRead));
        co_await onReadable(connection);
    }
}

}  // namespace ruvia::detail
