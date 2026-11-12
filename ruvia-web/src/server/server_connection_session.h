#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/server_connection.h"

#include "http/http_socket_tunnel_transport.h"
#include "http/http_stream_end.h"
#include "server/connection_scanner.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {

// The byte stream behind one server_connection: the accepted TCP socket, or
// the TLS stream whose writes leave through `tls_output`. Reads land in one
// buffer charged to the connection's inbound budget and reused for every
// read; each read and write that completes renews the scanner entry's
// activity, so idle_timeout_ measures time without I/O.
template <typename stream_type>
class server_connection_session final {
public:
    server_connection_session(stream_type& stream, tls_tunnel_output* tls_output, connection_scanner::entry& entry,
        std::pmr::memory_resource& inbound, const worker_handle& worker_value, const stop_token& stop_token_value,
        const server_connection_fields& fields)
        : transport_(stream, tls_output, &entry),
          entry_(entry),
          read_buffer_(&inbound),
          facade_(fields, stop_token_value, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk) {}
    server_connection_session(const server_connection_session&) = delete;
    server_connection_session& operator=(const server_connection_session&) = delete;

    [[nodiscard]] server_connection& connection() noexcept {
        return facade_;
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }
    void abort() noexcept {
        aborted_ = true;
        transport_.abort();
    }
    // Expires the facade once the handler returned. An operation the handler
    // left running is aborted first, so the join cannot wait on the peer.
    [[nodiscard]] task<void> join() {
        if (facade_.running_operations_ != 0) {
            abort();
        }
        co_await facade_.operations_.close_and_join();
    }
    // Ends the send direction the handler left open: close_notify for TLS.
    [[nodiscard]] task<void> finish() {
        if (!facade_.send_ended_) {
            facade_.send_ended_ = true;
            co_await end_send();
        }
    }

private:
    [[nodiscard]] task<void> end_send() {
        if (const auto error = co_await transport_.write_bytes({}, http_stream_end::end)) {
            throw inbound_transport_error(error, "server connection finish");
        }
    }
    static task<std::optional<std::span<const std::byte>>> read_thunk(void* raw) {
        auto& owner = *static_cast<server_connection_session*>(raw);
        if (owner.receive_ended_) {
            co_return std::nullopt;
        }
        owner.read_buffer_.clear();
        if (!(co_await owner.transport_.read_more(owner.read_buffer_))) {
            owner.receive_ended_ = true;
            co_return std::nullopt;
        }
        owner.entry_.touch();
        co_return std::as_bytes(std::span(owner.read_buffer_.data(), owner.read_buffer_.size()));
    }
    static task<void> write_thunk(void* raw, std::string_view bytes) {
        auto& owner = *static_cast<server_connection_session*>(raw);
        if (const auto error = co_await owner.transport_.write_bytes(bytes, http_stream_end::keep_open)) {
            throw inbound_transport_error(error, "server connection write");
        }
    }
    static task<void> finish_thunk(void* raw) {
        return static_cast<server_connection_session*>(raw)->end_send();
    }
    static void abort_thunk(void* raw) noexcept {
        static_cast<server_connection_session*>(raw)->abort();
    }

    http_socket_tunnel_transport<stream_type> transport_;
    connection_scanner::entry& entry_;
    std::pmr::string read_buffer_;
    bool receive_ended_{false};
    bool aborted_{false};
    server_connection facade_;
};

}  // namespace ruvia::detail
