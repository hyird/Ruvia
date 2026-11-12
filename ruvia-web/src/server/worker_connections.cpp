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
#include <asio/ip/v6_only.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/web/server_connection.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"
#include "integration/worker_capabilities.h"
#include "server/http_connection_close.h"
#include "server/http_connection_state.h"
#include "server/http_server_tls_contexts.h"
#include "server/peer_address.h"
#include "server/tcp_accept_error.h"
#include "tls/tls_peer.h"
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
    const std::optional<asio::ip::tcp::endpoint>& remote, std::pmr::string& address, std::uint16_t& port) {
    if (remote.has_value()) {
        assign_peer_address(address, remote->address());
        port = remote->port();
    }
}

}  // namespace

worker_connections::worker_connections(asio::io_context& io, const worker_handle& worker_value,
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
      listeners_(process_resource()),
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
      http3_endpoints_(process_resource())
#endif
{
    listeners_.reserve(listeners.size());
    for (const auto& configured : listeners) {
        listeners_.push_back(
            make_pmr_object<listener>(process_resource(), io, worker_value, configured, process_resource()));
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
    http3_endpoints_[http3_index] = make_pmr_object<http3_endpoint>(process_resource(), http3_services_,
        configured.session_,
        asio::ip::udp::endpoint(configured.endpoint_.address(), configured.session_.http3_->port_), http3_index,
        configured.tasks_);
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
    listeners_[index] = make_pmr_object<listener>(process_resource(), io_, worker_, definition, process_resource(), std::move(session));
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
    registry_.close_listener(&configured.session_);
    close_listener_sockets(configured);
    configured.retired_.notify();
}
void worker_connections::remove_listener(std::size_t index, std::shared_ptr<void> receipt, listener_remove_options options) noexcept {
    assert(worker_.is_current());
    // Not admitting: the worker's stop already retired it.
    if (index >= listeners_.size() || listeners_[index] == nullptr || !listeners_[index]->admitting_) {
        return;
    }
    auto& configured = *listeners_[index];
    configured.admitting_ = false;
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
                configured.socket_.async_accept(socket, asio::bind_allocator(std::pmr::polymorphic_allocator<std::byte>(process_resource()), std::move(handler)));
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
    accepted_connection_lease connection, std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id) {
    auto& socket = connection.socket();
    auto& lifecycle = connection.lifecycle();
    std::pmr::string remote_address(process_resource());
    std::uint16_t remote_port = 0;
    try {
        name_remote_endpoint(remote, remote_address, remote_port);
        if (!http_server_worker_running(state_) || !socket.is_open()) {
            co_return;
        }
        if (listener_value.connection_.has_value()) {
            co_await serve_connection(socket, socket,
                server_connection_fields{.id_ = connection_id,
                    .local_address_ = {},
                    .local_port_ = 0,
                    .remote_address_ = remote_address,
                    .remote_port_ = remote_port,
                    .tls_ = std::nullopt,
                    .state_ = &lifecycle.state()});
            ruvia::close_socket(socket);
            co_return;
        }
        const auto close_mode = co_await run_stream(listener_value, socket, socket,
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
        report_session_failure(socket, remote_address, std::current_exception());
    }
}

task<void> worker_connections::run_tls_session(http_server_session_config& listener_value,
    accepted_connection_lease connection, std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id) {
    auto& socket = connection.socket();
    auto& lifecycle = connection.lifecycle();
    std::pmr::string remote_address(process_resource());
    std::uint16_t remote_port = 0;
    try {
        name_remote_endpoint(remote, remote_address, remote_port);
        // A stopping worker admits no new request, and a handshake leads only to one.
        if (!http_server_worker_running(state_) || !socket.is_open()) {
            co_return;
        }
        connection_scanner::entry handshake_entry;
        std::optional<connection_scanner::guard> handshake_guard;
        handshake_guard.emplace(scanner_, handshake_entry, connection_transport::tcp(socket));
        handshake_entry.read_request_head(connection_scanner::phase::reading_initial);
        if (listener_value.accepts_plaintext()) {
            // Peek one byte under the same initial-read clocks used by TLS.
            // The TLS handshake below does not receive a fresh deadline.
            unsigned char first{};
            const auto completion = co_await ruvia::async_asio<std::size_t>([&socket, &first](auto handler) {
                socket.async_receive(asio::buffer(&first, 1), asio::socket_base::message_peek, std::move(handler));
            });
            if (completion.error_code() || completion.result() == 0 || handshake_entry.close_requested()) {
                co_return;
            }
            if (first != 0x16) {
                handshake_guard.reset();
                co_await run_session(listener_value, std::move(connection), remote, connection_id);
                co_return;
            }
        }
        // Declared before the TLS stream: its SSL borrows them while it lives.
        // Identity selection matches the local address; the ClientHello
        // observer names it.
        std::error_code local_error;
        const auto local = socket.local_endpoint(local_error);
        const auto local_address = local.address();
        std::pmr::string local_address_text(process_resource());
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
        server_tls_stream tls_stream(socket, *listener_value.tls_context_);
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
            ruvia::close_socket(socket);
            co_return;
        }
        // Never retain the handshake clocks while dispatching HTTP requests.
        handshake_guard.reset();
        std::pmr::string client_certificate(process_resource());
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
                    .state_ = &lifecycle.state()});
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

void worker_connections::adopt_connection(std::size_t index, native_socket native) {
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
    accept_socket(index, std::move(socket));
}

bool worker_connections::close_connection(std::uint64_t id) noexcept {
    assert(worker_.is_current());
    return registry_.close_connection(id);
}

void worker_connections::accept_socket(std::size_t listener_index, tcp_socket_type socket) {
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
    try {
        configure_accepted_socket(socket);
        std::error_code local_error;
        const auto local = socket.local_endpoint(local_error);
        if (local_error || !remote.has_value()) {
            return;
        }
        const auto connection_id = connection_ids_.next();
        connection_event event(connection_id, {local.address(), local.port()},
            {remote->address(), remote->port()}, false);
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
        accepted_connection_lease connection(std::move(socket), connection_count_, std::move(lifecycle));
        // The session borrows its listener, whose scope joins it before the
        // listener retires.
        auto& configured = *listeners_[listener_index];
        auto& listener_value = configured.session_;
        configured.tasks_.spawn(listener_value.tls() != nullptr
                                    ? run_tls_session(listener_value, std::move(connection), remote, connection_id)
                                    : run_session(listener_value, std::move(connection), remote, connection_id));
    } catch (...) {
        auto failure = std::current_exception();
        std::pmr::string remote_address(process_resource());
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
