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

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/http_server.h"

#include "connection_lifecycle.h"
#include "context_access.h"
#include "route_table.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include <ngtcp2/ngtcp2.h>
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "route_table.h"
#endif

#include <new>
#include <optional>
#include <span>

#include <asio/cancellation_signal.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/streaming.h"

#include "context_access.h"
#include "route_table.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_session.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
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

namespace ruvia::detail {
namespace {

// Reclaim read storage beyond the header limit once the pending bytes fit the
// initial size. Amortized growth and handed-over pipelines can both exceed it.
constexpr std::size_t read_buffer_shrink_capacity_bytes = max_http_header_bytes;
// Deferred access-log entries retained across coalesced writes.
constexpr std::size_t retained_deferred_access_entries = 64;

}  // namespace

// Reserve the initial capacity before exposing the active work set.
connection_read_buffer::connection_read_buffer() noexcept {
    storage_.reserve(initial_read_buffer_bytes);
}

std::span<char> connection_read_buffer::prepare_read() & {
    if (storage_.size() >= max_http_header_bytes) {
        return {};
    }
    const auto remaining = max_http_header_bytes - storage_.size();
    auto space = storage_.prepare_spare();
    if (space.empty()) {
        // Storage may outgrow the protocol window to amortize a short consumed
        // prefix; only the bytes still allowed in that window are writable.
        (void)storage_.prepare(std::min(initial_read_buffer_bytes, remaining));
        space = storage_.prepare_spare();
    }
    return space.first(std::min(space.size(), remaining));
}

void connection_read_buffer::consume(std::size_t bytes) noexcept {
    if (bytes > storage_.size()) {
        std::terminate();
    }
    storage_.consume(bytes);
}

void connection_read_buffer::assign(std::string_view bytes) {
    storage_.clear();
    storage_.append(bytes);
}

void connection_read_buffer::complete_request(const http1_request_buffer_completion& completion) {
    if (const auto* compaction = completion.compaction()) {
        consume(compaction->consumed_bytes());
        return;
    }
    if (const auto* restore = completion.pipeline_restore()) {
        assign(restore->pipeline());
        return;
    }
    // A discarded buffer is valid only when the connection plan closes. The
    // session must never reach reusable cleanup with that alternative.
    std::terminate();
}

void connection_read_buffer::trim() {
    if (storage_.capacity() > read_buffer_shrink_capacity_bytes && storage_.size() <= initial_read_buffer_bytes) {
        storage_.shrink_to(initial_read_buffer_bytes);
    }
}

http1_deferred_access_log::http1_deferred_access_log() noexcept = default;

void http1_deferred_access_log::defer(std::size_t head_end, const http_request& request,
    ip_address client_address, std::chrono::steady_clock::time_point start,
    http_status_code status, const request_io_metrics& io_metrics) {
    const auto method = request.method();
    const auto path = request.path();
    const auto text_begin = text_.size();
    const auto required_text = text_begin + method.size() + path.size();
    if (required_text > text_.capacity()) {
        text_.reserve(
            std::max<std::size_t>(required_text, std::max<std::size_t>(256, text_.capacity() * 2)));
    }
    text_.append(method);
    text_.append(path);
    if (entries_.size() == entries_.capacity()) {
        entries_.reserve(std::max<std::size_t>(8, entries_.capacity() * 2));
    }
    entries_.push_back({
        .head_end_ = head_end,
        .text_begin_ = text_begin,
        .method_bytes_ = method.size(),
        .path_bytes_ = path.size(),
        .client_address_ = client_address,
        .start_ = start,
        .io_metrics_ = io_metrics,
        .known_method_ = request.known_method(),
        .protocol_version_ = request.protocol_version(),
        .status_ = status,
    });
}

void http1_deferred_access_log::complete(
    const access_log_sink& access_log, ip_address remote_address, std::size_t written) noexcept {
    const std::string_view text = text_;
    for (const auto& entry : entries_) {
        if (entry.head_end_ > written) {
            break;
        }
        const auto fields = text.substr(entry.text_begin_);
        access_log.invoke(access_log_record_access::make({
            .method_ = fields.substr(0, entry.method_bytes_),
            .known_method_ = entry.known_method_,
            .path_ = fields.substr(entry.method_bytes_, entry.path_bytes_),
            .protocol_version_ = entry.protocol_version_,
            .remote_address_ = remote_address,
            .client_address_ = entry.client_address_,
            .status_ = entry.status_,
            .duration_micros_ = access_log_record_access::duration_since(entry.start_),
            .db_count_ = entry.io_metrics_.db_count_,
            .db_duration_micros_ = entry.io_metrics_.db_duration_micros_,
            .redis_count_ = entry.io_metrics_.redis_count_,
            .redis_duration_micros_ = entry.io_metrics_.redis_duration_micros_,
        }));
    }
    reset();
}

void http1_deferred_access_log::reset() noexcept {
    entries_.clear();
    if (entries_.capacity() > retained_deferred_access_entries) {
        std::vector<entry_type>(entries_.get_allocator()).swap(entries_);
    }
    http_clear_string_retaining_small(text_);
}

connection_work_set::connection_work_set() noexcept = default;

std::unique_ptr<connection_work_set> make_connection_work_set() {
    return std::make_unique<connection_work_set>();
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// Total time allowed for a graceful close: flushing TLS close_notify and
// lingering for the peer's end of input together.
constexpr auto graceful_close_time = std::chrono::seconds(2);
// Peer input a lingering close discards before it stops waiting for the end.
constexpr std::size_t linger_input_bytes = std::size_t{4} * 1024 * 1024;
constexpr std::size_t linger_read_bytes = 4096;

// Keep the closing transport reachable by stop and bound the whole sequence.
// Expiry closes the socket, which ends a blocked write or lingering read; the
// owner closes TCP after the sequence completes either way.
class graceful_close_bounds final {
public:
    graceful_close_bounds(asio::ip::tcp::socket& socket, connection_scanner& scanner,
        const worker_ref& worker_value)
        : guard_(scanner, entry_, connection_transport::tcp(socket)) {
        entry_.set_phase(connection_scanner::phase::writing);
        armed_ = worker_value.schedule_timer(timer_, deadline_after(graceful_close_time),
                     [target = &socket](worker_timer_outcome outcome) {
                         if (outcome == worker_timer_outcome::expired) {
                             close_socket(*target);
                         }
                     }) == worker_timer_schedule_status::scheduled;
    }

    graceful_close_bounds(const graceful_close_bounds&) = delete;
    graceful_close_bounds& operator=(const graceful_close_bounds&) = delete;

    // False when the worker timer queue is already stopping; the caller then
    // closes immediately instead of running an unbounded close.
    [[nodiscard]] bool armed() const noexcept {
        return armed_;
    }

private:
    connection_scanner::entry entry_;
    connection_scanner::guard guard_;
    worker_timer_registration timer_;
    bool armed_{false};
};

// RFC 9112 §9.6: half-close, then read until the peer's end of input. Any
// error -- including the bounds closing the socket -- ends the wait.
task<void> close_gracefully(asio::ip::tcp::socket& socket, server_tls_stream* tls,
    http_connection_close mode, connection_scanner& scanner, const worker_ref& worker_value) {
    const graceful_close_bounds bounds(socket, scanner, worker_value);
    if (!bounds.armed() || !socket.is_open()) {
        co_return;
    }
    if (tls != nullptr) {
        // RFC 9846 §6.1: send our alert, without waiting for the peer's.
        // Recording its alert as received makes SSL_shutdown finish after our
        // write; this terminal session never reads the TLS stream afterward.
        SSL* const ssl = tls->native_handle();
        const int state = SSL_get_shutdown(ssl);
        if ((state & SSL_SENT_SHUTDOWN) == 0) {
            SSL_set_shutdown(ssl, state | SSL_RECEIVED_SHUTDOWN);
            const auto completion = co_await async_asio([tls](auto handler) {
                tls->async_shutdown(std::move(handler));
            });
            if (completion.error_code()) {
                co_return;
            }
        }
    }
    if (mode != http_connection_close::linger) {
        co_return;
    }
    shutdown_http_connection_send(socket);
    // Remaining input is discarded below TLS: the session never decrypts again.
    std::array<char, linger_read_bytes> discarded;
    for (std::size_t total = 0; total < linger_input_bytes;) {
        const auto completion = co_await async_asio<std::size_t>([&socket, &discarded](auto handler) {
            socket.async_read_some(asio::buffer(discarded), std::move(handler));
        });
        if (completion.error_code()) {
            co_return;
        }
        total += completion.result();
    }
}

}  // namespace

task<void> linger_http_connection(
    asio::ip::tcp::socket& socket, connection_scanner& scanner, const worker_ref& worker_value) {
    return close_gracefully(socket, nullptr, http_connection_close::linger, scanner, worker_value);
}

task<void> close_http_connection_gracefully(server_tls_stream& stream, http_connection_close mode,
    connection_scanner& scanner, const worker_ref& worker_value) {
    return close_gracefully(stream.lowest_layer(), &stream, mode, scanner, worker_value);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

// A file removed, replaced or rewritten since its response was framed: a
// deployment race the client sees as an error response, not a server fault.
[[nodiscard]] bool is_file_changed_since_framing(std::error_code error) noexcept {
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory ||
           error == std::errc::state_not_recoverable;
}

}  // namespace

response_file_failure response_file_failure::read_failed(std::error_code error) {
    if (is_file_changed_since_framing(error)) {
        return response_file_failure(false, nullptr);
    }
    return response_file_failure(false, std::make_exception_ptr(std::system_error(error, "file response body read failed")));
}

response_file_failure response_file_failure::read_not_completed(blocking_status status) {
    // A worker without a pool is a configuration fault, and an open or read
    // still unfinished after the write timeout is a stalled filesystem.
    // Overload and shutdown are not faults of this response.
    std::exception_ptr fault;
    if (status == blocking_status::unavailable) {
        fault = std::make_exception_ptr(std::system_error(std::make_error_code(std::errc::operation_not_supported),
            "file response needs a blocking pool"));
    } else if (status == blocking_status::timed_out) {
        fault = std::make_exception_ptr(
            std::system_error(std::make_error_code(std::errc::timed_out), "file response I/O timed out"));
    }
    return response_file_failure(true, std::move(fault));
}

response_file_failure response_file_failure::read_threw(std::exception_ptr exception) noexcept {
    return response_file_failure(false, std::move(exception));
}

std::exception_ptr response_file_failure::response_error() const {
    if (unavailable_) {
        return std::make_exception_ptr(http_error({.status_ = ruvia::http_status::service_unavailable,
            .code_ = "file_read_unavailable",
            .message_ = "file could not be read in time"}));
    }
    return std::make_exception_ptr(http_error({.status_ = ruvia::http_status::internal_server_error,
        .code_ = "file_read_failed",
        .message_ = "file could not be read"}));
}

char* response_file_source::read_buffer::data() & {
    if (data_ == nullptr) {
        data_.reset(static_cast<char*>(
            ::operator new(response_file_read_ahead_bytes, std::align_val_t{alignof(std::max_align_t)})));
    }
    return data_.get();
}

void response_file_source::read_buffer::reset() noexcept {
    data_.reset();
}

void response_file_source::read_buffer::buffer_deleter::operator()(char* bytes) const noexcept {
    ::operator delete(bytes, std::align_val_t{alignof(std::max_align_t)});
}

// The path travels with the state to the pool thread, so it lives in the
// synchronized process resource.
response_file_source::read_state::read_state() = default;

response_file_source::response_file_source(const http_response& response, response_file_io io)
    : response_(response),
      io_(std::move(io)),
      segment_count_(response.body_segment_count()) {}

void response_file_source::select_file(http_response_file_view file) {
    // Multipart byte ranges name the same file view: keep its validated
    // handle and only reposition.
    if (current_path_ != file.native_path_c_str() || state_.identity_ != file.identity() ||
        state_.size_ != file.size()) {
        current_path_ = file.native_path_c_str();
        state_.path_.assign(file.native_path_c_str());
        state_.size_ = file.size();
        state_.identity_ = file.identity();
        state_.validated_ = false;
        state_.input_.reset();
        if (const auto* opened = file.opened_file()) {
            // An independent handle travels with the read state: a cancelled
            // response may be destroyed before a late read finishes.
            state_.input_ = duplicate_native_file(*opened, state_.error_);
        }
    }
    state_.position_ = file.offset();
}

void response_file_source::read_ahead(read_state& state, std::size_t bytes, bool completes_range) {
    if (!state.validated_) {
        if (!state.input_) {
            state.input_ = open_native_file(state.path_.c_str(), state.error_);
            if (state.error_) {
                return;
            }
        }
        state.error_ = validate_native_file(state.input_.get(), state.identity_, state.size_);
        if (state.error_) {
            return;
        }
        state.validated_ = true;
    }
    const auto remaining = bytes - state.filled_;
    if (remaining != 0) {
        state.error_ =
            read_native_file_at(state.input_.get(), state.position_, state.buffer_.data() + state.filled_, remaining);
        if (state.error_) {
            return;
        }
    }
    state.filled_ = bytes;
    state.position_ += remaining;
    if (completes_range) {
        state.error_ = validate_native_file(state.input_.get(), state.identity_, state.size_);
    }
}

bool response_file_source::settle_read() {
    if (state_.error_ && !failure_) {
        failure_.emplace(response_file_failure::read_failed(state_.error_));
    }
    return !failure_;
}

task<bool> response_file_source::fill(std::size_t bytes, bool completes_range) {
    if (!settle_read()) {
        co_return false;
    }
    state_.filled_ = 0;
#if defined(__linux__)
    // A validated handle first takes what the page cache already holds; only
    // a remainder waits on the pool.
    if (bytes != 0 && state_.validated_) {
        state_.filled_ = read_native_file_cached(
            state_.input_.get(), state_.position_, state_.buffer_.data(), bytes, state_.cached_read_supported_);
        state_.position_ += state_.filled_;
        if (state_.filled_ == bytes) {
            if (completes_range) {
                state_.error_ = validate_native_file(state_.input_.get(), state_.identity_, state_.size_);
            }
            co_return settle_read();
        }
    }
#endif
    auto result = co_await run_blocking(io_.worker_, {.timeout_ = io_.timeout_, .stop_token_ = io_.stop_},
        [state = std::move(state_), bytes, completes_range]() mutable {
            read_ahead(state, bytes, completes_range);
            return std::move(state);
        });
    if (result.status() != blocking_status::completed) {
        // The state (handle and buffer) stayed with the abandoned operation.
        failure_.emplace(response_file_failure::read_not_completed(result.status()));
        co_return false;
    }
    if (result.failed()) {
        // A throwing read retired its owned state on the pool thread.
        failure_.emplace(response_file_failure::read_threw(result.error()));
        co_return false;
    }
    state_ = std::move(result).take_value();
    co_return settle_read();
}

task<bool> response_file_source::open_first() {
    if (first_file_opened_) {
        co_return !failure_;
    }
    first_file_opened_ = true;
    for (auto index = next_segment_; index < segment_count_; ++index) {
        if (const auto segment = response_.body_segment(index); segment.file_) {
            select_file(*segment.file_);
            co_return co_await fill(0, segment.file_->length() == 0);
        }
    }
    co_return true;
}

task<response_body_chunk> response_file_source::next(std::size_t max_bytes) {
    // A body that opens with in-memory bytes (a multipart boundary) still has
    // its first file range validated before anything is yielded. A body that
    // opens with a file validates it on the read that yields its first chunk.
    if (!first_file_opened_ && next_segment_ < segment_count_ && !response_.body_segment(next_segment_).file_ &&
        !(co_await open_first())) {
        co_return response_body_chunk::failed();
    }
    first_file_opened_ = true;
    for (;;) {
        if (range_remaining_ != 0) {
            const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                std::min(max_bytes, response_file_read_ahead_bytes), range_remaining_));
            if (!(co_await fill(bytes, bytes == range_remaining_))) {
                co_return response_body_chunk::failed();
            }
            range_remaining_ -= bytes;
            co_return response_body_chunk::data(std::string_view(state_.buffer_.data(), bytes));
        }
        if (next_segment_ >= segment_count_) {
            co_return response_body_chunk::end();
        }
        const auto segment = response_.body_segment(next_segment_++);
        if (segment.file_) {
            select_file(*segment.file_);
            range_remaining_ = segment.file_->length();
            // Empty files still complete a range: validate their snapshot
            // through the same owned I/O state before releasing the head.
            if (range_remaining_ == 0 && !(co_await fill(0, true))) {
                co_return response_body_chunk::failed();
            }
            continue;
        }
        if (!segment.bytes_.empty()) {
            co_return response_body_chunk::data(segment.bytes_);
        }
    }
}

void response_file_source::release_buffer() noexcept {
    state_.buffer_.reset();
}

bool response_file_source::file_bytes_pending() const noexcept {
    if (range_remaining_ != 0) {
        return true;
    }
    // next() validates an empty file range without reading it and skips
    // empty in-memory segments.
    for (auto index = next_segment_; index < segment_count_; ++index) {
        const auto segment = response_.body_segment(index);
        if (segment.file_) {
            if (segment.file_->length() != 0) {
                return true;
            }
        } else if (!segment.bytes_.empty()) {
            return false;
        }
    }
    return false;
}

}  // namespace ruvia::detail

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
        inbound_buffer_account& inbound, const worker_ref& worker_value, const stop_token& stop_token_value,
        const server_connection_fields& fields)
        : transport_(stream, tls_output, &entry),
          entry_(entry),
          lifecycle_(fields.lifecycle_),
          worker_(worker_value),
          read_reservation_(&inbound),
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
        if (lifecycle_ != nullptr) {
            lifecycle_->set_reason(connection_close_reason::aborted);
        }
        transport_.abort();
    }
    // Expires the facade once the handler returned. An operation the handler
    // left running is aborted first, so the join cannot wait on the peer.
    [[nodiscard]] task<void> join() {
        if (facade_.read_active_ || facade_.write_active_) {
            abort();
        }
        co_await facade_.operations_.close_and_join();
    }
    // Ends the send direction the handler left open: close_notify for TLS.
    [[nodiscard]] task<void> finish() {
        if (!facade_.send_ended_) {
            facade_.send_ended_ = true;
            co_await end_send({});
        }
    }

private:
    static void wake_read(void* raw) noexcept {
        static_cast<asio::cancellation_signal*>(raw)->emit(asio::cancellation_type::terminal);
    }
    [[nodiscard]] task<void> end_send(operation_options operation) {
        operation_interrupt interrupt(interrupts_, worker_, operation, this, &abort_thunk, "server connection finish");
        const auto error = co_await transport_.write_bytes({}, http_stream_end::end);
        interrupt.throw_if_interrupted();
        if (error) {
            throw inbound_transport_error(error, "server connection finish");
        }
    }
    static task<std::optional<std::span<const std::byte>>> read_thunk(void* raw, operation_options operation) {
        auto& owner = *static_cast<server_connection_session*>(raw);
        operation_interrupt interrupt(owner.interrupts_, owner.worker_, operation, &owner.read_cancellation_,
            &wake_read, "server connection read");
        if (owner.receive_ended_) {
            co_return std::nullopt;
        }
        owner.read_buffer_.clear();
        if (!(co_await owner.transport_.read_more(
                owner.read_buffer_, [&interrupt] { interrupt.throw_if_interrupted(); }, owner.read_cancellation_.slot(), &owner.read_reservation_))) {
            owner.receive_ended_ = true;
            if (owner.lifecycle_ != nullptr) {
                owner.lifecycle_->set_reason(connection_close_reason::peer_eof);
            }
            co_return std::nullopt;
        }
        owner.entry_.touch();
        co_return std::as_bytes(std::span(owner.read_buffer_.data(), owner.read_buffer_.size()));
    }
    static task<void> write_thunk(void* raw, std::string_view bytes, operation_options operation) {
        auto& owner = *static_cast<server_connection_session*>(raw);
        operation_interrupt interrupt(owner.interrupts_, owner.worker_, operation, raw, &abort_thunk, "server connection write");
        const auto error = co_await owner.transport_.write_bytes(bytes, http_stream_end::keep_open);
        interrupt.throw_if_interrupted();
        if (error) {
            throw inbound_transport_error(error, "server connection write");
        }
    }
    static task<void> finish_thunk(void* raw, operation_options operation) {
        return static_cast<server_connection_session*>(raw)->end_send(std::move(operation));
    }
    static void abort_thunk(void* raw) noexcept {
        static_cast<server_connection_session*>(raw)->abort();
    }

    http_socket_tunnel_transport<stream_type> transport_;
    connection_scanner::entry& entry_;
    connection_lifecycle* lifecycle_;
    const worker_ref& worker_;
    std::string read_buffer_;
    inbound_buffer_reservation read_reservation_;
    asio::cancellation_signal read_cancellation_;
    bool receive_ended_{false};
    bool aborted_{false};
    operation_interrupt_source interrupts_;
    server_connection facade_;
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr const char* connection_lane_in_progress = "server connection operation already in progress on this direction";

task<std::optional<std::span<const std::byte>>> read_connection(void* target,
    task<std::optional<std::span<const std::byte>>> (*read)(void*, operation_options), bool& read_active,
    operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, connection_lane_in_progress);
    co_return co_await read(target, std::move(operation));
}

task<void> write_connection(void* target, task<void> (*write)(void*, std::string_view, operation_options),
    std::string bytes, bool& write_active, const bool& send_ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, connection_lane_in_progress);
    if (send_ended) {
        throw std::logic_error("server connection send direction has ended");
    }
    co_await write(target, bytes, std::move(operation));
}

task<void> finish_connection(void* target, task<void> (*finish_value)(void*, operation_options), bool& send_ended,
    bool& write_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, connection_lane_in_progress);
    if (!send_ended) {
        co_await finish_value(target, std::move(operation));
        send_ended = true;
    }
}

}  // namespace

server_connection::server_connection(const detail::server_connection_fields& fields,
    const ::ruvia::stop_token& stop_token_value, const worker_ref& worker_value, void* target, read_type read,
    write_type write, finish_type finish_value, abort_type abort_value) noexcept
    : worker_(worker_value),
      target_(target),
      read_(read),
      write_(write),
      finish_(finish_value),
      abort_(abort_value),
      id_(fields.id_),
      local_(fields.lifecycle_ != nullptr ? fields.lifecycle_->local_endpoint()
                                          : ip_endpoint(ip_address::parse(fields.local_address_).value_or(ip_address{}), fields.local_port_)),
      remote_(fields.lifecycle_ != nullptr ? fields.lifecycle_->remote_endpoint()
                                           : ip_endpoint(ip_address::parse(fields.remote_address_).value_or(ip_address{}), fields.remote_port_)),
      stop_token_(stop_token_value),
      state_(fields.state_) {
    if (fields.tls_.has_value()) {
        tls_.emplace(tls_connection_transport(
            fields.tls_->client_certificate_subject_, fields.tls_->alpn_, fields.tls_->server_name_));
    }
    operations_.bind_worker(worker_);
}

void server_connection::require_active() const {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("server connection has expired");
    }
}

task<std::optional<std::span<const std::byte>>> server_connection::read(operation_options operation) & {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_connection(target_, read_, read_active_, std::move(operation)));
}

task<void> server_connection::write(std::string_view bytes, operation_options operation) & {
    require_active();
    if (send_ended_) {
        throw std::logic_error("server connection send direction has ended");
    }
    return write_owned(std::string(bytes), std::move(operation));
}

task<void> server_connection::write(std::span<const std::byte> bytes, operation_options operation) & {
    return write(as_chars(bytes), std::move(operation));
}

task<void> server_connection::write_owned(std::string&& bytes, operation_options operation) {
    require_active();
    if (send_ended_) {
        throw std::logic_error("server connection send direction has ended");
    }
    auto owned = std::move(bytes);
    return ::ruvia::make_scoped_operation(operations_, write_connection(target_, write_, std::move(owned), write_active_, send_ended_, std::move(operation)));
}

task<void> server_connection::finish(operation_options operation) & {
    require_active();
    return ::ruvia::make_scoped_operation(
        operations_, finish_connection(target_, finish_, send_ended_, write_active_, std::move(operation)));
}

void server_connection::abort() & noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_);
    }
}

}  // namespace ruvia

namespace ruvia::detail {

// The handler owns the connection until its task completes; the connection
// then ends here, so a handler never has to close it itself. The TLS ciphertext
// writer outlives the session that writes through it and is joined on every
// exit.
template <typename stream_type>
task<void> worker_connections::serve_connection(
    stream_type& stream, tcp_socket_type& socket, server_connection_fields fields) {
    // A stopping worker has already closed the connection.
    if (!http_server_worker_running(state_) || !socket.is_open()) {
        co_return;
    }
    std::string local_address;
    std::error_code local_error;
    if (const auto local = socket.local_endpoint(local_error); !local_error) {
        assign_peer_address(local_address, local.address());
        fields.local_address_ = local_address;
        fields.local_port_ = local.port();
    }
    connection_scanner::entry scanner_entry;
    scanner_entry.bind_lifecycle(fields.lifecycle_);
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    inbound_buffer_account connection_buffers(
        *options_.inbound_budget_, options_.server_.max_inbound_buffer_bytes_per_connection_);
    std::optional<tls_tunnel_output> tls_output;
    if constexpr (requires { stream.next_layer(); }) {
        tls_output.emplace(*stream.native_handle(), socket, worker_, fields.lifecycle_);
        tls_output->start();
    }
    std::exception_ptr failure;
    {
        server_connection_session<stream_type> session(stream, tls_output.has_value() ? &*tls_output : nullptr,
            scanner_entry, connection_buffers, worker_, stop_token_, fields);
        try {
            co_await options_.connection_(session.connection());
        } catch (...) {
            failure = std::current_exception();
            if (fields.lifecycle_ != nullptr) {
                fields.lifecycle_->note_exception(failure);
            }
        }
        if (failure != nullptr) {
            session.abort();
        }
        co_await session.join();
        // A worker stop keeps the immediate close.
        if (failure == nullptr && !session.aborted() && !stop_token_.stop_requested()) {
            try {
                co_await session.finish();
            } catch (...) {
                failure = std::current_exception();
                if (fields.lifecycle_ != nullptr) {
                    fields.lifecycle_->note_exception(failure);
                }
            }
        }
    }
    if (tls_output.has_value()) {
        if (failure != nullptr) {
            tls_output->abort();
        }
        co_await tls_output->join();
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

template task<void> worker_connections::serve_connection<inbound_socket_stream>(
    inbound_socket_stream&, tcp_socket_type&, server_connection_fields);
template task<void> worker_connections::serve_connection<server_tls_stream>(
    server_tls_stream&, tcp_socket_type&, server_connection_fields);

}  // namespace ruvia::detail

namespace ruvia::detail {

struct request_stream_access final {
    [[nodiscard]] static request_stream make(context& context_value, response_stream_writer& response,
        const request_stream_tunnel_accept* accept_tunnel,
        const request_stream_websocket_accept* accept_websocket = nullptr) noexcept {
        return request_stream(context_value, response, accept_tunnel, accept_websocket);
    }

    [[nodiscard]] static task<void> close(request_stream& stream) {
        return stream.operation_scope_.close_and_join();
    }
};

namespace {

// Runs the connection's callback; returns its failure, if any.
task<std::exception_ptr> run_request_stream_callback(
    context& context_value, response_stream_writer& response, const request_stream_tunnel_accept* accept_tunnel,
    const request_stream_websocket_accept* accept_websocket = nullptr) {
    const auto callback = context_access::services(context_value).request_stream();
    auto stream = request_stream_access::make(context_value, response, accept_tunnel, accept_websocket);
    std::exception_ptr failure;
    try {
        co_await callback(stream);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await request_stream_access::close(stream);
    co_return failure;
}

// After the callback: a committed response or tunnel stands, and a failure
// past it propagates to the transport, which aborts the stream and reports it.
// Otherwise nothing reached the peer: an explicit connection abort is terminal;
// without it the request gets the framework's answer to the failure, and a
// callback that selected nothing is a fault. Unexpected failures reach the sink.
// No application error handler takes part.
void complete_request_stream(context& context_value, std::exception_ptr failure) {
    if (context_access::services(context_value).connection_aborted() ||
        context_access::services(context_value).request_reset()) {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return;
    }
    if (context_access::output_committed(context_value)) {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return;
    }
    if (failure == nullptr) {
        if (context_access::has_response(context_value)) {
            return;
        }
        failure = std::make_exception_ptr(std::logic_error("request stream callback completed without a response"));
    }
    auto answer = make_default_exception_response(failure);
    if (answer.unexpected_) {
        context_access::services(context_value)
            .connection_failure()
            .invoke(context_access::services(context_value).remote_address(), failure);
    }
    context_access::set_response(context_value, std::move(answer.response_));
}

// The response route of a request stream connection: the callback writes
// through the transport's response stream, bound to the context.
task<void> serve_request_stream(void*, context& context_value) {
    auto failure = co_await run_request_stream_callback(context_value, context_value.stream(), nullptr);
    complete_request_stream(context_value, std::move(failure));
}

[[nodiscard]] const route_entry& request_stream_response_route() noexcept {
    // Process-invariant and allocation-free: no route path, method token or
    // protocol to own.
    static const route_entry route({.method_ = http_known_method::unknown,
        .path_ = {},
        .endpoint_ = route_endpoint::response_stream(&serve_request_stream, request_body_mode::stream)});
    return route;
}

[[nodiscard]] const route_entry& request_stream_tunnel_route() noexcept {
    // The tunnel upgrade runs serve_request_stream_tunnel() for this route
    // instead of its handler.
    static const route_entry route({.method_ = http_known_method::unknown,
        .path_ = {},
        .endpoint_ = route_endpoint::tunnel(&serve_request_stream, {}, {})});
    return route;
}

}  // namespace

route_resolution resolve_request_stream_route(bool tunnel) noexcept {
    return route_resolution::resolved(tunnel ? request_stream_tunnel_route() : request_stream_response_route(), nullptr);
}

bool is_request_stream_tunnel_route(const route_entry& route) noexcept {
    return &route == &request_stream_tunnel_route();
}

bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept {
    return request.known_method() == http_known_method::connect || (!content && http1_request_offers_upgrade(request));
}

task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept,
    const request_stream_websocket_accept* accept_websocket) {
    auto failure = co_await run_request_stream_callback(context_value, context_value.stream(), &accept, accept_websocket);
    complete_request_stream(context_value, std::move(failure));
}

}  // namespace ruvia::detail

namespace ruvia {

const http_request& request_stream::request() const& noexcept {
    return detail::context_access::request(context_);
}

const conn_info& request_stream::conn() const& noexcept {
    return context_.conn();
}

request_body_state request_stream::body_state() const noexcept {
    return detail::context_access::services(context_).body_state();
}

void request_stream::reset() {
    detail::context_access::services(context_).reset_request({});
}

void request_stream::reset(http2_error_code error) {
    if (request().protocol_version() != http_protocol_version::http2) {
        throw std::invalid_argument("HTTP/2 reset code requires HTTP/2");
    }
    detail::context_access::services(context_).reset_request(error);
}

void request_stream::reset(http3_error_code error) {
    if (request().protocol_version() != http_protocol_version::http3) {
        throw std::invalid_argument("HTTP/3 reset code requires HTTP/3");
    }
    detail::context_access::services(context_).reset_request(error);
}

body_reader& request_stream::body() & {
    if (accept_tunnel_ != nullptr) {
        throw std::logic_error("a tunnel request carries no content: its bytes belong to accept_tunnel()");
    }
    return context_.req().body_reader();
}

std::span<const http_header> request_stream::trailers() const noexcept {
    return context_.req().trailers();
}

std::optional<std::string_view> request_stream::trailer(std::string_view name) const noexcept {
    return context_.req().trailer(name);
}

task<void> request_stream::inform(const http_interim_response_head& response, operation_options operation) & {
    return context_.inform(response, std::move(operation));
}

bool request_stream::early_data() const noexcept {
    return context_.early_data();
}

bool request_stream::push(http_push_request_view request) {
    return context_.push(request);
}

void request_stream::advertise_origins(std::span<const std::string_view> origins) {
    context_.advertise_origins(origins);
}

void request_stream::advertise_alternative_service(std::string_view value) {
    context_.advertise_alternative_service(value);
}

http_client_ref request_stream::client() const {
    return context_.client();
}

http_client_ref request_stream::client(std::string_view alias) const {
    return context_.client(alias);
}

#ifdef RUVIA_ENABLE_DATABASE
db_session request_stream::db() const {
    return context_.db();
}

db_session request_stream::db(std::string_view alias) const {
    return context_.db(alias);
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_session request_stream::redis() const {
    return context_.redis();
}

redis_session request_stream::redis(std::string_view alias) const {
    return context_.redis(alias);
}
#endif

context_response_editor request_stream::draft() & noexcept {
    return context_.draft();
}

task<http_response> request_stream::file(file_response_options options) {
    return make_scoped_operation(operation_scope_, context_.file(options));
}

task<http_response> request_stream::static_file(const static_root& root, static_file_response_options options) {
    return make_scoped_operation(operation_scope_, context_.static_file(root, options));
}

void request_stream::respond(http_response response) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    require_unselected_response();
    if (tunnel_requested() && request().known_method() == http_known_method::connect &&
        response.status().is_successful()) {
        throw std::logic_error("a 2xx response to CONNECT establishes the tunnel; use accept_tunnel()");
    }
    if (response.status() == http_status::switching_protocols) {
        throw std::logic_error("a 101 response requires accept_tunnel() or accept_websocket()");
    }
    context_.respond(std::move(response));
}

response_stream_writer& request_stream::response() & noexcept {
    return response_;
}

bool request_stream::tunnel_requested() const noexcept {
    return accept_tunnel_ != nullptr;
}

task<http_tunnel*> request_stream::accept_tunnel(http_status_code status, http_tunnel_route_config config) & {
    return make_scoped_operation(operation_scope_, accept_tunnel_impl(status, config));
}

task<http_tunnel*> request_stream::accept_tunnel_impl(http_status_code status, http_tunnel_route_config config) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    if (accept_tunnel_ == nullptr) {
        throw std::logic_error("accept_tunnel() requires a CONNECT or HTTP/1.1 Upgrade request");
    }
    require_unselected_response();
    if (request().known_method() == http_known_method::connect) {
        if (!status.is_successful()) {
            throw std::invalid_argument("a CONNECT tunnel is accepted with a 2xx status");
        }
        context_.draft().status(status);
    } else if (status != http_status::switching_protocols) {
        throw std::invalid_argument("an HTTP/1.1 Upgrade is accepted with 101 (Switching Protocols)");
    }
    co_return co_await (*accept_tunnel_)(status, config);
}

task<websocket_accept_result> request_stream::accept_websocket(websocket_route_config config) & {
    return make_scoped_operation(operation_scope_, accept_websocket_impl(std::move(config)));
}

task<websocket_accept_result> request_stream::accept_websocket_impl(websocket_route_config config) {
    // Validated before any state check so an invalid configuration selects nothing.
    const auto endpoint = detail::route_endpoint::websocket(nullptr, std::move(config));
    co_return co_await accept_websocket_endpoint(*endpoint.websocket());
}

task<websocket_accept_result> request_stream::accept_websocket_endpoint(const detail::websocket_route_endpoint& endpoint) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    if (accept_websocket_ == nullptr) {
        throw std::logic_error("accept_websocket() requires an Upgrade or extended CONNECT request");
    }
    require_unselected_response();
    co_return co_await (*accept_websocket_)(endpoint);
}

void request_stream::require_unselected_response() const {
    if (detail::context_access::services(context_).request_reset() ||
        detail::context_access::services(context_).connection_aborted() ||
        response_.committed() || detail::context_access::output_committed(context_) ||
        detail::context_access::has_response(context_)) {
        throw std::logic_error("the request's response is already selected or committed");
    }
}

::ruvia::stop_token request_stream::stop_token() const noexcept {
    return context_.stop_token();
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] protocol_byte_limit request_body_byte_limit(request_body_mode body_mode,
    const std::optional<std::size_t>& max_stream_body_bytes, std::size_t max_buffered_body_bytes,
    std::size_t route_limit) {
    if (body_mode != request_body_mode::stream) {
        const auto limit = route_limit != 0 && route_limit < max_buffered_body_bytes
                               ? route_limit
                               : max_buffered_body_bytes;
        return protocol_byte_limit::limited(limit);
    }
    if (!max_stream_body_bytes.has_value()) {
        return route_limit != 0 ? protocol_byte_limit::limited(route_limit)
                                : protocol_byte_limit::unlimited();
    }
    const auto limit =
        route_limit != 0 && route_limit < *max_stream_body_bytes ? route_limit : *max_stream_body_bytes;
    return protocol_byte_limit::limited(limit);
}

// RFC 9110 §7.4 against the transport connection's facts. A secured
// connection without a recorded certificate cannot vouch for any host.
[[nodiscard]] bool connection_carries_target(const http_request& request, const context_services& services) noexcept {
    const auto origin = decide_http_target_origin(request,
        {.secured_ = services.get_conn_info().tls() != nullptr, .trusted_gateway_ = services.peer_trusted()});
    switch (origin.action()) {
        case http_target_origin_action::accept:
            return true;
        case http_target_origin_action::verify_certificate: {
            auto* certificate = services.served_certificate();
            return certificate != nullptr && certificate->covers(origin);
        }
        case http_target_origin_action::misdirected:
            break;
    }
    return false;
}

}  // namespace
http_request_admission_policy http_request_admission_policy::server(const http_server_options& options) noexcept {
    return {
        .max_stream_body_bytes_ = options.server_.max_stream_body_bytes_,
        .max_buffered_body_bytes_ = options.server_.max_buffered_body_bytes_,
        .apply_rate_limit_ = true,
        .verify_target_origin_ = true,
    };
}

protocol_byte_limit http_request_body_limit(const route_resolution& resolution, request_body_mode mode,
    const http_request_admission_policy& policy) noexcept {
    const auto* resolved = resolution.resolved();
    return request_body_byte_limit(mode, policy.max_stream_body_bytes_, policy.max_buffered_body_bytes_,
        resolved != nullptr ? resolved->route().max_request_body_bytes() : 0);
}

http_request_rejection http_request_rejection::rate_limited(rate_limit_rejection rejection) noexcept {
    return http_request_rejection(rate_limit_rejection_error(), rejection);
}

task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request, http_request_rejection rejection, const context_services& services) {
    auto response = co_await routes.handle_error(request, rejection.error(), services);
    if (const auto* rate_limit = rejection.rate_limit()) {
        apply_rate_limit_rejection_headers(response, *rate_limit);
    }
    co_return response;
}

void http_response_tail::record_access(const access_log_sink& access_log, http_status_code status) const noexcept {
    if (!access_log.callback_) {
        return;
    }
    access_log.invoke(access_log_record_access::make({
        .method_ = request_->method(),
        .known_method_ = request_->known_method(),
        .path_ = request_->path(),
        .protocol_version_ = request_->protocol_version(),
        .remote_address_ = remote_address_,
        .client_address_ = client_address_,
        .status_ = status,
        .duration_micros_ = access_log_record_access::duration_since(start_),
        .db_count_ = io_metrics_.db_count_,
        .db_duration_micros_ = io_metrics_.db_duration_micros_,
        .redis_count_ = io_metrics_.redis_count_,
        .redis_duration_micros_ = io_metrics_.redis_duration_micros_,
    }));
}

void http_response_tail::defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred,
    std::size_t head_end, http_status_code status) const {
    if (!access_log.callback_) {
        return;
    }
    deferred.defer(head_end, *request_, client_address_, start_, status, io_metrics_);
}

http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, const context_services& services) {
    const auto& request = input.request_;
    // The host's compression and CORS policies cover request stream responses
    // too; the application-wide rate limit belongs to the routes alone, and a
    // router applies its own.
    const bool request_stream = static_cast<bool>(services.request_stream());
    const auto coding = input.coding_;
    // Negotiate against every coding. A document root or context::static_file
    // may serve an indexed precompressed sidecar even when this worker has no
    // runtime encoder; the encoder capability travels to representation
    // preparation instead of pruning the client's choices here. A failed
    // negotiation stays in the selection until the response status is known: a
    // buffered 204/205/304 carries no representation and must not become 406.
    auto coding_policy = http_response_coding_policy::selected(coding);
    // Keyed on the client, not the hop: behind a trusted proxy every request
    // would otherwise share the proxy's single key. Resolved once per request
    // and bound to the services every context of this request is created from.
    const auto* resolved = input.resolution_.resolved();
    http_request_admission admission(
        http_response_tail(request, coding_policy, {}, {}, input.start_), services);
    if (services.peer_trusted()) {
        admission.services_.bind_conn_info(services.resolve_conn_info(request));
    }
    const auto& connection = admission.services_.get_conn_info();
    const auto remote_address = connection.remote().address();
    const auto client_address = connection.client().address();
    admission.tail_.bind_addresses(remote_address, client_address);
    admission.tail_.bind_alternative_service(services.alternative_service());
    admission.services_.bind_response_coding(coding);
    if (input.pinned_arrival_.has_value()) {
        admission.services_.pin_arrival(*input.pinned_arrival_);
    }
    if (resolved != nullptr) {
        admission.services_.bind_route_body_limit(resolved->route().max_request_body_bytes());
    }
    admission.resolved_ = resolved;
    const auto* endpoint = resolved != nullptr ? &resolved->route().endpoint() : nullptr;
    admission.body_limit_ = http_request_body_limit(input.resolution_,
        endpoint != nullptr ? endpoint->request_body_mode() : request_body_mode::buffered, policy);

    const auto reject = [&](http_request_rejection rejection) {
        admission.endpoint_ = http_request_endpoint::rejection;
        admission.rejection_ = rejection;
        return std::move(admission);
    };

    // Before any expectation, limit or endpoint decision: a request this
    // connection cannot carry is not this server's to evaluate.
    if (policy.verify_target_origin_ && !connection_carries_target(request, services)) {
        return reject(http_request_rejection::error(http_error_info(
            {.status_ = http_status::misdirected_request, .message_ = "misdirected request"})));
    }

    // Expect extensions are valid HTTP syntax. The protocol reports the
    // semantic fact; this product implements only 100-continue and chooses the
    // RFC 9110-permitted 417 before reading request content.
    if (input.expectation_ != nullptr &&
        *input.expectation_ == http_server_expectation_plan::reject_unsupported) {
        return reject(http_request_rejection::error(
            [&] { const auto err = http_unsupported_expectation_error(); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()));
    }
    if (policy.apply_rate_limit_ && !request_stream) {
        if (const auto rejection = decide_request_rate_limit(
                admission.services_.rate_limiter(), client_address, input.pinned_arrival_)) {
            return reject(http_request_rejection::rate_limited(*rejection));
        }
    }
    // Content already in hand (buffered bodies and subrequests) is held
    // to the same ceiling a streaming protocol enforces while reading.
    if (admission.body_limit_.exceeds(request.body_bytes().size())) {
        return reject(http_request_rejection::error(
            [&] { const auto err = http_request_body_protocol_error(http_request_body_failure::too_large); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()));
    }
    if (endpoint == nullptr) {
        admission.endpoint_ = http_request_endpoint::buffered;
        return admission;
    }
    if (const auto* tunnel = endpoint->tunnel()) {
        admission.udp_tunnel_ = tunnel->protocol() == "connect-udp" ||
                                (request_stream && (request.protocol() == "connect-udp" || is_http_connect_udp_upgrade_request(request)));
        if (admission.udp_tunnel_ && validate_http_connect_udp_request(request).has_value()) {
            return reject(http_request_rejection::error(http_error_info(
                {.status_ = http_status::bad_request, .message_ = "invalid CONNECT-UDP request head"})));
        }
        admission.endpoint_ = http_request_endpoint::tunnel;
        return admission;
    }
    if (endpoint->websocket() != nullptr) {
        // An upgrade selects no HTTP response representation.
        admission.endpoint_ = http_request_endpoint::websocket;
        return admission;
    }
    if (endpoint->response_stream() != nullptr) {
        admission.endpoint_ = http_request_endpoint::response_stream;
        return admission;
    }
    const auto* buffered = endpoint->buffered();
    admission.endpoint_ = buffered != nullptr && buffered->request_body_mode() == request_body_mode::stream
                              ? http_request_endpoint::stream_body
                              : http_request_endpoint::buffered;
    return admission;
}

task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes,
    const http_request_admission_input& input, const http_request_admission_policy& policy,
    const context_services& services, http_response& response) {
    auto admission = admit_http_request(input, policy, services);
    if (const auto* rejection = admission.rejection()) {
        // As in the server: a rejection does not inherit the coding promise.
        admission.tail().disable_response_coding();
        response = co_await respond_http_request_rejection(
            routes, input.request_, *rejection, admission.services());
    } else {
        co_await routes.dispatch_buffered_response(input.request_, input.resolution_,
            document_root_binding::none(), admission.services(), admission.static_file_mode(), response);
    }
    co_return admission;
}
}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// A coded representation is worth sending only when strictly smaller than
// the identity bytes; the encoder stops at this cap instead of finishing.
// When the client refuses identity (RFC 9110 §12.5.3), any acceptable coding
// beats a 406, so the cap is lifted.
[[nodiscard]] http_content_encode_options compressed_body_options(
    std::string_view plain, bool identity_accepted) noexcept {
    if (!identity_accepted) {
        return {.max_encoded_bytes_ = (std::numeric_limits<std::size_t>::max)()};
    }
    return {.max_encoded_bytes_ = plain.empty() ? 0 : plain.size() - 1};
}

// Publishes an encoder result as the response representation. The commit
// stages every affected header before publishing the owned body, so a
// request-resource failure leaves the identity response usable for the typed
// 500 path instead of exposing a mixed body/metadata state.
[[nodiscard]] http_response_compression_status commit_compressed_body(
    http_response& response, http_content_coding coding, std::optional<std::string>&& encoding) {
    if (!encoding.has_value()) {
        return http_response_compression_status::not_applicable;
    }
    response.replace_body_with_content_encoding(std::move(*encoding), http_content_coding_token(coding));
    return http_response_compression_status::compressed;
}

[[nodiscard]] bool response_headers_skip_compression(const http_response& response) noexcept {
    if (response.header("Content-Range").has_value()) {
        return true;
    }
    if (const auto content_type = response.header("Content-Type");
        !content_type.has_value() || !is_http_media_type_compressible(*content_type)) {
        return true;
    }
    const bool has_cache_control = response.header("Cache-Control").has_value();
    cache_control_field_parser cache_parser;
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        switch (name.size()) {
            case 11U:
                if (http_ascii_equals_ignore_case(name, "Repr-Digest") ||
                    http_ascii_equals_ignore_case(name, "Content-MD5")) {
                    return true;
                }
                break;
            case 13U:
                if (http_ascii_equals_ignore_case(name, "Cache-Control")) {
                    cache_parser.update(header.value());
                }
                break;
            case 14U:
                if ((name[8] | 0x20) == 'd' && http_ascii_equals_ignore_case(name, "Content-Digest")) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return has_cache_control && cache_parser.finish().has(cache_control_directive::no_transform);
}

}  // namespace

std::optional<std::uint64_t> declared_content_length(const http_response& response) noexcept {
    const auto field = response.header("Content-Length");
    if (!field.has_value()) {
        return std::nullopt;
    }
    std::optional<std::uint64_t> declared;
    bool valid = true;
    http_visit_comma_separated_quoted_field_items(*field, [&](std::string_view item) noexcept {
        if (item.empty()) {
            valid = false;
            return false;
        }
        std::uint64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed);
        if (ec != std::errc{} || ptr != item.data() + item.size() ||
            (declared.has_value() && *declared != parsed)) {
            valid = false;
            return false;
        }
        declared = parsed;
        return true;
    });
    return valid ? declared : std::nullopt;
}

bool declared_length_permits_encoding(const http_response_coding_selection& selection,
    const compression_config& options, std::uint64_t declared) noexcept {
    // min_bytes_ only says when encoding stops paying off; a client that
    // refuses identity is served a coding at any size instead of a 406.
    return !(((declared == 0 || declared < options.min_bytes_) && selection.identity_accepted()) ||
             declared > options.max_bytes_);
}

namespace {

// An empty HEAD body is a HEAD handler describing the GET representation
// without producing it. Encoding those zero bytes would publish the coded
// length of nothing (RFC 9110 §8.6). Apply the coding GET would select, sized
// by the declared Content-Length, and drop that length: the coded size is
// unknown and §8.6 allows omitting it.
[[nodiscard]] http_buffered_compression_step apply_head_content_encoding(
    const http_response_coding_selection& selection, http_response& response,
    const compression_config& options, std::optional<std::uint64_t> declared) {
    const bool encode = declared.has_value() ? declared_length_permits_encoding(selection, options, *declared)
                                             : !selection.identity_accepted();
    if (!encode) {
        return {http_response_compression_status::not_applicable, false};
    }
    response.apply_content_encoding(http_content_coding_token(selection.coding()));
    return {http_response_compression_status::compressed, false};
}

}  // namespace

http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability) {
    // A file's bytes are served as the representation their validators and
    // HEAD response describe, whether the transport reads the file or the
    // context already carries them in memory.
    if (source_value == http_response_compression_source::buffered && response.has_file_representation()) {
        return http_response_compression_decision::fixed_representation;
    }
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return http_response_compression_decision::fixed_representation;
    }
    // An application-encoded body is kept, but whether it is sent (200) or
    // refused (406) still depends on Accept-Encoding, so caches must key on it
    // (RFC 9110 §12.5.5; Pingora depends_on_accept_encoding).
    if (response.header("Content-Encoding").has_value()) {
        response.add_vary_token("Accept-Encoding");
        return http_response_compression_decision::fixed_representation;
    }
    // Integrity fields describe the content or the selected representation,
    // both of which include the content coding (RFC 9530 §1, §2, §3; legacy
    // Content-MD5 likewise covered the sent content). Encoding here would
    // leave them describing bytes that are not sent.
    if (availability == http_response_coding_availability::identity_only ||
        response.status() == http_status::partial_content ||
        response_headers_skip_compression(response)) {
        return http_response_compression_decision::fixed_representation;
    }

    response.add_vary_token("Accept-Encoding");
    if (selection.coding() == http_content_coding::identity) {
        return http_response_compression_decision::negotiated_identity;
    }
    return http_response_compression_decision::encode;
}

http_buffered_compression_step apply_response_compression(const http_response_coding_selection& selection,
    http_known_method request_method, http_response& response, const compression_config& options,
    worker_codecs& codecs, bool offload_available) {
    constexpr http_buffered_compression_step not_applicable{http_response_compression_status::not_applicable, false};
    const auto body = response.body_bytes();

    const auto decision = prepare_response_compression(selection, request_method, response,
        http_response_compression_source::buffered,
        http_response_coding_availability::identity_and_compression);
    // Negotiation errors above still propagate; only encoding and publication
    // failures become a typed compression result.
    try {
        if (decision == http_response_compression_decision::encode && request_method == http_known_method::head &&
            body.empty() && (response.has_head_metadata_body() || response.header("Content-Length").has_value())) {
            const auto declared = declared_content_length(response);
            if (declared != std::uint64_t{0}) {
                return apply_head_content_encoding(selection, response, options, declared);
            }
        }
        if (decision != http_response_compression_decision::encode ||
            !declared_length_permits_encoding(selection, options, body.size())) {
            return not_applicable;
        }
        // sync_bytes_ is the inline ceiling. Larger bodies encode on the blocking
        // pool; without one they stay identity (or 406 when identity is refused)
        // rather than stall the event loop.
        if (body.size() > options.sync_bytes_) {
            return {http_response_compression_status::not_applicable, offload_available};
        }

        const auto coding = selection.coding();
        auto encoding = codecs.encoder(coding).encode(
            body, compressed_body_options(body, selection.identity_accepted()));
        return {commit_compressed_body(response, coding, std::move(encoding)), false};
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        return {http_response_compression_status::failed, false};
    }
}

task<http_response_compression_status> compress_response_offloaded(http_response_coding_selection selection,
    http_response& response, const worker_ref& worker_value, const stop_token& stop_token_value) {
    try {
        const auto coding = selection.coding();
        const bool identity_accepted = selection.identity_accepted();
        std::string plain(response.body_bytes());
        auto result_value = co_await run_blocking(worker_value, {.stop_token_ = stop_token_value},
            [coding, identity_accepted, plain = std::move(plain)]() {
                return encode_http_content(
                    coding, plain, compressed_body_options(plain, identity_accepted));
            });
        if (result_value.failed()) {
            ruvia::report_unhandled_failure("response compression", result_value.error());
            co_return http_response_compression_status::failed;
        }
        if (!result_value.completed()) {
            // Queue saturation and shutdown are overload/lifecycle outcomes,
            // not broken encoders. Preserve the identity representation when
            // identity is acceptable; otherwise report 503 Service Unavailable.
            co_return http_response_compression_status::unavailable;
        }
        co_return commit_compressed_body(response, coding, std::move(result_value).take_value());
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        // Input ownership, encoder execution and representation publication
        // leave the original identity body intact on failure.
        co_return http_response_compression_status::failed;
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

connection_transport connection_transport::tcp(asio::ip::tcp::socket& socket) noexcept {
    return connection_transport(
        &socket, [](void* target) noexcept { close_socket(*static_cast<asio::ip::tcp::socket*>(target)); },
        [](void* target) noexcept {
            std::error_code ignored;
            static_cast<asio::ip::tcp::socket*>(target)->cancel(ignored);
        });
}
namespace {

[[nodiscard]] worker_ref require_scanner_worker(worker_ref worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("connection scanner requires a valid worker");
    }
    return worker_value;
}

[[nodiscard]] bool timeout_expired(
    const std::optional<timeout_duration>& timeout, std::int64_t inactive_ms) noexcept {
    return timeout.has_value() && inactive_ms >= timeout->value().count();
}

[[nodiscard]] std::int64_t close_deadline_after(std::int64_t now, std::chrono::milliseconds delay) noexcept {
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    return now > maximum - delay.count() ? maximum : now + delay.count();
}

[[nodiscard]] std::int64_t activity_now_ms() noexcept {
    // 向上取整活动时间，避免毫秒截断让超时提前；扫描时钟仍向下取整。
    return std::chrono::ceil<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

connection_scanner::periodic_check_registration::~periodic_check_registration() noexcept {
    reset();
}
void connection_scanner::periodic_check_registration::reset() noexcept {
    if (entry_ != nullptr) {
        entry_->remove_periodic_check(*this);
    }
}
connection_scanner::entry::~entry() noexcept {
    connection_scanner::detach_entry(*this);
    detach_periodic_checks();
}
void connection_scanner::entry::touch() noexcept {
    if (scanner_ != nullptr) {
        last_active_ms_ = activity_now_ms();
    }
}
void connection_scanner::entry::note_write_progress() noexcept {
    if (scanner_ != nullptr) {
        last_active_ms_ = activity_now_ms();
        write_progress_ms_ = last_active_ms_;
    }
}
void connection_scanner::entry::set_phase(phase next_phase) noexcept {
    if (scanner_ == nullptr) {
        return;
    }
    const bool write_started = next_phase == phase::writing && !write_pending();
    touch();
    if (phase_ != next_phase) {
        phase_started_ms_ = last_active_ms_;
    }
    if (write_started) {
        write_progress_ms_ = last_active_ms_;
    }
    phase_ = next_phase;
    // A head read that completed before its cancel took effect returns the
    // connection to its phase timeouts; the stale request has no deadline.
    if (reading_request_head_) {
        close_requested_ = false;
    }
    reading_request_head_ = false;
    sync_reusable();
}
void connection_scanner::entry::read_request_head(phase timeout_phase) noexcept {
    set_phase(timeout_phase);
    reading_request_head_ = scanner_ != nullptr;
    close_requested_ = false;
    sync_reusable();
}
connection_scanner::entry::phase_scope::~phase_scope() {
    if (entry_.scanner_ == nullptr) {
        return;
    }
    entry_.touch();
    entry_.phase_started_ms_ = phase_started_ms_;
    entry_.phase_ = phase_;
    entry_.reading_request_head_ = reading_request_head_;
    entry_.sync_reusable();
}
connection_scanner::entry::write_scope::write_scope(entry& entry_value) noexcept
    : entry_(entry_value) {
    if (!entry_.write_pending()) {
        entry_.note_write_progress();
    }
    ++entry_.write_scopes_;
}
connection_scanner::entry::write_scope::~write_scope() {
    --entry_.write_scopes_;
    entry_.touch();
}
void connection_scanner::entry::enter_long_lived() noexcept {
    set_phase(phase::long_lived);
}
std::int64_t connection_scanner::entry::last_active_ms() const noexcept {
    return last_active_ms_;
}
void connection_scanner::entry::register_periodic_check(periodic_check_registration& registration,
    void* target, periodic_check_type tick, drain_check_type drain) noexcept {
    registration.reset();
    registration.entry_ = this;
    registration.target_ = target;
    registration.tick_ = tick;
    registration.drain_ = drain;
    registration.next_ = periodic_checks_;
    if (periodic_checks_ != nullptr) {
        periodic_checks_->prev_ = &registration;
    }
    periodic_checks_ = &registration;
}
void connection_scanner::entry::remove_periodic_check(
    periodic_check_registration& registration) noexcept {
    if (registration.entry_ != this) {
        return;
    }
    if (registration.prev_ != nullptr) {
        registration.prev_->next_ = registration.next_;
    } else {
        periodic_checks_ = registration.next_;
    }
    if (registration.next_ != nullptr) {
        registration.next_->prev_ = registration.prev_;
    }
    if (periodic_scan_next_ == &registration) {
        periodic_scan_next_ = registration.next_;
    }
    registration.entry_ = nullptr;
    registration.prev_ = registration.next_ = nullptr;
    registration.target_ = nullptr;
    registration.tick_ = nullptr;
    registration.drain_ = nullptr;
}
void connection_scanner::entry::detach_periodic_checks() noexcept {
    periodic_scan_next_ = nullptr;
    while (periodic_checks_ != nullptr) {
        periodic_checks_->reset();
    }
}
bool connection_scanner::entry::linked() const noexcept {
    return prev_ != nullptr && next_ != nullptr;
}
void connection_scanner::entry::sync_reusable() noexcept {
    const bool listed = reusable_next_ != nullptr;
    if (scanner_ == nullptr || reusable() == listed) {
        return;
    }
    if (listed) {
        unlink_reusable();
        return;
    }
    auto& sentinel = scanner_->impl_->reusable_sentinel_;
    reusable_prev_ = sentinel.reusable_prev_;
    reusable_next_ = &sentinel;
    sentinel.reusable_prev_->reusable_next_ = this;
    sentinel.reusable_prev_ = this;
}
void connection_scanner::entry::unlink_reusable() noexcept {
    if (reusable_next_ == nullptr) {
        return;
    }
    reusable_prev_->reusable_next_ = reusable_next_;
    reusable_next_->reusable_prev_ = reusable_prev_;
    reusable_prev_ = reusable_next_ = nullptr;
}
bool connection_scanner::entry::has_drain_check() const noexcept {
    for (const auto* registration = periodic_checks_; registration != nullptr; registration = registration->next_) {
        if (registration->drain_ != nullptr) {
            return true;
        }
    }
    return false;
}
void connection_scanner::entry::notify_drain() noexcept {
    // A callback may detach its own registration; read the successor first.
    for (auto* registration = periodic_checks_; registration != nullptr;) {
        auto* const next = registration->next_;
        if (registration->drain_ != nullptr) {
            registration->drain_(registration->target_);
        }
        registration = next;
    }
}
connection_scanner::guard::guard(
    connection_scanner& scanner, entry& entry_value, connection_transport transport_value)
    : entry_(entry_value) {
    scanner.impl_->register_entry(entry_, transport_value);
}
connection_scanner::guard::~guard() {
    connection_scanner::detach_entry(entry_);
}
connection_scanner::impl_type::impl_type(
    connection_scanner* owner_value, worker_ref worker_value, connection_scanner_options options)
    : owner_(owner_value),
      worker_(require_scanner_worker(std::move(worker_value))),
      timer_state_(std::make_shared<timer_state_type>(this)),
      options_(std::move(options)),
      cached_now_ms_(steady_now_ms()) {
    sentinel_.prev_ = sentinel_.next_ = &sentinel_;
    reusable_sentinel_.reusable_prev_ = reusable_sentinel_.reusable_next_ = &reusable_sentinel_;
}
bool connection_scanner::impl_type::retire() noexcept {
    // scanning_ is worker-local; only the worker thread may observe it. On that
    // thread a set flag means this call is nested inside the scan callback,
    // which already holds the timer mutex.
    const bool on_worker = worker_.is_current();
    const bool nested_in_scan = on_worker && scanning_;
    if (nested_in_scan) {
        stop();
        timer_state_->owner_ = nullptr;
        release_after_scan_ = true;
    } else if (on_worker) {
        stop();
        timer_state_->owner_ = nullptr;
    } else {
        // Another thread may be scanning; wait for that callback to finish.
        std::lock_guard lock(timer_state_->mutex_);
        stop();
        timer_state_->owner_ = nullptr;
    }
    // Unregistration keeps the scan cursors consistent, so a nested scan ends
    // without visiting any detached node.
    while (sentinel_.next_ != &sentinel_) {
        unregister_entry(*sentinel_.next_);
    }
    return nested_in_scan;
}

connection_scanner::connection_scanner(worker_ref worker_value, connection_scanner_options options)
    : impl_(std::make_unique<impl_type>(this, std::move(worker_value), std::move(options))) {}
connection_scanner::~connection_scanner() noexcept {
    if (impl_->retire()) {
        // The scan running further up this stack frees the impl.
        static_cast<void>(impl_.release());
    }
}
void connection_scanner::start() {
    impl_->start();
}
void connection_scanner::stop() noexcept {
    impl_->stop();
}
void connection_scanner::close_all() noexcept {
    impl_->close_all();
}
bool connection_scanner::evict_oldest_reusable() noexcept {
    auto& sentinel = impl_->reusable_sentinel_;
    for (auto* current = sentinel.reusable_next_; current != &sentinel; current = current->reusable_next_) {
        // A write still under way belongs to an exchange that only just
        // ended; its connection is not idle yet.
        if (current->write_pending()) {
            continue;
        }
        // Closed at once rather than asked to close: the new connection takes
        // the slot now. The close request keeps the entry out of the list
        // while its owner unwinds.
        if (current->lifecycle_ != nullptr) {
            current->lifecycle_->set_reason(connection_close_reason::aborted);
        }
        impl_type::record_close_request(*current, impl_->cached_now_ms_);
        current->transport_.close();
        return true;
    }
    return false;
}
void connection_scanner::detach_entry(entry& entry_value) noexcept {
    if (entry_value.scanner_ != nullptr) {
        entry_value.scanner_->impl_->unregister_entry(entry_value);
    }
}

void connection_scanner::impl_type::start() {
    if (running_) {
        return;
    }
    running_ = true;
    cached_now_ms_ = steady_now_ms();
    try {
        schedule();
    } catch (...) {
        running_ = false;
        throw;
    }
}
void connection_scanner::impl_type::stop() noexcept {
    running_ = false;
    timer_.cancel();
}
void connection_scanner::impl_type::register_entry(entry& entry_value, connection_transport transport_value) {
    if (entry_value.scanner_ != nullptr) {
        throw std::logic_error("connection scanner entry is already registered");
    }
    // A parked scanner resumes with its first entry. Armed before linking, so
    // a failure leaves the entry unregistered.
    if (running_ && !timer_.registered()) {
        schedule();
    }
    entry_value.transport_ = transport_value;
    entry_value.scanner_ = owner_;
    entry_value.touch();
    entry_value.phase_started_ms_ = entry_value.last_active_ms_;
    entry_value.phase_ = phase::idle;
    entry_value.close_requested_ = false;
    entry_value.next_ = sentinel_.next_;
    entry_value.prev_ = &sentinel_;
    sentinel_.next_->prev_ = &entry_value;
    sentinel_.next_ = &entry_value;
}
void connection_scanner::impl_type::unregister_entry(entry& entry_value) noexcept {
    if (entry_value.scanner_ != owner_ || !entry_value.linked()) {
        return;
    }
    if (entry_value.last_active_ms_ > retired_activity_ms_) {
        retired_activity_ms_ = entry_value.last_active_ms_;
    }
    if (scan_current_ == &entry_value) {
        scan_current_ = nullptr;
    }
    if (scan_next_ == &entry_value) {
        scan_next_ = entry_value.next_;
    }
    entry_value.prev_->next_ = entry_value.next_;
    entry_value.next_->prev_ = entry_value.prev_;
    entry_value.prev_ = entry_value.next_ = nullptr;
    entry_value.unlink_reusable();
    entry_value.transport_ = {};
    entry_value.detach_periodic_checks();
    entry_value.scanner_ = nullptr;
}
void connection_scanner::impl_type::close_all() noexcept {
    for (auto* current = sentinel_.next_; current != &sentinel_; current = current->next_) {
        if (current->lifecycle_ != nullptr) {
            current->lifecycle_->set_reason(connection_close_reason::cancelled);
        }
        current->transport_.close();
    }
}
void connection_scanner::impl_type::schedule() {
    if (!running_) {
        return;
    }
    const auto timer_state = timer_state_;
    // A stopped timer queue arms nothing: scanning ends with the worker, as it
    // does for a scan timer that stop_timers() cancels.
    (worker_).schedule_timer(timer_, ::ruvia::deadline_after(options_.scan_interval_.value()), [timer_state](worker_timer_outcome outcome) {
        if (outcome == worker_timer_outcome::cancelled) {
            return;
        }
        impl_type* destroy_after_lock = nullptr;
        {
            std::lock_guard lock(timer_state->mutex_);
            auto* scanner = timer_state->owner_;
            if (scanner == nullptr || !scanner->running_) {
                return;
            }
            // Expiry has consumed this timer. Release its token before callbacks,
            // which may stop, restart, or destroy the scanner.
            scanner->timer_.cancel();
            // 每轮采样一次比较时钟，供超时、协议检查和跨轮空闲判断共用。
            const auto interval_start = scanner->cached_now_ms_;
            scanner->cached_now_ms_ = steady_now_ms();
            scanner->scanning_ = true;
            const bool idle = scanner->scan(interval_start);
            scanner->scanning_ = false;
            if (scanner->release_after_scan_) {
                // A callback destroyed the scanner; its teardown already ran.
                destroy_after_lock = scanner;
            } else {
                // Never during an exchange or a burst: only after a whole interval
                // in which no entry carried a request exchange and none had activity
                // (I/O, registration or retirement), once per such quiet period. The
                // timer runs on the worker, whose thread heap this collects.
                if (idle && scanner->retired_activity_ms_ < interval_start) {
                    if (!scanner->quiet_trimmed_) {
                        collect_thread_heap();
                        scanner->quiet_trimmed_ = true;
                    }
                } else {
                    scanner->quiet_trimmed_ = false;
                }
                // With no entry left and the quiet period trimmed, nothing remains to
                // time: park until register_entry() re-arms the scan.
                const bool parked = scanner->quiet_trimmed_ && scanner->sentinel_.next_ == &scanner->sentinel_;
                if (!parked && !scanner->timer_.registered()) {
                    scanner->schedule();
                }
            }
        }
        if (destroy_after_lock != nullptr) {
            delete destroy_after_lock;
        }
    });
}
bool connection_scanner::impl_type::scan(std::int64_t interval_start) noexcept {
    const auto now = cached_now_ms_;
    bool idle = true;
    scan_next_ = sentinel_.next_;
    while (scan_next_ != &sentinel_) {
        scan_current_ = scan_next_;
        scan_next_ = scan_current_->next_;
        // Checked before this entry's callbacks, which may touch it at the new time.
        idle = idle && !active_since(*scan_current_, interval_start);
        scan_current_->periodic_scan_next_ = scan_current_->periodic_checks_;
        // A callback can retire the current entry or a later one. Unregistration
        // updates both scanner-owned cursors before the entry can be destroyed.
        while (scan_current_ != nullptr && scan_current_->periodic_scan_next_ != nullptr) {
            auto* registration = scan_current_->periodic_scan_next_;
            scan_current_->periodic_scan_next_ = registration->next_;
            registration->tick_(registration->target_, now);
        }
        if (scan_current_ != nullptr && is_timed_out(*scan_current_, now)) {
            expire(*scan_current_);
        }
        scan_current_ = nullptr;
    }
    scan_next_ = nullptr;
    return idle;
}
bool connection_scanner::impl_type::active_since(const entry& entry_value, std::int64_t interval_start) noexcept {
    // A connection waiting for, or still reading, a request head has no
    // exchange; neither has an upgraded one between messages, nor a
    // multiplexed session without a stream. Anything else (a handshake, an
    // admitted request, a response) is an exchange.
    return entry_value.last_active_ms_ >= interval_start || entry_value.write_pending() ||
           (!entry_value.reading_request_head_ && entry_value.phase_ != phase::long_lived &&
               entry_value.phase_ != phase::awaiting_stream);
}
void connection_scanner::impl_type::expire(entry& entry_value) noexcept {
    if (entry_value.lifecycle_ != nullptr) {
        entry_value.lifecycle_->set_reason(connection_close_reason::timeout);
    }
    if (!entry_value.close_requested_) {
        const auto now = cached_now_ms_;
        if (entry_value.reading_request_head_) {
            request_close(entry_value, close_deadline_after(now, options_.scan_interval_.value()));
            return;
        }
        // A stalled write cannot carry a graceful end (GOAWAY, close alert);
        // a write still progressing is followed by the drain's own output.
        if (entry_value.phase_ != phase::writing && !write_stalled(entry_value, now) &&
            entry_value.has_drain_check()) {
            // The callback may retire the entry; record the request first.
            record_close_request(entry_value, close_deadline_after(now, connection_close_grace));
            entry_value.notify_drain();
            return;
        }
    }
    entry_value.transport_.close();
}
void connection_scanner::impl_type::request_close(entry& entry_value, std::int64_t deadline_ms) noexcept {
    // The pending read completes with operation_aborted; its owner then ends
    // the connection with its close alert instead of an abrupt close. A TLS
    // read can lose the cancel (the ssl operation resumes reading a partial
    // record), so the deadline closes the socket of one that never returns.
    record_close_request(entry_value, deadline_ms);
    entry_value.transport_.cancel();
}
void connection_scanner::impl_type::record_close_request(entry& entry_value, std::int64_t deadline_ms) noexcept {
    // Repeated requests (a timeout, then a drain) keep the earliest deadline.
    if (!entry_value.close_requested_ || deadline_ms < entry_value.close_deadline_ms_) {
        entry_value.close_deadline_ms_ = deadline_ms;
    }
    entry_value.close_requested_ = true;
    entry_value.sync_reusable();
}
bool connection_scanner::impl_type::is_timed_out(const entry& entry_value, std::int64_t now) const noexcept {
    // A graceful close request is bounded by its own deadline, which later
    // activity (the GOAWAY or alert write it asked for) does not extend.
    if (entry_value.close_requested_) {
        return now >= entry_value.close_deadline_ms_;
    }
    if (write_stalled(entry_value, now)) {
        return true;
    }
    // 活动时间独立采样，不能把配置的扫描间隔当成调度延迟的上界。
    const auto inactive_ms = now - entry_value.last_active_ms_;
    const auto phase_elapsed_ms = now - entry_value.phase_started_ms_;
    switch (entry_value.phase_) {
        case phase::reading_initial:
            return timeout_expired(options_.initial_read_timeout_, inactive_ms) ||
                   timeout_expired(options_.initial_read_completion_timeout_, phase_elapsed_ms);
        case phase::reading_payload:
            return timeout_expired(options_.payload_read_timeout_, inactive_ms) ||
                   timeout_expired(options_.payload_read_completion_timeout_, phase_elapsed_ms);
        case phase::writing:
        case phase::long_lived:
            return false;
        case phase::awaiting_stream:
            return timeout_expired(options_.idle_timeout_, phase_elapsed_ms);
        case phase::idle:
        default:
            return timeout_expired(options_.idle_timeout_, inactive_ms);
    }
}
bool connection_scanner::impl_type::write_stalled(const entry& entry_value, std::int64_t now) const noexcept {
    return entry_value.write_pending() &&
           timeout_expired(options_.write_timeout_, now - entry_value.write_progress_ms_);
}

}  // namespace ruvia::detail
