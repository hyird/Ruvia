#include "server/worker_connections.h"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory_resource>
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
#include "ruvia/core/worker_runtime.h"
#include "ruvia/http/http_ascii.h"

#include "context/context_services.h"
#include "integration/worker_capabilities.h"
#include "server/accepted_socket.h"
#include "server/http_server_connection_guards.h"
#include "server/http_server_stream_session.h"
#include "server/http_server_tls_handshake.h"
#include "server/http_server_tls_identity.h"

namespace ruvia::detail {
namespace {

enum class accept_error_action { retry,
    report_and_retry,
    fail };

// Peer disappearance and readiness races are normal accept outcomes. Resource
// and network failures leave the listener usable but must remain observable.
accept_error_action classify_accept_error(const asio::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset ||
        error == asio::error::eof || error == asio::error::try_again ||
        error == asio::error::would_block || error == asio::error::interrupted) {
        return accept_error_action::retry;
    }
#if defined(_WIN32)
    if (error.category() == asio::system_category() && error.value() == ERROR_NETNAME_DELETED) {
        return accept_error_action::retry;
    }
#endif
    if (error == asio::error::no_descriptors || error == asio::error::no_buffer_space ||
        error == asio::error::no_memory || error == std::errc::too_many_files_open_in_system ||
        error == std::errc::protocol_error || error == asio::error::network_down ||
        error == asio::error::network_unreachable || error == asio::error::host_unreachable ||
        error == asio::error::no_protocol_option || error == asio::error::operation_not_supported ||
        error == asio::error::no_permission || error == asio::error::timed_out) {
        return accept_error_action::report_and_retry;
    }
#if defined(EHOSTDOWN) && defined(ENONET)
    if (error.category() == asio::system_category() && (error.value() == EHOSTDOWN || error.value() == ENONET)) {
        return accept_error_action::report_and_retry;
    }
#endif
    return accept_error_action::fail;
}

void configure_listener_address(asio::ip::tcp::acceptor& socket) {
    socket.set_option(asio::socket_base::reuse_address(true));
#if !defined(_WIN32)
#if defined(SO_REUSEPORT)
    const int enabled = 1;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable TCP listener SO_REUSEPORT");
    }
#else
    throw std::system_error(asio::error::operation_not_supported, "TCP worker listeners require SO_REUSEPORT");
#endif
#endif
}

int select_alpn_protocol(SSL*, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void*) noexcept {
    static constexpr unsigned char protocols[] = {
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols,
            static_cast<unsigned int>(sizeof(protocols)), in, in_length) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    // RFC 7301 §3.2: a client that offers ALPN without http/1.1 must get a
    // fatal no_application_protocol alert (OpenSSL sends it for this result),
    // not a handshake that silently proceeds with HTTP/1.1.
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// RFC 6066 SNI: switch the connection to the per-host SSL_CTX when the client's
// server name matches a configured certificate; otherwise keep the default.
int select_sni_context(SSL* ssl, int*, void* arg) noexcept {
    if (ssl == nullptr || arg == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (name == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const auto& lookup = *static_cast<const sni_context_lookup_type*>(arg);
    for (const auto& [host, context] : lookup) {
        if (http_ascii_equals_ignore_case(host, name)) {
            SSL_set_SSL_CTX(ssl, context->native_handle());
            break;
        }
    }
    return SSL_TLSEXT_ERR_OK;
}

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
// empty when the socket no longer knows its peer.
void resolve_remote_endpoint(tcp_socket_type& socket, std::pmr::string& address, std::uint16_t& port) {
    std::error_code remote_ec;
    const auto remote_endpoint = socket.remote_endpoint(remote_ec);
    if (!remote_ec) {
        ruvia::assign_remote_address(address, remote_endpoint.address());
        port = remote_endpoint.port();
    }
}

}  // namespace

worker_connections::worker_connections(asio::io_context& io, const worker_handle& worker_value,
    worker_memory& memory, const route_table& routes_value, worker_capabilities& capabilities,
    http_server_options& options, const stop_token& stop_token_value, http_server_worker_state& state_value,
    task_scope& tasks, std::span<const http_server_listener_definition> listeners, worker_runtime& runtime)
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
      work_sets_(memory) {
    listeners_.reserve(listeners.size());
    for (const auto& configured : listeners) {
        listeners_.push_back(make_pmr_object<listener>(memory.resource(), io, configured, memory.resource()));
    }
    options_.connection_failure_.counter_ = &failures_;
}
void worker_connections::prepare() {
    try {
        for (auto& configured : listeners_) {
            configure_tls(configured->session_);
            auto& socket = configured->socket_;
            socket.open(configured->endpoint_.protocol());
            configure_listener_address(socket);
            socket.bind(configured->endpoint_);
            socket.listen(asio::socket_base::max_listen_connections);
        }
        scanner_.start();
    } catch (...) {
        close_listeners();
        scanner_.stop();
        retire_tls();
        throw;
    }
}
void worker_connections::open_admission() noexcept {
    drain_.open();
    try {
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            tasks_.spawn(run_accept_loop(index));
        }
    } catch (...) {
        report_accept_failure(std::current_exception(), true);
    }
}
void worker_connections::drain() noexcept {
    close_listeners();
    drain_.begin();
    scanner_.drain();
}
void worker_connections::stop() noexcept {
    close_listeners();
    scanner_.stop();
    scanner_.close_all();
    drain_.complete();
}
bool worker_connections::wait_until_drained(std::chrono::steady_clock::time_point deadline) {
    return drain_.wait_until(deadline);
}
void worker_connections::retire_tls() noexcept {
    for (auto& configured : listeners_) {
        auto& session = configured->session_;
        session.tls_context_.reset();
        session.sni_lookup_.clear();
        session.sni_contexts_.clear();
    }
}
http_server_stats worker_connections::stats() const noexcept {
    http_server_stats result;
    result.active_connections_ = drain_.active();
    result.connections_refused_ = refused_.load(std::memory_order_relaxed);
    result.connection_failures_ = failures_.load(std::memory_order_relaxed);
    result.accept_failures_ = accept_failures_.load(std::memory_order_relaxed);
    return result;
}

void worker_connections::close_listeners() noexcept {
    for (auto& configured : listeners_) {
        asio::error_code ignored;
        configured->retry_.cancel(ignored);
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

void worker_connections::report_accept_failure(std::exception_ptr failure, bool fatal) noexcept {
    accept_failures_.fetch_add(1, std::memory_order_relaxed);
    if (fatal) {
        close_listeners();
        (void)runtime_.fail(std::move(failure));
    } else {
        ruvia::report_unhandled_failure("web accept", std::move(failure));
    }
}

void worker_connections::configure_tls(http_server_session_config& listener_value) {
    listener_value.sni_contexts_.clear();
    listener_value.sni_lookup_.clear();
    const auto* tls = listener_value.tls();
    if (tls == nullptr) {
        listener_value.tls_context_.reset();
        return;
    }
    const auto configure = [tls](asio::ssl::context& context_value,
                               const tls_certificate& certificate) {
        context_value.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                                  asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                                  asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
        SSL_CTX_set_options(context_value.native_handle(), SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_alpn_select_cb(context_value.native_handle(), select_alpn_protocol, nullptr);
        configure_http_server_tls_identity(
            context_value.native_handle(), certificate, tls->client_certificates_);
        install_tls_session_ticket_keys(*context_value.native_handle(), *tls);
    };

    // Per-host SNI certificates first, so the lookup can point at stable storage.
    listener_value.sni_contexts_.reserve(tls->sni_identities_.size());
    for (const auto& sni : tls->sni_identities_) {
        auto& context_value = listener_value.sni_contexts_.emplace_back(asio::ssl::context::tls_server);
        configure(context_value, sni.certificate_);
    }
    listener_value.sni_lookup_.reserve(tls->sni_identities_.size());
    for (std::size_t i = 0; i < tls->sni_identities_.size(); ++i) {
        listener_value.sni_lookup_.emplace_back(tls->sni_identities_[i].host_, &listener_value.sni_contexts_[i]);
    }

    listener_value.tls_context_.emplace(asio::ssl::context::tls_server);
    auto& context_value = *listener_value.tls_context_;
    configure(context_value, tls->certificate_);
    if (!listener_value.sni_lookup_.empty()) {
        SSL_CTX_set_tlsext_servername_callback(context_value.native_handle(), &select_sni_context);
        SSL_CTX_set_tlsext_servername_arg(context_value.native_handle(), &listener_value.sni_lookup_);
    }
}

// Plain and TLS sessions are separate coroutines so a plain connection's frame
// carries none of the TLS stream, handshake and certificate state. Both keep
// the remote address outside their try so the failure report can name the
// peer; it stays empty if the failure happened before the address was resolved.
task<void> worker_connections::run_session(
    http_server_session_config& listener_value, accepted_connection_lease connection) {
    auto& socket = connection.socket();
    std::pmr::string remote_address(memory_.allocator<char>());
    std::uint16_t remote_port = 0;
    try {
        resolve_remote_endpoint(socket, remote_address, remote_port);
        const auto close_mode = co_await run_stream(listener_value, socket, socket,
            capabilities_.make_context_services(stop_token_).with_plain_transport(remote_address, remote_port));
        std::error_code ignored;
        if (close_mode == http_connection_close::after_response && !stop_token_.stop_requested()) {
            // Closing after the final response satisfies RFC 9112 §9.3 without
            // reading the remaining request body. No peer FIN is awaited.
            socket.shutdown(tcp_socket_type::shutdown_send, ignored);
        }
        // Do not keep the descriptor alive until deferred coroutine-frame retirement.
        socket.close(ignored);
    } catch (...) {
        report_session_failure(socket, remote_address, std::current_exception());
    }
}

task<void> worker_connections::run_tls_session(
    http_server_session_config& listener_value, accepted_connection_lease connection) {
    auto& socket = connection.socket();
    std::pmr::string remote_address(memory_.allocator<char>());
    std::uint16_t remote_port = 0;
    try {
        resolve_remote_endpoint(socket, remote_address, remote_port);
        // A draining worker admits no new request, and a handshake leads only to one.
        if (!http_server_worker_running(state_)) {
            co_return;
        }
        asio::ssl::stream<tcp_socket_type&> tls_stream(socket, *listener_value.tls_context_);
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
            connection_scanner::entry_type handshake_entry;
            connection_scanner::guard_type handshake_guard(scanner_, handshake_entry, socket);
            handshake_entry.read_request_head(connection_scanner::phase_type::reading_initial);
            const auto handshake_completion =
                co_await ruvia::async_asio(tls_server_handshake_initiator{&tls_stream});
            if (handshake_completion.error_code()) {
                ruvia::close_socket(socket);
                co_return;
            }
        }
        std::pmr::string client_certificate(memory_.allocator<char>());
        extract_tls_client_certificate(tls_stream.native_handle(), client_certificate);
        const auto close_mode = co_await run_stream(listener_value, tls_stream, socket,
            capabilities_.make_context_services(stop_token_)
                .with_tls_transport(remote_address, client_certificate, remote_port));
        // A worker stop keeps the immediate close; scanner close_all reaches a
        // graceful close that is already running.
        if (close_mode != http_connection_close::abort && !stop_token_.stop_requested()) {
            co_await close_http_connection_gracefully(tls_stream, scanner_, worker_);
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

void worker_connections::accept_socket(std::size_t listener_index, tcp_socket_type socket) {
    const auto& max_connections = options_.server_.max_connections_per_worker_;
    if (max_connections.has_value() && drain_.active() >= *max_connections) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    try {
        ruvia::configure_accepted_socket(socket);
        accepted_connection_lease connection(std::move(socket), drain_);
        auto& listener_value = listeners_[listener_index]->session_;
        tasks_.spawn(listener_value.tls() != nullptr ? run_tls_session(listener_value, std::move(connection))
                                                     : run_session(listener_value, std::move(connection)));
    } catch (...) {
        accept_failures_.fetch_add(1, std::memory_order_relaxed);
        options_.connection_failure_.invoke({}, std::current_exception());
    }
}

}  // namespace ruvia::detail
