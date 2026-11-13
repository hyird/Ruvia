#pragma once

#include <string_view>
#include <utility>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"

#include "context/context_services.h"
#include "server/connection_scanner.h"
#include "server/http_connection_close.h"
#include "server/http_server_options.h"
#include "server/inbound_socket_stream.h"

namespace ruvia::detail {

class route_table;

// What an HTTP/2 session needs from the server that accepted the connection.
// context_services is copied per session; the remaining dependencies are stable
// server-owned borrows that cannot be absent or rebound.
class http2_sans_io_session_context final {
public:
    http2_sans_io_session_context(context_services services, const http_server_options& options,
        connection_scanner::entry& scanner_entry)
        : services_(std::move(services)),
          options_(options),
          scanner_entry_(scanner_entry) {}

    [[nodiscard]] const http_server_options& options() const& noexcept RUVIA_LIFETIMEBOUND {
        return options_;
    }
    const http_server_options& options() const&& = delete;

    [[nodiscard]] connection_scanner::entry& scanner_entry() const& noexcept RUVIA_LIFETIMEBOUND {
        return scanner_entry_;
    }
    connection_scanner::entry& scanner_entry() const&& = delete;

    [[nodiscard]] const context_services& services() const& noexcept RUVIA_LIFETIMEBOUND {
        return services_;
    }
    const context_services& services() const&& = delete;

private:
    context_services services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
};

// HTTP/2 supports exactly the two transports owned by the Web server. Keeping
// these concrete overloads out of headers prevents every server/test consumer
// from instantiating the complete session coroutine again. The result is
// notify when the peer ended its input cleanly, linger when the session ended
// with GOAWAY (a completed drain or a connection error), otherwise abort.
[[nodiscard]] task<http_connection_close> run_http2_sans_io_session(inbound_socket_stream& stream,
    const route_table& routes_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes = {});

[[nodiscard]] task<http_connection_close> run_http2_sans_io_session(server_tls_stream& stream,
    const route_table& routes_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes = {});

}  // namespace ruvia::detail
