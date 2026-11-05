#pragma once

#include <system_error>

#include <asio/error.hpp>

#include "tls/tls_stream_end.h"

namespace ruvia::detail {

// A failed I/O operation on the inbound connection's own transport: the socket
// or TLS stream this server accepted. The type carries the failure's
// provenance, so connection_failure_sink can tell a connection that ended (the
// peer reset it, or this server cancelled its I/O) from a handler or upstream
// failure that happens to carry the same error code. Handlers still observe it
// as a std::system_error.
class inbound_transport_error final : public std::system_error {
public:
    inbound_transport_error(std::error_code code, const char* what_arg)
        : std::system_error(code, what_arg) {}
};

// Errors with which an inbound connection ends without any fault on this side:
// the peer closed it (including a TLS peer that closed TCP without
// close_notify), reset or aborted it, the network gave up on a vanished peer,
// or this server ended the I/O itself. operation_aborted is our cancellation
// of I/O in flight; bad_descriptor is I/O started after our own close (stop,
// drain, or a timeout that closes the socket). Only this server closes an
// inbound socket, so neither code can come from anything else on the inbound
// transport.
[[nodiscard]] inline bool is_inbound_connection_end(const std::error_code& error) noexcept {
    return is_stream_read_end(error) || error == asio::error::connection_reset ||
           error == asio::error::connection_aborted || error == asio::error::broken_pipe ||
           error == asio::error::operation_aborted || error == asio::error::bad_descriptor ||
           error == asio::error::timed_out || error == asio::error::not_connected ||
           error == asio::error::shut_down;
}

}  // namespace ruvia::detail
