#if defined(_WIN32)
#include <mswsock.h>
#include <ws2tcpip.h>

#include <asio/windows/object_handle.hpp>
#include <asio/windows/overlapped_ptr.hpp>
#endif
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#include <array>
#include <limits>
#include <vector>

#include <asio/bind_allocator.hpp>
#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/post.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/http/proxy_protocol.h"
#include "ruvia/web/http_server.h"

#include "http/context_access.h"
#include "http/operation_interrupt.h"
#include "http2/http2_sans_io_session.h"
#include "server/app_state.h"
#include "server/connection_lifecycle.h"
#include "server/route_table.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include <ngtcp2/ngtcp2.h>
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3/http3_session.h"
#endif

namespace ruvia::detail {
namespace {

void configure_accepted_socket(asio::ip::tcp::socket& socket) {
    // Accepted sockets disable Nagle and keep the system keep-alive default.
    constexpr client_tcp_config policies{.keep_alive_ = tcp_keep_alive_policy::system_default};
    if (const auto error = apply_tcp_socket_policies(socket, policies)) {
        throw std::system_error(error, "failed to configure accepted TCP socket");
    }
}

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

// The scanner also collects the worker's thread heap after an idle interval.
[[nodiscard]] connection_scanner_options make_connection_scanner_options(
    const http_server_options& options) noexcept {
    const auto& server = options.server_;
    return connection_scanner_options{.scan_interval_ = server.connection_scan_interval_,
        .idle_timeout_ = server.idle_timeout_,
        .initial_read_timeout_ = server.request_header_timeout_,
        .payload_read_timeout_ = server.request_body_timeout_,
        .write_timeout_ = server.write_timeout_,
        .initial_read_completion_timeout_ = server.header_completion_timeout_,
        .payload_read_completion_timeout_ = server.body_completion_timeout_};
}

// Names the peer for the session's services and its failure report. Both stay
// empty when the socket no longer knew its peer.
void name_remote_endpoint(
    const std::optional<asio::ip::tcp::endpoint>& remote, std::string& address, std::uint16_t& port) {
    if (remote.has_value()) {
        assign_peer_address(address, remote->address());
        port = remote->port();
    }
}

}  // namespace

worker_connections::worker_connections(asio::io_context& io, const worker_ref& worker_value,
    const route_table& routes_value, worker_capabilities& capabilities,
    http_server_options& options, const stop_token& stop_token_value, http_server_worker_state& state_value,
    task_scope& tasks, std::span<const http_server_listener_definition> listeners, worker_failure_sink failure,
    std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    )
    : io_(io),
      worker_(worker_value),
      routes_(routes_value),
      capabilities_(capabilities),
      options_(options),
      stop_token_(stop_token_value),
      state_(state_value),
      tasks_(tasks),
      failure_(failure),
      worker_index_(worker_index),
      listeners_(),
      scanner_(worker_value, make_connection_scanner_options(options))
#if defined(RUVIA_ENABLE_HTTP3)
      ,
      http3_placement_(http3_placement),
      http3_services_{.io_ = io,
          .worker_ = worker_value,
          .routes_ = routes_value,
          .capabilities_ = capabilities,
          .options_ = options,
          .scanner_ = scanner_,
          .connection_count_ = connection_count_,
          .connection_ids_ = connection_ids_,
          .registry_ = registry_,
          .stop_token_ = stop_token_value,
          .state_ = state_value,
          .refused_ = refused_,
          .placement_ = http3_placement},
      http3_endpoints_()
#endif
{
    listeners_.reserve(listeners.size());
    for (const auto& configured : listeners) {
        listeners_.push_back(
            std::make_unique<listener>(io, worker_value, configured));
        prepare_listener_gauge(listeners_.size() - 1, configured.endpoint_);
    }
#if defined(RUVIA_ENABLE_HTTP3)
    // Every worker (and the Windows acceptor) numbers the HTTP/3 listeners
    // alike, so a handed-over datagram names its endpoint by index.
    for (std::size_t index = 0; index < listeners_.size(); ++index) {
        if (listeners_[index]->session_.http3_.has_value()) {
            add_http3_endpoint(index, http3_endpoints_.size());
        }
    }
#endif
    options_.connection_failure_.counter_ = &failures_;
}
#if defined(RUVIA_ENABLE_HTTP3)
void worker_connections::add_http3_endpoint(std::size_t listener_index, std::size_t http3_index) {
    auto& configured = *listeners_[listener_index];
    if (http3_endpoints_.size() <= http3_index) {
        http3_endpoints_.resize(http3_index + 1);
    }
    http3_endpoints_[http3_index] = std::make_unique<http3_endpoint>(http3_services_, configured.session_, asio::ip::udp::endpoint(configured.endpoint_.address(), configured.session_.http3_->port_), http3_index, configured.tasks_, gauges_[listener_index]->count_);
    configured.http3_index_ = http3_index;
}
#endif
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
    // A listener removed since the acceptor took the socket admits nothing:
    // the socket closes with `accepted`.
    if (listener_index >= listeners_.size() || listeners_[listener_index] == nullptr ||
        !listeners_[listener_index]->admitting_) {
        return;
    }
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
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            prepare_listener_resources(index);
        }
        scanner_.start();
    } catch (...) {
        // The listeners already prepared retire with the failed worker's stop.
        for (auto& configured : listeners_) {
            close_listener_sockets(*configured);
        }
        scanner_.stop();
        retire_tls();
        throw;
    }
}
void worker_connections::prepare_listener_resources(std::size_t index) {
    auto& configured = *listeners_[index];
    if (configured.session_.tls() && !configured.session_.tls_context_) {
        configure_http_server_tls_contexts(configured.session_, static_cast<bool>(options_.tls_client_hello_));
    }
#if !defined(_WIN32)
    auto& socket = configured.socket_;
    const auto inherited =
        configured.inherited_ != nullptr ? configured.inherited_->claim_tcp(worker_index_) : invalid_native_socket;
    if (inherited != invalid_native_socket) {
        // Checked to listen on this endpoint when it was handed over.
        std::error_code error;
        socket.assign(configured.endpoint_.protocol(), inherited, error);
        if (error) {
            close_native_socket(inherited);
            throw std::system_error(error, "failed to adopt an inherited TCP listener socket");
        }
    } else if (!configured.externally_accepted_) {
        socket.open(configured.endpoint_.protocol());
        configure_listener_address(socket);
        if (configured.session_.ipv6_only_) {
            socket.set_option(asio::ip::v6_only(*configured.session_.ipv6_only_));
        }
        socket.bind(configured.endpoint_);
        socket.listen(asio::socket_base::max_listen_connections);
    }
#endif
    tasks_.spawn(retire_listener(index));
    configured.prepared_ = true;
#if defined(RUVIA_ENABLE_HTTP3)
    // After configure_http_server_tls_contexts built the QUIC context.
    if (configured.http3_index_.has_value()) {
#if defined(_WIN32)
        http3_endpoints_[*configured.http3_index_]->open(nullptr);
#else
        http3_endpoints_[*configured.http3_index_]->open(configured.inherited_.get());
#endif
    }
#endif
#if !defined(_WIN32)
    // What this worker did not claim belongs to the other workers and closes
    // with the last of them.
    configured.inherited_.reset();
#endif
    if (configured.session_.ticket_keys_.has_value() && !rotating_ticket_keys_) {
        tasks_.spawn(rotate_tls_ticket_keys());
    }
#if !defined(_WIN32)
    if (!configured.externally_accepted_) {
        configured.tasks_.spawn(run_accept_loop(index));
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_ && !http3_attached_) {
        http3_placement_.directory_->attach(http3_placement_.worker_index_, this,
            [](void* target, std::size_t listener_index, http3_handoff_datagram& datagram) noexcept {
                static_cast<worker_connections*>(target)->receive_http3_handoff(listener_index, datagram);
            });
        http3_attached_ = true;
    }
#endif
}
void worker_connections::open_admission() noexcept {
    for (std::size_t index = 0; index < listeners_.size(); ++index) {
        start_listener(index);
    }
}
void worker_connections::start_listener(std::size_t index) noexcept {
    auto& configured = *listeners_[index];
    configured.admitting_ = true;
    gauges_[index]->serving_.store(true, std::memory_order_relaxed);
    gauges_[index]->published_.store(true, std::memory_order_relaxed);
    configured.activated_.notify();
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_) {
        http3_endpoints_[*configured.http3_index_]->start();
    }
#endif
}
void worker_connections::prepare_listener(std::size_t index, [[maybe_unused]] std::optional<std::size_t> http3_index,
    const http_server_listener_definition& definition, bool externally_accepted
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_directory* directory
#endif
) {
    prepare_listener(index, http3_index, definition, externally_accepted, {}
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        directory
#endif
    );
}
void worker_connections::prepare_listener(std::size_t index, [[maybe_unused]] std::optional<std::size_t> http3_index,
    const http_server_listener_definition& definition, bool externally_accepted,
    prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_directory* directory
#endif
) {
    assert(worker_.is_current());
    if (!http_server_worker_running(state_)) {
        throw std::logic_error("the worker is stopping");
    }
    if (listeners_.size() <= index) {
        listeners_.resize(index + 1);
    }
    listeners_[index] = std::make_unique<listener>(io_, worker_, definition, std::move(session));
    prepare_listener_gauge(index, definition.endpoint_);
    listeners_[index]->externally_accepted_ = externally_accepted;
    try {
#if defined(RUVIA_ENABLE_HTTP3)
        if (http3_index.has_value()) {
            // The application creates the directory with its first HTTP/3 listener.
            if (http3_placement_.directory_ == nullptr) {
                http3_placement_.directory_ = directory;
                http3_services_.placement_.directory_ = directory;
            }
            add_http3_endpoint(index, *http3_index);
        }
#endif
        prepare_listener_resources(index);
    } catch (...) {
        if (listeners_[index]->prepared_) {
            discard_listener(index);
            throw;
        }
        // No task owns the listener yet.
#if defined(RUVIA_ENABLE_HTTP3)
        if (http3_index.has_value() && *http3_index < http3_endpoints_.size()) {
            http3_endpoints_[*http3_index].reset();
        }
#endif
        listeners_[index].reset();
        throw;
    }
}
void worker_connections::open_listener(std::size_t index) noexcept {
    assert(worker_.is_current());
    // A stopping worker admits nothing; its stop retires the listener.
    if (!http_server_worker_running(state_) || index >= listeners_.size() || listeners_[index] == nullptr) {
        return;
    }
    start_listener(index);
}
void worker_connections::discard_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
    assert(worker_.is_current());
    if (index >= listeners_.size() || listeners_[index] == nullptr) {
        return;
    }
    // A listener in a slot was prepared, so a task awaits its retirement.
    auto& configured = *listeners_[index];
    configured.retirement_receipt_ = std::move(receipt);
    gauges_[index]->serving_.store(false, std::memory_order_relaxed);
    registry_.close_listener(&configured.session_);
    close_listener_sockets(configured);
    configured.retired_.notify();
}
void worker_connections::remove_listener(std::size_t index, [[maybe_unused]] std::shared_ptr<void> receipt, listener_remove_options options) noexcept {
    assert(worker_.is_current());
    // Not admitting: the worker's stop already retired it.
    if (index >= listeners_.size() || listeners_[index] == nullptr || !listeners_[index]->admitting_) {
        return;
    }
    auto& configured = *listeners_[index];
    configured.admitting_ = false;
    gauges_[index]->serving_.store(false, std::memory_order_relaxed);
    if (options.close_connections_) {
        registry_.close_listener(&configured.session_);
    }
#if !defined(_WIN32)
    // Closing cancels the pending accept; the loop then exits.
    asio::error_code ignored;
    configured.socket_.close(ignored);
    if (configured.accepting_) {
        configured.removal_receipt_ = std::move(receipt);
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_.has_value()) {
        http3_endpoints_[*configured.http3_index_]->retire();
    }
#endif
    configured.retired_.notify();
}
#if !defined(_WIN32)
listener_socket_duplicates worker_connections::duplicate_listener(std::size_t index) {
    assert(worker_.is_current());
    if (index >= listeners_.size() || listeners_[index] == nullptr || !listeners_[index]->admitting_ ||
        !listeners_[index]->socket_.is_open()) {
        throw std::logic_error("the listener no longer accepts on this worker");
    }
    auto& configured = *listeners_[index];
    listener_socket_duplicates duplicates;
    duplicates.tcp_ = duplicate_native_socket(configured.socket_.native_handle());
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_.has_value()) {
        try {
            duplicates.udp_ = http3_endpoints_[*configured.http3_index_]->duplicate_socket();
        } catch (...) {
            close_native_socket(duplicates.tcp_);
            throw;
        }
    }
#endif
    return duplicates;
}
#endif
void worker_connections::close_listener_sockets(listener& configured) noexcept {
    configured.admitting_ = false;
    configured.activated_.notify();
#if !defined(_WIN32)
    asio::error_code ignored;
    configured.socket_.close(ignored);
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_.has_value()) {
        http3_endpoints_[*configured.http3_index_]->close();
    }
#endif
}
task<void> worker_connections::retire_listener(std::size_t index) {
    auto& configured = *listeners_[index];
    co_await configured.retired_.wait();
    co_await configured.tasks_.join();
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_.has_value()) {
        http3_endpoints_[*configured.http3_index_].reset();
    }
#endif
    listeners_[index].reset();
}
void worker_connections::stop() noexcept {
    {
        std::lock_guard lock(gauges_mutex_);
        for (const auto& gauge : gauges_) {
            if (gauge) {
                gauge->serving_.store(false, std::memory_order_relaxed);
            }
        }
    }
    for (auto& configured : listeners_) {
        if (configured != nullptr) {
            configured->admitting_ = false;
        }
    }
#if !defined(_WIN32)
    close_listeners();
#endif
    scanner_.stop();
    scanner_.close_all();
#if defined(RUVIA_ENABLE_HTTP3)
    // After close_all() sent each QUIC connection's CONNECTION_CLOSE.
    if (std::exchange(http3_attached_, false)) {
        http3_placement_.directory_->detach(http3_placement_.worker_index_);
    }
    for (auto& endpoint : http3_endpoints_) {
        if (endpoint != nullptr) {
            endpoint->close();
        }
    }
#endif
    // Each listener retires once the tasks these closes end have unwound.
    for (auto& configured : listeners_) {
        if (configured != nullptr && configured->prepared_) {
            configured->activated_.notify();
            configured->retired_.notify();
        }
    }
}
#if defined(RUVIA_ENABLE_HTTP3)
void worker_connections::receive_http3_handoff(std::size_t listener_index, http3_handoff_datagram& datagram) noexcept {
    // A retired endpoint's slot stays empty.
    if (listener_index < http3_endpoints_.size() && http3_endpoints_[listener_index] != nullptr) {
        http3_endpoints_[listener_index]->receive_handoff(datagram);
    }
}
#endif
void worker_connections::retire_tls() noexcept {
    for (auto& configured : listeners_) {
        if (configured != nullptr) {
            auto& session = configured->session_;
            session.tls_context_.reset();
            session.quic_tls_context_.reset();
            session.ticket_keys_.reset();
        }
    }
}
void worker_connections::adopt_tls_identities(
    std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept {
    assert(worker_.is_current());
    if (listener_index < listeners_.size() && listeners_[listener_index] != nullptr) {
        listeners_[listener_index]->session_.adopt_tls_identities(std::move(identities));
    }
}
// RFC 9325 §3.4: adopts each TLS listener's next ticket key epoch as it begins.
// It keeps running while the worker serves a TLS listener and ends when its
// timers stop, so this worker holds no key past its validity by more than the
// timer latency. The keys change between handshakes on this thread, never
// during one. Every listener of the run shares one key epoch origin, so a
// listener added while this waits rotates at the instant it waits for.
task<void> worker_connections::rotate_tls_ticket_keys() {
    rotating_ticket_keys_ = true;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        auto next = std::chrono::steady_clock::time_point::max();
        for (auto& configured : listeners_) {
            if (configured == nullptr) {
                continue;
            }
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
            co_await sleep_until(worker_, next, stop_token_) == timer_sleep_result::stop_requested) {
            break;
        }
    }
    // A TLS listener added later starts the next one.
    rotating_ticket_keys_ = false;
}
void worker_connections::prepare_listener_gauge(std::size_t index, const asio::ip::tcp::endpoint& endpoint) {
    auto gauge = std::make_shared<listener_gauge>(endpoint, connection_count_);
    std::lock_guard lock(gauges_mutex_);
    if (gauges_.size() <= index) {
        gauges_.resize(index + 1);
    }
    gauges_[index] = std::move(gauge);
}

std::shared_ptr<connection_count> worker_connections::listener_connections(const asio::ip::tcp::endpoint& endpoint) const {
    std::lock_guard lock(gauges_mutex_);
    for (auto it = gauges_.rbegin(); it != gauges_.rend(); ++it) {
        if (*it && (*it)->published_.load(std::memory_order_relaxed) && (*it)->endpoint_ == endpoint) {
            return std::shared_ptr<connection_count>(*it, &(*it)->count_);
        }
    }
    throw std::invalid_argument("no listener serves this endpoint");
}

http_server_stats worker_connections::stats() const {
    http_server_stats result;
    result.active_connections_ = connection_count_.active();
    result.connections_refused_ = refused_.load(std::memory_order_relaxed);
    result.connection_failures_ = failures_.load(std::memory_order_relaxed);
    result.accept_failures_ = accept_failures_.load(std::memory_order_relaxed);
    std::lock_guard lock(gauges_mutex_);
    for (const auto& gauge : gauges_) {
        if (!gauge) {
            continue;
        }
        const auto active = gauge->count_.active();
        if (gauge->serving_.load(std::memory_order_relaxed) || active != 0) {
            result.listeners_.push_back({.address_ = to_ip_address(gauge->endpoint_.address()),
                .port_ = gauge->endpoint_.port(),
                .active_connections_ = active});
        }
    }
    return result;
}

#if !defined(_WIN32)
void worker_connections::close_listeners() noexcept {
    for (auto& configured : listeners_) {
        if (configured != nullptr) {
            asio::error_code ignored;
            configured->socket_.close(ignored);
        }
    }
}

task<void> worker_connections::run_accept_loop(std::size_t index) {
    auto& configured = *listeners_[index];
    co_await configured.activated_.wait();
    configured.accepting_ = true;
    try {
        while (http_server_worker_running(state_) && configured.socket_.is_open()) {
            // The accepted socket is born on this worker and never changes
            // executor. Only the long-lived listener task wraps acceptance.
            tcp_socket_type socket(io_);
            const auto completion = co_await ruvia::async_asio([&configured, &socket](auto handler) {
                configured.socket_.async_accept(socket, std::move(handler));
            });
            if (!http_server_worker_running(state_) || !configured.socket_.is_open()) {
                break;
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
            if (action == accept_error_action::retry) {
                continue;
            }
            try {
                throw std::system_error(error, "recoverable TCP accept error");
            } catch (...) {
                report_accept_failure(std::current_exception(), false);
            }
            if (co_await sleep_for(worker_, std::chrono::milliseconds(25), stop_token_) ==
                timer_sleep_result::stop_requested) {
                break;
            }
        }
    } catch (...) {
        report_accept_failure(std::current_exception(), true);
    }
    // A removal waits for this loop to exit.
    configured.accepting_ = false;
    configured.removal_receipt_.reset();
}
#endif

void worker_connections::report_accept_failure(std::exception_ptr failure, bool fatal) noexcept {
    accept_failures_.fetch_add(1, std::memory_order_relaxed);
    if (fatal) {
#if !defined(_WIN32)
        close_listeners();
#endif
        failure_.notify(std::move(failure));
    } else {
        ruvia::report_unhandled_failure("web accept", std::move(failure));
    }
}

// Plain and TLS sessions are separate coroutines so a plain connection's frame
// carries none of the TLS stream, handshake and certificate state. Both keep
// the remote address outside their try so the failure report can name the
// peer; it stays empty if the failure happened before the address was resolved.
task<void> worker_connections::run_session(http_server_session_config& listener_value,
    accepted_connection_lease connection, std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix) {
    auto& socket = connection.socket();
    auto& lifecycle = connection.lifecycle();
    inbound_socket_stream input(socket, prefix, &lifecycle);
    std::string remote_address;
    std::uint16_t remote_port = 0;
    try {
        name_remote_endpoint(remote, remote_address, remote_port);
        if (!http_server_worker_running(state_) || !socket.is_open()) {
            co_return;
        }
        if (listener_value.connection_.has_value()) {
            co_await serve_connection(input, socket,
                server_connection_fields{.id_ = connection_id,
                    .local_address_ = {},
                    .local_port_ = 0,
                    .remote_address_ = remote_address,
                    .remote_port_ = remote_port,
                    .tls_ = std::nullopt,
                    .state_ = &lifecycle.state(),
                    .lifecycle_ = &lifecycle});
            ruvia::close_socket(socket);
            co_return;
        }
        const auto close_mode = co_await run_stream(listener_value, input, socket,
            capabilities_.make_context_services(stop_token_)
                .with_plain_transport(connection_id, remote_address, remote_port)
                .with_connection_lifecycle(lifecycle)
                .with_request_stream(listener_value.request_stream_ ? options_.request_stream_ : request_stream_callback_ref{}));
        // Plain TCP has no close alert: only a lingering close differs from an
        // immediate one. A worker stop closes at once; scanner close_all
        // reaches a lingering close that is already running.
        if (close_mode == http_connection_close::linger && !stop_token_.stop_requested()) {
            co_await linger_http_connection(socket, scanner_, worker_);
        }
        ruvia::close_socket(socket);
    } catch (...) {
        lifecycle.note_exception(std::current_exception());
        report_session_failure(socket, remote_address, std::current_exception());
    }
}

task<void> worker_connections::run_tls_session(http_server_session_config& listener_value,
    accepted_connection_lease connection, std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix) {
    auto& socket = connection.socket();
    auto& lifecycle = connection.lifecycle();
    inbound_socket_stream input(socket, prefix, &lifecycle);
    std::string remote_address;
    std::uint16_t remote_port = 0;
    try {
        name_remote_endpoint(remote, remote_address, remote_port);
        // A stopping worker admits no new request, and a handshake leads only to one.
        if (!http_server_worker_running(state_) || !socket.is_open()) {
            co_return;
        }
        connection_scanner::entry handshake_entry;
        handshake_entry.bind_lifecycle(&lifecycle);
        std::optional<connection_scanner::guard> handshake_guard;
        handshake_guard.emplace(scanner_, handshake_entry, connection_transport::tcp(socket));
        handshake_entry.read_request_head(connection_scanner::phase::reading_initial);
        if (listener_value.accepts_plaintext()) {
            // Peek one byte under the same initial-read clocks used by TLS.
            // The TLS handshake below does not receive a fresh deadline.
            unsigned char first = prefix.empty() ? 0 : std::to_integer<unsigned char>(prefix.front());
            if (prefix.empty()) {
                const auto completion = co_await ruvia::async_asio<std::size_t>([&socket, &first](auto handler) {
                    socket.async_receive(asio::buffer(&first, 1), asio::socket_base::message_peek, std::move(handler));
                });
                if (completion.error_code() || completion.result() == 0 || handshake_entry.close_requested()) {
                    lifecycle.note_error(completion.error_code());
                    co_return;
                }
            }
            if (first != 0x16) {
                handshake_guard.reset();
                co_await run_session(listener_value, std::move(connection), remote, connection_id, std::move(prefix));
                co_return;
            }
        }
        // Declared before the TLS stream: its SSL borrows them while it lives.
        // Identity selection matches the local address; the ClientHello
        // observer names it.
        std::error_code local_error;
        const auto local = socket.local_endpoint(local_error);
        const auto local_address = local.address();
        std::string local_address_text;
        std::optional<tls_client_hello_observation> client_hello;
        if (options_.tls_client_hello_ || tls_certificate_selection_enabled(listener_value)) {
            if (!local_error) {
                assign_peer_address(local_address_text, local_address);
            }
            client_hello.emplace(tls_client_hello_observation{.callback_ = options_.tls_client_hello_,
                .fields_ = {.connection_id_ = connection_id,
                    .local_address_ = local_address_text,
                    .local_port_ = local_error ? std::uint16_t{0} : local.port(),
                    .remote_address_ = remote_address,
                    .remote_port_ = remote_port,
                    .quic_ = false,
                    .message_ = {}}});
        }
        server_tls_stream tls_stream(input, *listener_value.tls_context_);
        if (!local_error) {
            bind_tls_local_address(*tls_stream.native_handle(), local_address);
        }
        if (client_hello.has_value()) {
            observe_tls_client_hello(*tls_stream.native_handle(), *client_hello);
        }
        const auto handshake_completion = co_await ruvia::async_asio([&tls_stream](auto handler) {
            tls_stream.async_handshake(asio::ssl::stream_base::server, std::move(handler));
        });
        if (handshake_completion.error_code() || handshake_entry.close_requested()) {
            lifecycle.note_error(handshake_completion.error_code());
            ruvia::close_socket(socket);
            co_return;
        }
        // Never retain the handshake clocks while dispatching HTTP requests.
        handshake_guard.reset();
        std::string client_certificate;
        std::optional<std::string_view> client_subject;
        if (listener_value.tls()->client_certificates_.has_value()) {
            client_subject = extract_tls_client_certificate(tls_stream.native_handle(), client_certificate);
        }
        auto& native_tls = *tls_stream.native_handle();
        const auto alpn = tls_selected_alpn(native_tls);
        const auto server_name = tls_server_name(native_tls);
        if (listener_value.connection_.has_value()) {
            co_await serve_connection(tls_stream, socket,
                server_connection_fields{.id_ = connection_id,
                    .local_address_ = {},
                    .local_port_ = 0,
                    .remote_address_ = remote_address,
                    .remote_port_ = remote_port,
                    .tls_ = server_connection_fields::tls_type{.client_certificate_subject_ = client_subject,
                        .alpn_ = alpn,
                        .server_name_ = server_name},
                    .state_ = &lifecycle.state(),
                    .lifecycle_ = &lifecycle});
            ruvia::close_socket(socket);
            co_return;
        }
        tls_served_certificate served_certificate(SSL_get_certificate(&native_tls));
        const auto services = capabilities_.make_context_services(stop_token_)
                                  .with_tls_transport(connection_id, remote_address, client_subject, alpn, server_name,
                                      remote_port, served_certificate)
                                  .with_connection_lifecycle(lifecycle)
                                  .with_alternative_service(listener_value.alternative_service_)
                                  .with_request_stream(listener_value.request_stream_ ? options_.request_stream_
                                                                                      : request_stream_callback_ref{});
#if defined(RUVIA_ENABLE_HTTP2)
        const auto close_mode = alpn == "h2"
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
        lifecycle.note_exception(std::current_exception());
        report_session_failure(socket, remote_address, std::current_exception());
    }
}

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

void worker_connections::adopt_connection(std::size_t index, native_socket native, adopt_options options) {
    struct native_owner final {
        native_socket socket_;
        ~native_owner() {
            close_native_socket(socket_);
        }
    } owner{native};
    tcp_socket_type socket(io_);
    if (!worker_.is_current() || index >= listeners_.size() || listeners_[index] == nullptr ||
        !listeners_[index]->admitting_ || !http_server_worker_running(state_)) {
        throw std::logic_error("connection service is not accepting on this worker");
    }
    std::error_code error;
    socket.assign(listeners_[index]->endpoint_.protocol(), native, error);
    if (error) {
        throw std::system_error(error, "failed to adopt an accepted connection");
    }
    owner.socket_ = invalid_native_socket;
    (void)socket.remote_endpoint(error);
    if (error) {
        throw std::invalid_argument("adopted connection must be a connected TCP socket");
    }
    accept_socket(index, std::move(socket), options);
}

bool worker_connections::close_connection(std::uint64_t id) noexcept {
    assert(worker_.is_current());
    return registry_.close_connection(id);
}

void worker_connections::accept_socket(std::size_t index, tcp_socket_type socket, adopt_options options) {
    if (options.prefix_.size() > options_.server_.max_inbound_buffer_bytes_per_connection_) {
        throw std::length_error("adopted prefix exceeds the connection input budget");
    }
    std::vector<std::byte> prefix(options.prefix_.begin(), options.prefix_.end());
    if (listeners_[index]->session_.proxy_protocol_) {
        listeners_[index]->tasks_.spawn(prepare_connection(index, std::move(socket), std::move(prefix), options.remote_));
    } else {
        const auto bytes_read = prefix.size();
        admit_socket(index, std::move(socket), options.remote_, std::move(prefix), bytes_read);
    }
}

task<void> worker_connections::prepare_connection(std::size_t index, tcp_socket_type socket,
    std::vector<std::byte> prefix, std::optional<ip_endpoint> remote) {
    try {
        operation_interrupt_source source;
        auto limit = options_.server_.header_completion_timeout_;
        if (const auto request = options_.server_.request_header_timeout_; request && (!limit || *request < *limit)) {
            limit = request;
        }
        operation_interrupt interrupt(source, worker_, {.timeout_ = operation_timeout(limit.value_or(std::chrono::seconds(30))), .stop_token_ = stop_token_}, &socket, [](void* raw) noexcept { ruvia::close_socket(*static_cast<tcp_socket_type*>(raw)); }, "PROXY protocol header");
        proxy_protocol_parser parser;
        std::array<std::byte, 1024> buffer;
        std::span<const std::byte> input = prefix;
        auto bytes_read = prefix.size();
        for (;;) {
            const auto parsed = parser.parse(input);
            if (parsed.complete_) {
                const auto& header = parser.header();
                if (header.transport() == proxy_protocol_transport::datagram) {
                    throw proxy_protocol_error("a TCP listener requires a stream PROXY transport");
                }
                if (header.source()) {
                    remote = header.source();
                }
                std::vector<std::byte> remaining(input.begin() + parsed.consumed_bytes_, input.end());
                interrupt.throw_if_interrupted();
                admit_socket(index, std::move(socket), remote, std::move(remaining), bytes_read);
                co_return;
            }
            const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                socket.async_read_some(asio::buffer(buffer), std::move(handler));
            });
            interrupt.throw_if_interrupted();
            if (completion.error_code() || completion.result() == 0) {
                co_return;
            }
            bytes_read += completion.result();
            input = std::span<const std::byte>(buffer.data(), completion.result());
        }
    } catch (const proxy_protocol_error&) {
        // Malformed mandatory prefaces are untrusted input, not host failures.
    } catch (const std::system_error&) {
        // EOF, timeout and shutdown close only this pending connection.
    } catch (...) {
        report_session_failure(socket, {}, std::current_exception());
    }
}

void worker_connections::admit_socket(std::size_t listener_index, tcp_socket_type socket,
    std::optional<ip_endpoint> effective_remote, std::vector<std::byte> prefix, std::size_t bytes_read) {
    assert(worker_.is_current());
    if (!http_server_worker_running(state_)) {
        return;
    }

    // Read the peer before the socket moves into the lease: a failure to
    // start the session destroys the lease, and its socket, with it.
    std::optional<asio::ip::tcp::endpoint> remote;
    std::error_code remote_error;
    if (auto endpoint = socket.remote_endpoint(remote_error); !remote_error) {
        remote = endpoint;
    }
    if (effective_remote) {
        remote.emplace(to_asio_address(effective_remote->address()), effective_remote->port());
    }
    try {
        configure_accepted_socket(socket);
        std::error_code local_error;
        const auto local = socket.local_endpoint(local_error);
        if (local_error || !remote.has_value()) {
            return;
        }
        const auto connection_id = connection_ids_.next();
        connection_event event(connection_id, {to_ip_address(local.address()), local.port()},
            {to_ip_address(remote->address()), remote->port()}, false);
        if (options_.connection_admission_ && !options_.connection_admission_(event)) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto& max_connections = options_.server_.max_connections_per_worker_;
        // Rejected peers must not evict an existing reusable connection.
        if (max_connections.has_value() && connection_count_.active() >= *max_connections &&
            !scanner_.evict_oldest_reusable()) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        connection_lifecycle lifecycle(registry_, &listeners_[listener_index]->session_, std::move(event), options_.connection_open_, options_.connection_close_,
            0, connection_transport::tcp(socket));
        lifecycle.note_read(bytes_read);
        accepted_connection_lease connection(std::move(socket), gauges_[listener_index]->count_, std::move(lifecycle));
        // The session borrows its listener, whose scope joins it before the
        // listener retires.
        auto& configured = *listeners_[listener_index];
        auto& listener_value = configured.session_;
        configured.tasks_.spawn(listener_value.tls() != nullptr
                                    ? run_tls_session(listener_value, std::move(connection), remote, connection_id, std::move(prefix))
                                    : run_session(listener_value, std::move(connection), remote, connection_id, std::move(prefix)));
    } catch (...) {
        auto failure = std::current_exception();
        std::string remote_address;
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

#if defined(_WIN32)

namespace ruvia::detail {
namespace {

// AcceptEx operations kept outstanding per listener: a burst of connections
// is taken without waiting for each completion to reissue the next; the
// listen backlog holds the rest. Product limit.
constexpr std::size_t accepts_per_listener = 8;
// AcceptEx reserves each address with 16 bytes of slack.
constexpr DWORD accept_address_bytes = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
// The pause after a failed accept, so a persistent resource error does not
// spin the acceptor.
constexpr std::chrono::milliseconds accept_retry_delay{25};
// The acceptor's queue holds the datagrams workers send; each worker keeps
// at most http3_max_datagrams_in_flight in it.
constexpr std::size_t minimum_queue_capacity = 64;

[[nodiscard]] std::error_code last_socket_error() noexcept {
    return {::WSAGetLastError(), asio::system_category()};
}

// No other socket, of this process or another, may bind the endpoint while
// the acceptor holds it.
template <typename socket_type>
void claim_exclusive_address(socket_type& socket) {
    const BOOL enabled = TRUE;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enabled),
            static_cast<int>(sizeof(enabled))) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to enable listener SO_EXCLUSIVEADDRUSE");
    }
}

[[nodiscard]] LPFN_ACCEPTEX resolve_accept_ex(SOCKET listener) {
    GUID identifier = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX accept_ex = nullptr;
    DWORD bytes = 0;
    if (::WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, static_cast<DWORD>(sizeof(identifier)),
            &accept_ex, static_cast<DWORD>(sizeof(accept_ex)), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to resolve listener AcceptEx");
    }
    return accept_ex;
}

#if defined(RUVIA_ENABLE_HTTP3)
template <typename function_type>
[[nodiscard]] function_type resolve_message_extension(SOCKET socket, GUID identifier) {
    function_type function = nullptr;
    DWORD bytes = 0;
    if (::WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, static_cast<DWORD>(sizeof(identifier)),
            &function, static_cast<DWORD>(sizeof(function)), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to resolve UDP message extension");
    }
    return function;
}

void enable_udp_packet_info(asio::ip::udp::socket& socket) {
    const DWORD enabled = 1;
    const auto native = socket.native_handle();
    const auto enable = [&](int level, int option) {
        return ::setsockopt(native, level, option, reinterpret_cast<const char*>(&enabled),
            static_cast<int>(sizeof(enabled)));
    };
    if (socket.local_endpoint().protocol() == asio::ip::udp::v6()) {
        if (enable(IPPROTO_IPV6, IPV6_PKTINFO) == SOCKET_ERROR) {
            throw std::system_error(last_socket_error(), "failed to enable IPv6 UDP packet information");
        }
        DWORD only_v6 = 0;
        int size = sizeof(only_v6);
        if (::getsockopt(native, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&only_v6), &size) == SOCKET_ERROR) {
            throw std::system_error(last_socket_error(), "failed to inspect UDP dual-stack policy");
        }
        if (only_v6 != 0) {
            return;
        }
    }
    if (enable(IPPROTO_IP, IP_PKTINFO) == SOCKET_ERROR) {
        const auto error = last_socket_error();
        // Winsock documents WSAEINVAL on dual-stack sockets when IPv4 is disabled.
        if (error.value() == WSAEINVAL && socket.local_endpoint().protocol() == asio::ip::udp::v6()) {
            const auto probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (probe == INVALID_SOCKET && ::WSAGetLastError() == WSAEAFNOSUPPORT) {
                return;
            }
            if (probe != INVALID_SOCKET) {
                (void)::closesocket(probe);
            }
        }
        throw std::system_error(error, "failed to enable IPv4 UDP packet information");
    }
}

struct udp_packet_destination final {
    asio::ip::udp::endpoint local_;
    std::uint32_t interface_index_;
};

[[nodiscard]] std::optional<udp_packet_destination> packet_destination(
    WSAMSG& message, const asio::ip::udp::endpoint& remote, std::uint16_t port) {
    if ((message.dwFlags & MSG_CTRUNC) != 0) {
        return std::nullopt;
    }
    for (auto* header = WSA_CMSG_FIRSTHDR(&message); header != nullptr; header = WSA_CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO &&
            header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
            IN_PKTINFO info{};
            std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
            asio::ip::address_v4::bytes_type bytes{};
            std::memcpy(bytes.data(), &info.ipi_addr, bytes.size());
            asio::ip::address address{asio::ip::address_v4(bytes)};
            if (remote.address().is_v6()) {
                address = asio::ip::make_address_v6(asio::ip::v4_mapped, address.to_v4());
            }
            return udp_packet_destination{{address, port}, info.ipi_ifindex};
        }
        if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO &&
            header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
            IN6_PKTINFO info{};
            std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
            asio::ip::address_v6::bytes_type bytes{};
            std::memcpy(bytes.data(), &info.ipi6_addr, bytes.size());
            asio::ip::address_v6 address(bytes);
            if (address.is_link_local() || address.is_multicast_link_local()) {
                address.scope_id(info.ipi6_ifindex);
            }
            return udp_packet_destination{{address, port}, info.ipi6_ifindex};
        }
    }
    // Never invent the destination from a wildcard socket's bind address.
    return std::nullopt;
}
#endif

[[nodiscard]] std::size_t acceptor_queue_capacity(std::size_t worker_count) {
#if defined(RUVIA_ENABLE_HTTP3)
    constexpr auto max_workers =
        ((std::numeric_limits<std::size_t>::max)() - minimum_queue_capacity) / http3_max_datagrams_in_flight;
    if (worker_count > max_workers) {
        return (std::numeric_limits<std::size_t>::max)();
    }
    return minimum_queue_capacity + worker_count * http3_max_datagrams_in_flight;
#else
    (void)worker_count;
    return minimum_queue_capacity;
#endif
}

}  // namespace

windows_accepted_socket::~windows_accepted_socket() {
    if (handle_ != INVALID_SOCKET) {
        (void)::closesocket(handle_);
    }
}

struct windows_acceptor::worker_slot final {
    business_worker target_;
#if defined(RUVIA_ENABLE_HTTP3)
    // Datagrams this worker queued for sending that the acceptor has not
    // sent yet.
    std::atomic<std::size_t> egress_in_flight_{0};
#endif
};

struct windows_acceptor::tcp_listener final {
    tcp_listener(asio::io_context& io, asio::ip::tcp::endpoint endpoint, std::size_t index,
        std::optional<std::size_t> http3_index, std::shared_ptr<inherited_listener_sockets> inherited, std::optional<bool> ipv6_only)
        : socket_(io),
          endpoint_(endpoint),
          ipv6_only_(ipv6_only),
          index_(index),
          http3_index_(http3_index),
          inherited_(std::move(inherited)) {}

    asio::ip::tcp::acceptor socket_;
    asio::ip::tcp::endpoint endpoint_;
    std::optional<bool> ipv6_only_;
    // The listener's index in every business worker.
    std::size_t index_;
    // Its HTTP/3 service's index among udp_listeners_.
    std::optional<std::size_t> http3_index_;
    LPFN_ACCEPTEX accept_ex_{nullptr};
    // Until opening claims its socket.
    std::shared_ptr<inherited_listener_sockets> inherited_;
    // run_accepts() operations of this listener; a removal holds its
    // receipt until none is left.
    std::size_t accepts_{0};
    std::shared_ptr<void> removal_receipt_;
    std::optional<worker_signal> activated_;
    // From opening until removal or the acceptor's stop.
    bool admitting_{false};
};

#if defined(RUVIA_ENABLE_HTTP3)
struct windows_acceptor::udp_transport final {
    explicit udp_transport(asio::io_context& io)
        : socket_(io) {}

    asio::ip::udp::socket socket_;
    std::size_t listeners_{0};
    std::vector<std::byte> receive_buffer_{http3_max_udp_payload_bytes};
    bool receiving_{false};
    LPFN_WSARECVMSG receive_message_{nullptr};
    LPFN_WSASENDMSG send_message_{nullptr};
};

struct windows_acceptor::udp_listener final {
    udp_listener([[maybe_unused]] asio::io_context& io, asio::ip::udp::endpoint endpoint, std::shared_ptr<const http3_secret> secret,
        std::size_t index, std::shared_ptr<inherited_listener_sockets> inherited, std::optional<bool> ipv6_only)
        : endpoint_(endpoint),
          ipv6_only_(ipv6_only),
          secret_(std::move(secret)),
          index_(index),
          inherited_(std::move(inherited)) {}

    ~udp_listener() {
        release_transport();
    }

    void release_transport() noexcept {
        if (transport_ != nullptr && !std::exchange(transport_released_, true) && --transport_->listeners_ == 0) {
            std::error_code ignored;
            transport_->socket_.close(ignored);
        }
    }

    std::shared_ptr<udp_transport> transport_;
    asio::ip::udp::endpoint endpoint_;
    std::optional<bool> ipv6_only_;
    std::shared_ptr<const http3_secret> secret_;
    // The HTTP/3 endpoint's index in every business worker.
    std::size_t index_;
    // Until opening claims its socket.
    std::shared_ptr<inherited_listener_sockets> inherited_;
    // The last worker releases this logical listener's transport claim.
    // A re-added listener may still need the same exclusive UDP socket.
    std::size_t released_{0};
    bool transport_released_{false};
    std::optional<worker_signal> activated_;
    std::shared_ptr<void> removal_receipt_;
    bool receiving_{false};
};
#endif

windows_acceptor::windows_acceptor(std::span<const http_server_listener_definition> listeners,
    std::size_t worker_count,
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_directory* directory,
#endif
    worker_failure_sink failure)
    : runtime_({.queue_capacity_ = acceptor_queue_capacity(worker_count),
          .io_policy_ = worker_io_policy::single_owner}),
      io_(runtime_.io_context()),
      failure_sink_(failure),
      finalize_signal_(runtime_.handle()),
      tasks_(runtime_.handle()),
      workers_(),
      tcp_listeners_()
#if defined(RUVIA_ENABLE_HTTP3)
      ,
      directory_(directory),
      udp_listeners_()
#endif
{
    if (worker_count == 0) {
        throw std::invalid_argument("the Windows acceptor requires business workers");
    }
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.push_back(std::make_unique<worker_slot>());
    }
    tcp_listeners_.reserve(listeners.size());
    for (std::size_t index = 0; index < listeners.size(); ++index) {
        const auto& definition = listeners[index];
        std::optional<std::size_t> http3_index;
#if defined(RUVIA_ENABLE_HTTP3)
        if (definition.http3_.has_value()) {
            if (directory_ == nullptr || definition.http3_->secret_ == nullptr) {
                throw std::logic_error("Windows HTTP/3 acceptor requires the worker directory and a secret");
            }
            http3_index = udp_listeners_.size();
            udp_listeners_.push_back(std::make_unique<udp_listener>(io_, asio::ip::udp::endpoint(definition.endpoint_.address(), definition.http3_->port_), definition.http3_->secret_, *http3_index, definition.inherited_sockets_, definition.ipv6_only_));
        }
#endif
        tcp_listeners_.push_back(std::make_unique<tcp_listener>(io_, definition.endpoint_, index, http3_index, definition.inherited_sockets_, definition.ipv6_only_));
    }
    runtime_.configure({
        .startup_ = [this] {
            // Startup runs before the context does; the task starts in its first turn.
            asio::post(io_, [this] {
                ruvia::async_start(run(),
                    asio::bind_executor(io_, [this](std::exception_ptr failure) noexcept {
                                                     if (failure) {
                                                         (void)runtime_.fail(failure);
                                                     }
                                                 }));
            }); },
        .serve_ = [this] { serve_on_context(); complete_phase(false); },
        .stop_admission_ = [this] {
            stopping_ = true;
            close_admission();
#if defined(RUVIA_ENABLE_HTTP3)
            for (auto& listener : udp_listeners_) {
                if (listener != nullptr && listener->transport_ != nullptr) {
                    std::error_code ignored;
                    listener->transport_->socket_.close(ignored);
                }
            }
#endif
            runtime_.finalize(); },
        .finalize_ = [this] { finalize_signal_.notify(); },
        .failure_ = [this](std::exception_ptr failure) noexcept {
            complete_phase(true, failure);
            complete_phase(false, failure);
            failure_sink_.notify(failure); },
        .shutdown_ = [this] {
            try { throw std::logic_error("HTTP server acceptor stopped before its phase completed"); }
            catch (...) {
                const auto failure = std::current_exception();
                complete_phase(true, failure);
                complete_phase(false, failure);
            } },
    });
}

windows_acceptor::~windows_acceptor() {
    runtime_.request_stop();
    runtime_.finalize();
    runtime_.join();
}

void windows_acceptor::bind_worker(std::size_t index, business_worker target) {
    if (index >= workers_.size() || !target.worker_.valid() || target.load_ == nullptr || target.target_ == nullptr ||
        target.adopt_ == nullptr) {
        throw std::invalid_argument("invalid Windows acceptor worker");
    }
    workers_[index]->target_ = target;
}

bool windows_acceptor::start() {
    for (const auto& slot : workers_) {
        if (slot->target_.adopt_ == nullptr) {
            throw std::logic_error("Windows acceptor worker was not bound");
        }
    }
    return runtime_.start() && runtime_.wait_until_ready();
}

bool windows_acceptor::serve() {
    runtime_.request_serve();
    return runtime_.wait_until_serving();
}

void windows_acceptor::complete_phase(bool startup, std::exception_ptr failure) noexcept {
    std::optional<runtime_control_completion::receipt> receipt;
    {
        std::lock_guard lock(phase_mutex_);
        auto& pending = startup ? startup_receipt_ : serve_receipt_;
        receipt.swap(pending);
    }
    if (receipt) {
        receipt->complete(std::move(failure));
    }
}

task<void> windows_acceptor::start(worker_ref caller) {
    runtime_control_completion completion(caller);
    {
        std::lock_guard lock(phase_mutex_);
        if (startup_receipt_) {
            throw std::logic_error("HTTP server acceptor is already starting");
        }
        startup_receipt_.emplace(completion.make_receipt());
    }
    try {
        for (const auto& slot : workers_) {
            if (!slot->target_.adopt_) {
                throw std::logic_error("Windows acceptor worker was not bound");
            }
        }
        if (!runtime_.start()) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
    } catch (...) {
        complete_phase(true, std::current_exception());
    }
    co_await completion.wait();
    if (const auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
}

task<void> windows_acceptor::serve(worker_ref caller) {
    runtime_control_completion completion(caller);
    {
        std::lock_guard lock(phase_mutex_);
        if (serve_receipt_) {
            throw std::logic_error("HTTP server acceptor is already opening");
        }
        serve_receipt_.emplace(completion.make_receipt());
    }
    try {
        runtime_.request_serve();
        if (runtime_.state() != worker_runtime_state::running) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
    } catch (...) {
        complete_phase(false, std::current_exception());
    }
    co_await completion.wait();
    if (const auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
}

task<void> windows_acceptor::join(worker_ref caller, asio::io_context& caller_io) {
    if (!caller.is_current()) {
        throw std::logic_error("acceptor join must run on its caller worker");
    }
    const auto native = co_await runtime_.prepare_thread_join(caller);
    if (native != 0) {
        std::unique_ptr<void, decltype(&::CloseHandle)> native_owner(reinterpret_cast<HANDLE>(native), &::CloseHandle);
        asio::windows::object_handle thread(caller_io);
        std::error_code error;
        thread.assign(native_owner.get(), error);
        if (error) {
            throw std::system_error(error, "failed to assign acceptor thread wait handle");
        }
        (void)native_owner.release();
        const auto waited = co_await ruvia::async_asio([&thread](auto handler) { thread.async_wait(std::move(handler)); });
        if (waited.failed()) {
            throw std::system_error(waited.error_code(), "failed to await acceptor thread exit");
        }
    }
    runtime_.join();
}

void windows_acceptor::stop_admission() noexcept {
    // A refused control means the acceptor is already stopping, which closes
    // admission itself.
    (void)runtime_.post_control([this] { close_admission(); });
}

void windows_acceptor::request_stop() noexcept {
    runtime_.request_stop();
}

void windows_acceptor::join() {
    runtime_.join();
}

std::exception_ptr windows_acceptor::failure() noexcept {
    return runtime_.take_failure();
}

task<void> windows_acceptor::run() {
    try {
        // A stop applied before this task first runs binds nothing.
        if (!stopping_) {
            open_listeners();
            // Ignored once stopping: core has already aborted the barrier.
            runtime_.mark_ready();
            complete_phase(true);
        }
    } catch (...) {
        if (!stopping_) {
            (void)runtime_.fail(std::current_exception());
        }
    }
    co_await finalize_signal_.wait();
    try {
        co_await tasks_.join();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

void windows_acceptor::open_listeners() {
    for (auto& listener : tcp_listeners_) {
        open_tcp_listener(*listener);
    }
#if defined(RUVIA_ENABLE_HTTP3)
    for (auto& listener : udp_listeners_) {
        open_udp_listener(*listener);
    }
#endif
    for (auto& listener : tcp_listeners_) {
        prepare_listener_tasks(*listener);
    }
}

void windows_acceptor::open_tcp_listener(tcp_listener& listener) {
    auto& socket = listener.socket_;
    const auto inherited = listener.inherited_ != nullptr ? listener.inherited_->claim_tcp(0) : invalid_native_socket;
    listener.inherited_.reset();
    if (inherited != invalid_native_socket) {
        // Checked to listen on this endpoint when it was handed over; its
        // first association with an I/O completion port is this one.
        std::error_code error;
        socket.assign(listener.endpoint_.protocol(), static_cast<SOCKET>(inherited), error);
        if (error) {
            close_native_socket(inherited);
            throw std::system_error(error, "failed to adopt an inherited TCP listener socket");
        }
    } else {
        socket.open(listener.endpoint_.protocol());
        claim_exclusive_address(socket);
        if (listener.ipv6_only_) {
            socket.set_option(asio::ip::v6_only(*listener.ipv6_only_));
        }
        socket.bind(listener.endpoint_);
        socket.listen(asio::socket_base::max_listen_connections);
    }
    listener.accept_ex_ = resolve_accept_ex(socket.native_handle());
}

#if defined(RUVIA_ENABLE_HTTP3)
void windows_acceptor::open_udp_listener(udp_listener& listener) {
    if (listener.inherited_ == nullptr || listener.inherited_->udp().empty()) {
        for (const auto& existing : udp_listeners_) {
            if (existing != nullptr && existing->endpoint_ == listener.endpoint_ &&
                existing->transport_ != nullptr && existing->transport_->socket_.is_open()) {
                if (listener.ipv6_only_ && existing->transport_->socket_.local_endpoint().address().is_v6()) {
                    asio::ip::v6_only value;
                    existing->transport_->socket_.get_option(value);
                    if (value.value() != *listener.ipv6_only_) {
                        throw std::invalid_argument("shared HTTP/3 socket IPV6_V6ONLY does not match");
                    }
                }
                listener.transport_ = existing->transport_;
                ++listener.transport_->listeners_;
                listener.inherited_.reset();
                return;
            }
        }
    }
    auto transport = std::make_shared<udp_transport>(io_);
    auto& socket = transport->socket_;
    const auto inherited = listener.inherited_ != nullptr ? listener.inherited_->claim_udp()
                                                          : std::vector<native_socket>();
    listener.inherited_.reset();
    if (!inherited.empty()) {
        // listen() admitted exactly one, bound to this endpoint.
        std::error_code error;
        socket.assign(listener.endpoint_.protocol(), static_cast<SOCKET>(inherited.front()), error);
        if (error) {
            close_native_socket(inherited.front());
            throw std::system_error(error, "failed to adopt an inherited UDP listener socket");
        }
    } else {
        socket.open(listener.endpoint_.protocol());
        claim_exclusive_address(socket);
        if (listener.ipv6_only_) {
            socket.set_option(asio::ip::v6_only(*listener.ipv6_only_));
        }
        socket.bind(listener.endpoint_);
    }
    enable_udp_packet_info(socket);
    transport->receive_message_ = resolve_message_extension<LPFN_WSARECVMSG>(socket.native_handle(), WSAID_WSARECVMSG);
    transport->send_message_ = resolve_message_extension<LPFN_WSASENDMSG>(socket.native_handle(), WSAID_WSASENDMSG);
    // Sending runs on this thread for every worker: a full send buffer
    // loses the datagram instead of blocking the acceptor.
    socket.non_blocking(true);
    ++transport->listeners_;
    listener.transport_ = std::move(transport);
}
#endif

void windows_acceptor::serve_on_context() {
    if (stopping_) {
        return;
    }
    admitting_ = true;
    for (auto& listener : tcp_listeners_) {
        start_listener(*listener);
    }
}

void windows_acceptor::prepare_listener_tasks(tcp_listener& listener) {
    listener.activated_.emplace(runtime_.handle());
    for (std::size_t operation = 0; operation < accepts_per_listener; ++operation) {
        tasks_.spawn(run_accepts(listener));
    }
#if defined(RUVIA_ENABLE_HTTP3)
    if (listener.http3_index_.has_value()) {
        auto& udp = *udp_listeners_[*listener.http3_index_];
        if (!udp.transport_->receiving_) {
            udp.activated_.emplace(runtime_.handle());
            tasks_.spawn(run_receives(udp));
            udp.transport_->receiving_ = true;
            udp.receiving_ = true;
        }
    }
#endif
}

void windows_acceptor::start_listener(tcp_listener& listener) noexcept {
    listener.admitting_ = true;
    listener.activated_->notify();
#if defined(RUVIA_ENABLE_HTTP3)
    if (listener.http3_index_) {
        auto& udp = *udp_listeners_[*listener.http3_index_];
        if (udp.activated_) {
            udp.activated_->notify();
        }
    }
#endif
}

void windows_acceptor::prepare_listener(std::size_t index, [[maybe_unused]] std::optional<std::size_t> http3_index,
    const http_server_listener_definition& definition
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_directory* directory
#endif
) {
    if (stopping_) {
        throw std::logic_error("the Windows acceptor is stopping");
    }
    if (tcp_listeners_.size() <= index) {
        tcp_listeners_.resize(index + 1);
    }
    auto tcp = std::make_unique<tcp_listener>(io_, definition.endpoint_, index, http3_index, definition.inherited_sockets_, definition.ipv6_only_);
#if defined(RUVIA_ENABLE_HTTP3)
    std::unique_ptr<udp_listener> udp(
        nullptr, pmr_object_deleter<udp_listener>{});
    if (http3_index.has_value()) {
        if (udp_listeners_.size() <= *http3_index) {
            udp_listeners_.resize(*http3_index + 1);
        }
        // The application creates the directory with its first HTTP/3 listener.
        if (directory_ == nullptr) {
            directory_ = directory;
        }
        udp = std::make_unique<udp_listener>(io_, asio::ip::udp::endpoint(definition.endpoint_.address(), definition.http3_->port_), definition.http3_->secret_, *http3_index, definition.inherited_sockets_, definition.ipv6_only_);
    }
#endif
    open_tcp_listener(*tcp);
#if defined(RUVIA_ENABLE_HTTP3)
    if (udp != nullptr) {
        open_udp_listener(*udp);
        udp_listeners_[*http3_index] = std::move(udp);
    }
#endif
    tcp_listeners_[index] = std::move(tcp);
    try {
        prepare_listener_tasks(*tcp_listeners_[index]);
    } catch (...) {
        discard_listener(index);
        throw;
    }
}

void windows_acceptor::open_listener(std::size_t index) noexcept {
    if (!admitting_ || index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    start_listener(*tcp_listeners_[index]);
}

void windows_acceptor::discard_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
    if (index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    remove_listener(index, receipt);
#if defined(RUVIA_ENABLE_HTTP3)
    if (const auto http3_index = tcp_listeners_[index]->http3_index_; http3_index.has_value()) {
        auto& udp = *udp_listeners_[*http3_index];
        if (udp.receiving_) {
            udp.removal_receipt_ = std::move(receipt);
        }
        udp.release_transport();
        if (udp.activated_) {
            udp.activated_->notify();
        }
    }
#endif
}

void windows_acceptor::remove_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
    if (index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    auto& listener = *tcp_listeners_[index];
    listener.admitting_ = false;
    if (listener.activated_) {
        listener.activated_->notify();
    }
    // Completes its pending AcceptEx operations as aborted.
    std::error_code ignored;
    listener.socket_.close(ignored);
    if (listener.accepts_ != 0) {
        listener.removal_receipt_ = std::move(receipt);
    }
}

void windows_acceptor::close_admission() noexcept {
    admitting_ = false;
    // Closing a listener completes its pending AcceptEx operations as aborted.
    for (auto& listener : tcp_listeners_) {
        if (listener != nullptr) {
            listener->admitting_ = false;
            if (listener->activated_) {
                listener->activated_->notify();
            }
            std::error_code ignored;
            listener->socket_.close(ignored);
        }
    }
#if defined(RUVIA_ENABLE_HTTP3)
    // The sockets stay open for sending until request_stop().
    for (auto& listener : udp_listeners_) {
        if (listener != nullptr && listener->transport_ != nullptr) {
            std::error_code ignored;
            if (listener->activated_) {
                listener->activated_->notify();
            }
            listener->transport_->socket_.cancel(ignored);
        }
    }
#endif
}

task<void> windows_acceptor::run_accepts(tcp_listener& listener) {
    ++listener.accepts_;
    co_await listener.activated_->wait();
    // AcceptEx writes both addresses here; the frame outlives the operation.
    std::array<std::byte, 2 * accept_address_bytes> addresses{};
    const auto protocol = listener.endpoint_.protocol();
    try {
        while (admitting_ && listener.admitting_) {
            // Not associated with any completion port: the business worker
            // that adopts it binds it to its own.
            windows_accepted_socket accepted(::WSASocketW(protocol.family(), protocol.type(), protocol.protocol(),
                nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
            std::error_code error;
            if (accepted.get() == INVALID_SOCKET) {
                error = last_socket_error();
            } else {
                const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::windows::overlapped_ptr native(io_, std::move(handler));
                    DWORD bytes = 0;
                    const BOOL accepted_now = listener.accept_ex_(listener.socket_.native_handle(), accepted.get(),
                        addresses.data(), 0, accept_address_bytes, accept_address_bytes, &bytes, native.get());
                    const int accept_error = accepted_now ? 0 : ::WSAGetLastError();
                    if (accepted_now || accept_error == ERROR_IO_PENDING) {
                        // Even a synchronous success posts its completion packet.
                        (void)native.release();
                    } else {
                        native.complete(asio::error_code(accept_error, asio::system_category()), 0);
                    }
                });
                error = completion.error_code();
            }
            // A stop, or this listener's removal, aborted the accept.
            if (!admitting_ || !listener.admitting_) {
                break;
            }
            if (!error) {
                // Gives the socket the listener's properties (getpeername,
                // shutdown); no completion port is involved.
                const SOCKET listening = listener.socket_.native_handle();
                if (::setsockopt(accepted.get(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                        reinterpret_cast<const char*>(&listening), static_cast<int>(sizeof(listening))) == SOCKET_ERROR) {
                    error = last_socket_error();
                } else {
                    dispatch(listener.index_, std::move(accepted));
                    continue;
                }
            }
            report_accept_error(error);
            if (co_await ruvia::sleep_for(runtime_.handle(), accept_retry_delay) == timer_sleep_result::stop_requested) {
                break;
            }
        }
    } catch (...) {
        // A fatal listener failure stops the application.
        (void)runtime_.fail(std::current_exception());
    }
    // A removal waits until every accept of the listener ended.
    if (--listener.accepts_ == 0) {
        listener.removal_receipt_.reset();
    }
}

void windows_acceptor::report_accept_error(const std::error_code& error) {
    const auto action = classify_accept_error(error);
    if (action == accept_error_action::retry) {
        return;
    }
    accept_failures_.fetch_add(1, std::memory_order_relaxed);
    if (action == accept_error_action::fail) {
        throw std::system_error(error, "fatal TCP AcceptEx error");
    }
    try {
        throw std::system_error(error, "recoverable TCP AcceptEx error");
    } catch (...) {
        ruvia::report_unhandled_failure("web accept", std::current_exception());
    }
}

void windows_acceptor::dispatch(std::size_t listener_index, windows_accepted_socket accepted) {
    const auto& target = workers_[select_worker()]->target_;
    // Admission is reserved before the callable takes the socket; a refused
    // socket stays here and closes with `accepted`.
    const auto status = target.worker_.post_factory([&]() -> move_only_function<void()> {
        return [adopt = target.adopt_, owner = target.target_, listener_index, owned = std::move(accepted)]() mutable {
            adopt(owner, listener_index, std::move(owned));
        };
    });
    if (status == post_status::queue_full) {
        refused_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t windows_acceptor::select_worker() noexcept {
    // The worker holding the fewest connections; scanning starts after the
    // last choice, so ties (a burst) rotate across workers.
    const auto count = workers_.size();
    auto chosen = next_worker_;
    auto least = workers_[chosen]->target_.load_->active();
    for (std::size_t step = 1; step < count; ++step) {
        const auto index = (next_worker_ + step) % count;
        const auto load = workers_[index]->target_.load_->active();
        if (load < least) {
            least = load;
            chosen = index;
        }
    }
    next_worker_ = (chosen + 1) % count;
    return chosen;
}

#if defined(RUVIA_ENABLE_HTTP3)
task<void> windows_acceptor::run_receives(udp_listener& listener) {
    // A removed logical listener can release its claim while another listener
    // at this address continues receiving on the same exclusive socket.
    const auto transport = listener.transport_;
    auto& socket = transport->socket_;
    auto& buffer = transport->receive_buffer_;
    struct receipt_release final {
        udp_listener& listener_;
        ~receipt_release() {
            listener_.receiving_ = false;
            listener_.removal_receipt_.reset();
        }
    } release{listener};
    co_await listener.activated_->wait();
    try {
        while (admitting_ && socket.is_open()) {
            asio::ip::udp::endpoint remote;
            alignas(WSACMSGHDR) std::array<char,
                WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) + WSA_CMSG_SPACE(sizeof(IN_PKTINFO))>
                control{};
            WSABUF payload{};
            payload.buf = reinterpret_cast<char*>(buffer.data());
            payload.len = static_cast<ULONG>(buffer.size());
            WSAMSG message{};
            message.name = remote.data();
            message.namelen = static_cast<int>(remote.capacity());
            message.lpBuffers = &payload;
            message.dwBufferCount = 1;
            message.Control.buf = control.data();
            message.Control.len = static_cast<ULONG>(control.size());
            const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::windows::overlapped_ptr native(io_, std::move(handler));
                DWORD received = 0;
                const int result = transport->receive_message_(
                    socket.native_handle(), &message, &received, native.get(), nullptr);
                const int error = result == 0 ? 0 : ::WSAGetLastError();
                if (result == 0 || error == WSA_IO_PENDING) {
                    (void)native.release();
                } else {
                    native.complete(asio::error_code(error, asio::system_category()), 0);
                }
            });
            // A stop, or the last worker's release of a removed listener.
            if (!admitting_ || !socket.is_open()) {
                co_return;
            }
            if (const auto error = completion.error_code()) {
                if (http3_ignorable_receive_error(error)) {
                    continue;
                }
                throw std::system_error(error, "HTTP/3 UDP receive failed");
            }
            remote.resize(static_cast<std::size_t>(message.namelen));
            const auto destination = packet_destination(message, remote, listener.endpoint_.port());
            if (destination) {
                steer(listener, std::span<const std::byte>(buffer.data(), completion.result()),
                    remote, destination->local_, destination->interface_index_);
            }
        }
    } catch (...) {
        // The listener cannot receive any more: its connections time out and
        // the failure is reported once.
        ruvia::report_unhandled_failure("HTTP/3 receive", std::current_exception());
    }
}

void windows_acceptor::steer(
    const udp_listener& listener, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    ngtcp2_version_cid header{};
    const auto decoded = ngtcp2_pkt_decode_version_cid(
        &header, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), http3_connection_id_length);
    if (decoded != 0 && decoded != NGTCP2_ERR_VERSION_NEGOTIATION) {
        return;
    }
    const auto destination = std::span<const std::uint8_t>(header.dcid, header.dcidlen);
    std::optional<std::size_t> owner;
    std::size_t listener_index = listener.index_;
    const udp_listener* initial_listener = &listener;
    for (const auto& candidate : udp_listeners_) {
        if (candidate != nullptr && candidate->transport_ == listener.transport_) {
            if (!candidate->transport_released_) {
                initial_listener = candidate.get();
            }
            if (const auto identified = http3_connection_id_owner(*candidate->secret_, destination, workers_.size())) {
                owner = identified;
                listener_index = candidate->index_;
                break;
            }
        }
    }
    if (!owner.has_value()) {
        // Only a long header can open a connection or earn Version
        // Negotiation; a short header naming no worker is dropped.
        if (header.version == 0) {
            return;
        }
        listener_index = initial_listener->index_;
        owner = http3_initial_worker(*initial_listener->secret_, destination, workers_.size());
    }
    (void)directory_->hand_off(*owner, listener_index, bytes, remote, local, interface_index);
}

bool windows_acceptor::transmit(std::size_t worker_index, std::size_t http3_listener_index,
    const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    auto token = http3_in_flight_token::reserve(workers_[worker_index]->egress_in_flight_);
    if (!token) {
        return false;
    }
    try {
        const auto status = runtime_.handle().post_factory([&]() -> move_only_function<void()> {
            std::vector<std::byte> owned(bytes.begin(), bytes.end());
            return [this, http3_listener_index, remote, local, interface_index, owned = std::move(owned),
                       token = std::move(token)]() mutable noexcept {
                send_datagram(http3_listener_index, remote, owned, local, interface_index);
            };
        });
        return status == post_status::accepted;
    } catch (...) {
        return false;
    }
}

void windows_acceptor::send_datagram(std::size_t http3_listener_index, const asio::ip::udp::endpoint& remote,
    std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local,
    std::uint32_t interface_index) noexcept {
    // Errors (a full send buffer, a closed socket, an unreachable peer) lose
    // the datagram, as the network may; QUIC recovery handles the peer.
    if (const auto& listener = udp_listeners_[http3_listener_index]; listener != nullptr && listener->transport_ != nullptr) {
        auto& transport = *listener->transport_;
        if (!transport.socket_.is_open() || local.address().is_unspecified()) {
            return;
        }
        auto destination = remote;
        WSABUF payload{};
        payload.buf = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
        payload.len = static_cast<ULONG>(bytes.size());
        alignas(WSACMSGHDR) std::array<char, WSA_CMSG_SPACE(sizeof(IN6_PKTINFO))> control{};
        WSAMSG message{};
        message.name = destination.data();
        message.namelen = static_cast<int>(destination.size());
        message.lpBuffers = &payload;
        message.dwBufferCount = 1;
        message.Control.buf = control.data();
        const auto source = local.address();
        const bool ipv4 = remote.address().is_v4() ||
                          (remote.address().is_v6() && remote.address().to_v6().is_v4_mapped());
        auto* header = reinterpret_cast<WSACMSGHDR*>(control.data());
        if (ipv4) {
            asio::ip::address_v4::bytes_type address{};
            if (source.is_v4()) {
                address = source.to_v4().to_bytes();
            } else if (source.to_v6().is_v4_mapped()) {
                const auto mapped = source.to_v6().to_bytes();
                std::memcpy(address.data(), mapped.data() + 12, address.size());
            } else {
                return;
            }
            IN_PKTINFO info{};
            std::memcpy(&info.ipi_addr, address.data(), address.size());
            info.ipi_ifindex = interface_index;
            message.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(info)));
            header->cmsg_level = IPPROTO_IP;
            header->cmsg_type = IP_PKTINFO;
            header->cmsg_len = WSA_CMSG_LEN(sizeof(info));
            std::memcpy(WSA_CMSG_DATA(header), &info, sizeof(info));
        } else {
            if (!source.is_v6()) {
                return;
            }
            IN6_PKTINFO info{};
            const auto address = source.to_v6().to_bytes();
            std::memcpy(&info.ipi6_addr, address.data(), address.size());
            info.ipi6_ifindex = interface_index == 0
                                    ? static_cast<ULONG>(source.to_v6().scope_id())
                                    : interface_index;
            message.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(info)));
            header->cmsg_level = IPPROTO_IPV6;
            header->cmsg_type = IPV6_PKTINFO;
            header->cmsg_len = WSA_CMSG_LEN(sizeof(info));
            std::memcpy(WSA_CMSG_DATA(header), &info, sizeof(info));
        }
        DWORD sent = 0;
        (void)transport.send_message_(transport.socket_.native_handle(), &message, 0, &sent, nullptr, nullptr);
    }
}

void windows_acceptor::release_http3_listener(std::size_t http3_listener_index) noexcept {
    // A refused control means the acceptor is stopping, which closes every
    // socket itself.
    (void)runtime_.post_control([this, http3_listener_index] {
        auto& listener = *udp_listeners_[http3_listener_index];
        if (++listener.released_ == workers_.size()) {
            listener.release_transport();
        }
    });
}
#endif

}  // namespace ruvia::detail

#endif
