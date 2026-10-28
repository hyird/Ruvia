#include "server/worker_connections.h"

#include <algorithm>
#include <array>
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
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/http/http_ascii.h"

#include "context/context_services.h"
#include "http2/cleartext_upgrade.h"
#include "integration/worker_capabilities.h"
#include "server/http_connection_close.h"
#include "server/http_server_connection_guards.h"
#include "server/http_server_stream_session.h"
#include "server/http_server_tls_handshake.h"
#include "server/http_server_tls_identity.h"
#include "server/windows_accept_domain.h"
#include "tls/tls_protocol_policy.h"
#include "tls/tls_served_certificate.h"
#include "util/tcp_socket_policies.h"

namespace ruvia::detail {
namespace {

void configure_accepted_socket(asio::ip::tcp::socket& socket) {
    if (const auto error = apply_tcp_socket_policies(
            socket, tcp_no_delay_policy::enable, tcp_keep_alive_policy::system_default)) {
        throw std::system_error(error, "failed to configure accepted TCP socket");
    }
}

void assign_remote_address(std::pmr::string& output, const asio::ip::address& address) {
    if (!address.is_v4()) {
        output = address.to_string();
        return;
    }
    const auto bytes_value = address.to_v4().to_bytes();
    std::array<char, 15> buffer;
    char* cursor_value = buffer.data();
    for (std::size_t i = 0; i < bytes_value.size(); ++i) {
        if (i != 0) {
            *cursor_value++ = '.';
        }
        const auto [ptr, ec] = std::to_chars(cursor_value, buffer.data() + buffer.size(), bytes_value[i]);
        if (ec != std::errc{}) {
            output = address.to_string();
            return;
        }
        cursor_value = ptr;
    }
    output.assign(buffer.data(), static_cast<std::size_t>(cursor_value - buffer.data()));
}

bool is_http2_alpn_selected(server_tls_stream& tls_stream) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int selected_length = 0;
    SSL_get0_alpn_selected(tls_stream.native_handle(), &selected, &selected_length);
    return selected_length == 2 && selected != nullptr && std::memcmp(selected, "h2", 2) == 0;
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
    // Every HTTP version this server speaks over TLS, in preference order;
    // RFC 7301 §3.2 reserves the alert for offers naming none of them.
    static constexpr unsigned char protocols[] = {
        2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1', 8, 'h', 't', 't', 'p', '/', '1', '.', '0'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols,
            static_cast<unsigned int>(sizeof(protocols)), in, in_length) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// RFC 6066 §3: a session never resumes under a server_name other than the one
// it was established with. Each connection's session ID context names its
// offered server_name (ASCII case-insensitively), and OpenSSL resumes a
// session -- TLS 1.2 by ticket or session ID, TLS 1.3 by PSK -- only when the
// contexts match, so any other name, or none, gets a full handshake (RFC 9846
// §4.6.1 permits declining such a PSK). Names longer than a context are bound
// by their SHA-256 digest.
int bind_session_to_server_name(SSL* ssl, int* alert, void*) noexcept {
    // Without server_name: one NUL byte, which no host name contains.
    std::array<unsigned char, SSL_MAX_SID_CTX_LENGTH> context{};
    std::size_t context_size = 1;
    const unsigned char* extension = nullptr;
    std::size_t extension_size = 0;
    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &extension, &extension_size) == 1) {
        // One host_name entry, the only shape OpenSSL accepts; it fails any
        // other shape later in this handshake, so the context is irrelevant.
        if (extension_size < 5 || static_cast<std::size_t>((extension[0] << 8) | extension[1]) != extension_size - 2 ||
            extension[2] != TLSEXT_NAMETYPE_host_name) {
            return SSL_CLIENT_HELLO_SUCCESS;
        }
        const auto name_size = static_cast<std::size_t>((extension[3] << 8) | extension[4]);
        if (name_size != extension_size - 5 || name_size > TLSEXT_MAXLEN_host_name) {
            return SSL_CLIENT_HELLO_SUCCESS;
        }
        if (name_size == 0) {
            // An empty name differs from an absent one: two NUL bytes.
            context_size = 2;
        } else if (name_size <= context.size()) {
            for (std::size_t i = 0; i < name_size; ++i) {
                context[i] = http_ascii_to_lower(extension[5 + i]);
            }
            context_size = name_size;
        } else {
            std::array<unsigned char, TLSEXT_MAXLEN_host_name> name;
            for (std::size_t i = 0; i < name_size; ++i) {
                name[i] = http_ascii_to_lower(extension[5 + i]);
            }
            unsigned int digest_size = 0;
            if (EVP_Digest(name.data(), name_size, context.data(), &digest_size, EVP_sha256(), nullptr) != 1 ||
                digest_size != context.size()) {
                *alert = SSL_AD_INTERNAL_ERROR;
                return SSL_CLIENT_HELLO_ERROR;
            }
            context_size = digest_size;
        }
    }
    if (SSL_set_session_id_context(ssl, context.data(), static_cast<unsigned int>(context_size)) != 1) {
        *alert = SSL_AD_INTERNAL_ERROR;
        return SSL_CLIENT_HELLO_ERROR;
    }
    return SSL_CLIENT_HELLO_SUCCESS;
}

// RFC 6066 SNI: switch the connection to the per-host SSL_CTX when the client's
// server name matches a configured certificate; otherwise keep the default
// identity. Answering an unrecognized name with the default certificate,
// rather than RFC 9325 §3.7's unrecognized_name alert, is a product choice
// documented on tls_config.
int select_sni_context(SSL* ssl, int*, void* arg) noexcept {
    if (ssl == nullptr || arg == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (name == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const std::string_view server_name(name);
    const auto& lookup = *static_cast<const sni_context_lookup_type*>(arg);
    for (const auto& [host, context] : lookup) {
        if (http_ascii_equals_ignore_case(host, server_name)) {
            // for a name that has its own certificate: fail the handshake.
            if (SSL_set_SSL_CTX(ssl, context->native_handle()) == nullptr) {
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
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
        assign_remote_address(address, remote_endpoint.address());
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
#if defined(_WIN32)
void worker_connections::attach_windows_accept(windows_accept_domain& domain, std::size_t origin) {
    domain.attach(origin, runtime_, {
                                        .target_ = this,
                                        .accepted_ = [](void* target, std::size_t index, tcp_socket_type socket) { static_cast<worker_connections*>(target)->accept_socket(index, std::move(socket)); },
                                        .failure_ = [](void* target, std::exception_ptr failure, bool fatal) noexcept { static_cast<worker_connections*>(target)->report_accept_failure(std::move(failure), fatal); },
                                    });
    windows_accept_ = &domain;
    windows_origin_ = origin;
}

task<void> worker_connections::run_windows_accepts() {
    co_await windows_accept_->run(windows_origin_);
    // No origin can issue, cancel, or update accept context using these HANDLEs.
    close_listeners();
}
#endif
void worker_connections::prepare() {
    try {
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            auto& configured = listeners_[index];
            configure_tls(configured->session_);
            auto& socket = configured->socket_;
            socket.open(configured->endpoint_.protocol());
            configure_listener_address(socket);
            socket.bind(configured->endpoint_);
            socket.listen(asio::socket_base::max_listen_connections);
#if defined(_WIN32)
            windows_accept_->stage_listener(windows_origin_, index, socket);
#endif
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
        if (std::ranges::any_of(listeners_, [](const auto& configured) { return configured->session_.ticket_keys_.has_value(); })) {
            tasks_.spawn(rotate_tls_ticket_keys());
        }
#if defined(_WIN32)
        windows_accept_->serve(windows_origin_);
#else
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            tasks_.spawn(run_accept_loop(index));
        }
#endif
    } catch (...) {
        report_accept_failure(std::current_exception(), true);
    }
}
void worker_connections::drain() noexcept {
#if defined(_WIN32)
    windows_accept_->request_stop();
#else
    close_listeners();
#endif
    drain_.begin();
    scanner_.drain();
}
void worker_connections::stop() noexcept {
#if defined(_WIN32)
    if (windows_accept_ != nullptr) {
        windows_accept_->request_stop();
    }
#else
    close_listeners();
#endif
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
        session.ticket_keys_.reset();
    }
}
// RFC 9325 §3.4: adopts each TLS listener's next ticket key epoch as it begins.
// It keeps running while the worker drains and ends when its timers stop, so
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
    result.active_connections_ = drain_.active();
    result.connections_refused_ = refused_.load(std::memory_order_relaxed);
    result.connection_failures_ = failures_.load(std::memory_order_relaxed);
    result.accept_failures_ = accept_failures_.load(std::memory_order_relaxed);
    return result;
}

void worker_connections::close_listeners() noexcept {
    for (auto& configured : listeners_) {
        asio::error_code ignored;
#if !defined(_WIN32)
        try {
            (void)configured->retry_.cancel();
        } catch (...) {
            ruvia::report_unhandled_failure("web accept retry cancellation", std::current_exception());
        }
#endif
        configured->socket_.close(ignored);
    }
}

#if !defined(_WIN32)
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
#if defined(_WIN32)
        windows_accept_->request_stop();
#else
        close_listeners();
#endif
        (void)runtime_.fail(std::move(failure));
    } else {
        ruvia::report_unhandled_failure("web accept", std::move(failure));
    }
}

void worker_connections::configure_tls(http_server_session_config& listener_value) {
    listener_value.tls_context_.reset();
    listener_value.sni_contexts_.clear();
    listener_value.sni_lookup_.clear();
    listener_value.ticket_keys_.reset();
    const auto* tls = listener_value.tls();
    if (tls == nullptr) {
        return;
    }
    auto& ticket_keys = listener_value.ticket_keys_.emplace();
    if (!tls_session_ticket_keys(*tls).copy_to(std::chrono::steady_clock::now(), ticket_keys)) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
    const auto configure = [tls, &ticket_keys](asio::ssl::context& context_value,
                               const tls_certificate& certificate) {
        context_value.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                                  asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                                  asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
        apply_tls_protocol_policy(*context_value.native_handle(), tls->max_outgoing_records_);
        // RFC 9325 §4.2.1: the server's order decides, whatever the client prefers.
        SSL_CTX_set_options(context_value.native_handle(), SSL_OP_CIPHER_SERVER_PREFERENCE);
        SSL_CTX_set_alpn_select_cb(context_value.native_handle(), select_alpn_protocol, nullptr);
        configure_http_server_tls_identity(
            context_value.native_handle(), certificate, tls->client_certificates_);
        ticket_keys.install(*context_value.native_handle());
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
    // The client hello callback runs before OpenSSL looks up the offered
    // session, which the server_name callback would be too late for.
    SSL_CTX_set_client_hello_cb(context_value.native_handle(), &bind_session_to_server_name, nullptr);
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
            connection_scanner::guard handshake_guard(scanner_, handshake_entry, socket);
            handshake_entry.read_request_head(connection_scanner::phase::reading_initial);
            const auto handshake_completion =
                co_await ruvia::async_asio(tls_server_handshake_initiator{&tls_stream});
            if (handshake_completion.error_code()) {
                ruvia::close_socket(socket);
                co_return;
            }
        }
        std::pmr::string client_certificate(memory_.allocator<char>());
        if (listener_value.tls()->client_certificates_.has_value()) {
            extract_tls_client_certificate(tls_stream.native_handle(), client_certificate);
        }
        tls_served_certificate served_certificate(SSL_get_certificate(tls_stream.native_handle()));
        const auto services = capabilities_.make_context_services(stop_token_)
                                  .with_tls_transport(remote_address, client_certificate, remote_port, served_certificate);
        const auto close_mode = is_http2_alpn_selected(tls_stream)
                                    ? co_await run_http2(tls_stream, socket, services)
                                    : co_await run_stream(listener_value, tls_stream, socket, services);
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

template <typename stream_type>
task<http_connection_close> worker_connections::run_http2(stream_type& stream, tcp_socket_type& socket,
    context_services services) {
    connection_scanner::entry scanner_entry;
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, socket);
    co_return co_await run_http2_server_session(http2_server_session_setup<stream_type>{
        .stream_ = stream, .socket_ = socket, .memory_ = memory_, .routes_ = routes_, .options_ = options_, .scanner_entry_ = scanner_entry, .services_ = services});
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
    assert(worker_.is_current());
    if (!http_server_worker_running(state_)) {
        return;
    }
    const auto& max_connections = options_.server_.max_connections_per_worker_;
    if (max_connections.has_value() && drain_.active() >= *max_connections) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    try {
        configure_accepted_socket(socket);
        accepted_connection_lease connection(std::move(socket), drain_);
        auto& listener_value = listeners_[listener_index]->session_;
        tasks_.spawn(listener_value.tls() != nullptr ? run_tls_session(listener_value, std::move(connection))
                                                     : run_session(listener_value, std::move(connection)));
    } catch (...) {
        auto failure = std::current_exception();
        std::pmr::string remote_address(memory_.allocator<char>());
        std::uint16_t remote_port = 0;
        try {
            resolve_remote_endpoint(socket, remote_address, remote_port);
        } catch (...) {
            remote_address.clear();
        }
        accept_failures_.fetch_add(1, std::memory_order_relaxed);
        options_.connection_failure_.invoke(remote_address, std::move(failure));
    }
}

}  // namespace ruvia::detail
