#pragma once

#include <cstdint>
#include <system_error>

#include <asio/error.hpp>
#include <asio/ssl/error.hpp>

// How a transport stream's input ends, and which inbound transport errors end
// a connection rather than report a fault.
namespace ruvia::detail {

enum class http_stream_end : std::uint8_t { keep_open,
    end };

// Whether a read error marks the end of input rather than a read failure: a
// TCP FIN or TLS close_notify (eof), or a TLS peer that closed TCP without
// close_notify (stream_truncated). Message-delimited protocols above the
// stream still detect a truncated message themselves; the transport only
// reports that no more bytes follow.
[[nodiscard]] inline bool is_stream_read_end(const std::error_code& error) noexcept {
    return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
}

// A failed I/O operation on the inbound connection's own transport: the socket
// or TLS stream this server accepted. The type carries the failure's
// provenance, so connection_failure_sink can tell a connection that ended (the
// peer reset it, or this server cancelled its I/O) from a handler or upstream
// failure that happens to carry the same error code. Handlers still observe it
// as a std::system_error.
class inbound_transport_error final : public std::system_error {
public:
    inbound_transport_error(std::error_code code, const char* what_arg) :
        std::system_error(code, what_arg) {}
};

// Errors with which an inbound connection ends without any fault on this side:
// the peer closed it (including a TLS peer that closed TCP without
// close_notify), reset or aborted it, the network gave up on a vanished peer,
// or this server ended the I/O itself. Cancellation may carry Asio's native
// operation_aborted code or the generic operation_canceled condition used by
// the TLS output pump; bad_descriptor is I/O started after our own close
// (stop, drain, or a timeout). Only this server closes an inbound socket, so
// these codes cannot come from anything else on the inbound transport.
[[nodiscard]] inline bool is_inbound_connection_end(const std::error_code& error) noexcept {
    return is_stream_read_end(error) || error == asio::error::connection_reset || error == asio::error::connection_aborted || error == asio::error::broken_pipe || error == asio::error::operation_aborted || error == std::errc::operation_canceled || error == asio::error::bad_descriptor || error == asio::error::timed_out || error == asio::error::not_connected || error == asio::error::shut_down;
}

}  // namespace ruvia::detail
