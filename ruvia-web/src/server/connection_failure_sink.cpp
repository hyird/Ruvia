#include <system_error>

#include <asio/error.hpp>

#include "server/http_server_options.h"
#include "server/inbound_transport_error.h"
#include "util/error_code_text.h"

namespace ruvia::detail {
namespace {

// Errors with which a connection ends without any fault on this side: the peer
// reset, aborted, or closed it, the network gave up on a vanished peer, or this
// server cancelled the I/O itself (operation_aborted only arises from our own
// cancellation: stop, drain, or a timeout that closes the socket).
[[nodiscard]] bool is_connection_end(const std::error_code& error) noexcept {
    return error == asio::error::connection_reset || error == asio::error::connection_aborted ||
           error == asio::error::broken_pipe || error == asio::error::eof ||
           error == asio::error::operation_aborted || error == asio::error::timed_out ||
           error == asio::error::not_connected || error == asio::error::shut_down;
}

// Only the connection's own transport can end it normally. A handler or
// upstream failure is a real fault even when it carries one of the codes above:
// an upstream request that timed out or was reset says nothing about the
// inbound peer.
[[nodiscard]] bool is_connection_end(const std::exception_ptr& exception) noexcept {
    try {
        std::rethrow_exception(exception);
    } catch (const inbound_transport_error& error) {
        return is_connection_end(error.code());
    } catch (...) {
        return false;
    }
}

}  // namespace

void connection_failure_sink::invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept {
    if (exception == nullptr || is_connection_end(exception)) {
        return;
    }
    if (counter_ != nullptr) {
        counter_->fetch_add(1, std::memory_order_relaxed);
    }
    if (callback_) {
        callback_(connection_failure_record_access::make(remote_address, std::move(exception)));
        return;
    }
    report_unhandled_web_failure("web connection", exception);
}

}  // namespace ruvia::detail
