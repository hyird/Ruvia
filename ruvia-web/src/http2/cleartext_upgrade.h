#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http2_framing.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"
#include "router/route_table.h"
#include "server/connection_scanner.h"
#include "server/http_connection_close.h"
#include "server/http_connection_state.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

// What starting an HTTP/2 server session takes from the connection that hands
// it over: the stream to speak on, the socket underneath it, the worker's
// memory and scanner entry, and the routes, options and services every
// request on the session will be served with.
template <typename stream_type>
struct http2_server_session_setup final {
    stream_type& stream_;
    asio::ip::tcp::socket& socket_;
    worker_memory& memory_;
    const route_table& routes_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    context_services services_;
};

enum class cleartext_http2_dispatch_result : std::uint8_t {
    continue_http1,
    continue_read_loop,
    // The connection ends; close it at once.
    session_finished,
    // The HTTP/2 session ended after its GOAWAY; close it lingering.
    session_lingering,
};

// The client preface must be the first bytes of the connection (RFC 9113
// §3.4). auto_https reserves the cleartext listener for HTTP/1 redirects, and a
// connection that already completed an HTTP/1 exchange stays HTTP/1, so both
// refuse prior-knowledge HTTP/2. Preface classification itself is protocol.
[[nodiscard]] inline http2_cleartext_preface_probe probe_cleartext_http2_preface(
    std::string_view current, bool http1_only) noexcept {
    if (http1_only) {
        return http2_cleartext_preface_probe::http1;
    }
    return probe_http2_cleartext_preface(current);
}

// Entry point for a direct HTTP/2 connection (TLS ALPN h2, or a cleartext client
// preface). Runs the sans-I/O session and reports how its transport should close.
template <typename stream_type>
task<http_connection_close> run_http2_server_session(
    http2_server_session_setup<stream_type> setup, std::string_view initial_bytes = {}) {
    (void)setup.socket_;  // the sans-I/O session needs only the (possibly TLS) setup.stream
    co_return co_await run_http2_sans_io_session(setup.stream_, setup.routes_, setup.memory_,
        http2_sans_io_session_context(std::move(setup.services_), setup.options_, setup.scanner_entry_),
        initial_bytes);
}

template <typename stream_type>
task<cleartext_http2_dispatch_result> dispatch_cleartext_http2_preface(
    http2_server_session_setup<stream_type> setup, connection_read_buffer& read_buffer,
    bool http1_only) {
    const auto current = read_buffer.pending();
    switch (probe_cleartext_http2_preface(current, http1_only)) {
        case http2_cleartext_preface_probe::http1:
            co_return cleartext_http2_dispatch_result::continue_http1;
        case http2_cleartext_preface_probe::complete_preface:
            co_return co_await run_http2_server_session(setup, current) == http_connection_close::linger
                ? cleartext_http2_dispatch_result::session_lingering
                : cleartext_http2_dispatch_result::session_finished;
        case http2_cleartext_preface_probe::need_more_preface: {
            setup.scanner_entry_.read_request_head(connection_scanner::phase::reading_initial);
            const auto space = read_buffer.prepare_read();
            auto read_completion = co_await ruvia::async_asio<std::size_t>(
                [&setup, space](auto handler) mutable {
                    setup.stream_.async_read_some(
                        asio::buffer(space.data(), space.size()),
                        std::move(handler));
                });
            const auto ec = read_completion.error_code();
            const auto bytes_read = read_completion.result();
            if (ec) {
                co_return cleartext_http2_dispatch_result::session_finished;
            }
            read_buffer.commit_read(bytes_read);
            setup.scanner_entry_.touch();
            co_return cleartext_http2_dispatch_result::continue_read_loop;
        }
        case http2_cleartext_preface_probe::drop_connection:
            co_return cleartext_http2_dispatch_result::session_finished;
    }

    co_return cleartext_http2_dispatch_result::session_finished;
}

}  // namespace ruvia::detail
