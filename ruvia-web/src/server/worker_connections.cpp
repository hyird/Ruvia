#include "server/worker_connections.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#include <asio/bind_allocator.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_runtime.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"
#include "integration/worker_capabilities.h"
#include "server/http_connection_close.h"
#include "server/http_server_connection_guards.h"
#include "server/http_server_stream_session.h"
#include "server/http_server_tls_contexts.h"
#include "server/http_server_tls_handshake.h"
#include "server/peer_address.h"
#include "server/tcp_accept_error.h"
#include "tls/tls_served_certificate.h"
#include "util/tcp_socket_policies.h"

namespace ruvia::detail {
namespace {

void configure_accepted_socket(asio::ip::tcp::socket& socket) {
    // Accepted sockets disable Nagle and keep the system keep-alive default.
    constexpr client_tcp_config policies{.keep_alive_ = tcp_keep_alive_policy::system_default};
    if (const auto error = apply_tcp_socket_policies(socket, policies)) {
        throw std::system_error(error, "failed to configure accepted TCP socket");
    }
}

#if defined(RUVIA_ENABLE_HTTP2)
bool is_http2_alpn_selected(server_tls_stream& tls_stream) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int selected_length = 0;
    SSL_get0_alpn_selected(tls_stream.native_handle(), &selected, &selected_length);
    return selected_length == 2 && selected != nullptr && std::memcmp(selected, "h2", 2) == 0;
}
#endif

#if !defined(_WIN32)
void configure_listener_address(asio::ip::tcp::acceptor& socket) {
    socket.set_option(asio::socket_base::reuse_address(true));
#if defined(SO_REUSEPORT)
    const int enabled = 1;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable TCP listener SO_REUSEPORT");
    }
#else
    throw std::system_error(asio::error::operation_not_supported, "TCP worker listeners require SO_REUSEPORT");
#endif
}
#endif

// The scanner also trims the worker's memory after an idle interval.
[[nodiscard]] connection_scanner_options make_connection_scanner_options(
    const http_server_options& options, worker_memory& memory) noexcept {
    const auto& server = options.server_;
    return connection_scanner_options{.scan_interval_ = server.connection_scan_interval_,
        .idle_timeout_ = server.idle_timeout_,
        .initial_read_timeout_ = server.request_header_timeout_,
        .payload_read_timeout_ = server.request_body_timeout_,
        .write_timeout_ = server.write_timeout_,
        .initial_read_completion_timeout_ = server.header_completion_timeout_,
        .payload_read_completion_timeout_ = server.body_completion_timeout_,
        .trim_when_idle_ = &memory};
}

// Names the peer for the session's services and its failure report. Both stay
// empty when the socket no longer knew its peer.
void name_remote_endpoint(
    const std::optional<asio::ip::tcp::endpoint>& remote, std::pmr::string& address, std::uint16_t& port) {
    if (remote.has_value()) {
        assign_peer_address(address, remote->address());
        port = remote->port();
    }
}

}  // namespace

worker_connections::worker_connections(asio::io_context& io, const worker_handle& worker_value,
    worker_memory& memory, const route_table& routes_value, worker_capabilities& capabilities,
    http_server_options& options, const stop_token& stop_token_value, http_server_worker_state& state_value,
    task_scope& tasks, std::span<const http_server_listener_definition> listeners, worker_runtime& runtime
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    )
    : io_(io),
      worker_(worker_value),
      memory_(memory),
      routes_(routes_value),
      capabilities_(capabilities),
      options_(options),
      stop_token_(stop_token_value),
      state_(state_value),
      tasks_(tasks),
      runtime_(runtime),
      listeners_(memory.resource()),
      scanner_(worker_value, make_connection_scanner_options(options, memory)),
      work_sets_(memory)
#if defined(RUVIA_ENABLE_HTTP3)
      ,
      http3_placement_(http3_placement),
      http3_services_{.io_ = io,
          .worker_ = worker_value,
          .memory_ = memory,
          .routes_ = routes_value,
          .capabilities_ = capabilities,
          .options_ = options,
          .scanner_ = scanner_,
          .connection_count_ = connection_count_,
          .tasks_ = tasks,
          .stop_token_ = stop_token_value,
          .state_ = state_value,
          .refused_ = refused_,
          .placement_ = http3_placement},
      http3_endpoints_(memory.resource())
#endif
{
    listeners_.reserve(listeners.size());
    for (const auto& configured : listeners) {
        listeners_.push_back(make_pmr_object<listener>(memory.resource(), io, configured, memory.resource()));
    }
#if defined(RUVIA_ENABLE_HTTP3)
    // Every worker (and the Windows acceptor) numbers the HTTP/3 listeners
    // alike, so a handed-over datagram names its endpoint by index.
    for (auto& configured : listeners_) {
        if (configured->session_.http3_.has_value()) {
            http3_endpoints_.push_back(make_pmr_object<http3_endpoint>(memory.resource(), http3_services_,
                configured->session_,
                asio::ip::udp::endpoint(configured->endpoint_.address(), configured->session_.http3_->port_),
                http3_endpoints_.size()));
        }
    }
#endif
    options_.connection_failure_.counter_ = &failures_;
}
#if defined(_WIN32)
windows_acceptor::business_worker worker_connections::acceptor_target() noexcept {
    return {
        .worker_ = worker_,
        .load_ = &connection_count_,
        .target_ = this,
        .adopt_ = [](void* target, std::size_t listener_index, windows_accepted_socket accepted) {
            static_cast<worker_connections*>(target)->adopt_socket(listener_index, std::move(accepted));
        },
    };
}

void worker_connections::adopt_socket(std::size_t listener_index, windows_accepted_socket accepted) {
    assert(worker_.is_current());
    tcp_socket_type socket(io_);
    std::error_code error;
    socket.assign(listeners_[listener_index]->endpoint_.protocol(), accepted.get(), error);
    if (error) {
        // The acceptor's socket closes with `accepted`.
        try {
            throw std::system_error(error, "failed to bind an accepted TCP socket to its worker");
        } catch (...) {
            report_accept_failure(std::current_exception(), false);
        }
        return;
    }
    // The socket now owns the handle.
    (void)accepted.release();
    accept_socket(listener_index, std::move(socket));
}
#endif
void worker_connections::prepare() {
    try {
        for (auto& configured : listeners_) {
            configure_http_server_tls_contexts(configured->session_);
#if !defined(_WIN32)
            auto& socket = configured->socket_;
            socket.open(configured->endpoint_.protocol());
            configure_listener_address(socket);
            socket.bind(configured->endpoint_);
            socket.listen(asio::socket_base::max_listen_connections);
#endif
        }
#if defined(RUVIA_ENABLE_HTTP3)
        // After configure_http_server_tls_contexts built the QUIC contexts.
        for (auto& endpoint : http3_endpoints_) {
            endpoint->open();
        }
#endif
        scanner_.start();
    } catch (...) {
#if !defined(_WIN32)
        close_listeners();
#endif
#if defined(RUVIA_ENABLE_HTTP3)
        for (auto& endpoint : http3_endpoints_) {
            endpoint->close();
        }
#endif
        scanner_.stop();
        retire_tls();
        throw;
    }
}
void worker_connections::open_admission() noexcept {
    try {
        if (std::ranges::any_of(listeners_, [](const auto& configured) { return configured->session_.ticket_keys_.has_value(); })) {
            tasks_.spawn(rotate_tls_ticket_keys());
        }
#if !defined(_WIN32)
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            tasks_.spawn(run_accept_loop(index));
        }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
        if (!http3_endpoints_.empty()) {
            http3_placement_.directory_->attach(http3_placement_.worker_index_, this,
                [](void* target, std::size_t listener_index, http3_handoff_datagram& datagram) noexcept {
                    static_cast<worker_connections*>(target)->receive_http3_handoff(listener_index, datagram);
                });
        }
        for (auto& endpoint : http3_endpoints_) {
            endpoint->start();
        }
#endif
    } catch (...) {
        report_accept_failure(std::current_exception(), true);
    }
}
void worker_connections::stop() noexcept {
#if !defined(_WIN32)
    close_listeners();
#endif
    scanner_.stop();
    scanner_.close_all();
#if defined(RUVIA_ENABLE_HTTP3)
    // After close_all() sent each QUIC connection's CONNECTION_CLOSE.
    if (!http3_endpoints_.empty()) {
        http3_placement_.directory_->detach(http3_placement_.worker_index_);
    }
    for (auto& endpoint : http3_endpoints_) {
        endpoint->close();
    }
#endif
}
#if defined(RUVIA_ENABLE_HTTP3)
void worker_connections::receive_http3_handoff(std::size_t listener_index, http3_handoff_datagram& datagram) noexcept {
    if (listener_index < http3_endpoints_.size()) {
        http3_endpoints_[listener_index]->receive_handoff(datagram);
    }
}
#endif
void worker_connections::retire_tls() noexcept {
    for (auto& configured : listeners_) {
        auto& session = configured->session_;
        session.tls_context_.reset();
        session.sni_contexts_.clear();
        session.quic_tls_context_.reset();
        session.quic_sni_contexts_.clear();
        session.ticket_keys_.reset();
    }
}
// RFC 9325 §3.4: adopts each TLS listener's next ticket key epoch as it begins.
// It keeps running while the worker serves and ends when its timers stop, so
// this worker holds no key past its validity by more than the timer latency.
// The keys change between handshakes on this thread, never during one.
task<void> worker_connections::rotate_tls_ticket_keys() {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        auto next = std::chrono::steady_clock::time_point::max();
        for (auto& configured : listeners_) {
            auto& session = configured->session_;
            const auto* tls = session.tls();
            if (tls == nullptr || !session.ticket_keys_.has_value()) {
                continue;
            }
            auto& ring = tls_session_ticket_keys(*tls);
            if (!ring.copy_to(now, *session.ticket_keys_)) {
                // This worker issues no tickets until a copy succeeds; retry
                // soon rather than waiting out the epoch, since another worker
                // may already have generated the shared key.
                try {
                    throw std::runtime_error("failed to generate TLS session ticket keys");
                } catch (...) {
                    ruvia::report_unhandled_failure("TLS session ticket key rotation", std::current_exception());
                }
                next = std::min(next, now + tls_ticket_key_retry_delay);
            }
            next = std::min(next, ring.next_rotation(now));
        }
        if (next == std::chrono::steady_clock::time_point::max() ||
            co_await sleep_for(worker_, next - now) == timer_sleep_result::stop_requested) {
            co_return;
        }
    }
}
http_server_stats worker_connections::stats() const noexcept {
    http_server_stats result;
    result.active_connections_ = connection_count_.active();
    result.connections_refused_ = refused_.load(std::memory_order_relaxed);
    result.connection_failures_ = failures_.load(std::memory_order_relaxed);
    result.accept_failures_ = accept_failures_.load(std::memory_order_relaxed);
    return result;
}

#if !defined(_WIN32)
void worker_connections::close_listeners() noexcept {
    for (auto& configured : listeners_) {
        asio::error_code ignored;
        try {
            (void)configured->retry_.cancel();
        } catch (...) {
            ruvia::report_unhandled_failure("web accept retry cancellation", std::current_exception());
        }
        configured->socket_.close(ignored);
    }
}

task<void> worker_connections::run_accept_loop(std::size_t index) {
    auto& configured = *listeners_[index];
    try {
        while (http_server_worker_running(state_) && configured.socket_.is_open()) {
            // The accepted socket is born on this worker and never changes
            // executor. Only the long-lived listener task wraps acceptance.
            tcp_socket_type socket(io_);
            const auto completion = co_await ruvia::async_asio([this, &configured, &socket](auto handler) {
                configured.socket_.async_accept(socket, asio::bind_allocator(memory_.allocator(), std::move(handler)));
            });
            if (!http_server_worker_running(state_) || !configured.socket_.is_open()) {
                co_return;
            }
            const auto error = completion.error_code();
            if (!error) {
                accept_socket(index, std::move(socket));
                continue;
            }
            const auto action = classify_accept_error(error);
            if (action == accept_error_action::fail) {
                throw std::system_error(error, "fatal TCP accept error");
            }
            if (action == accept_error_action::report_and_retry) {
                try {
                    throw std::system_error(error, "recoverable TCP accept error");
                } catch (...) {
                    report_accept_failure(std::current_exception(), false);
                }
            }
            configured.retry_.expires_after(std::chrono::milliseconds(25));
            const auto retry = co_await ruvia::async_asio([this, &configured](auto handler) {
                configured.retry_.async_wait(asio::bind_allocator(memory_.allocator(), std::move(handler)));
            });
            if (!http_server_worker_running(state_) || !configured.socket_.is_open()) {
                co_return;
            }
            if (retry.error_code()) {
                throw std::system_error(retry.error_code(), "TCP accept retry failed");
            }
        }
    } catch (...) {
        report_accept_failure(std::current_exception(), true);
    }
}
#endif

void worker_connections::report_accept_failure(std::exception_ptr failure, bool fatal) noexcept {
    accept_failures_.fetch_add(1, std::memory_order_relaxed);
    if (fatal) {
#if !defined(_WIN32)
        close_listeners();
#endif
        (void)runtime_.fail(std::move(failure));
    } else {
        ruvia::report_unhandled_failure("web accept", std::move(failure));
    }
}

// Plain and TLS sessions are separate coroutines so a plain connection's frame
// carries none of the TLS stream, handshake and certificate state. Both keep
// the remote address outside their try so the failure report can name the
// peer; it stays empty if the failure happened before the address was resolved.
task<void> worker_connections::run_session(http_server_session_config& listener_value,
    accepted_connection_lease connection, std::optional<asio::ip::tcp::endpoint> remote) {
    auto& socket = connection.socket();
    std::pmr::string remote_address(memory_.allocator<char>());
    std::uint16_t remote_port = 0;
    try {
        name_remote_endpoint(remote, remote_address, remote_port);
        const auto close_mode = co_await run_stream(listener_value, socket, socket,
            capabilities_.make_context_services(stop_token_).with_plain_transport(remote_address, remote_port));
        // Plain TCP has no close alert: only a lingering close differs from an
        // immediate one. A worker stop closes at once; scanner close_all
        // reaches a lingering close that is already running.
        if (close_mode == http_connection_close::linger && !stop_token_.stop_requested()) {
            co_await linger_http_connection(socket, scanner_, worker_);
        }
        ruvia::close_socket(socket);
    } catch (...) {
        report_session_failure(socket, remote_address, std::current_exception());
    }
}

task<void> worker_connections::run_tls_session(http_server_session_config& listener_value,
    accepted_connection_lease connection, std::optional<asio::ip::tcp::endpoint> remote) {
    auto& socket = connection.socket();
    std::pmr::string remote_address(memory_.allocator<char>());
    std::uint16_t remote_port = 0;
    try {
        name_remote_endpoint(remote, remote_address, remote_port);
        // A stopping worker admits no new request, and a handshake leads only to one.
        if (!http_server_worker_running(state_)) {
            co_return;
        }
        server_tls_stream tls_stream(socket, *listener_value.tls_context_);
        {
            // The TLS handshake has its own initial-read deadline. It must be
            // released the moment the handshake resolves and before the
            // session is dispatched: the session installs and continuously
            // refreshes its own scanner entry, but this handshake entry stays
            // pinned at reading_initial with a frozen last-active time. Left
            // registered across the session, the scanner would close an active
            // connection's socket one request_header_timeout after the handshake
            // regardless of session activity -- severing long-lived TLS
            // sessions (websocket, keep-alive, slow uploads, streaming).
            connection_scanner::entry handshake_entry;
            connection_scanner::guard handshake_guard(scanner_, handshake_entry, connection_transport::tcp(socket));
            handshake_entry.read_request_head(connection_scanner::phase::reading_initial);
            const auto handshake_completion =
                co_await ruvia::async_asio(tls_server_handshake_initiator{&tls_stream});
            if (handshake_completion.error_code()) {
                ruvia::close_socket(socket);
                co_return;
            }
        }
        std::pmr::string client_certificate(memory_.allocator<char>());
        std::optional<std::string_view> client_subject;
        if (listener_value.tls()->client_certificates_.has_value()) {
            client_subject = extract_tls_client_certificate(tls_stream.native_handle(), client_certificate);
        }
        tls_served_certificate served_certificate(SSL_get_certificate(tls_stream.native_handle()));
        const auto services = capabilities_.make_context_services(stop_token_)
                                  .with_tls_transport(remote_address, client_subject, remote_port, served_certificate)
                                  .with_alternative_service(listener_value.alternative_service_);
#if defined(RUVIA_ENABLE_HTTP2)
        const auto close_mode = is_http2_alpn_selected(tls_stream)
                                    ? co_await run_http2(tls_stream, socket, services)
                                    : co_await run_stream(listener_value, tls_stream, socket, services);
#else
        const auto close_mode = co_await run_stream(listener_value, tls_stream, socket, services);
#endif
        // A worker stop keeps the immediate close; scanner close_all reaches a
        // graceful close that is already running.
        if (close_mode != http_connection_close::abort && !stop_token_.stop_requested()) {
            co_await close_http_connection_gracefully(tls_stream, close_mode, scanner_, worker_);
        }
        ruvia::close_socket(socket);
    } catch (...) {
        report_session_failure(socket, remote_address, std::current_exception());
    }
}

#if defined(RUVIA_ENABLE_HTTP2)
template <typename stream_type>
task<http_connection_close> worker_connections::run_http2(stream_type& stream, tcp_socket_type& socket,
    context_services services) {
    connection_scanner::entry scanner_entry;
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    co_return co_await run_http2_sans_io_session(
        stream, routes_, memory_, http2_sans_io_session_context(std::move(services), options_, scanner_entry));
}
#endif

void worker_connections::report_session_failure(
    tcp_socket_type& socket, std::string_view remote_address, std::exception_ptr failure) noexcept {
    // Last-resort safety net: any exception that escapes the session
    // body (including bad_alloc, error-handler failures, or framework
    // bugs) must not reach tasks_, where a child failure stops the scope.
    // Socket state may be partially written or completely fine; we
    // cannot safely emit anything new, so just drop the connection.
    //
    // Dropping the connection must not also drop the reason: this is the
    // only place that reason exists, so it goes to the connection-failure
    // sink before the frame unwinds.
    ruvia::close_socket(socket);
    options_.connection_failure_.invoke(remote_address, std::move(failure));
}

void worker_connections::accept_socket(std::size_t listener_index, tcp_socket_type socket) {
    assert(worker_.is_current());
    if (!http_server_worker_running(state_)) {
        return;
    }
    const auto& max_connections = options_.server_.max_connections_per_worker_;
    // At the cap, the connection idle longest between requests gives up its
    // slot (nginx-style reuse); only when none is idle is the new one refused.
    // The evicted socket closes now and its slot frees as its session
    // unwinds, so the gauge may briefly exceed the cap by those evictions
    // while the open sockets do not.
    if (max_connections.has_value() && connection_count_.active() >= *max_connections &&
        !scanner_.evict_oldest_reusable()) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Read the peer before the socket moves into the lease: a failure to
    // start the session destroys the lease, and its socket, with it.
    std::optional<asio::ip::tcp::endpoint> remote;
    std::error_code remote_error;
    if (auto endpoint = socket.remote_endpoint(remote_error); !remote_error) {
        remote = endpoint;
    }
    try {
        configure_accepted_socket(socket);
        accepted_connection_lease connection(std::move(socket), connection_count_);
        auto& listener_value = listeners_[listener_index]->session_;
        tasks_.spawn(listener_value.tls() != nullptr ? run_tls_session(listener_value, std::move(connection), remote)
                                                     : run_session(listener_value, std::move(connection), remote));
    } catch (...) {
        auto failure = std::current_exception();
        std::pmr::string remote_address(memory_.allocator<char>());
        std::uint16_t remote_port = 0;
        try {
            name_remote_endpoint(remote, remote_address, remote_port);
        } catch (...) {
            remote_address.clear();
        }
        accept_failures_.fetch_add(1, std::memory_order_relaxed);
        options_.connection_failure_.invoke(remote_address, std::move(failure));
    }
}

}  // namespace ruvia::detail
