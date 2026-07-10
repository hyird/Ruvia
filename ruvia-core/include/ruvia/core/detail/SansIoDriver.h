#pragma once

// Generic sans-I/O connection pump (ruvia-core).
//
// Drives ANY protocol core that exposes the sans-I/O contract over an asio stream,
// delegating all protocol/framework work (draining events, dispatching handlers,
// submitting responses) to an `onReadable` callback invoked after each feed. It is
// templated on the connection, stream and callback types so the runtime stays
// independent of every protocol implementation and application dispatcher.
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

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"

namespace ruvia::detail {

template <typename Connection, typename Stream, typename OnReadable>
Task<void> pumpSansIoConnection(Connection& connection, Stream& stream, OnReadable onReadable) {
    std::array<char, 16384> readBuffer;
    for (;;) {
        // Flush everything the core has queued before blocking on the next read, so a
        // peer waiting on our reply is never starved. submit* appends
        // synchronously, so this drains the pending output completely.
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
