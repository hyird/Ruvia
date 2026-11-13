#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/append.hpp>
#include <asio/associator.hpp>
#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/detail/handler_cont_helpers.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/types.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/http_client.h"

#include "context_access.h"
#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/http2_connection.h"
#endif
#ifdef RUVIA_ENABLE_HTTP3
#include "ruvia/http/http3_connection.h"
#endif
#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/http2_connection.h"
#endif
#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/http2_connection.h"
#endif

#include <cmath>
#include <coroutine>
#include <cstring>
#include <ranges>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "ruvia/web/db/db.h"

#ifdef _WIN32
#include <winsock2.h>

#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <charconv>
#include <condition_variable>
#include <filesystem>
#include <initializer_list>
#include <iosfwd>
#include <mutex>
#include <unordered_map>

#include <asio/cancellation_signal.hpp>
#include <asio/fwd.hpp>
#include <asio/ip/udp.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/http/http3_connection.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_server.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/streaming.h"

namespace ruvia::detail {

struct client_resolve_result final {
    asio::ip::tcp::resolver::results_type addresses_{};
    // Numeric hosts do not need a heap-backed resolver result.
    std::optional<asio::ip::tcp::endpoint> literal_{};
    std::error_code error_{};
};

using client_resolve_operation = std::variant<client_resolve_result, task<client_resolve_result>>;

// False for an IP literal, which connects without a lookup.
[[nodiscard]] bool client_host_needs_lookup(std::string_view host);

// A DNS host is looked up on the blocking pool of the client's event loop;
// without one the client cannot connect, so it is rejected at construction.
void require_client_host_lookup(std::string_view host, const worker_ref& worker);

// HTTP、WebSocket、Redis 和数据库共用的系统名称查询。系统 getaddrinfo 不可
// 抢占；查询卸载到客户端事件循环借用的有界阻塞池，只携带自有输入，取消或停机先
// 释放等待者，再丢弃迟到结果。IP 字面量不查询，也不需要池。
class client_host_resolver final {
public:
    explicit client_host_resolver(const worker_ref& worker) noexcept
        : worker_(&worker) {}
    client_host_resolver(const client_host_resolver&) = delete;
    client_host_resolver& operator=(const client_host_resolver&) = delete;
    client_host_resolver(client_host_resolver&& other) noexcept;
    client_host_resolver& operator=(client_host_resolver&& other) noexcept;

    [[nodiscard]] client_resolve_operation resolve(std::string_view host, std::uint16_t port,
        const operation_deadline& timeout, stop_token stop = {});
    void cancel() noexcept;

private:
    struct query;
    [[nodiscard]] task<client_resolve_result> resolve_query(query input, operation_deadline timeout, stop_token stop);
    const worker_ref* worker_;
    std::optional<stop_source> stop_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {
inline constexpr std::chrono::milliseconds client_connection_attempt_delay{250};

// Traverse RFC 6724 resolver order while alternating families (RFC 8305 §4).
// The resolver owns the addresses; traversal and duplicate suppression allocate
// nothing, allowing a bounded transport race to construct candidates lazily.
template <typename results_type>
class client_address_sequence final {
public:
    using endpoint_type = std::remove_cvref_t<decltype(std::declval<const results_type&>().begin()->endpoint())>;
    explicit client_address_sequence(const results_type& addresses)
        : addresses_(addresses),
          first_(addresses.begin()),
          other_(addresses.begin()),
          first_is_v6_(!addresses.empty() && addresses.begin()->endpoint().address().is_v6()),
          pending_(advance()) {}
    client_address_sequence(results_type&&) = delete;

    [[nodiscard]] bool has_next() const noexcept {
        return pending_.has_value();
    }

    [[nodiscard]] std::optional<endpoint_type> next() {
        auto result = std::move(pending_);
        if (result.has_value()) {
            pending_ = advance();
        }
        return result;
    }

private:
    [[nodiscard]] std::optional<endpoint_type> advance() {
        for (unsigned family = 0; family != 2; ++family) {
            auto& iterator = next_first_ ? first_ : other_;
            const bool is_v6 = next_first_ ? first_is_v6_ : !first_is_v6_;
            next_first_ = !next_first_;
            while (iterator != addresses_.end()) {
                const auto current = iterator++;
                const auto endpoint = current->endpoint();
                if (endpoint.address().is_v6() != is_v6) {
                    continue;
                }
                bool repeated = false;
                for (auto previous = addresses_.begin(); previous != current; ++previous) {
                    if (previous->endpoint() == endpoint) {
                        repeated = true;
                        break;
                    }
                }
                if (!repeated) {
                    return endpoint;
                }
            }
        }
        return std::nullopt;
    }

    const results_type& addresses_;
    decltype(std::declval<const results_type&>().begin()) first_;
    decltype(std::declval<const results_type&>().begin()) other_;
    bool first_is_v6_;
    bool next_first_{true};
    std::optional<endpoint_type> pending_;
};

struct client_happy_eyeballs_result final {
    std::optional<asio::ip::tcp::socket> socket_;
    std::error_code error_;
};

// RFC 8305 §4-5 connection-attempt phase. The platform resolver supplies the
// RFC 6724 sorted list; this operation interleaves address families, staggers
// attempts, keeps earlier attempts alive, and joins every loser after the first
// success or cancellation.
class client_happy_eyeballs final {
public:
    client_happy_eyeballs(const worker_ref& worker, const asio::ip::tcp::socket::executor_type& executor,
        const asio::ip::tcp::resolver::results_type& addresses);
    client_happy_eyeballs(worker_ref&&, const asio::ip::tcp::socket::executor_type&,
        const asio::ip::tcp::resolver::results_type&) = delete;
    ~client_happy_eyeballs();
    client_happy_eyeballs(const client_happy_eyeballs&) = delete;
    client_happy_eyeballs& operator=(const client_happy_eyeballs&) = delete;

    [[nodiscard]] task<client_happy_eyeballs_result> connect() &;
    task<client_happy_eyeballs_result> connect() && = delete;
    void cancel() noexcept;

private:
    struct attempt final {
        attempt(const asio::ip::tcp::socket::executor_type& executor, asio::ip::tcp::endpoint endpoint)
            : socket_(executor),
              endpoint_(std::move(endpoint)) {}
        attempt(const attempt&) = delete;
        attempt& operator=(const attempt&) = delete;
        attempt(attempt&&) noexcept = default;
        attempt& operator=(attempt&&) noexcept = default;

        asio::ip::tcp::socket socket_;
        asio::ip::tcp::endpoint endpoint_;
        bool active_{false};
    };

    void start_next() noexcept;
    void schedule_next() noexcept;
    void on_delay(worker_timer_outcome outcome) noexcept;
    void on_connect(std::size_t index, const std::error_code& error) noexcept;
    void close_active() noexcept;
    static void close_socket(asio::ip::tcp::socket& socket) noexcept;

    const worker_ref& worker_;
    worker_signal changed_;
    std::vector<attempt> attempts_;
    std::size_t next_{0};
    std::size_t active_{0};
    std::optional<std::size_t> winner_;
    std::exception_ptr failure_;
    std::error_code last_error_;
    bool launch_ready_{false};
    bool cancelled_{false};
    worker_timer_registration delay_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class client_phase : unsigned char { fresh,
    connecting,
    connected,
    closing,
    closed };

// Composed by an address-stable protocol owner, after its backend and memory.
// Owns the sole standalone-client startup/cancel/retirement chain. Backend
// connect/close/join and the not-ready error are explicit compile-time policies.
// No type erasure, extra sharing, or locking is added to operation admission.
template <typename owner_type>
class client_lifecycle final {
public:
    client_lifecycle(owner_type& owner_value, const event_loop& loop, const worker_ref& worker_value,
        client_phase initial = client_phase::fresh)
        : owner_(owner_value),
          loop_(loop),
          worker_(worker_value),
          close_signal_(worker_value),
          phase_(initial) {
        operations_.bind_worker(worker_value);
    }

    ~client_lifecycle() {
        if (phase_.load(std::memory_order_acquire) != client_phase::closed ||
            !close_complete_ || operations_.has_pending_operations()) {
            std::terminate();
        }
    }

    client_lifecycle(const client_lifecycle&) = delete;
    client_lifecycle& operator=(const client_lifecycle&) = delete;

    [[nodiscard]] static event_loop require_loop(event_loop loop) {
        if (!loop.worker().valid()) {
            throw std::invalid_argument(message(" requires a valid event loop"));
        }
        return loop;
    }

    void bind_stop() {
        try {
            std::weak_ptr<owner_type> weak = owner_.shared_from_this();
            stop_registration_ = loop_.on_stop([weak = std::move(weak)]() -> task<void> {
                if (const auto state = weak.lock()) {
                    co_await shutdown_owned(state, shutdown_mode::retirement);
                }
            });
        } catch (...) {
            owner_.backend().close_now();
            phase_.store(client_phase::closed, std::memory_order_release);
            // No client or stop callback has been published yet.
            if (close_started_ || close_complete_) {
                std::terminate();
            }
            close_complete_ = true;
            throw;
        }
    }

    [[nodiscard]] task<void> connect() {
        return connect_owned(owner_.shared_from_this());
    }

    void require_ready() const {
        require_current(" must be used on its bound event loop");
        if (phase_.load(std::memory_order_acquire) != client_phase::connected) {
            owner_type::throw_not_ready();
        }
    }

    [[nodiscard]] ::ruvia::operation_scope& operation_scope() noexcept {
        return operations_;
    }

    [[nodiscard]] stop_token token() const noexcept {
        return stop_.token();
    }

    void request_close() noexcept {
        stop_.request_stop();
        if (!begin_close()) {
            return;
        }
        if (worker_.is_current()) {
            start_close();
            return;
        }
        try {
            if (!loop_.defer_cleanup([state = owner_.shared_from_this()] { state->lifecycle_.start_close(); }) &&
                phase_.load(std::memory_order_acquire) != client_phase::closed) {
                std::terminate();
            }
        } catch (...) {
            if (phase_.load(std::memory_order_acquire) != client_phase::closed) {
                std::terminate();
            }
        }
    }

    [[nodiscard]] task<void> shutdown() {
        return shutdown_owned(owner_.shared_from_this(), shutdown_mode::caller);
    }

private:
    enum class shutdown_mode : unsigned char { caller,
        retirement };

    [[nodiscard]] static std::string message(std::string_view suffix) {
        std::string result(owner_type::client_name);
        result.append(" client");
        result.append(suffix);
        return result;
    }

    void require_current(std::string_view suffix) const {
        if (!worker_.is_current()) {
            throw std::logic_error(message(suffix));
        }
    }

    [[nodiscard]] static task<void> connect_owned(std::shared_ptr<owner_type> state_value) {
        co_await state_value->lifecycle_.connect_on_worker();
    }

    task<void> connect_on_worker() {
        require_current(" must connect on its bound event loop");
        auto expected = client_phase::fresh;
        if (!phase_.compare_exchange_strong(expected, client_phase::connecting,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (expected == client_phase::closing || expected == client_phase::closed) {
                throw std::runtime_error(message(" closed before connecting"));
            }
            throw std::logic_error(message(" can only connect once"));
        }
        // Only the startup lease holder may tear down a failed connection.
        try {
            connecting_ = true;
            if (stop_.stop_requested() || !worker_.accepting()) {
                throw std::runtime_error(message(" closed before connecting"));
            }
            co_await owner_.backend().connect();
            expected = client_phase::connecting;
            if (!phase_.compare_exchange_strong(expected, client_phase::connected,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                throw std::runtime_error(message(" stopped while connecting"));
            }
            connecting_ = false;
            close_signal_.notify();
        } catch (...) {
            owner_.backend().close_now();
            stop_.request_stop();
            phase_.store(client_phase::closed, std::memory_order_release);
            connecting_ = false;
            if (close_started_) {
                close_signal_.notify();
            } else if (!close_complete_) {
                close_complete_ = true;
                close_signal_.notify();
            }
            throw;
        }
    }

    [[nodiscard]] bool begin_close() noexcept {
        auto phase = phase_.load(std::memory_order_acquire);
        while (phase != client_phase::closed && phase != client_phase::closing) {
            if (phase_.compare_exchange_weak(phase, client_phase::closing,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static task<void> shutdown_owned(std::shared_ptr<owner_type> state_value,
        shutdown_mode mode) {
        auto& lifecycle = state_value->lifecycle_;
        if (!lifecycle.loop_.worker().is_current()) {
            throw std::logic_error("client shutdown must run on its bound event loop");
        }
        // The coroutine frame retains the client until its cleanup completes.
        lifecycle.start_close();
        while (!lifecycle.close_complete_) {
            co_await lifecycle.close_signal_.wait();
        }
        if (mode == shutdown_mode::caller && lifecycle.close_failure_) {
            std::rethrow_exception(lifecycle.close_failure_);
        }
    }

    void start_close() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        (void)begin_close();
        stop_.request_stop();
        owner_.backend().close_now();
        if (close_started_ || close_complete_) {
            return;
        }
        close_started_ = true;
        try {
            ruvia::async_start(close_on_worker(), asio::bind_executor(loop_.executor(), [state = owner_.shared_from_this()](std::exception_ptr failure) {
                state->lifecycle_.finish_close(std::move(failure));
            }));
        } catch (...) {
            std::terminate();
        }
    }

    task<void> close_on_worker() {
        while (connecting_) {
            co_await close_signal_.wait();
        }
        std::exception_ptr failure;
        if constexpr (requires { owner_.backend().join(); }) {
            try {
                co_await owner_.backend().join();
            } catch (...) {
                failure = std::current_exception();
            }
        }
        try {
            co_await operations_.close_and_join();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

    void finish_close(std::exception_ptr failure) noexcept {
        if (!close_started_ || close_complete_ || connecting_ || operations_.has_pending_operations()) {
            std::terminate();
        }
        phase_.store(client_phase::closed, std::memory_order_release);
        close_failure_ = std::move(failure);
        close_complete_ = true;
        close_signal_.notify();
        if (close_failure_) {
            loop_.report_failure(close_failure_);
        }
    }

    owner_type& owner_;
    const event_loop& loop_;
    const worker_ref& worker_;
    stop_source stop_;
    event_loop_stop_registration stop_registration_;
    worker_signal close_signal_;
    bool close_started_{};
    bool close_complete_{};
    std::exception_ptr close_failure_;
    std::atomic<client_phase> phase_;
    bool connecting_{};
    // Last: invalidate cold operations before backend and allocator retirement.
    ::ruvia::operation_scope operations_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline bool is_client_ip_address(std::string_view host) noexcept {
    return ::ruvia::is_valid_http_ipv4_literal(host) || ::ruvia::is_valid_http_ipv6_literal(host);
}

// A client transport host, also used for TLS SNI and peer verification: an
// unbracketed IP literal or an ASCII DNS host name.
inline void validate_client_origin_host(
    std::string_view host, const char* empty_message, const char* invalid_message) {
    ensure_config_host(host, empty_message, invalid_message);
    if (!is_client_ip_address(host) && !is_valid_dns_host(host)) {
        throw std::invalid_argument(invalid_message);
    }
}

// File paths are validated when the storage is built, before any file is read.
inline void validate_client_tls_config(const client_tls_config& config) {
    validate_tls_record_limit(config.max_outgoing_records_);
    if (config.mode_ != client_tls_mode::verify_identity && config.mode_ != client_tls_mode::disabled) {
        throw std::invalid_argument("invalid client TLS mode");
    }
    switch (config.minimum_version_) {
        case tls_version::tls_1_1:
        case tls_version::tls_1_2:
        case tls_version::tls_1_3:
            break;
        default:
            throw std::invalid_argument("invalid client TLS minimum version");
    }
    if (config.server_name_.has_value()) {
        if (config.server_name_->find('\0') != std::string_view::npos) {
            throw std::invalid_argument("client TLS configuration must not contain NUL bytes");
        }
        // The override replaces the origin host for SNI and peer verification.
        validate_client_origin_host(*config.server_name_, "TLS server name must not be empty", "TLS server name is invalid");
    }
    if (config.mode_ == client_tls_mode::disabled &&
        (config.ca_file_.has_value() || config.identity_.has_value() || config.server_name_.has_value())) {
        throw std::invalid_argument("disabled TLS must not have TLS credentials or a server name");
    }
    // Database drivers read identity files themselves; every client keeps
    // the one file-based shape.
    if (config.identity_.has_value() &&
        (!config.identity_->certificate_chain_pem_.empty() || !config.identity_->private_key_pem_.empty())) {
        throw std::invalid_argument("client TLS identities must be PEM files, not in-memory PEM");
    }
}

// Whether a client storage parses its TLS files. Owners that build their own
// SSL_CTXs parse once at startup so every context shares the result; database
// drivers read the paths themselves and plaintext clients never use them.
enum class client_tls_files : std::uint8_t {
    keep_paths,
    parse,
};

// Startup-owned copy of a validated client_tls_config. The private key
// password is used only while parsing and is not retained.
struct client_tls_config_storage final {
    // Validates the file paths. With client_tls_files::parse and TLS enabled,
    // also reads the trust store and the client identity (when configured);
    // throws when either cannot be loaded.
    client_tls_config_storage(
        const client_tls_config& source_value, client_tls_files files);

    // Copies share the parsed trust store and identity; no file is re-read.
    client_tls_config_storage(const client_tls_config_storage& source_value);
    client_tls_config_storage(client_tls_config_storage&&) noexcept = default;
    client_tls_config_storage& operator=(client_tls_config_storage&&) noexcept = default;
    // Reads the client identity, and the trust store unless one was supplied.
    void parse_files(std::string_view private_key_password);

    // SNI and verified peer name: the configured override, else the origin host.
    [[nodiscard]] std::string_view tls_host(std::string_view configured_host) const noexcept {
        return server_name_.has_value() ? std::string_view(*server_name_) : configured_host;
    }

    client_tls_mode mode_;
    tls_version minimum_version_;
    std::uint64_t max_outgoing_records_;
    // nullopt selects the system or driver trust store.
    std::optional<tls_file_name> ca_file_;
    std::optional<tls_identity_files> identity_;
    std::optional<std::string> server_name_;
    // Parsed by client_tls_files::parse: the trust store and the configured
    // client identity.
    std::optional<tls_trust_store> trust_store_;
    std::optional<tls_certificate> certificate_;
};

// Owns paths and password before returning the cold task. Blocking work owns
// only TLS material, never a client's worker-affine configuration or callbacks.
// A started parser is joined even after cancellation; callers reject publication.
[[nodiscard]] task<client_tls_config_storage> prepare_client_tls_config(
    worker_ref worker, client_tls_config_storage config,
    std::string_view private_key_password, stop_token stop);

enum class client_tls_setup_error : std::uint8_t {
    none,
    reset_failed,
    sni_failed,
    alpn_failed,
    peer_identity_failed,
    protocol_failed,
};

// The ALPN offer of one client protocol. HTTP/1 and WebSocket offer
// http/1.1; protocols without an ALPN identifier offer nothing.
enum class client_alpn : std::uint8_t {
    none,
    http11,
    http2,
    negotiate,
};
[[nodiscard]] std::span<const unsigned char> client_alpn_bytes(client_alpn mode) noexcept;

// Startup-only. Applies the shared TLS protocol policy (tls_protocol_policy.h);
// trust and client identity loading have one implementation; the ALPN offer is
// installed per connection by the TCP transport.
// The storage must come from a validated client_tls_config with TLS enabled.
void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config);
// Resets the reusable SSL of a client stream, prepares SNI (DNS names only, one
// trailing dot removed), the expected peer host or IP, and installs the ALPN
// offer. An h2-only offer requires TLS 1.2 or newer (RFC 9113).
// A host name matches DNS-ID subjectAltNames only: no Common Name
// fallback, no partial-label wildcards. `host` need not be NUL-terminated.
[[nodiscard]] client_tls_setup_error prepare_client_tls_stream(SSL& ssl, std::string_view host, std::span<const unsigned char> protocols);
// Carries a setup failure through transport results; message() names it.
[[nodiscard]] std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

class client_tls_socket;

// Completion of a socket write in flight: one that waits for the reactor,
// ended inside a record, or finished synchronously and must still observe a
// cancel() before the stream continues.
template <typename handler_type>
class client_tls_write_handler final {
public:
    client_tls_write_handler(handler_type&& handler, client_tls_socket& socket, bool counted)
        : handler_(std::move(handler)),
          socket_(&socket),
          counted_(counted) {}

    void operator()(const std::error_code& error, std::size_t bytes);

    // Asio's associators and continuation hook forward to the wrapped handler.
    handler_type handler_;

private:
    client_tls_socket* socket_;
    bool counted_;
};

// The TCP layer under the client's TLS stream. Every TLS record reaches the
// socket through async_write_some() here, whichever stream operation produced
// it: a write, the handshake, or a read answering a post-handshake message
// (a TLS 1.2 HelloRequest makes the read send a no_renegotiation alert). So
// this layer alone knows when each record handed to the socket is whole on
// the wire and none was lost to a failed or cancelled write: only then can
// another record follow without corrupting the stream. The socket must be in
// non-blocking mode; a write the kernel takes whole at once is complete even
// though its handler runs later.
class client_tls_socket final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = client_tls_socket;
    using native_handle_type = asio::ip::tcp::socket::native_handle_type;
    using protocol_type = asio::ip::tcp;
    using endpoint_type = asio::ip::tcp::endpoint;

    explicit client_tls_socket(const executor_type& executor)
        : socket_(executor) {}

    [[nodiscard]] executor_type get_executor() noexcept {
        return socket_.get_executor();
    }
    [[nodiscard]] lowest_layer_type& lowest_layer() noexcept {
        return *this;
    }
    [[nodiscard]] const lowest_layer_type& lowest_layer() const noexcept {
        return *this;
    }
    // The raw socket, for plaintext connections, socket options and the
    // CONNECT tunnel, which writes its own TLS records.
    [[nodiscard]] asio::ip::tcp::socket& socket() noexcept {
        return socket_;
    }
    // Replaces the unopened pre-handshake socket with the winner of the
    // Happy Eyeballs race. No TLS record operation may be active yet.
    void replace_socket(asio::ip::tcp::socket socket) noexcept {
        if (writes_ != 0) {
            std::terminate();
        }
        socket_ = std::move(socket);
        reset();
    }
    // Every record the stream handed to the socket is whole on the wire, in
    // sequence: no write is pending or partly done, and none failed.
    [[nodiscard]] bool records_whole() const noexcept {
        return writes_ == 0 && !write_failed_;
    }
    [[nodiscard]] bool is_open() const noexcept {
        return socket_.is_open();
    }
    [[nodiscard]] native_handle_type native_handle() {
        return socket_.native_handle();
    }
    void open(const protocol_type& protocol = asio::ip::tcp::v4()) {
        reset();
        socket_.open(protocol);
    }
    void open(const protocol_type& protocol, std::error_code& error) noexcept {
        reset();
        (void)socket_.open(protocol, error);
    }
    void connect(const endpoint_type& endpoint) {
        reset();
        socket_.connect(endpoint);
    }
    void connect(const endpoint_type& endpoint, std::error_code& error) noexcept {
        reset();
        (void)socket_.connect(endpoint, error);
    }
    void cancel() {
        std::error_code error;
        cancel(error);
        if (error) {
            throw std::system_error(error);
        }
    }
    void cancel(std::error_code& error) noexcept {
        cancelled_ = true;
        if (writes_ != 0) {
            write_failed_ = true;
        }
        (void)socket_.cancel(error);
    }
    void close() {
        std::error_code error;
        close(error);
        if (error) {
            throw std::system_error(error);
        }
    }
    void close(std::error_code& error) noexcept {
        cancelled_ = true;
        if (writes_ != 0) {
            write_failed_ = true;
        }
        std::error_code ignored;
        (void)socket_.cancel(ignored);
        (void)socket_.close(error);
    }
    void reset() noexcept {
        cancelled_ = false;
        write_failed_ = false;
    }

    template <typename buffers_type>
    std::size_t read_some(const buffers_type& buffers, std::error_code& error) {
        return socket_.read_some(buffers, error);
    }
    template <typename buffers_type>
    std::size_t write_some(const buffers_type& buffers, std::error_code& error) {
        return socket_.write_some(buffers, error);
    }
    template <typename buffers_type, typename handler_type>
    void async_read_some(const buffers_type& buffers, handler_type&& handler) {
        if (cancelled_) {
            asio::post(socket_.get_executor(),
                asio::append(std::forward<handler_type>(handler), asio::error::operation_aborted, std::size_t{0}));
            return;
        }
        socket_.async_read_some(buffers, std::forward<handler_type>(handler));
    }
    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type&& handler) {
        using completion_type = client_tls_write_handler<std::decay_t<handler_type>>;
        if (cancelled_) {
            write_failed_ = true;
            asio::post(socket_.get_executor(),
                asio::append(std::forward<handler_type>(handler), asio::error::operation_aborted, std::size_t{0}));
            return;
        }
        // Like Asio's own speculative write, but its outcome stays visible:
        // only a write that waits for the reactor or ends inside a record is
        // in flight. Queued writes keep their order, so a direct write runs
        // only while none waits.
        if (writes_ == 0 && socket_.non_blocking()) {
            std::error_code error;
            const auto written = socket_.write_some(buffers, error);
            if (error != asio::error::would_block && error != asio::error::try_again &&
                error != asio::error::interrupted) {
                if (error) {
                    write_failed_ = true;
                    asio::post(socket_.get_executor(), asio::append(std::forward<handler_type>(handler), error, written));
                    return;
                }
                const bool partial = written != asio::buffer_size(buffers);
                if (partial) {
                    ++writes_;
                }
                try {
                    asio::post(socket_.get_executor(),
                        asio::append(completion_type(std::forward<handler_type>(handler), *this, partial), error, written));
                } catch (...) {
                    if (partial) {
                        --writes_;
                    }
                    write_failed_ = true;
                    throw;
                }
                return;
            }
        }
        ++writes_;
        try {
            socket_.async_write_some(buffers, completion_type(std::forward<handler_type>(handler), *this, true));
        } catch (...) {
            --writes_;
            write_failed_ = true;
            throw;
        }
    }

private:
    template <typename>
    friend class client_tls_write_handler;

    asio::ip::tcp::socket socket_;
    std::size_t writes_{0};
    // A failed or cancelled write may leave a record partly sent, or records
    // unsent whose sequence numbers a later record would skip.
    bool write_failed_{false};
    bool cancelled_{false};
};

template <typename handler_type>
void client_tls_write_handler<handler_type>::operator()(const std::error_code& error, std::size_t bytes) {
    if (counted_) {
        --socket_->writes_;
    }
    if (socket_->cancelled_) {
        socket_->write_failed_ = true;
        std::move(handler_)(asio::error::operation_aborted, 0);
        return;
    }
    if (error) {
        socket_->write_failed_ = true;
    }
    std::move(handler_)(error, bytes);
}

template <typename handler_type>
[[nodiscard]] bool asio_handler_is_continuation(client_tls_write_handler<handler_type>* handler) {
    return asio_handler_cont_helpers::is_continuation(handler->handler_);
}

}  // namespace ruvia::detail

namespace asio {

template <template <typename, typename> class associator_type, typename handler_type, typename candidate_type>
struct associator<associator_type, ruvia::detail::client_tls_write_handler<handler_type>, candidate_type>
    : associator_type<handler_type, candidate_type> {
    static typename associator_type<handler_type, candidate_type>::type get(
        const ruvia::detail::client_tls_write_handler<handler_type>& handler) noexcept {
        return associator_type<handler_type, candidate_type>::get(handler.handler_);
    }
    static auto get(const ruvia::detail::client_tls_write_handler<handler_type>& handler,
        const candidate_type& candidate) noexcept
        -> decltype(associator_type<handler_type, candidate_type>::get(handler.handler_, candidate)) {
        return associator_type<handler_type, candidate_type>::get(handler.handler_, candidate);
    }
};

}  // namespace asio

namespace ruvia::detail {

class client_happy_eyeballs;

// Why an owner stopped a transport. The latest non-none reason wins until the
// owner starts a new operation with reset_abort().
enum class client_abort_reason : std::uint8_t { none,
    timeout,
    cancelled,
    closing };

enum class client_transport_status : std::uint8_t {
    ok,
    // The peer ended the byte stream (TCP FIN, close_notify or truncation).
    eof,
    // abort_ names the reason: an owner abort or an expired deadline.
    aborted,
    resolve_failed,
    connect_failed,
    tls_failed,
    io_failed,
};

struct client_transport_result final {
    client_transport_status status_{client_transport_status::ok};
    client_abort_reason abort_{client_abort_reason::none};
    std::size_t bytes_{0};
    std::error_code error_{};

    [[nodiscard]] bool ok() const noexcept {
        return status_ == client_transport_status::ok;
    }
};

// Counts plaintext bytes completed by socket/TLS I/O, including the completed
// prefix of an operation that also reports an error.
struct client_wire_counters final {
    std::size_t sent_{};
    std::size_t received_{};
};

enum class client_idle_transport_status : std::uint8_t {
    reusable,
    closed,
    pending_data,
};

// Borrowed origin of one transport; the owner's startup config outlives it.
struct client_tcp_endpoint final {
    // Validated unbracketed host used for DNS.
    std::string_view host_;
    std::uint16_t port_;
    // nullptr selects plaintext TCP.
    const client_tls_config_storage* tls_;
    client_tcp_config tcp_;
    // ALPN wire offer borrowed until transport retirement; empty for TCP.
    std::span<const unsigned char> alpn_{};
    // TLS SNI and verified peer name, used verbatim; empty selects the TLS
    // configuration's server name, else host_.
    std::string_view identity_host_{};
    void* resolve_context_{};
    // Resolves host_ and port_ in place of the transport's own resolver.
    task<client_resolve_result> (*resolve_)(void*, client_host_resolver&, const operation_deadline&, std::string_view, std::uint16_t){};
};

// True for an OpenSSL failure or a TLS stream truncated without close_notify.
[[nodiscard]] bool is_tls_error(const std::error_code& error) noexcept;

// The one outbound TCP/TLS byte transport shared by the HTTP/1, WebSocket and
// Redis clients. Protocol-neutral: callers map results onto their error
// types. One worker owns the socket; the read lane (resolve, connect, TLS
// handshake, reads) and the write lane have independent deadlines so a
// full-duplex exchange can bound its upload separately. A connected socket
// stays in non-blocking mode: every operation on it is asynchronous or a
// bounded probe. Address-stable. Construction throws std::invalid_argument
// for a DNS host on a worker without a blocking pool.
class client_tcp_transport final {
public:
    client_tcp_transport(asio::io_context& io, asio::ssl::context& tls, const worker_ref& worker_value,
        client_tcp_endpoint endpoint, client_wire_counters& counters);
    client_tcp_transport(asio::io_context&, asio::ssl::context&, worker_ref&&, client_tcp_endpoint,
        client_wire_counters&) = delete;
    ~client_tcp_transport();
    client_tcp_transport(const client_tcp_transport&) = delete;
    client_tcp_transport& operator=(const client_tcp_transport&) = delete;

    [[nodiscard]] bool secure() const noexcept {
        return endpoint_.tls_ != nullptr;
    }
    [[nodiscard]] const client_tcp_endpoint& endpoint() const noexcept {
        return endpoint_;
    }
    // Resolves, connects, applies socket policies and, for TLS, prepares SNI,
    // peer identity and the ALPN offer before the handshake. h2-only offers
    // require TLS 1.2; a peer selecting h2 below TLS 1.2 is rejected before
    // application I/O. Every operation borrowing a previous connection must
    // have finished.
    [[nodiscard]] task<client_transport_result> connect(const operation_deadline& timeout);
    // Replaces the endpoint's ALPN offer for every later connect().
    void offer_alpn(client_alpn alpn) noexcept {
        endpoint_.alpn_ = client_alpn_bytes(alpn);
    }
    [[nodiscard]] const asio::ip::tcp::endpoint& peer_endpoint() const noexcept {
        return peer_endpoint_;
    }
    [[nodiscard]] std::chrono::steady_clock::duration connect_duration() const noexcept {
        return connect_duration_;
    }
    [[nodiscard]] std::chrono::steady_clock::duration tls_duration() const noexcept {
        return tls_duration_;
    }
    // Selected ALPN protocol of the current TLS connection; empty for TCP.
    [[nodiscard]] std::string_view alpn() noexcept;
    // HTTP/1 clients offer only http/1.1: a TLS peer either skips ALPN or
    // must select it.
    [[nodiscard]] bool alpn_allows_http11() noexcept {
        if (!secure()) {
            return true;
        }
        const auto selected = alpn();
        return selected.empty() || selected == "http/1.1";
    }
    [[nodiscard]] task<client_transport_result> read_some(std::span<char> bytes, const operation_deadline& timeout);
    // Writes every byte under the write lane's own deadline. The buffer
    // sequence form sends framed parts (chunk prefix, payload, suffix) as one
    // transport write, which is one TLS record sequence and one deadline cycle.
    [[nodiscard]] task<client_transport_result> write(std::string_view bytes, const operation_deadline& timeout);
    [[nodiscard]] task<client_transport_result> write(
        std::span<const asio::const_buffer> buffers, const operation_deadline& timeout);

    [[nodiscard]] client_abort_reason abort_reason() const noexcept {
        return abort_reason_;
    }
    void reset_abort() noexcept {
        abort_reason_ = client_abort_reason::none;
    }
    // Records the reason (replacing an earlier one) and cancels pending
    // resolve and socket operations; the established socket stays open until
    // close(). During an address race, every attempt is closed and joined.
    void abort(client_abort_reason reason) noexcept;
    // Closes the connection at once. An established TLS connection first
    // sends close_notify (RFC 9112 §9.8, RFC 9846 §6.1) with one non-blocking
    // write when the alert can follow every record already sent whole: no
    // record write, from a write or from a read answering a post-handshake
    // message, still waits for the socket or ended inside a record, and none
    // failed or was cancelled. A peer that is not reading does not delay the
    // close, it only misses the alert. A pending read does not hold the alert
    // back; the close cancels it.
    void close() noexcept;
    // Bounded non-blocking check of a connected, idle transport that never
    // runs the TLS engine. An HTTP/1 or Redis peer sends nothing between
    // exchanges, so readable or TLS-buffered bytes are distinguished from an
    // end of stream before either condition retires the connection. Pending TLS
    // data can be close_notify, a post-handshake record or an unsolicited response.
    [[nodiscard]] client_idle_transport_status idle_status() noexcept;
    // Completes once the socket of a connected, idle transport is readable,
    // at its end or failed, without reading and without running the TLS
    // engine; idle_status() then classifies it. Only while no other operation
    // is pending: cancel_wait() ends it early.
    [[nodiscard]] task<void> wait_readable();
    void cancel_wait() noexcept;

    // Raw stream for the CONNECT tunnel adapter, which owns TLS shutdown: the
    // transport no longer sends close_notify for this connection.
    [[nodiscard]] asio::ssl::stream<client_tls_socket>& stream() noexcept {
        tls_close_ready_ = false;
        return stream_;
    }

private:
    struct lane_deadline final {
        deadline_lifecycle<> deadline_;
        worker_timer_registration timer_;
    };

    template <typename buffers_type>
    [[nodiscard]] task<client_transport_result> write_buffers(buffers_type buffers, const operation_deadline& timeout);
    void send_close_notify() noexcept;
    [[nodiscard]] bool arm(lane_deadline& lane, const operation_deadline& timeout);
    [[nodiscard]] bool clear(lane_deadline& lane) noexcept;
    [[nodiscard]] client_transport_result finish(lane_deadline& lane, const operation_deadline& timeout,
        const std::error_code& error, std::size_t bytes, client_transport_status failure) noexcept;
    [[nodiscard]] client_transport_result aborted() const noexcept;

    const worker_ref& worker_;
    asio::ssl::context& tls_;
    client_tcp_endpoint endpoint_;
    std::optional<asio::ip::tcp::endpoint> literal_endpoint_{};
    client_wire_counters& counters_;
    asio::ip::tcp::endpoint peer_endpoint_;
    std::chrono::steady_clock::duration connect_duration_{};
    std::chrono::steady_clock::duration tls_duration_{};
    client_host_resolver resolver_;
    asio::ssl::stream<client_tls_socket> stream_;
    lane_deadline read_;
    lane_deadline write_;
    client_abort_reason abort_reason_{client_abort_reason::none};
    bool tls_started_{false};
    // Non-null only while the address race is active; abort() cancels and the
    // connect coroutine joins every attempted socket before clearing it.
    client_happy_eyeballs* connecting_{nullptr};
    // The TLS connection is established and has not failed, so a close may
    // append close_notify once its socket layer holds whole records only.
    bool tls_close_ready_{false};
    // A composed write_buffers call is in flight: it may span multiple TLS
    // records across posted speculative completions.
    bool writing_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline std::error_code http_client_abort_error(client_abort_reason reason) noexcept {
    switch (reason) {
        case client_abort_reason::timeout:
            return std::make_error_code(std::errc::timed_out);
        case client_abort_reason::cancelled:
            return std::make_error_code(std::errc::operation_canceled);
        case client_abort_reason::none:
        case client_abort_reason::closing:
            break;
    }
    return make_error_code(http_client_errc::closing);
}

// The HTTP client's mapping of a failed client_tcp_transport result.
[[nodiscard]] inline std::error_code http_client_transport_error(
    const client_transport_result& result) noexcept {
    switch (result.status_) {
        case client_transport_status::aborted:
            return http_client_abort_error(result.abort_);
        case client_transport_status::resolve_failed:
            return make_error_code(http_client_errc::resolve_failed);
        case client_transport_status::connect_failed:
            return make_error_code(http_client_errc::connect_failed);
        case client_transport_status::tls_failed:
            return make_error_code(http_client_errc::tls_failed);
        case client_transport_status::ok:
        case client_transport_status::eof:
        case client_transport_status::io_failed:
            break;
    }
    return result.error_ == std::errc::protocol_error ? make_error_code(http_client_errc::protocol_error)
                                                      : make_error_code(http_client_errc::io_error);
}

// The one message of each error class an exchange reports without a more
// specific transport or protocol description.
[[nodiscard]] inline std::string_view http_client_error_message(std::error_code code) noexcept {
    if (code == std::make_error_code(std::errc::timed_out)) {
        return "HTTP client request timed out";
    }
    if (code == std::make_error_code(std::errc::operation_canceled)) {
        return "HTTP client request cancelled";
    }
    if (code == make_error_code(http_client_errc::closing)) {
        return "HTTP client pool is closing";
    }
    if (code == make_error_code(http_client_errc::response_too_large)) {
        return "HTTP response exceeds configured byte limit";
    }
    if (code == make_error_code(http_client_errc::tls_failed)) {
        return "HTTP client TLS connection failed";
    }
    if (code == make_error_code(http_client_errc::io_error)) {
        return "HTTP client connection failed";
    }
    if (code == make_error_code(http_client_errc::protocol_error)) {
        return "HTTP protocol failed";
    }
    if (code == make_error_code(http_client_errc::handshake_rejected)) {
        return "WebSocket handshake was rejected";
    }
    if (code == make_error_code(http_client_errc::message_too_large)) {
        return "WebSocket message exceeds configured byte limit";
    }
    return "HTTP request failed";
}

[[noreturn]] inline void throw_http_client_abort(client_abort_reason reason) {
    const auto code = http_client_abort_error(reason);
    throw http_client_error(code, http_client_error_message(code));
}

// A non-value wait outcome of the pool's slot, connect or HTTP/1 exchange
// schedulers; `operation` names the wait in timeout and cancellation messages.
[[noreturn]] inline void throw_http_client_wait_failure(worker_wait_status status, std::string_view operation) {
    switch (status) {
        case worker_wait_status::timed_out:
            throw http_client_error(std::make_error_code(std::errc::timed_out),
                std::string(operation).append(" timed out"));
        case worker_wait_status::cancelled:
            throw http_client_error(std::make_error_code(std::errc::operation_canceled),
                std::string(operation).append(" cancelled"));
        case worker_wait_status::closed:
        case worker_wait_status::worker_stopping:
            throw http_client_error(make_error_code(http_client_errc::closing),
                http_client_error_message(make_error_code(http_client_errc::closing)));
        case worker_wait_status::value:
            break;
    }
    std::terminate();
}

// The code of a failure the client reported as http_client_error.
[[nodiscard]] inline std::optional<std::error_code> http_client_failure_code(
    const std::exception_ptr& failure) noexcept {
    try {
        std::rethrow_exception(failure);
    } catch (const http_client_error& error) {
        return error.code();
    } catch (...) {
        return std::nullopt;
    }
}

// `eof_message` describes a peer close where the caller still needed bytes.
[[noreturn]] inline void throw_http_client_transport_error(
    const client_transport_result& result, std::string_view eof_message) {
    if (result.status_ == client_transport_status::aborted) {
        throw_http_client_abort(result.abort_);
    }
    if (result.status_ == client_transport_status::eof) {
        throw http_client_error(make_error_code(http_client_errc::io_error), eof_message);
    }
    throw http_client_error(http_client_transport_error(result), error_code_text(result.error_));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_pool;
class http_client_tunnel_state;
class http_client_upload_state;
class http_client_output_queue;
struct http_client_config_storage;

// Syntax of http_client_request_options' route overrides, independently of
// any alias; throws std::invalid_argument.
void validate_http_client_route_options(const http_client_request_options& options);
// The checks of configure(): route overrides and the field source.
void validate_http_client_request_options(const http_client_request_options& options);
// Shares a request's informational callback between its exchanges; null when empty.
[[nodiscard]] std::shared_ptr<http_client_informational_callback> share_http_client_informational(
    http_client_informational_callback&& callback);

// Every field of `options` except the informational callback, which the
// exchanges of a request (one per redirect hop) share through
// http_client_request_storage::informational_ instead.
[[nodiscard]] http_client_request_options copy_http_client_request_settings(const http_client_request_options& options);

// The effective origin and transport of one request: the alias defaults with
// the request's route overrides applied. Views borrow the alias configuration
// or the owning request storage.
struct http_client_route final {
    http_origin_view origin_;
    // RFC 3986 authority with the scheme's default port omitted.
    std::string_view authority_;
    std::string_view dial_host_;
    std::uint16_t dial_port_{};
    // TLS SNI and verified peer name; empty for http.
    std::string_view server_name_;
    // The authority is the alias authority: its cookie jar applies.
    bool alias_origin_{true};
    // Dial target and server name are the alias defaults: a connection of
    // this transport is authenticated for the alias origin (server push).
    bool alias_transport_{true};
    // The alias's versions, or the request's override.
    http_client_versions versions_{};
};

struct http_client_websocket_handshake final {
    http_client_websocket_handshake(websocket_client_negotiation_config_view config);
    websocket_client_negotiation negotiation_;
    std::array<char, 24> key_;
    std::string selected_subprotocol_;
    websocket_compression compression_{};
};

class http_client_request_storage final {
public:
    // Complete owning boundary: reserve field metadata once and normalize/copy
    // every field before asynchronous work can outlive caller storage.
    http_client_request_storage(std::string_view method, std::string_view target,
        std::span<const http_header_view> fields);

    http_client_request_storage(const http_client_request_storage&) = delete;
    http_client_request_storage& operator=(const http_client_request_storage&) = delete;
    http_client_request_storage(http_client_request_storage&& other) noexcept;
    http_client_request_storage& operator=(http_client_request_storage&&) noexcept = delete;

    http_client_request_storage& set_body(std::string_view body);

    void set_host_stop(stop_token token) noexcept {
        host_stop_ = std::move(token);
    }
    [[nodiscard]] const stop_token& host_stop() const noexcept {
        return host_stop_;
    }

    void set_tunnel(std::string_view authority, std::string_view protocol);
    void set_websocket(std::shared_ptr<http_client_websocket_handshake> handshake) noexcept {
        websocket_ = std::move(handshake);
    }
    [[nodiscard]] http_client_websocket_handshake* websocket() const noexcept {
        return websocket_.get();
    }
    void bind_tunnel(http_client_tunnel_state& tunnel) noexcept {
        tunnel_ = &tunnel;
    }
    [[nodiscard]] http_client_tunnel_state* tunnel() const noexcept {
        return tunnel_;
    }
    [[nodiscard]] http_client_output_queue* output() const noexcept;
    [[nodiscard]] bool is_tunnel() const noexcept {
        return is_tunnel_;
    }
    void set_upgrade() noexcept {
        is_upgrade_ = true;
        is_tunnel_ = true;
    }
    [[nodiscard]] bool is_upgrade() const noexcept {
        return is_upgrade_;
    }
    [[nodiscard]] std::string_view tunnel_authority() const& noexcept {
        return tunnel_authority_;
    }
    std::string_view tunnel_authority() const&& = delete;
    [[nodiscard]] std::string_view tunnel_protocol() const& noexcept {
        return tunnel_protocol_;
    }
    std::string_view tunnel_protocol() const&& = delete;
    void bind_upload(http_client_upload_state& upload) noexcept {
        upload_ = &upload;
    }
    [[nodiscard]] http_client_upload_state* upload() const noexcept {
        return upload_;
    }
    [[nodiscard]] std::string_view method() const& noexcept {
        return method_;
    }
    [[nodiscard]] std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view target() const& noexcept {
        return target_;
    }
    [[nodiscard]] std::string_view target() const&& = delete;

    [[nodiscard]] std::string_view body() const& noexcept {
        return body_;
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] bool has_body() const noexcept {
        return has_body_;
    }
    [[nodiscard]] const std::vector<http_header>& headers() const noexcept {
        return headers_;
    }
    [[nodiscard]] bool udp_tunnel() const noexcept {
        return tunnel_protocol_ == "connect-udp";
    }
    // Validates the route overrides against `config` and fixes route(). Only
    // once the storage reached the address it keeps while its request runs:
    // a move drops the resolved views.
    void resolve_route(const http_client_config_storage& config);
    [[nodiscard]] const http_client_route& route() const& noexcept {
        return *route_;
    }
    const http_client_route& route() const&& = delete;
    // informational_ stays empty; the request's callback is informational_.
    http_client_request_options options_;
    std::shared_ptr<http_client_informational_callback> informational_;
    std::shared_ptr<http_client_pool> pool_owner_;

private:
    http_client_request_storage(
        std::string_view method, std::string_view target);

    std::string method_;
    std::string target_;
    std::vector<http_header> headers_;
    std::string body_;
    std::string tunnel_authority_;
    std::string tunnel_protocol_;
    std::shared_ptr<http_client_websocket_handshake> websocket_;
    stop_token host_stop_;
    bool is_tunnel_{};
    bool is_upgrade_{false};
    http_client_tunnel_state* tunnel_{};
    bool has_body_{false};
    http_client_upload_state* upload_{};
    // The normalized authority override that route() views.
    std::string route_authority_;
    std::optional<http_client_route> route_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_result_budget_domain final {
public:
    explicit http_client_result_budget_domain(http_client_result_budget_config config);

    http_client_result_budget_domain(const http_client_result_budget_domain&) = delete;
    http_client_result_budget_domain& operator=(const http_client_result_budget_domain&) = delete;

    [[nodiscard]] bool try_reserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;

    // Only the owning worker accesses receive accounting. Escaped results
    // independently use the thread-safe retained-result counter above.
    [[nodiscard]] bool reserve_in_flight(std::size_t bytes) noexcept;
    void release_in_flight(std::size_t bytes) noexcept;

private:
    const std::size_t max_retained_bytes_;
    std::atomic<std::size_t> retained_bytes_{0};
    const std::size_t max_in_flight_bytes_;
    std::size_t in_flight_bytes_{0};
};

// Move-only reservation carried by an escaped result. It owns the budget domain
// independently of the client and releases its exact charge at destruction.
class http_client_result_budget_lease final {
public:
    http_client_result_budget_lease(const http_client_result_budget_lease&) = delete;
    http_client_result_budget_lease& operator=(const http_client_result_budget_lease&) = delete;
    http_client_result_budget_lease(http_client_result_budget_lease&& other) noexcept;
    ~http_client_result_budget_lease();

    [[nodiscard]] static std::optional<http_client_result_budget_lease> try_acquire(
        const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes) noexcept;
    [[nodiscard]] std::pair<std::shared_ptr<http_client_result_budget_domain>, std::size_t> release() && noexcept {
        return {std::move(domain_), std::exchange(bytes_, 0)};
    }

private:
    http_client_result_budget_lease(
        const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept;

    // Keep the domain alive until after releasing the reservation.
    std::shared_ptr<http_client_result_budget_domain> domain_;
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_pool;

// Worker-affine bounds shared by every HTTP client alias of one owner (a
// standalone http_client, or one application worker): idle connections across
// aliases, ordered by one idle clock, and concurrent background DNS refreshes.
// Pools join with their first connection and leave before retiring them.
class http_client_worker_limits final {
public:
    explicit http_client_worker_limits(const http_client_worker_config& config);
    http_client_worker_limits(const http_client_worker_limits&) = delete;
    http_client_worker_limits& operator=(const http_client_worker_limits&) = delete;

    void join(http_client_pool& pool);
    void leave(http_client_pool& pool) noexcept;
    [[nodiscard]] std::uint64_t next_idle_order() noexcept {
        return ++idle_clock_;
    }
    // Closes the least recently used idle connections of any alias while the
    // worker keeps more than its bound; alias bounds apply first.
    void enforce_idle_limits() noexcept;
    // A background refresh lookup may start: false once the worker runs its
    // bound of them, and the caller defers to its next refresh period.
    [[nodiscard]] bool try_begin_dns_refresh() noexcept;
    void end_dns_refresh() noexcept;

private:
    void enforce_idle_limit(bool multiplexed, std::size_t maximum) noexcept;

    std::size_t max_idle_http1_;
    std::size_t max_idle_multiplexed_;
    std::size_t max_dns_refreshes_;
    std::size_t dns_refreshes_{0};
    std::uint64_t idle_clock_{0};
    std::vector<http_client_pool*> pools_{};
};

// The per-worker state every pool of one owner shares.
struct http_client_worker_services final {
    std::shared_ptr<http_client_result_budget_domain> result_budget_;
    std::shared_ptr<http_client_worker_limits> limits_;
};

// Validates `config`; throws std::invalid_argument.
[[nodiscard]] http_client_worker_services make_http_client_worker_services(const http_client_worker_config& config);

}  // namespace ruvia::detail

namespace ruvia::detail {
struct http_client_config_storage;
class http_client_request_storage;
struct client_received_cookie;

struct client_prepared_request_scratch final {
    static constexpr std::size_t inline_header_capacity = 16;
    static constexpr std::size_t inline_cookie_capacity = 256;

    client_prepared_request_scratch() = default;

    std::array<http_header_view, inline_header_capacity> inline_headers_;
    std::vector<http_header_view> overflow_headers_;
    std::array<char, inline_cookie_capacity> inline_cookie_header_;
    std::string overflow_cookie_header_;
};

// Origin policy owns cookies and automatic headers independently of connection
// negotiation, cancellation, and transport retirement.
class client_request_policy final {
public:
    explicit client_request_policy(const http_client_config_storage& config);
    // The complete view borrows the request, policy text and both scratch buffers;
    // keep their owners alive through protocol serialization.
    [[nodiscard]] http_client_request_view prepare_request(
        const http_client_request_storage& request, client_prepared_request_scratch& scratch);
    // Stored and configured cookies belong to the fixed origin: false for a
    // request whose authority override or tunnel authority names another
    // host or port. Requires a resolved request route.
    [[nodiscard]] bool addresses_origin(const http_client_request_storage& request) const noexcept;
    // Retains the Set-Cookie fields of a final head received for an origin
    // request with `request_target`.
    void retain_response_cookies(std::string_view request_target, std::span<const http_header> headers);

private:
    const http_client_config_storage& config_;
    http_cookie_jar jar_;
};
}  // namespace ruvia::detail

namespace ruvia {
class operation_deadline;
}  // namespace ruvia

namespace ruvia::detail {

class client_request_policy;
class http_client_response_state;
struct http_client_websocket_handshake;

// One received final response head, after its protocol driver stored the
// response fields in the state's headers_.
struct http_client_final_head final {
    http_received_status status_;
    http_protocol_version protocol_version_;
    std::optional<http_client_request_content_signal> request_content_signal_;
    // Target of a request addressed to the fixed origin, whose jar retains the
    // head's Set-Cookie fields; nullopt for a tunnel to another authority.
    std::optional<std::string_view> origin_target_;
    // The protocol's verdict that this head opens the request's tunnel: a 2xx
    // to CONNECT, or the HTTP/1 101 upgrade of CONNECT-UDP (RFC 9298 section 3).
    bool opens_tunnel_{false};
};

// Stores a received HTTP/2 or HTTP/3 final head in the state: its fields
// (moved when they already use the state's resource), Content-Length and the
// framing its status implies for the request method.
void retain_http_client_binary_response_head(http_client_response_state& state_value, http_client_response_head&& head);

// The final-head publication shared by HTTP/1, HTTP/2 and HTTP/3: stops an upload the
// response completed, records status, version and body plan, retains origin
// cookies, settles tunnel acceptance (rejecting an invalid CONNECT-UDP head),
// selects collection of an encoded body, then wakes the head consumer.
void publish_http_client_final_head(
    http_client_response_state& state_value, client_request_policy& policy, const http_client_final_head& head);

// Validates an RFC 8441 Extended CONNECT WebSocket response head (HTTP/2 or
// HTTP/3) and records the negotiated subprotocol and compression on `websocket`.
void accept_extended_connect_websocket_response(http_client_websocket_handshake& websocket,
    http_status_code status, std::span<const http_header> headers, bool stream_ended,
    std::string_view error_message);

// Ends a received body: decodes a collected Content-Encoding within the
// response byte limit, failing with timeout when `timeout` expired before or
// during decoding.
void finish_http_client_response_body(http_client_response_state& state_value, const operation_deadline& timeout,
    bool content_semantics_present, std::size_t max_decoded_bytes);

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_pool;
class http_client_response_state;
class http_client_result_budget_domain;

// Response storage and in-flight budget accounting of one client pool, plus the
// registry of live response states so the pool can retire their transport
// borrows on join. Worker-affine, reference counted by the pool root and every
// live state.
class http_client_response_memory_domain final {
public:
    class deleter_type final {
    public:
        void operator()(http_client_response_memory_domain* domain) const noexcept;
    };

    using owner = std::unique_ptr<http_client_response_memory_domain, deleter_type>;

    [[nodiscard]] static owner create(const worker_ref& worker,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);

    http_client_response_memory_domain(const http_client_response_memory_domain&) = delete;
    http_client_response_memory_domain& operator=(const http_client_response_memory_domain&) = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept {
        return worker_;
    }

    void charge_in_flight(std::size_t bytes_value);
    void release_in_flight(std::size_t bytes_value) noexcept;

    [[nodiscard]] http_client_response_state* create_state(http_client_pool& pool);
    void destroy_state(http_client_response_state* state) noexcept;
    void detach_transport_bindings(http_client_pool& pool) noexcept;

private:
    enum class phase_type : unsigned char { prepared,
        active,
        retired };

    http_client_response_memory_domain(const worker_ref& worker_value,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    ~http_client_response_memory_domain();

    void destroy() noexcept;
    void require_current() const noexcept;
    void attach_state(http_client_response_state& state) noexcept;
    void unlink_state(http_client_response_state& state) noexcept;
    void retain() noexcept;
    void release() noexcept;
    void release_root() noexcept;

    worker_ref worker_;
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    http_client_response_state* state_head_{};
    std::size_t references_{1};
    phase_type phase_{phase_type::prepared};

    friend class deleter_type;
    friend class http_client_response_state;
};

class http_client_in_flight_reservation final {
public:
    constexpr http_client_in_flight_reservation() noexcept = default;
    explicit constexpr http_client_in_flight_reservation(http_client_response_memory_domain* domain) noexcept
        : domain_(domain) {}
    ~http_client_in_flight_reservation() {
        clear();
    }
    http_client_in_flight_reservation(const http_client_in_flight_reservation&) = delete;
    http_client_in_flight_reservation& operator=(const http_client_in_flight_reservation&) = delete;
    http_client_in_flight_reservation(http_client_in_flight_reservation&& other) noexcept
        : domain_(std::exchange(other.domain_, nullptr)),
          bytes_(std::exchange(other.bytes_, 0)) {}
    http_client_in_flight_reservation& operator=(http_client_in_flight_reservation&& other) noexcept {
        if (this != &other) {
            clear();
            domain_ = std::exchange(other.domain_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }

    void resize_to(std::size_t target_bytes) {
        if (domain_ == nullptr || target_bytes == bytes_) {
            return;
        }
        if (target_bytes > bytes_) {
            domain_->charge_in_flight(target_bytes - bytes_);
        } else {
            domain_->release_in_flight(bytes_ - target_bytes);
        }
        bytes_ = target_bytes;
    }

    void shrink_to(std::size_t target_bytes) noexcept {
        if (domain_ != nullptr && target_bytes < bytes_) {
            domain_->release_in_flight(bytes_ - target_bytes);
            bytes_ = target_bytes;
        }
    }

    void clear() noexcept {
        if (domain_ != nullptr && bytes_ != 0) {
            domain_->release_in_flight(bytes_);
            bytes_ = 0;
        }
    }

    void swap(http_client_in_flight_reservation& other) noexcept {
        std::swap(domain_, other.domain_);
        std::swap(bytes_, other.bytes_);
    }

private:
    http_client_response_memory_domain* domain_{nullptr};
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The configured User-Agent (RFC 9110 section 10.1.5) of the HTTP client and
// both WebSocket client protocols: sent when a request or handshake carries no
// User-Agent field of its own; empty sends none.
inline void validate_client_user_agent(std::string_view user_agent) {
    if (!user_agent.empty() && !is_valid_http_header_value(user_agent)) {
        throw std::invalid_argument("client user agent is invalid");
    }
}

// The scheme of an alias and its fixed origin, absent for a scheme-only
// base_url_ whose requests each name their authority.
struct http_client_origin_spec final {
    http_scheme scheme_;
    std::optional<http_origin_view> origin_;
};

// A nonempty set of compiled-in versions; HTTP/3 only over https.
inline void validate_http_client_versions(const http_client_versions& versions, http_scheme scheme) {
    if (!versions.http1_ && !versions.http2_ && !versions.http3_) {
        throw std::invalid_argument("HTTP client versions must not be empty");
    }
#ifndef RUVIA_ENABLE_HTTP2
    if (versions.http2_) {
        throw std::invalid_argument("HTTP/2 support is disabled");
    }
#endif
#ifndef RUVIA_ENABLE_HTTP3
    if (versions.http3_) {
        throw std::invalid_argument("HTTP/3 support is disabled");
    }
#endif
    if (versions.http3_ && scheme != http_scheme::https) {
        throw std::invalid_argument("HTTP/3 requires HTTPS");
    }
}

[[nodiscard]] inline http_client_origin_spec validate_http_client_config(const http_client_config& config) {
    auto url = config.base_url_.view();
    const bool tls = http_ascii_equals_ignore_case(url.substr(0, 8), "https://");
    if (!tls && !http_ascii_equals_ignore_case(url.substr(0, 7), "http://")) {
        throw std::invalid_argument("HTTP client base_url must be an http(s) origin");
    }
    url.remove_prefix(tls ? 8 : 7);
    const http_client_origin_spec spec{.scheme_ = tls ? http_scheme::https : http_scheme::http, .origin_ = std::nullopt};
    const auto scheme = spec.scheme_;
    if (url.empty()) {
        // An egress alias without a fixed origin: every destination comes
        // from a request, so the dial addresses (Alt-Svc alternatives included)
        // must pass a validator, and no origin-scoped cookie jar exists.
        if (!config.address_validator_) {
            throw std::invalid_argument("HTTP client alias without a fixed origin requires an address_validator_");
        }
        if (!config.cookies_.empty() || config.received_cookies_ != http_client_received_cookie_policy::ignore) {
            throw std::invalid_argument("HTTP client alias without a fixed origin has no cookie jar");
        }
        if (config.tls_.server_name_) {
            throw std::invalid_argument("HTTP client alias without a fixed origin takes each TLS server name from its request");
        }
    }
    if (url.ends_with('/')) {
        url.remove_suffix(1);
    }
    std::optional<http_authority_view> authority;
    if (!url.empty()) {
        authority = parse_http_authority(url);
        if (!authority || authority->host().empty() || authority->port_kind() == http_authority_port_kind::empty ||
            authority->port() == 0 || url.find_first_of("/?#") != std::string_view::npos) {
            throw std::invalid_argument("HTTP client base_url must contain only a fixed origin or a scheme");
        }
    }
    auto result = spec;
    if (authority) {
        result.origin_ = tls ? http_origin_view::https({.host_ = authority->host(), .port_ = authority->effective_port(443)})
                             : http_origin_view::http({.host_ = authority->host(), .port_ = authority->effective_port(80)});
    }
    auto host = result.origin_ ? result.origin_->host() : std::string_view{};
    if (host.starts_with('[')) {
        host = host.substr(1, host.size() - 2);
    }
    if (config.push_.max_queued_pushes_ == 0 || config.push_.max_concurrent_pushes_ == 0 ||
        (config.push_.timeout_ && config.push_.timeout_->value().count() <= 0)) {
        throw std::invalid_argument("HTTP push bounds and configured timeout must be positive");
    }
    if (config.advertisements_.max_queued_advertisements_ == 0 || config.advertisements_.max_retained_bytes_ == 0) {
        throw std::invalid_argument("HTTP advertisement bounds must be positive");
    }
    if (config.advertisements_.receive_origins_ && scheme != http_scheme::https) {
        throw std::invalid_argument("ORIGIN advertisements require authenticated HTTPS");
    }
    validate_http_client_versions(config.versions_, scheme);
    if (config.http3_.max_concurrent_streams_ == 0 || config.http3_.max_alt_svc_entries_ == 0 ||
        config.http3_.max_datagram_bytes_ == 0 || config.http3_.max_stream_output_bytes_ == 0) {
        throw std::invalid_argument("HTTP/3 resource bounds must be positive");
    }
    if (config.http3_.max_datagram_bytes_ < 1200 || config.http3_.max_datagram_bytes_ > 65527 ||
        config.http3_.max_concurrent_streams_ > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::invalid_argument("HTTP/3 transport limits exceed QUIC bounds");
    }
    validate_client_tls_config(config.tls_);
    if (scheme == http_scheme::https && config.tls_.mode_ == client_tls_mode::disabled) {
        throw std::invalid_argument("HTTPS client requires TLS; use the http scheme for plaintext");
    }
    if (config.received_cookies_ != http_client_received_cookie_policy::ignore &&
        config.received_cookies_ != http_client_received_cookie_policy::retain_and_send) {
        throw std::invalid_argument("http client received cookie policy is invalid");
    }
    if (config.dial_host_) {
        validate_client_origin_host(config.dial_host_->view(), "HTTP client dial host must not be empty", "HTTP client dial host is invalid");
    }
    if (config.dial_port_ && config.dial_port_->value() == 0) {
        throw std::invalid_argument("HTTP client dial port must be positive");
    }
    detail::ensure_positive_optional_durations("HTTP DNS refresh interval must be positive", config.dns_refresh_interval_);
    if (result.origin_) {
        validate_client_origin_host(
            host, "http client host must not be empty", "http client host is invalid");
    }
    if (config.connection_limits_.max_open_ && *config.connection_limits_.max_open_ == 0) {
        throw std::invalid_argument("http client open connection limit must be greater than zero");
    }
    detail::ensure_positive_size(config.max_concurrent_http2_streams_per_connection_,
        "http client HTTP/2 stream limit per connection must be greater than zero");
    detail::ensure_positive_size(
        config.max_buffered_requests_, "http client buffered request limit must be greater than zero");
    detail::ensure_positive_size(config.max_cookies_, "http client cookie limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_cookie_bytes_, "http client cookie byte limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_response_bytes_, "http client response byte limit must be greater than zero");
    detail::ensure_positive_optional_durations("configured http client timeouts must be greater than zero",
        std::optional{config.connect_timeout_}, config.write_timeout_, config.read_timeout_, config.request_timeout_,
        config.acquire_timeout_, config.idle_timeout_, config.http2_ping_interval_);
    validate_client_user_agent(config.user_agent_.view());
    validate_client_tcp_config(config.tcp_);
    // Every configured cookie is validated, including one a later entry with
    // the same name replaces; capacity applies to the deduplicated set.
    for (const auto& cookie : config.cookies_) {
        if (!is_valid_http_header_name(cookie.name()) || !::ruvia::is_valid_cookie_value(cookie.value())) {
            throw std::invalid_argument("configured HTTP client cookie is invalid");
        }
    }
    return result;
}

// Capacity of the deduplicated configured cookies, each stored with the
// default "/" path like a retained cookie.
inline void validate_http_client_configured_cookie_capacity(
    std::span<const http_header> cookies, std::size_t max_cookies, std::size_t max_cookie_bytes) {
    if (cookies.size() > max_cookies) {
        throw std::invalid_argument("configured HTTP client cookies exceed the client cookie limit");
    }
    std::size_t cookie_bytes = 0;
    for (const auto& cookie : cookies) {
        for (const auto bytes : {cookie.name().size(), cookie.value().size(), std::size_t{1}}) {
            if (bytes > max_cookie_bytes - cookie_bytes) {
                throw std::invalid_argument("configured HTTP client cookies exceed the client byte limit");
            }
            cookie_bytes += bytes;
        }
    }
}

struct http_client_config_storage final {
    http_client_config_storage(const http_client_config& source_value,
        client_tls_files files = client_tls_files::parse)
        : http_client_config_storage(
              source_value, validate_http_client_config(source_value), files) {}

    http_client_config_storage(
        const http_client_config_storage& source_value)
        : http_client_config_storage(source_value,
              http_client_origin_spec{.scheme_ = source_value.scheme_,
                  .origin_ = source_value.fixed_origin_ ? std::optional(source_value.origin()) : std::nullopt},
              client_tls_files::keep_paths) {}

    std::string host_;
    std::string dial_host_;
    std::uint16_t dial_port_;
    std::shared_ptr<const http_client_address_validator> address_validator_;
    std::optional<timeout_duration> dns_refresh_interval_;
    bool decode_content_;
    // False for a scheme-only base_url_: host_, uri_host_ and authority_ stay
    // empty, each request names its authority, and dial_host_ / dial_port_
    // (empty / 0 unless configured) default to that authority.
    bool fixed_origin_;
    // RFC 3986 uri-host (IPv6 literals bracketed) and the default-port-omitted
    // authority of the fixed origin, computed once at startup.
    std::string uri_host_;
    http_scheme scheme_;
    std::uint16_t port_;
    std::string authority_;
    http_client_connection_limits connection_limits_;
    std::size_t max_concurrent_http2_streams_per_connection_;
    std::size_t max_buffered_requests_;
    std::size_t max_cookies_;
    std::size_t max_cookie_bytes_;
    timeout_duration connect_timeout_;
    std::optional<timeout_duration> write_timeout_;
    std::optional<timeout_duration> read_timeout_;
    std::optional<timeout_duration> request_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<timeout_duration> idle_timeout_;
    std::optional<timeout_duration> http2_ping_interval_;
    std::size_t max_response_bytes_;
    http_client_versions versions_;
    http_client_advertisement_config advertisements_;
    http_client_push_config push_;
    http_client_http3_config http3_;
    client_tls_config_storage tls_;
    client_tcp_config tcp_;
    http_client_received_cookie_policy received_cookies_;
    std::string user_agent_;
    // Configured name/value cookie pairs, one allocation per pair; a later
    // configured entry replaces an earlier one of the same name in place.
    std::vector<http_header> cookies_;

    // Validating factory for the fixed origin. Owners that are never moved
    // cache the returned view, which borrows uri_host_. Requires fixed_origin_.
    [[nodiscard]] http_origin_view origin() const {
        const http_origin_options options{.host_ = uri_host_, .port_ = port_};
        return scheme_ == http_scheme::https ? http_origin_view::https(options) : http_origin_view::http(options);
    }

private:
    [[nodiscard]] static std::string_view transport_host(const std::optional<http_origin_view>& origin) noexcept {
        if (!origin) {
            return {};
        }
        auto host = origin->host();
        return host.starts_with('[') ? host.substr(1, host.size() - 2) : host;
    }
    [[nodiscard]] static std::string_view user_agent_view(const http_client_config& source_value) noexcept {
        return source_value.user_agent_.view();
    }
    [[nodiscard]] static std::string_view user_agent_view(const http_client_config_storage& source_value) noexcept {
        return source_value.user_agent_;
    }
    [[nodiscard]] static std::string_view dial_host_view(const http_client_config& source, const std::optional<http_origin_view>& origin) noexcept {
        return source.dial_host_ ? source.dial_host_->view() : transport_host(origin);
    }
    [[nodiscard]] static std::string_view dial_host_view(const http_client_config_storage& source, const std::optional<http_origin_view>&) noexcept {
        return source.dial_host_;
    }
    [[nodiscard]] static std::uint16_t dial_port_value(const http_client_config& source, const std::optional<http_origin_view>& origin) noexcept {
        if (source.dial_port_) {
            return source.dial_port_->value();
        }
        return origin ? origin->port() : std::uint16_t{0};
    }
    [[nodiscard]] static std::uint16_t dial_port_value(const http_client_config_storage& source, const std::optional<http_origin_view>&) noexcept {
        return source.dial_port_;
    }

    // Startup parses TLS files; asynchronous updates first own paths only.
    // Allocator-rebound copies share already prepared material.
    template <typename source_type>
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const source_type& source_value, http_scheme scheme, client_tls_files files) {
        if constexpr (std::same_as<source_type, http_client_config>) {
            return {source_value.tls_,
                scheme == http_scheme::https ? files : client_tls_files::keep_paths};
        } else {
            return {source_value.tls_};
        }
    }

    // Resolve representation differences at the boundary, then own every field
    // and cookie through one allocator-bound construction path.
    template <typename source_type>
        requires(std::same_as<source_type, http_client_config> ||
                    std::same_as<source_type, http_client_config_storage>)
    http_client_config_storage(
        const source_type& source_value, http_client_origin_spec spec, client_tls_files files)
        : host_(transport_host(spec.origin_)),
          dial_host_(dial_host_view(source_value, spec.origin_)),
          dial_port_(dial_port_value(source_value, spec.origin_)),
          address_validator_(source_value.address_validator_),
          dns_refresh_interval_(source_value.dns_refresh_interval_),
          decode_content_(source_value.decode_content_),
          fixed_origin_(spec.origin_.has_value()),
          uri_host_(spec.origin_ ? spec.origin_->host() : std::string_view{}),
          scheme_(spec.scheme_),
          port_(spec.origin_ ? spec.origin_->port() : (spec.scheme_ == http_scheme::https ? 443 : 80)),
          authority_(spec.origin_ ? make_http_origin_authority(origin()) : std::string{}),
          connection_limits_(source_value.connection_limits_),
          max_concurrent_http2_streams_per_connection_(source_value.max_concurrent_http2_streams_per_connection_),
          max_buffered_requests_(source_value.max_buffered_requests_),
          max_cookies_(source_value.max_cookies_),
          max_cookie_bytes_(source_value.max_cookie_bytes_),
          connect_timeout_(source_value.connect_timeout_),
          write_timeout_(source_value.write_timeout_),
          read_timeout_(source_value.read_timeout_),
          request_timeout_(source_value.request_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          idle_timeout_(source_value.idle_timeout_),
          http2_ping_interval_(source_value.http2_ping_interval_),
          max_response_bytes_(source_value.max_response_bytes_),
          versions_(source_value.versions_),
          advertisements_(source_value.advertisements_),
          push_(source_value.push_),
          http3_(source_value.http3_),
          tls_(tls_storage(source_value, scheme_, files)),
          tcp_(source_value.tcp_),
          received_cookies_(source_value.received_cookies_),
          user_agent_(user_agent_view(source_value)) {
        cookies_.reserve(source_value.cookies_.size());
        for (const auto& cookie : source_value.cookies_) {
            if constexpr (std::same_as<source_type, http_client_config>) {
                const auto existing = std::ranges::find_if(cookies_,
                    [name = cookie.name()](const http_header& entry) { return entry.name() == name; });
                if (existing != cookies_.end()) {
                    *existing = http_header(cookie.name(), cookie.value());
                    continue;
                }
            }
            cookies_.emplace_back(cookie.name(), cookie.value());
        }
        if constexpr (std::same_as<source_type, http_client_config>) {
            validate_http_client_configured_cookie_capacity(cookies_, max_cookies_, max_cookie_bytes_);
        }
    }
};

using http_client_definition_type = named_capability_definition<http_client_config_storage>;

}  // namespace ruvia::detail

namespace ruvia {
class response_stream_writer;
}

namespace ruvia::detail {

struct http_client_route;

class http_client_pool;
class http_client_response_memory_domain;

inline std::uint64_t next_nonzero_id(std::uint64_t& value) noexcept {
    if (++value == 0) {
        ++value;
    }
    return value;
}

// Owned by the response memory domain, not by a connection/session arena.
// At most one application chunk is queued; wire drivers borrow it until their
// protocol cursor commits it. Terminal wakeups never free a pending SSL span.
class http_client_output_queue final {
public:
    http_client_output_queue(const worker_ref& worker_value)
        : data_(worker_value),
          space_(worker_value) {}
    void notify_data() noexcept {
        data_.notify();
        if (wake_ != nullptr) {
            wake_(wake_target_);
        }
    }
    void acknowledge_chunk() noexcept {
        chunk_ready_ = false;
        http_clear_string_retaining_small(chunk_, http_body_buffer_bytes);
        space_.notify();
    }
    void finish() noexcept {
        ended_ = true;
        std::string(chunk_.get_allocator()).swap(chunk_);
        space_.notify();
    }
    void stop() noexcept {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        notify_data();
        space_.notify();
    }
    void* wake_target_{};
    void (*wake_)(void*) noexcept {};
    worker_signal data_;
    worker_signal space_;
    std::string chunk_;
    bool chunk_ready_{};
    bool end_requested_{};
    bool completion_pending_{};
    bool ended_{};
    bool stopped_{};
    bool operation_active_{};
    ::ruvia::operation_scope output_scope_;
};

// Transfers the response of an exchange or a push to its consumer once, after
// the final head arrived. Embedded in the address-stable response state; one
// started receive runs at a time, and the owner retiring closes the cold ones.
class http_client_response_handoff final {
public:
    [[nodiscard]] bool taken() const noexcept {
        return taken_;
    }
    [[nodiscard]] bool has_pending_receive() const noexcept {
        return scope_.has_pending_operations();
    }
    // A cold receive of `pin`'s response, bound to this handoff's scope;
    // `head_message` (static text) reports a failure before the head. Its
    // own `operation` interrupts only this wait.
    [[nodiscard]] task<http_client_response> receive(http_client_response pin, std::string_view head_message, operation_options operation);
    // The owner retires: its pending receives close, and once the response was
    // transferred the owner's handle no longer consumes it.
    void release(http_client_response& owner) noexcept;

private:
    [[nodiscard]] task<http_client_response> receive_owned(http_client_response pin, std::string_view head_message, operation_options operation);

    ::ruvia::operation_scope scope_;
    bool taken_{false};
    bool active_{false};
};

class http_client_tunnel_state final {
public:
    http_client_tunnel_state(const worker_ref& worker_value, http_client_tunnel_config config)
        : output_(worker_value),
          config_(config) {}
    http_client_output_queue output_;
    http_client_tunnel_config config_;
    bool udp_{};
    bool accepted_{};
    bool receive_ended_{};
};

class http_client_upload_state final {
public:
    http_client_upload_state(const worker_ref& worker_value, http_client_upload_config config)
        : output_(worker_value),
          config_(config) {}
    http_client_upload_state(worker_ref&&, http_client_upload_config) = delete;
    http_client_output_queue output_;
    http_client_upload_config config_;
    std::vector<http_header> trailers_;
    // The caller stopped sending through abandon(); the response stays valid.
    bool abandoned_{};
    // Upload-only failures belong to the writer, not to an otherwise valid
    // response. HTTP/1 may record them while the response is still arriving;
    // late failures in any protocol must preserve an already validated EOF.
    std::exception_ptr failure_;
    bool content_released_{};
    http_client_response_handoff response_;
    std::unique_ptr<http_client_request> redirect_request_;
};

// Composite per-stream deadline timer shared by HTTP/2 and HTTP/3 client
// streams: combines the operation timeout with active read/write inactivity
// deadlines and reschedules lazily when a deadline only moves later.
struct http_client_stream_timer final {
    using time_point = std::chrono::steady_clock::time_point;

    std::optional<time_point> read_deadline_;
    std::optional<time_point> write_deadline_;
    std::optional<time_point> timer_at_;
    worker_timer_registration timer_;

    void cancel() noexcept {
        timer_.cancel();
        timer_at_.reset();
    }

    [[nodiscard]] std::optional<time_point> earliest_deadline(const operation_deadline& timeout) const noexcept {
        auto deadline = timeout.deadline();
        for (const auto bound : {read_deadline_, write_deadline_}) {
            if (bound && (!deadline || *bound < *deadline)) {
                deadline = bound;
            }
        }
        return deadline;
    }

    [[nodiscard]] bool expired(const operation_deadline& timeout,
        time_point now = std::chrono::steady_clock::now()) const noexcept {
        return timeout.expired() || (read_deadline_ && *read_deadline_ <= now) ||
               (write_deadline_ && *write_deadline_ <= now);
    }

    template <typename callback_type>
    void schedule(const worker_ref& worker, const operation_deadline& timeout, callback_type&& on_expired) {
        const auto deadline = earliest_deadline(timeout);
        if (!deadline || (timer_at_ && *timer_at_ <= *deadline)) {
            return;
        }
        timer_.cancel();
        timer_at_.reset();
        if (worker.schedule_timer(timer_, *deadline,
                [on_expired = std::forward<callback_type>(on_expired)](worker_timer_outcome outcome) mutable noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        on_expired();
                    }
                }) == worker_timer_schedule_status::scheduled) {
            timer_at_ = deadline;
        }
    }

    template <typename timeout_source_type, typename expired_type, typename error_type>
    void schedule_with_retry(const worker_ref& worker, const timeout_source_type& timeout_source,
        expired_type on_expired, error_type on_error) {
        const auto current_timeout = [&timeout_source]() noexcept -> const operation_deadline& {
            if constexpr (std::is_pointer_v<timeout_source_type>) {
                return *timeout_source;
            } else {
                return timeout_source;
            }
        };
        schedule(worker, current_timeout(),
            [this, &worker, &timeout_source, current_timeout, on_expired, on_error]() noexcept {
                timer_at_.reset();
                if (expired(current_timeout())) {
                    on_expired();
                    return;
                }
                try {
                    schedule_with_retry(worker, timeout_source, on_expired, on_error);
                } catch (...) {
                    on_error(std::current_exception());
                }
            });
    }
};

template <typename wake_fn_type, typename stop_predicate_type, typename release_fn_type,
    typename send_chunk_fn_type, typename finish_fn_type>
[[nodiscard]] task<void> run_multiplexed_client_upload(const worker_ref& worker,
    const http_client_request_storage& request, http_client_output_queue& upload, worker_signal& stream_signal,
    wake_fn_type&& wake_fn, stop_predicate_type&& stop_predicate, release_fn_type&& release_content,
    send_chunk_fn_type&& send_chunk, finish_fn_type&& finish_upload) {
    upload.wake_target_ = &wake_fn;
    upload.wake_ = [](void* target) noexcept { (*static_cast<std::remove_reference_t<wake_fn_type>*>(target))(); };
    struct upload_wake_guard final {
        http_client_output_queue& upload_;
        ~upload_wake_guard() {
            upload_.wake_ = nullptr;
            upload_.wake_target_ = nullptr;
        }
    } wake_guard{upload};
    worker_timer_registration continue_timer;
    if (request.upload() != nullptr && !request.upload()->content_released_) {
        (void)worker.schedule_timer(continue_timer,
            deadline_after(request.upload()->config_.continue_timeout_.value()),
            [&upload, policy = request.upload()](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && !upload.stopped_) {
                    policy->content_released_ = true;
                    upload.notify_data();
                }
            });
    }
    while (!upload.ended_ && !upload.stopped_ && !stop_predicate()) {
        if (request.is_tunnel() ? !request.tunnel()->accepted_ : !request.upload()->content_released_) {
            co_await stream_signal.wait();
            continue;
        }
        continue_timer.cancel();
        if (!release_content()) {
            break;
        }
        if (upload.chunk_ready_) {
            if (co_await send_chunk(std::string_view(upload.chunk_))) {
                upload.acknowledge_chunk();
            }
        } else if (upload.end_requested_) {
            if (!(co_await finish_upload())) {
                break;
            }
            if (!stop_predicate() && !upload.stopped_ && !upload.ended_) {
                upload.finish();
            }
        } else {
            co_await stream_signal.wait();
        }
    }
}

enum class http_client_response_transport : std::uint8_t {
    unassigned,
    http1,
    http2,
    http3,
};

// Address-stable response storage. Network stream registration and connection
// leases are added here rather than to the movable public response wrapper.
class http_client_response_state final {
public:
    explicit http_client_response_state(http_client_response_memory_domain& memory_domain);

    // Body algorithms run on the address-stable storage owner. The public
    // facade tracks cold frames in body_operation_scope_; the algorithms claim
    // body_operation_active_ only when their lazy task starts. `operation` is
    // validated by the facade and timed from here.
    template <typename view>
    [[nodiscard]] task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> consume_body(operation_options operation, response_stream_writer* output = nullptr);
    [[nodiscard]] task<http_client_response_bytes> read_all(std::size_t max_bytes, operation_options operation);
    void retain_reference() noexcept;
    void release_reference() noexcept;
    [[nodiscard]] http_client_response_memory_domain& memory_domain() const noexcept {
        return memory_domain_;
    }
    // Response storage is worker-local: throws std::logic_error(`message`)
    // when called off the worker that owns it.
    void require_owner_worker(const char* message) const;
    void notify_producer_space() noexcept;
    // The consumer's wait for the final head: rethrows the producer's failure
    // or reports `head_message` for its error code. An encoded body is decoded
    // completely before the head is returned, so its failure is observed here.
    [[nodiscard]] task<void> await_head(std::string_view head_message);
    void rethrow_response_failure() const;
    // The running consumer (body read or head receive) or output operation's
    // own interruption, reported as http_client_error timeout / cancelled.
    [[nodiscard]] bool read_interrupted() const noexcept {
        return read_operation_ != nullptr && read_operation_->interrupted();
    }
    void throw_if_read_interrupted() const;
    void throw_if_output_interrupted() const;
    // A forward's downstream response_stream_writer step runs with the time
    // left in `steps`; http_client_error(timeout) once none is left.
    [[nodiscard]] operation_options downstream_step(const operation_steps& steps) const;
    // Rethrows the exception being handled, reporting a downstream step that
    // `steps` interrupted as this client's http_client_error.
    [[noreturn]] void rethrow_downstream_failure(const operation_steps& steps) const;
    void discard_response_body() noexcept;
    void discard_failed_encoded_response() noexcept;
    [[nodiscard]] task<void> dispatch_informational(const operation_deadline& timeout);
    void request_informational_stop() noexcept {
        informational_stop_requested_ = informational_stop_requested_ || informational_active_;
        if (informational_abort_) {
            informational_abort_->request_stop();
        }
    }
    void release_consumed_body_prefix();
    [[nodiscard]] bool append_received_body(std::string_view bytes_value, std::size_t max_response_bytes) {
        const auto retained = buffered_.size() - offset_ + pending_.size();
        if (collect_all_ && bytes_value.size() > max_response_bytes - (std::min)(retained, max_response_bytes)) {
            discard_response_body();
            return false;
        }
        // The consumer's last borrowed view lives in buffered, not pending.
        // Appending producer output must never relocate that view.
        pending_.append(bytes_value);
        data_signal_.notify();
        return true;
    }

    // One retention path: consume response-owned fields or copy borrowed fields.
    // Owned fields must already use this response's resource. A 100 (Continue)
    // releases request content gated on it.
    template <typename fields_type>
        requires(std::is_same_v<std::remove_cvref_t<fields_type>, std::span<const http_header>> ||
                 std::is_same_v<fields_type, std::vector<http_header>>)
    void retain_informational(http_status_code status, fields_type&& fields_value,
        std::optional<http_client_request_content_signal> request_content_signal) {
        constexpr bool transferring = std::is_same_v<fields_type, std::vector<http_header>>;
        http_client_informational_response head(status);
        if constexpr (!transferring) {
            head.headers_.reserve(fields_value.size());
        }
        // Fieldless heads still consume response-budgeted vector storage.
        // Each head's own field count is bounded by its decoder; across heads
        // only the retained bytes are bounded, so many small interim heads
        // (e.g. repeated 103 Early Hints) stay acceptable.
        auto budget = informational_budget_;
        for (const auto& field : fields_value) {
            if (!budget.add(field.name().size()) || !budget.add(field.value().size()) ||
                !budget.add(http_field_line_overhead)) {
                throw http_client_error(make_error_code(http_client_errc::protocol_error), "informational response metadata exceeds byte limit");
            }
            if constexpr (!transferring) {
                head.headers_.emplace_back(field.name(), field.value());
            }
        }
        if constexpr (transferring) {
            head.headers_ = std::move(fields_value);
        }
        informational_.push_back(std::move(head));
        informational_budget_ = budget;
        if (upload_ && request_content_signal == http_client_request_content_signal::continue_value) {
            upload_->content_released_ = true;
            upload_->output_.notify_data();
        }
    }

    [[nodiscard]] http_client_output_queue* output() noexcept {
        return tunnel_ ? &tunnel_->output_ : upload_ ? &upload_->output_
                                                     : nullptr;
    }
    [[nodiscard]] bool receive_complete() const noexcept {
        return complete_ || (tunnel_ && tunnel_->accepted_ && tunnel_->receive_ended_);
    }
    std::optional<http_client_tunnel_state> tunnel_;
    std::optional<http_client_upload_state> upload_;
    worker_signal head_signal_;
    worker_signal data_signal_;
    worker_signal space_signal_;
    http_client_pool* pool_{nullptr};
    http_received_status status_{http_status::ok};
    http_known_method request_method_{http_known_method::unknown};
    http_protocol_version protocol_version_{http_protocol_version::http11};
    std::optional<http_response_body_plan> response_body_plan_{};
    http_client_response_framing framing_{http_client_response_framing::none};
    std::optional<std::uint64_t> content_length_;
    http_client_connection_info connection_info_;
    bool request_written_{false};
    std::optional<timeout_duration> effective_connect_timeout_;
    std::optional<timeout_duration> effective_read_timeout_;
    std::optional<timeout_duration> effective_write_timeout_;
    std::optional<timeout_duration> effective_acquire_timeout_;
    std::shared_ptr<http_client_informational_callback> informational_callback_;
    stop_token informational_stop_;
    std::optional<stop_source> informational_abort_;
    bool informational_active_{false};
    bool informational_stop_requested_{false};
    bool decode_content_{true};
    std::vector<http_client_informational_response> informational_;
    // One aggregate byte budget across every retained interim head.
    http_field_section_budget informational_budget_;
    std::size_t informational_delivered_{};
    std::vector<http_header> headers_;
    std::vector<http_header> trailers_;
    std::string buffered_;
    // Producers only append here. The consumer swaps it with `buffered` before
    // returning a view, so later network progress cannot invalidate that view.
    std::string pending_;
    std::size_t offset_{0};
    std::size_t buffered_limit_{default_max_buffered_body_bytes};
    std::exception_ptr failure_;
    // The pool retired before the exchange completed; consumers report
    // http_client_error::closing with their own operation message.
    bool pool_closed_{false};
    std::size_t references_{1};
    bool head_ready_{false};
    bool complete_{false};
    bool abandoned_{false};
    bool collect_all_{false};
    bool body_operation_active_{false};
    // The interrupts of the running consumer and output operations; null
    // while none runs. Their source routes stop requests on this worker.
    const operation_interrupt* read_operation_{};
    const operation_interrupt* output_operation_{};
    bool body_decode_required_{false};
    // Content-Encoding parsed with the final head while decoding is pending.
    std::optional<http_content_coding_field_result> content_coding_{};
    http_client_response_transport transport_{http_client_response_transport::unassigned};
#ifdef RUVIA_ENABLE_HTTP2
    std::optional<::ruvia::http2_received_data_credit> http2_data_credit_{};
#endif
#ifdef RUVIA_ENABLE_HTTP3
    // Receive credit of buffered response DATA, returned as the consumer
    // takes it (RFC 9000 §4.1).
    std::optional<::ruvia::http3_received_data_credit> http3_data_credit_{};
#endif
    std::optional<std::deque<std::string>> http3_datagrams_;
    std::size_t http3_datagram_bytes_{0};
    bool http3_native_datagrams_{false};
    // Pool position and pool-unique serial of the connection owner carrying
    // this response; positions are reused once their owner is reclaimed.
    std::size_t connection_index_{0};
    std::uint64_t connection_serial_{0};
    std::uint64_t connection_generation_{0};
    // The request's route while its exchange runs: a final head may teach it
    // Alt-Svc. Null for pushes and once the request finished.
    const http_client_route* route_{};
    std::uint64_t request_id_{0};
    std::uint64_t cancellation_id_{0};
    std::uint64_t stream_id_{0};
    std::optional<http_push_request> promised_request_{};
    http_client_response_handoff push_response_;
    operation_interrupt_source interrupt_source_;
    // Declared after every field a body coroutine borrows, so the operation
    // scope closes while they are alive.
    ::ruvia::operation_scope body_operation_scope_;

private:
    void promote_pending_data();
    void detach_transport_bindings() noexcept;

    http_client_response_memory_domain& memory_domain_;
    http_client_response_state* previous_memory_state_{};
    http_client_response_state* next_memory_state_{};

    friend class http_client_response_memory_domain;
};

enum class http_client_operation_lane : std::uint8_t {
    read,
    output,
};

// Binds one operation's own timeout and stop token to a response lane while it
// runs. A read interruption wakes the consumer and keeps buffered bytes; an
// output interruption stops the output and abandons the exchange or tunnel,
// since a partly sent chunk cannot be withdrawn. An already-stopped token
// throws http_client_error(cancelled) before anything is armed.
class http_client_operation_interruption final {
public:
    http_client_operation_interruption(http_client_response_state& state, http_client_operation_lane lane,
        const operation_options& operation);
    ~http_client_operation_interruption();
    http_client_operation_interruption(const http_client_operation_interruption&) = delete;
    http_client_operation_interruption& operator=(const http_client_operation_interruption&) = delete;

private:
    static void wake_reader(void* state) noexcept;
    static void abort_output(void* state) noexcept;

    http_client_response_state& state_;
    http_client_operation_lane lane_;
    operation_interrupt interrupt_;
};

// Publish both identities before arm(), which may deliver cancellation inline.
// The owner must declare this after any generation/stream guard so callback
// retirement precedes unpublishing the operation.
template <typename target_type>
class http_client_response_cancellation final {
public:
    http_client_response_cancellation(const std::shared_ptr<target_type>& target,
        std::uint64_t& identity, http_client_response_state& response)
        : response_(response),
          registration_(target, identity) {
        response_.cancellation_id_ = registration_.id();
    }
    http_client_response_cancellation(std::shared_ptr<target_type>&&, std::uint64_t&, http_client_response_state&) = delete;
    http_client_response_cancellation(const std::shared_ptr<target_type>&&, std::uint64_t&, http_client_response_state&) = delete;
    http_client_response_cancellation(const http_client_response_cancellation&) = delete;
    http_client_response_cancellation& operator=(const http_client_response_cancellation&) = delete;
    http_client_response_cancellation(http_client_response_cancellation&&) = delete;
    http_client_response_cancellation& operator=(http_client_response_cancellation&&) = delete;
    ~http_client_response_cancellation() {
        registration_.reset();
        if (response_.cancellation_id_ == registration_.id()) {
            response_.cancellation_id_ = 0;
        }
    }
    void arm(const stop_token& stop) {
        registration_.arm(stop);
    }

private:
    http_client_response_state& response_;
    worker_cancellation_registration<target_type> registration_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_output_queue;
class http_client_request_storage;
class http_client_pool;
class http_client_response_state;
struct http_client_websocket_handshake;

#ifdef RUVIA_ENABLE_HTTP2
enum class http2_client_session_start_status : std::uint8_t {
    started,
    // transport_ names the failure, including EOF before the peer SETTINGS.
    transport_failed,
    protocol_failed,
};

struct http2_client_session_start_result final {
    http2_client_session_start_status status_{http2_client_session_start_status::started};
    client_transport_result transport_{};
};

enum class http2_client_data_status : std::uint8_t {
    // Every byte was submitted and its output reached the transport, or the
    // session ended meanwhile; the caller judges that end itself.
    submitted,
    // The caller's stop condition held, or the session ended, before every
    // byte was submitted.
    stopped,
    // The core refused the DATA: the stream no longer sends, or the input
    // contradicts its declared length or state (std::logic_error).
    rejected,
};

class http_client_connection;

// The HTTP/2 client connection driver: preface, the sole socket reader and
// writer, event drain, DATA submission under flow control, PING liveness and
// generation-safe failure over a borrowed client_tcp_transport. The writer
// transfers complete frame-bounded batches into its own storage, so a feed()
// or submit during an in-flight write never invalidates the bytes being
// written. Worker-affine and address-stable; the owner joins `tasks` before
// retiring it.
class http2_client_session final {
public:
    // `ping_interval` bounds silence: no received frame for one interval sends
    // a PING, and another silent interval fails the session with timeout.
    http2_client_session(http_client_connection& handler, client_tcp_transport& transport, task_scope& tasks,
        const worker_ref& worker_value, std::optional<timeout_duration> ping_interval);
    http2_client_session(http_client_connection&, client_tcp_transport&, task_scope&, worker_ref&&,
        std::optional<timeout_duration>) = delete;

    http2_client_session(const http2_client_session&) = delete;
    http2_client_session& operator=(const http2_client_session&) = delete;

    // Starts a new generation over the freshly connected transport: flushes the
    // client preface, reads until the peer SETTINGS (draining events through
    // the handler), then spawns the reader and writer. The previous
    // generation's tasks must have finished.
    [[nodiscard]] task<http2_client_session_start_result> start(
        http2_connection_options options, const operation_deadline& timeout);

    // The live connection of the current generation; check usable() first.
    [[nodiscard]] ::ruvia::http2_connection& connection() noexcept;
    [[nodiscard]] bool has_connection() const noexcept;
    // Accepts new streams: started, not failed and not draining.
    [[nodiscard]] bool usable() const noexcept;
    [[nodiscard]] std::size_t available_request_streams(std::size_t local_limit) const noexcept;

    // Reader or writer of some generation is still running.
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] bool writing() const noexcept;
    // begin_close() queued its GOAWAY and the generation has not ended yet.
    [[nodiscard]] bool closing() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    // GOAWAY, an unprocessed request or exhausted stream IDs: no new streams.
    void mark_draining() noexcept;
    void wake_writer() noexcept;
    // Notified after every input batch, state transition and completed write.
    [[nodiscard]] worker_signal& state_signal() noexcept;
    // Notified only when the request-stream capacity grew (a stream closed or
    // the peer raised SETTINGS_MAX_CONCURRENT_STREAMS) or the session stopped
    // admitting streams, so a parked request submitter is not woken by
    // unrelated input. A submitter's own cancellation notifies it explicitly.
    [[nodiscard]] worker_signal& capacity_signal() noexcept;

    // Fails the current generation once: aborts the transport, reports the
    // failure to the handler and wakes every waiter.
    void fail(const client_transport_result& transport, std::exception_ptr failure = {}) noexcept;

    // The one client-initiated close of a running generation (RFC 9113
    // section 6.8: GOAWAY before closing). Stops admitting streams, queues
    // GOAWAY(NO_ERROR) and lets the writer fail the generation with `transport`
    // once every queued frame reached the transport, or after
    // one second. Streams still open are not cancelled
    // here; the bound ends them with the generation. Returns true while that
    // close is pending. Without a running generation that can send the frame
    // (or a timer to bound it) the generation fails at once and this returns
    // false.
    bool begin_close(const client_transport_result& transport) noexcept;

    // Requests a RST_STREAM (CANCEL unless the stream's input was malformed);
    // allocation failure fails the connection.
    void submit_reset(std::uint32_t stream_id, http2_error_code error = http2_error_code::cancel) noexcept;

    // Submits `bytes` on `stream_id` in DATA frames of at most `chunk_bytes`,
    // waiting for each batch to reach the transport and for send window.
    // An empty input ending the stream is still one DATA frame, sent only
    // while both send windows are positive (RFC 9113 section 6.9).
    // `stop()` is checked before each submission and after each wait. Input
    // beyond the declared Content-Length is rejected (invalid_message), as is
    // a stream that no longer sends (std::logic_error); other exceptions
    // propagate.
    template <typename stop_type>
    [[nodiscard]] task<http2_client_data_status> send_data(std::uint32_t stream_id, std::string_view bytes,
        http2_end_stream end_stream, std::size_t chunk_bytes, stop_type stop, bool* end_submitted = nullptr) {
        const auto generation = generation_;
        bool end_pending = http2_ends_stream(end_stream);
        while (!bytes.empty() || end_pending) {
            if (ended(generation) || stop()) {
                co_return http2_client_data_status::stopped;
            }
            if (connection_->stream_aborted(stream_id)) {
                co_return http2_client_data_status::rejected;
            }
            http2_data_submit_result submitted{};
            try {
                submitted = connection_->submit_data(stream_id, bytes, end_stream, chunk_bytes);
            } catch (const std::logic_error&) {
                co_return http2_client_data_status::rejected;
            }
            bytes.remove_prefix(submitted.consumed_bytes_);
            if (submitted.status_ == http2_data_submit_status::complete) {
                if (end_pending && end_submitted != nullptr) {
                    *end_submitted = true;
                }
                end_pending = false;
            }
            wake_writer();
            if (submitted.status_ == http2_data_submit_status::closed ||
                submitted.status_ == http2_data_submit_status::invalid_message) {
                co_return http2_client_data_status::rejected;
            }
            if (submitted.status_ == http2_data_submit_status::expectation_pending) {
                // 100 (Continue) and the continue timeout both wake state waiters.
                co_await state_signal_.wait();
                continue;
            }
            co_await wait_output_flushed(stop);
            if (submitted.status_ == http2_data_submit_status::backpressured) {
                co_await wait_send_window(generation, stream_id, stop);
            }
        }
        co_return http2_client_data_status::submitted;
    }

    // Ends a request stream with `trailers`, or without them with an empty
    // END_STREAM DATA frame, which waits for positive send windows like
    // send_data(). Rejected when the stream no longer sends (std::logic_error),
    // the content is incomplete or a trailer is invalid (invalid_message).
    template <typename stop_type>
    [[nodiscard]] task<http2_client_data_status> finish_request(
        std::uint32_t stream_id, std::span<const http_header_view> trailers, stop_type stop) {
        const auto generation = generation_;
        for (;;) {
            if (ended(generation) || stop()) {
                co_return http2_client_data_status::stopped;
            }
            if (connection_->stream_aborted(stream_id)) {
                co_return http2_client_data_status::rejected;
            }
            http2_finish_status finished{};
            try {
                finished = connection_->finish_request(stream_id, trailers);
            } catch (const std::logic_error&) {
                co_return http2_client_data_status::rejected;
            }
            wake_writer();
            if (finished == http2_finish_status::accepted) {
                co_await wait_output_flushed(stop);
                co_return http2_client_data_status::submitted;
            }
            if (finished != http2_finish_status::backpressured) {
                co_return http2_client_data_status::rejected;
            }
            co_await wait_send_window(generation, stream_id, stop);
        }
    }

    [[nodiscard]] bool output_flushed() const noexcept;

    // Waits until every queued frame reached the transport, the session ended
    // or `stop()` holds.
    template <typename stop_type>
    [[nodiscard]] task<void> wait_output_flushed(stop_type stop) {
        const auto generation = generation_;
        while (!ended(generation) && (connection_->wants_write() || writing_) && !stop()) {
            co_await state_signal_.wait();
        }
    }

    // Waits until the stream has send window again, WINDOW_UPDATE and
    // SETTINGS producing no stream event, or the session ended, the stream
    // was aborted or `stop()` holds.
    template <typename stop_type>
    [[nodiscard]] task<void> wait_send_window(std::uint64_t generation, std::uint32_t stream_id, stop_type& stop) {
        while (!ended(generation) && !stop()) {
            const auto window = connection_->send_window_state(stream_id);
            if (!window || window->available_ != 0 || connection_->stream_aborted(stream_id)) {
                co_return;
            }
            co_await state_signal_.wait();
        }
    }

    // Drops the retired connection once no generation is running. Escaped
    // DATA credits stay valid; they only reference the memory resource.
    void reset() noexcept;

private:
    static constexpr std::size_t read_buffer_bytes = 16384;
    static constexpr std::size_t write_batch_bytes = 2 * http2_default_max_frame_size;

    [[nodiscard]] bool ended(std::uint64_t generation) const noexcept;

    void schedule_ping(std::uint64_t generation, std::chrono::steady_clock::time_point deadline);

    // Any received frame, the PING ACK included, proves liveness (as Go's
    // ReadIdleTimeout): one silent interval sends a PING, and the session
    // fails only when no input arrived during the interval after it. Input is
    // compared with when the PING was sent, not with the timer's firing time,
    // which may trail its deadline.
    void check_liveness(std::uint64_t generation) noexcept;

    void take_output_batch(std::string& output);

    [[nodiscard]] bool feed(std::string_view bytes_value);

    void drain();

    // Every input batch and completed write may close streams or apply peer
    // SETTINGS; only a grown request capacity wakes capacity waiters.
    void publish_state() noexcept;

    void fail(std::uint64_t generation, const client_transport_result& transport, std::exception_ptr failure) noexcept;

    void finish_task(std::uint64_t generation) noexcept;

    struct task_guard final {
        http2_client_session& session_;
        std::uint64_t generation_;
        ~task_guard();
    };

    [[nodiscard]] task<void> run_reader(std::uint64_t generation);

    [[nodiscard]] task<void> run_writer(std::uint64_t generation);

    http_client_connection& handler_;
    client_tcp_transport& transport_;
    task_scope& tasks_;
    const worker_ref& worker_;
    std::optional<timeout_duration> ping_interval_;
    std::optional<::ruvia::http2_connection> connection_;
    http_io_buffer read_buffer_;
    std::string write_buffer_;
    worker_signal write_signal_;
    worker_signal state_signal_;
    worker_signal capacity_signal_;
    std::uint64_t generation_{0};
    std::size_t session_tasks_{0};
    // Request-stream capacity at the last publish_state().
    std::size_t request_capacity_{0};
    // When the reader last received bytes, while ping_interval_ is set.
    std::chrono::steady_clock::time_point last_input_{};
    // When the outstanding liveness PING was queued.
    std::chrono::steady_clock::time_point ping_sent_at_{};
    // The generation's end once begin_close() flushed its GOAWAY.
    client_transport_result close_result_{};
    bool failed_{false};
    bool draining_{false};
    bool writing_{false};
    // begin_close() is pending for the current generation.
    bool closing_{false};
    // A liveness PING is outstanding: no input arrived since ping_sent_at_.
    bool ping_pending_{false};
    // Last: destroyed first, before any state their completions read.
    worker_timer_registration close_timer_;
    worker_timer_registration ping_timer_;
};
#endif

// One physical connection owner of an http_client_pool for one route: its
// transport, the HTTP/1 parse buffers and the HTTP/2 session with its stream
// registry. Reconnects keep the route. HTTP/1 exchanges run through
// http1_client_exchange; the pool owns admission, policy and reclamation.
// Worker-affine and address-stable.
class http_client_connection final {
public:
    enum class protocol_type : std::uint8_t { unknown,
        http1,
        http2 };

#ifdef RUVIA_ENABLE_HTTP2
    // One registered HTTP/2 request or push stream; a stack or driver record
    // that unregisters before its response state can be released.
    struct http2_stream final {
        http2_stream(const worker_ref& worker_value, http_client_response_state& state_value)
            : signal_(worker_value),
              state_(&state_value) {}
        http2_stream(worker_ref&&, http_client_response_state&) = delete;

        worker_signal signal_;
        http_client_response_state* state_;
        http_client_websocket_handshake* websocket_{};
        std::optional<std::error_code> error_;
        std::exception_ptr failure_;
        std::size_t registry_index_{0};
        std::uint64_t request_id_{0};
        std::uint64_t cancellation_id_{0};
        std::uint32_t stream_id_{0};
        // Borrowed from the request or push driver while registered.
        const operation_deadline* timeout_{nullptr};
        std::string_view request_target_;
        http_client_stream_timer timer_;
        bool complete_{false};
        // Caller-owned buffered suffix or its socket output is still pending.
        bool request_body_pending_{false};
        // The tunnel's final DATA frame entered session output. Its transport
        // completion can precede the upload coroutine's scheduled resumption.
        bool tunnel_end_submitted_{false};
        bool retryable_{false};
        // The peer refused the request with RST_STREAM(HTTP_1_1_REQUIRED)
        // before any response head (RFC 9113 section 7).
        bool http11_required_{false};
        // The request addresses the fixed origin, whose jar retains the
        // response's Set-Cookie fields.
        bool addresses_origin_{true};

        [[nodiscard]] bool failed() const noexcept {
            return error_.has_value() || failure_ != nullptr;
        }
    };
#endif

    // `server_name` is the TLS SNI and verified name (empty for http); `alpn`
    // is the TLS ALPN offer, and over plaintext http2 selects prior-knowledge
    // h2c, anything else HTTP/1.1.
    http_client_connection(http_client_pool& pool, std::size_t index, std::uint64_t serial,
        std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, bool alias_route,
        client_alpn alpn);
    void capture_connection_info(http_client_response_state& response, bool reused) const;
    void retire_addresses() noexcept;
    [[nodiscard]] std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    ~http_client_connection();
    http_client_connection(const http_client_connection&) = delete;
    http_client_connection& operator=(const http_client_connection&) = delete;

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }
    [[nodiscard]] std::uint64_t serial() const noexcept {
        return serial_;
    }
    // The route is the alias default, authenticated for the alias origin.
    [[nodiscard]] bool alias_route() const noexcept {
        return alias_route_;
    }
    [[nodiscard]] bool serves(std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name,
        client_alpn alpn) const noexcept;
    // Kept between uses: an HTTP/1 connection between exchanges, or an HTTP/2
    // session admitting streams without any.
    [[nodiscard]] bool idle() const noexcept;
    // Nothing runs on or borrows this owner any longer; the pool may destroy it.
    [[nodiscard]] bool reclaimable() const noexcept;
#ifdef RUVIA_ENABLE_HTTP2
    // Advisory only; the worker-affine request driver consumes the selected
    // capacity synchronously before another request can select it.
    [[nodiscard]] std::size_t available_http2_request_streams() const noexcept;
#endif
    // No transport, operation, session or stream uses this slot: a request
    // routed here connects at once.
    [[nodiscard]] bool idle_unconnected() const noexcept {
#ifdef RUVIA_ENABLE_HTTP2
        if (http2_.running() || !http2_streams_.empty()) {
            return false;
        }
#endif
        return !connected_ && http1_operations_ == 0;
    }

    // Serialized by connect_scheduler_. Returns true only when the existing
    // transport remains connected and is reused; a replacement returns false.
    // A previous HTTP/2 generation that is draining or still unwinding is
    // waited for first, within `acquire_timeout`.
    [[nodiscard]] task<bool> ensure_connected(
        const operation_deadline& timeout, const operation_deadline& acquire_timeout,
        stop_token stop_token_value, http_client_response_state& response);
    // Terminal close of the current connection; a running HTTP/2 session
    // fails through its single transition so every stream is woken.
    void close() noexcept;
    // Starts the idle bound of a connected HTTP/1 connection between exchanges
    // or an HTTP/2 connection without streams; its expiry closes the
    // connection unless it is in use again. A bound that cannot be armed
    // closes the connection at once. An idle HTTP/1 connection is also
    // watched: peer EOF, reset or unsolicited bytes close it at once.
    void begin_idle() noexcept;
    void end_idle() noexcept;
    // Closes a connection nothing uses; an HTTP/2 session sends GOAWAY first.
    void close_idle() noexcept;
    // Applies the retention policy to both buffers. Only between exchanges,
    // when no transport operation borrows their storage.
    void retain_idle_buffers() noexcept;
    // Pool shutdown: permanently refuses new leases, aborts the connect or
    // HTTP/1 exchange in progress with closing, then closes. An HTTP/2
    // session without streams closes with GOAWAY first.
    void shutdown() noexcept;
    // Lease cancellation: aborts the connect or HTTP/1 exchange in progress
    // with `reason` and closes the transport.
    void cancel(std::uint64_t generation, client_abort_reason reason) noexcept;
    // Stops the bound HTTP/1 response's output and wakes its producer waits.
    [[nodiscard]] bool cancel_by_id(std::uint64_t cancellation_id) noexcept;
    void abort_http1_response() noexcept;
    // Throws the HTTP client error of the transport's abort reason, if any.
    void throw_if_aborted() const;

#ifdef RUVIA_ENABLE_HTTP2
    // HTTP/2 (http2/http_client_http2.cpp). False when the multiplexed session
    // had ended (or the peer required HTTP/1.1, RFC 9113 section 7) and its
    // replacement connection speaks HTTP/1.1 instead: nothing was sent, and
    // the caller runs the request as an HTTP/1 exchange on that connection.
    // (ensure_connected() already rejects that outcome for an http2_only
    // client.) `acquire_timeout` bounds the first attempt's reconnect and
    // stream-capacity waits; a retry attempt gets its own acquisition bound.
    [[nodiscard]] task<bool> execute_http2(const http_client_request_storage& request,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop_token_value,
        http_client_response_state& state_value);
    // Registers a stream whose timeout_ is set and arms its deadline timer.
    void register_http2_stream(http2_stream& stream);
    void remove_http2_stream(http2_stream& stream) noexcept;
    void cancel_http2_stream(std::uint64_t request_id, client_abort_reason reason,
        http2_error_code error = http2_error_code::cancel) noexcept;
    [[nodiscard]] std::uint64_t next_http2_request_id() noexcept;
    void reprioritize(http_client_response_state& state_value, http_priority priority);
    // Wakes the writer to flush WINDOW_UPDATE for released DATA credit; the
    // stream's read inactivity bound restarts as the peer may send again.
    void release_response_data(http_client_response_state& state_value) noexcept;

    // http2_client_session handler.
    void on_http2_event(http2_event& event);
    [[nodiscard]] task<void> deliver_http2_informational(std::uint32_t stream_id);
    [[nodiscard]] std::optional<timeout_duration> http2_write_timeout() const noexcept;
    void on_http2_failure(const client_transport_result& transport, std::exception_ptr failure) noexcept;
    // http2_client_session handler: request-stream capacity grew, or the last
    // session task ended.
    void on_http2_capacity() noexcept;
    void on_http2_stopped() noexcept;
#endif

private:
    // The route's transport names; transport_ borrows them, so they precede it.
    std::string dial_host_;
    std::string server_name_;

public:
    client_tcp_transport transport_;
    http_io_buffer read_buffer_;
    http_io_buffer write_buffer_;
#ifdef RUVIA_ENABLE_HTTP2
    http2_client_session http2_;
#endif
    pool_lease_scheduler connect_scheduler_;
    pool_lease_scheduler http1_scheduler_;
    // HTTP/1 exchanges holding or waiting for http1_scheduler_.
    std::size_t http1_operations_{0};
    // Borrowed only while one serialized HTTP/1 exchange is active.
    http_client_response_state* http1_response_{nullptr};
    // Bumped around every connect and HTTP/1 exchange so stale cancellations
    // cannot reach a later operation.
    std::uint64_t generation_{0};
    std::uint64_t cancellation_id_{0};
    protocol_type protocol_{protocol_type::unknown};
    bool connected_{false};
    bool retired_{false};
    bool address_retired_{false};
    bool fresh_{false};
    // The connection key's offer; reconnects keep it.
    const client_alpn alpn_;
    // Requests the pool routed here that have not released it yet.
    std::size_t users_{0};
    // Order in which the connection last turned idle (larger is newer); 0
    // while it is in use or after it closed.
    std::uint64_t idle_order_{0};

private:
#ifdef RUVIA_ENABLE_HTTP2
    struct http2_state_waiter final {
        std::uint64_t cancellation_id_{};
        http2_state_waiter* next_{};
    };
    [[nodiscard]] http2_stream* find_http2_stream(std::uint32_t stream_id) noexcept;
    [[nodiscard]] http2_stream* find_http2_request(std::uint64_t request_id) noexcept;
    [[nodiscard]] bool http2_exchange_complete(const http2_stream& stream) noexcept;
    void fail_http2_stream(http2_stream& stream, std::exception_ptr failure, bool reset_stream) noexcept;
    // Wakes a stream's driver, and capacity waiters while it has no stream ID.
    void wake_http2_stream(http2_stream& stream) noexcept;
    // Peer input for the stream restarts its read inactivity bound.
    void restart_http2_stream_read(http2_stream& stream);
    // Arms timer_ for the earlier of the total and read deadlines.
    void schedule_http2_stream_timer(http2_stream& stream);
    void arm_http2_write_timer(worker_timer_registration& write_timer, const http2_stream& stream);
#endif
#ifdef RUVIA_ENABLE_HTTP2
    // Client close of a session that admits no further request: GOAWAY first
    // (RFC 9113 section 6.8), bounded by the session.
    void close_http2_session() noexcept;
#endif
    // Pingora's idle_poll: one watcher per connection, parked while the
    // connection is in use and retired with the pool.
    [[nodiscard]] task<void> watch_idle_http1();
#ifdef RUVIA_ENABLE_HTTP2
    void publish_http2_head(http2_stream& stream, http2_response_head_event& event);
    void consume_http2_data(std::uint32_t stream_id, std::string_view bytes, http2_received_data_credit credit);
    void finish_http2_message(std::uint32_t stream_id, http2_message_end_event& end);
    [[nodiscard]] task<bool> send_http2_request_data(http2_stream& stream, std::string_view bytes,
        http2_end_stream end_stream, const http_client_output_queue* upload = nullptr);
    [[nodiscard]] task<void> stream_http2_upload(const http_client_request_storage& request, http2_stream& stream,
        http_client_output_queue& upload);
    // The one wait for a previous HTTP/2 generation to retire: its session and
    // every stream that borrows its transport.
    [[nodiscard]] task<void> wait_for_http2_session_stop(
        const operation_deadline& timeout, stop_token stop_token_value, http_client_response_state& response);
#endif

    http_client_pool& pool_;
    std::size_t index_;
    std::uint64_t serial_;
    bool alias_route_;
    std::uint64_t connection_id_{};
    ip_address peer_ip_{};
#ifdef RUVIA_ENABLE_HTTP2
    std::vector<http2_stream*> http2_streams_;
    http2_stream* last_http2_stream_{nullptr};
    std::uint64_t next_http2_request_id_{0};
    // Request heads accepted by the current HTTP/2 generation: a later
    // request's stream runs on a reused connection.
    std::size_t http2_session_requests_{0};
    http2_state_waiter* state_waiters_{};
#endif
    // Idle watch of an HTTP/1 connection: wanted between begin_idle() and
    // end_idle(); each end bumps idle_period_ so a late readiness of an
    // earlier period is ignored.
    worker_signal idle_watch_signal_;
    std::uint64_t idle_period_{0};
    bool idle_watch_wanted_{false};
    // The watcher's readiness wait is pending and not yet cancelled.
    bool idle_wait_pending_{false};
    bool idle_watcher_started_{false};
    // Last: destroyed first, so its completion never sees a retiring connection.
    worker_timer_registration idle_timer_;
};

}  // namespace ruvia::detail

#ifdef RUVIA_ENABLE_HTTP3
namespace ruvia::detail {
class http_client_pool;
class http_client_request_storage;
class http_client_response_state;

// The QUIC endpoint and TLS identity of one HTTP/3 connection owner.
struct http3_client_route final {
    std::string host_;
    std::uint16_t port_;
    std::string server_name_;
};

// QUIC TLS shared by one pool's HTTP/3 connections: one SSL_CTX and resumption
// tickets isolated per route, so a ticket never resumes another server name or
// dial target. Least recently used routes give way beyond the idle bound.
class http3_client_tls final {
public:
    struct ticket;
    explicit http3_client_tls(http_client_pool& pool);
    ~http3_client_tls();
    http3_client_tls(const http3_client_tls&) = delete;
    http3_client_tls& operator=(const http3_client_tls&) = delete;
    [[nodiscard]] ticket* find(const http3_client_route& route) noexcept;
    [[nodiscard]] ticket& store(const http3_client_route& route);
    // A DNS change retires the endpoints remembered for 0-RTT.
    void forget_endpoints() noexcept;

    asio::ssl::context context_;

private:
    std::vector<ticket> tickets_;
    std::size_t capacity_;
    std::uint64_t clock_{};
};

// Worker-affine UDP/TLS/QUIC session owner of one route. The pool keeps this
// owner alive until it is reclaimable, and through join() on shutdown,
// including cancellation completions of both UDP directions.
class http3_client_connection final {
public:
    http3_client_connection(http_client_pool& pool, http3_client_tls& tls, std::size_t index, std::uint64_t serial,
        std::string_view host, std::uint16_t port, std::string_view server_name);
    ~http3_client_connection();
    http3_client_connection(const http3_client_connection&) = delete;
    http3_client_connection& operator=(const http3_client_connection&) = delete;
    [[nodiscard]] task<bool> execute(const http_client_request_storage& request, bool fallback,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout,
        stop_token stop, http_client_response_state& response);
    void close() noexcept;
    void retire_addresses() noexcept;
    [[nodiscard]] std::uint64_t connection_id() const noexcept;
    void shutdown() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] bool cancel_by_id(std::uint64_t id) noexcept;
    void abandon(http_client_response_state& response, bool malformed) noexcept;
    void release_data(http_client_response_state& response) noexcept;
    void reprioritize(http_client_response_state& response, http_priority priority);
    [[nodiscard]] std::size_t available() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] task<void> migrate(std::string_view local_address, std::uint16_t port,
        const operation_deadline& timeout, stop_token stop);
    [[nodiscard]] task<bool> send_datagram(http_client_response_state& response, std::span<const std::byte> bytes);
    [[nodiscard]] std::uint64_t serial() const noexcept {
        return serial_;
    }
    [[nodiscard]] const http3_client_route& route() const noexcept {
        return route_;
    }
    // The current QUIC peer while a session is open.
    [[nodiscard]] std::optional<asio::ip::address> peer_address() const noexcept;
    [[nodiscard]] bool serves(std::string_view host, std::uint16_t port, std::string_view server_name) const noexcept;
    // An endpoint selection (resolve and handshake race) is in progress;
    // further requests of the route queue behind it.
    [[nodiscard]] bool connecting() const noexcept;
    // An open session without streams.
    [[nodiscard]] bool idle() const noexcept;
    // Nothing runs on or borrows this owner any longer; the pool may destroy it.
    [[nodiscard]] bool reclaimable() const noexcept;

    bool fresh_{false};
    // Requests the pool routed here that have not released it yet.
    std::size_t users_{0};
    // Order in which the owner last turned idle (larger is newer); 0 in use.
    std::uint64_t idle_order_{0};

private:
    class session;
    class address_race;
    [[nodiscard]] task<void> select_endpoint(bool allow_early,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout,
        stop_token stop, http_client_response_state& response);
    http3_client_tls& tls_;
    http3_client_route route_;
    std::uint64_t serial_;
    std::unique_ptr<session> session_;
    std::unique_ptr<address_race> race_;
};
}  // namespace ruvia::detail
#endif

namespace ruvia::detail {
class http_client_advertisement_memory;

// The pool owns this bounded queue. Returned owners retain their separate
// retained-bytes accounting, not the client pool or its transport lifecycle.
class http_client_advertisement_queue final {
public:
    http_client_advertisement_queue(const worker_ref& worker_value, http_client_advertisement_config config);
    http_client_advertisement_queue(worker_ref&&, http_client_advertisement_config) = delete;
    ~http_client_advertisement_queue();
    http_client_advertisement_queue(const http_client_advertisement_queue&) = delete;
    http_client_advertisement_queue& operator=(const http_client_advertisement_queue&) = delete;

    // Drops (counted by dropped()) an observation beyond the configured bounds.
    void retain(std::size_t slot, http_protocol_version protocol, const http_origin_advertisement& origins);
    void retain(std::size_t slot, const http_alternative_service_advertisement& service);
    [[nodiscard]] std::optional<http_client_advertisement> next();
    void retire() noexcept;
    [[nodiscard]] std::size_t dropped() const noexcept {
        return dropped_;
    }

private:
    void require_current() const noexcept;
    [[nodiscard]] bool admit(std::size_t bytes) noexcept;
    const worker_ref& worker_;
    http_client_advertisement_config config_;
    http_client_advertisement_memory* memory_{};
    std::deque<http_client_advertisement> pending_;
    std::size_t dropped_{};
};
}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_connection;
struct http_tunnel_input;
class http_client_response_state;

class http_client_pool;
using http_client_cancellation_target = worker_cancellation_target<http_client_pool>;

// The one retry rule of both wire protocols for a request that failed on a
// reused connection (Pingora RetryType::ReusedOnly): replayed once on a new
// connection only after a plain I/O failure (TLS failures, timeouts and
// cancellation excluded), for an idempotent request (RFC 9110 section 9.2.2)
// whose content is buffered, before any response head was published.
[[nodiscard]] bool can_retry_reused_connection_request(std::optional<std::error_code> failure,
    const http_client_request_storage& request, const http_client_response_state& response) noexcept;

// One origin client on one worker: request admission and buffering limits,
// on-demand connections keyed by route (dial host, dial port, TLS server name,
// ALPN offer) with open and idle bounds, origin policy (cookies, automatic
// headers, push acceptance) and statistics. Wire protocols run in
// http_client_connection (HTTP/2 session), http1_client_exchange and
// http3_client_connection.
class http_client_pool final {
public:
    // Process-lifetime storage backs the pool and its retained responses.
    http_client_pool(asio::io_context& io_context, const worker_ref& worker_value,
        http_client_config_storage config, const http_client_worker_services& services);
    http_client_pool(asio::io_context&, worker_ref&&, http_client_config_storage,
        const http_client_worker_services&) = delete;
    ~http_client_pool();
    http_client_pool(const http_client_pool&) = delete;
    http_client_pool& operator=(const http_client_pool&) = delete;

    [[nodiscard]] task<http_client_response> execute(http_client_request_storage request, operation_options options);
    [[nodiscard]] task<http_client_exchange> open_request(
        http_client_request_storage request, http_client_upload_config upload, operation_options options);
    [[nodiscard]] task<http_client_tunnel_result> open_tunnel(
        http_client_request_storage request, http_client_tunnel_config config, operation_options options);
    void close_now() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] http_client_stats stats() const noexcept;
    [[nodiscard]] std::vector<http_client_endpoint_address> dns_snapshot() const;
    void update_dns(std::span<const http_client_endpoint_address> addresses);
    [[nodiscard]] task<void> refresh_dns(operation_options options);
    [[nodiscard]] bool close_connection(std::uint64_t id) noexcept;
    [[nodiscard]] std::uint64_t next_connection_id() noexcept;
    [[nodiscard]] task<client_resolve_result> resolve_addresses(std::string_view host, std::uint16_t port,
        client_host_resolver& resolver, const operation_deadline& timeout);
    [[nodiscard]] std::optional<http_client_advertisement> next_advertisement() {
        return advertisements_.next();
    }
    [[nodiscard]] std::optional<http_client_push> next_push();
    [[nodiscard]] std::string_view host() const noexcept {
        return config_.host_;
    }
    [[nodiscard]] std::uint16_t port() const noexcept {
        return config_.port_;
    }
    [[nodiscard]] http_scheme scheme() const noexcept {
        return config_.scheme_;
    }

    // Response-state operations of the public response, tunnel and push owners.
    // Malformed input resets the owning protocol stream with its protocol
    // error; abandonment otherwise cancels it.
    void abandon_response(http_client_response_state& state_value, bool malformed = false) noexcept;
    void reprioritize(http_client_response_state& state_value, http_priority priority);
    void release_response_data(http_client_response_state& state_value) noexcept;
    // TLS-authenticated responses of `route` update its entries in this pool's
    // bounded Alt-Svc cache.
    void observe_alt_svc(const http_client_route& route, std::span<const http_header> fields);
    [[nodiscard]] task<void> migrate_http3(std::string local_address, std::uint16_t port,
        operation_options options, stop_token host_stop);
#ifdef RUVIA_ENABLE_HTTP3
    // `operation` follows the client's per-operation contract: an interrupted
    // read keeps queued input, an interrupted send abandons the tunnel.
    [[nodiscard]] task<std::optional<http_tunnel_input>> read_http3_tunnel_input(
        http_client_response_state& state, operation_options operation);
    [[nodiscard]] task<bool> send_http3_datagram(
        http_client_response_state& state, std::string_view payload, operation_options operation);
#endif
    [[nodiscard]] http_client_response_memory_domain& response_memory() noexcept {
        return *response_memory_;
    }
    // worker_cancellation_target hook; routes to TCP and HTTP/2 owners.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

    // Origin services shared by the pool's connections and exchange drivers.
    [[nodiscard]] asio::io_context& io_context() noexcept {
        return io_context_;
    }
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] const http_client_config_storage& config() const noexcept {
        return config_;
    }
    // The fixed origin; std::logic_error for an alias without one.
    [[nodiscard]] const http_origin_view& origin() const {
        if (!origin_) {
            throw std::logic_error("HTTP client alias has no fixed origin");
        }
        return *origin_;
    }
    [[nodiscard]] client_request_policy& policy() noexcept {
        return policy_;
    }
    [[nodiscard]] http_client_advertisement_queue& advertisements() noexcept {
        return advertisements_;
    }
    [[nodiscard]] task_scope& tasks() noexcept {
        return background_tasks_;
    }
    [[nodiscard]] stop_token pool_stop_token() const noexcept {
        return dns_stop_.token();
    }
    [[nodiscard]] const std::shared_ptr<http_client_cancellation_target>& cancellation_target() const noexcept {
        return cancellation_target_;
    }
    [[nodiscard]] asio::ssl::context& tls_context() noexcept {
        return tls_context_;
    }
    [[nodiscard]] client_wire_counters& wire_counters() noexcept {
        return wire_counters_;
    }
    void record_idle_connection_with_pending_data() noexcept {
        ++idle_connections_with_pending_data_;
    }
    // Worker idle bound (http_client_worker_limits): this pool's idle count of
    // one kind (an upper bound until recount_idle()), its least recently used
    // idle connection's order, and closing that connection.
    [[nodiscard]] std::size_t idle_count(bool multiplexed) const noexcept;
    void recount_idle() noexcept;
    [[nodiscard]] std::optional<std::uint64_t> least_recent_idle(bool multiplexed) const noexcept;
    [[nodiscard]] bool close_least_recent_idle(std::optional<bool> multiplexed) noexcept;
    // A connection gained capacity or settled its protocol: admission waiters
    // re-check.
    void admission_changed() noexcept;
    // A connection closed or one of its tasks ended: it may be reclaimable.
    void connection_retired() noexcept;
    // The origin answered HTTP_1_1_REQUIRED (RFC 9113 section 7) on a
    // connection offering h2 and http/1.1: every later connect of such an
    // offer in this pool offers only http/1.1, like Pingora's prefer_h1.
    [[nodiscard]] bool prefers_http11() const noexcept {
        return prefers_http11_;
    }
    void require_http11() noexcept {
        prefers_http11_ = true;
    }
#ifdef RUVIA_ENABLE_HTTP2
    // Admits or rejects one HTTP/2 PUSH_PROMISE of `connection`.
    void accept_http2_push(http_client_connection& connection, const http2_push_promise_event& promise);
#endif

private:
    struct request_admission;
    struct admission_waiter;
    void require_accepting() const;
    void require_open() const;
    void require_fixed_dial() const;
    void validate_addresses(const client_resolve_result& result) const;
    void install_addresses(client_resolve_result result);
    void schedule_dns_refresh();
    // One refresh period: looks up each DNS dial target with a live connection
    // within the worker's concurrency bound, and retires connections to
    // addresses the answer no longer contains.
    [[nodiscard]] task<void> refresh_live_routes();
    void retire_unresolved(std::string_view host, std::uint16_t port, const client_resolve_result& result) noexcept;
    [[nodiscard]] task<void> background_refresh_dns();
#ifdef RUVIA_ENABLE_HTTP2
    struct http2_push_driver;
#endif
    using connection_owner_type = std::unique_ptr<http_client_connection>;

#ifdef RUVIA_ENABLE_HTTP2
    [[nodiscard]] task<void> run_http2_push(std::unique_ptr<http2_push_driver> driver);
#endif
    [[nodiscard]] task<void> execute_into(
        http_client_request_storage request, operation_options options, http_client_response_state* state);
    [[nodiscard]] task<void> execute_request_into(
        http_client_request_storage request, operation_options options, http_client_response_state* state);
    // Routes the request to a reusable connection of its route, a new one
    // within max_open_, or waits within the acquisition bound. The returned
    // connection counts this request among its users_.
    [[nodiscard]] task<http_client_connection*> acquire_tcp_connection(const http_client_request_storage& request,
        const operation_deadline& acquire_timeout, stop_token stop, http_client_response_state& state,
        request_admission& admission);
    void release_tcp_connection(http_client_connection& connection) noexcept;
    [[nodiscard]] task<void> execute_tcp_request(const http_client_request_storage& request,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop,
        http_client_response_state& state, request_admission& admission);
    // One wait for admission_changed(), the acquisition deadline or a stop;
    // the caller re-checks every condition. Throws timeout once the bound expired.
    [[nodiscard]] task<void> wait_for_admission(const operation_deadline& acquire_timeout);
    // The TCP connection key's offer for the request's versions: TLS ALPN, or
    // over plaintext http2 for prior-knowledge h2c and http11 otherwise.
    [[nodiscard]] client_alpn tcp_offer(const http_client_request_storage& request) const noexcept;
    void throw_if_admission_ended(const stop_token& stop, const http_client_response_state& state) const;
    // A new connection fits within max_open_, possibly after closing the least
    // recently used idle connection (reclaimed later).
    [[nodiscard]] bool admits_new_connection() noexcept;
    void mark_idle(std::uint64_t& idle_order, bool multiplexed) noexcept;
    void enforce_idle_limits() noexcept;
    void start_reclaimer();
    [[nodiscard]] task<void> run_reclaimer();
    void reclaim_connections() noexcept;
    [[nodiscard]] http_client_connection* find_connection(const http_client_response_state& state) noexcept;
    [[nodiscard]] task<void> execute_tcp(http_client_connection& connection, const http_client_request_storage& request,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop_token_value,
        http_client_response_state& state_value);
    // Runs the request as an HTTP/1 exchange on `connection`, reconnecting it
    // when needed. False, with nothing sent, when the connection speaks HTTP/2.
    // `connected_http1_generation` names a connection this request has just
    // negotiated, which is not idle-probed.
    [[nodiscard]] task<bool> execute_http1(http_client_connection& connection,
        const http_client_request_storage& request, const operation_deadline& timeout,
        const operation_deadline& acquire_timeout, stop_token stop_token_value, http_client_response_state& state_value,
        std::optional<std::uint64_t> connected_http1_generation);
    // Spawns the request driver for `state_value`, which the caller's response owns.
    void spawn_request(http_client_request_storage request, operation_options options,
        http_client_response_state& state_value);

#ifdef RUVIA_ENABLE_HTTP3
    // An h3 alternative (RFC 7838) advertised by the responses of one route:
    // its dial host, dial port and TLS server name.
    struct alternative_service final {
        std::string route_host_;
        std::uint16_t route_port_;
        std::string server_name_;
        std::string host_;
        std::uint16_t port_;
        std::chrono::steady_clock::time_point expires_;
        // A failed alternative is not tried again before then.
        std::chrono::steady_clock::time_point broken_until_;
        std::uint64_t used_;
    };
    [[nodiscard]] alternative_service* find_alternative_service(
        const http_client_route& route, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] task<bool> execute_http3(const http_client_request_storage& request,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout,
        stop_token stop, http_client_response_state& state, request_admission& admission);
    [[nodiscard]] http3_client_connection* find_http3_connection(const http_client_response_state& state) noexcept;
    void release_http3_connection(http3_client_connection& connection) noexcept;
#endif
    asio::io_context& io_context_;
    worker_ref worker_;
    http_client_config_storage config_;
    // Borrows config_.uri_host_; the pool is address-stable and never moved.
    std::optional<http_origin_view> origin_;
    client_request_policy policy_;
    http_client_advertisement_queue advertisements_;
    std::deque<http_client_push> pushes_;
#ifdef RUVIA_ENABLE_HTTP2
    std::size_t active_pushes_{};
#endif
    std::size_t received_pushes_{};
    std::size_t rejected_pushes_{};
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    std::shared_ptr<http_client_worker_limits> worker_limits_;
    http_client_response_memory_domain::owner response_memory_{};
    client_wire_counters wire_counters_;
    asio::ssl::context tls_context_;
    std::shared_ptr<http_client_cancellation_target> cancellation_target_;
    task_scope background_tasks_;
    // Owners stay at one address; a null entry is a free position.
    std::vector<connection_owner_type> connections_;
#ifdef RUVIA_ENABLE_HTTP3
    // Bounded by http3_.max_alt_svc_entries_ across routes, least recently
    // used first out.
    std::vector<alternative_service> alternative_services_{};
    std::uint64_t alternative_clock_{};
    // Created with the first HTTP/3 connection.
    std::unique_ptr<http3_client_tls> http3_tls_{};
    std::vector<std::unique_ptr<http3_client_connection>> http3_connections_{};
#endif
    worker_signal admission_signal_;
    worker_signal reclaim_signal_;
    admission_waiter* admission_waiters_{};
    std::size_t open_connections_{0};
    std::size_t dns_refreshes_{0};
    // Upper bounds of the idle connections, recounted when exceeded.
    std::size_t idle_http1_{0};
    std::size_t idle_multiplexed_{0};
    std::uint64_t next_connection_serial_{0};
    bool closing_{false};
    bool reclaimer_started_{false};
    std::size_t requests_buffered_{0};
    std::size_t requests_in_flight_{0};
    std::size_t completed_requests_{0};
    std::size_t failed_requests_{0};
    std::size_t idle_connections_with_pending_data_{0};
    bool prefers_http11_{false};
    bool background_joined_{false};
    client_resolve_result dns_addresses_;
    std::uint64_t dns_revision_{};
    std::size_t dns_active_{};
    worker_signal dns_changed_;
    worker_timer_registration dns_refresh_timer_;
    stop_source dns_stop_;
};

}  // namespace ruvia::detail

namespace ruvia {
class http1_request_content_writer;
}  // namespace ruvia

namespace ruvia::detail {

class http_client_connection;
class http_client_output_queue;
class http_client_pool;
class http_client_request_storage;
class http_client_response_state;
class tls_tunnel_output;

// One serialized HTTP/1 exchange on a connected http_client_connection: the
// request head and content, a full-duplex streaming upload, the response head,
// body and Content-Encoding, or an accepted CONNECT/Upgrade byte tunnel. The caller holds
// the connection's pool lease, and its cancellation, for the whole run.
class http1_client_exchange final {
public:
    http1_client_exchange(http_client_pool& pool, http_client_connection& connection) noexcept
        : pool_(pool),
          connection_(connection) {}

    http1_client_exchange(const http1_client_exchange&) = delete;
    http1_client_exchange& operator=(const http1_client_exchange&) = delete;

    [[nodiscard]] task<void> run(const http_client_request_storage& request, const operation_deadline& timeout,
        stop_token stop_token_value, http_client_response_state& state_value);

private:
    // Receives the final response head, then the body or an accepted tunnel.
    [[nodiscard]] task<void> receive(const http_client_request_storage& request, const operation_deadline& timeout,
        http_client_response_state& state_value, http1_client_response_parser& parser);
    // Waits until the consumer releases pending and borrowed response bytes.
    [[nodiscard]] task<void> wait_for_body_space(
        http_client_response_state& state_value, const operation_deadline& timeout);
    void expire_tunnel() noexcept;
    [[nodiscard]] task<void> write_tunnel_output(http_client_output_queue& output, const operation_deadline& timeout,
        tls_tunnel_output* tls, std::exception_ptr& failure);
    [[nodiscard]] task<void> run_tunnel(http_client_response_state& state_value, const operation_deadline& timeout,
        bool websocket);
    [[nodiscard]] task<void> write_immediate(std::span<const asio::const_buffer> parts, const operation_deadline& timeout,
        http1_client_response_parser& parser, bool& completed, std::exception_ptr& failure);
    [[nodiscard]] task<void> write_upload(http_client_response_state& state_value, const operation_deadline& timeout,
        http1_request_content_writer writer, http1_client_response_parser& parser, std::exception_ptr& failure);
    // One transport write of all `parts` (a head, or a framed upload chunk:
    // prefix, payload, suffix) under the write timeout.
    [[nodiscard]] task<void> write(std::span<const asio::const_buffer> parts, const operation_deadline& timeout);
    // Restarts the exchange-level inactivity bound, when armed: read
    // inactivity counted from the last progress in either direction.
    void restart_inactivity() noexcept;
    void stop_inactivity() noexcept;
    // Appends at most `max_bytes` read from the transport to `target` under the
    // read inactivity bound; returns the read, which is not ok() at the peer's
    // end of stream, and throws any other transport failure.
    [[nodiscard]] task<client_transport_result> read_append(
        http_io_buffer& target, std::size_t max_bytes, const operation_deadline& timeout,
        bool read_inactivity_bound = true);

    http_client_pool& pool_;
    http_client_connection& connection_;
    http_client_response_state* response_{};
    // Armed while request content may still be sent before the final response
    // head (read_timeout_), and through an accepted tunnel (its idle_timeout_).
    std::optional<timeout_duration> inactivity_bound_;
    // Last: destroyed first, so its expiry never sees a retiring exchange.
    worker_timer_registration inactivity_timer_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_pool;

// Every pool of one owner shares the owner's worker services (result budget
// and worker limits): one per standalone client, one per application worker. Process-lifetime storage
// backs the pools and their retained responses.
class http_client_registry final {
public:
    http_client_registry(asio::io_context& io_context, const worker_ref& worker_value,
        std::span<const http_client_definition_type> definitions,
        const http_client_worker_services& services);
    http_client_registry(asio::io_context& io_context, const worker_ref& worker_value,
        std::span<const http_client_config> configs,
        const http_client_worker_services& services);
    http_client_registry(asio::io_context&, worker_ref&&,
        std::span<const http_client_definition_type>,
        const http_client_worker_services&) = delete;
    http_client_registry(asio::io_context&, worker_ref&&,
        std::span<const http_client_config>,
        const http_client_worker_services&) = delete;
    ~http_client_registry();
    http_client_registry(const http_client_registry&) = delete;
    http_client_registry& operator=(const http_client_registry&) = delete;

    // Owns inputs immediately; TLS files are prepared off-worker before the
    // atomic publication. Shutdown joins started preparation, and discarded
    // cold operations cannot publish. Removed pools retain only live borrows.
    [[nodiscard]] task<void> upsert(const http_client_config& config, operation_options options = {});
    [[nodiscard]] bool remove(std::string_view alias);

    void close_now() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] http_client_ref get(::ruvia::operation_scope& scope, stop_token host_stop = {}) const;
    [[nodiscard]] http_client_ref get(std::string_view alias, ::ruvia::operation_scope& scope, stop_token host_stop = {}) const;
    [[nodiscard]] http_client_ref get_single_or_default(::ruvia::operation_scope& scope, stop_token host_stop = {}) const;
    [[nodiscard]] std::shared_ptr<http_client_pool> find_origin(http_scheme scheme, std::string_view authority) const;

private:
    using pool_owner_type = std::shared_ptr<http_client_pool>;
    struct alias_binding final {
        std::string alias_;
        pool_owner_type pool_;
        std::size_t order_;
    };
    using alias_map_type = std::vector<alias_binding>;
    struct origin_preparation;
    // Startup aliases without a ca_file_ share system_trust_.
    [[nodiscard]] http_client_config_storage startup_config(const http_client_definition_type& definition);
    [[nodiscard]] http_client_config_storage startup_config(const http_client_config& config);
    using preparation_owner_type = std::unique_ptr<origin_preparation>;

    [[nodiscard]] task<void> prepare_origin(preparation_owner_type input, operation_options options);
    void require_prepared(stop_token stop, const operation_deadline& timeout) const;
    void publish(std::string alias, pool_owner_type replacement);

    void require_mutable() const;
    [[nodiscard]] task<void> retire_origin(alias_map_type::iterator position,
        pool_owner_type replacement, task<void> joining);

    template <typename config_type>
    void initialize(std::span<const config_type> configs);
    asio::io_context& io_context_;
    const worker_ref& worker_;
    http_client_worker_services services_;
    alias_map_type aliases_;
    task_scope retirements_;
    stop_source preparation_stop_;
    // The system trust store, read once for every alias without a ca_file_:
    // OpenSSL shares one X509_STORE across their contexts.
    std::optional<tls_trust_store> system_trust_;
    std::size_t next_origin_order_{};
    bool closing_{false};
    operation_scope preparations_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// SHA-256 over input fed in pieces, the one digest the database layer keys
// statements, cache entries and migration history by. Input that fits the
// inline buffer is digested in one call; longer input streams through an
// OpenSSL context created once it overflows.
class db_sha256 final {
public:
    static constexpr std::size_t digest_size = 32;
    using digest_type = std::array<unsigned char, digest_size>;

    db_sha256() noexcept = default;
    static_assert(digest_size == SHA256_DIGEST_LENGTH);

    ~db_sha256() {
        if (context_ != nullptr) {
            EVP_MD_CTX_free(context_);
        }
    }

    db_sha256(const db_sha256&) = delete;
    db_sha256& operator=(const db_sha256&) = delete;

    void update(std::string_view bytes) {
        if (bytes.empty()) {
            return;
        }
        if (context_ == nullptr) {
            if (bytes.size() <= buffer_.size() - buffered_) {
                std::memcpy(buffer_.data() + buffered_, bytes.data(), bytes.size());
                buffered_ += bytes.size();
                return;
            }
            context_ = EVP_MD_CTX_new();
            if (context_ == nullptr || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1 ||
                (buffered_ != 0 && EVP_DigestUpdate(context_, buffer_.data(), buffered_) != 1)) {
                throw_digest_failure();
            }
        }
        if (EVP_DigestUpdate(context_, bytes.data(), bytes.size()) != 1) {
            throw_digest_failure();
        }
    }

    // Eight little-endian bytes.
    void update_u64_le(std::uint64_t value) {
        std::array<char, 8> encoded;
        for (unsigned index = 0; index < encoded.size(); ++index) {
            encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
        }
        update({encoded.data(), encoded.size()});
    }

    // Ends the digest; nothing may be fed afterwards.
    [[nodiscard]] digest_type finish() {
        digest_type digest;
        if (context_ == nullptr) {
            if (SHA256(buffer_.data(), buffered_, digest.data()) == nullptr) {
                throw_digest_failure();
            }
            return digest;
        }
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(context_, digest.data(), &size) != 1 || size != digest.size()) {
            throw_digest_failure();
        }
        return digest;
    }

private:
    [[noreturn]] static void throw_digest_failure() {
        throw std::runtime_error("database SHA-256 digest failed");
    }

    std::array<unsigned char, 512> buffer_;
    std::size_t buffered_{0};
    evp_md_ctx_st* context_{nullptr};
};

[[nodiscard]] inline db_sha256::digest_type db_sha256_of(std::string_view bytes) {
    db_sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

}  // namespace ruvia::detail

// Execution of compiled db_query statements: one deadline and one chain for a
// query, or a query and its count, run directly or through the result cache.

namespace ruvia {
class db_query;

// A statement copied into the worker's memory resource so it outlives the
// caller's arguments: a suspended query keeps borrowing its SQL and parameters
// long after the expression that produced them has ended.
struct prepared_db_statement final {
    std::string sql_;
    std::vector<db_value> params_;
};

// Raw SQL is checked once, where the caller hands it over: it must hold a
// statement, and no NUL byte, since both client libraries read SQL as a C
// string. Compiled SQL is built that way; parameters are checked where they
// are encoded for the client library.
void validate_db_sql_text(std::string_view sql);

[[nodiscard]] prepared_db_statement prepare_db_statement(
    std::string_view sql, std::span<const db_value> params);

// PostgreSQL NOTIFY as a pg_notify() call, so channel and payload travel as
// bound parameters rather than as SQL text.
[[nodiscard]] prepared_db_statement prepare_db_notify(
    std::string_view channel, std::string_view payload);

namespace detail {

[[nodiscard]] inline db_value clone_db_value_for_resource(
    const db_value& value) {
    switch (value.type()) {
        case db_value_type::null:
            return db_value(nullptr);
        case db_value_type::string:
            return db_value_access::owned_string(
                std::string(value.text()));
        case db_value_type::signed_value:
            return db_value(value.signed_value());
        case db_value_type::unsigned_value:
            return db_value(value.unsigned_value());
        case db_value_type::double_value:
            return db_value(value.double_value());
        case db_value_type::bool_value:
            return db_value(value.bool_value());
    }
    return db_value(nullptr);
}

[[nodiscard]] inline std::vector<db_value> clone_db_values(
    std::span<const db_value> values) {
    std::vector<db_value> output;
    output.reserve(values.size());
    for (const auto& value : values) {
        output.push_back(clone_db_value_for_resource(value));
    }
    return output;
}

struct request_io_metrics;

#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif

// Where a plan's statements run: a pooled connection, or the slot an open
// transaction holds. A value adapter, not a lease owner.
struct db_query_backend final {
    db_pool_ref_type pool_;
    std::optional<std::size_t> slot_;
    // The request's counter block; null outside a request.
    request_io_metrics* io_metrics_{nullptr};
    // Set when the database statement itself failed: a transaction statement
    // failure has already retired the transaction's lease.
    bool* backend_failed_{nullptr};
#ifdef RUVIA_ENABLE_REDIS
    // Pooled backends only: a transaction's reads may include its own
    // uncommitted writes, which the shared cache must never see or replace.
    db_query_cache_state* cache_{nullptr};
#endif
};

struct db_query_step final {
    std::string sql_;
    std::vector<db_value> params_;
#ifdef RUVIA_ENABLE_REDIS
    // Present only when a configured cache applies to this statement.
    std::optional<std::string> cache_key_;
    std::optional<std::chrono::milliseconds> cache_duration_;
#endif
};

// Preparation owns every statement and cache key before returning a cold task.
// Transaction state and its exclusive lease remain with the outer operation.
struct db_query_plan final {
    db_query_step first_;
    std::optional<db_query_step> second_;
    [[nodiscard]] static db_query_plan prepare(
        const db_query& query, const db_query* count, const db_query_backend& backend);
};

template <bool with_count>
using db_query_result = std::conditional_t<with_count, std::pair<db_rows, db_rows>, db_rows>;

template <bool with_count>
[[nodiscard]] task<db_query_result<with_count>> execute_db_query_plan(
    db_query_plan plan, db_query_backend backend, operation_options options);

// Runs one statement on the backend without a cache. `options` is owned by the
// awaiting frame, which must outlive the returned task; a pooled statement
// takes it over, a transaction statement borrows it.
[[nodiscard]] task<db_rows> query_db_backend(const db_query_backend& backend, std::string sql,
    std::vector<db_value> params, operation_options& options);

[[nodiscard]] inline operation_options db_remaining_options(
    const operation_options& base, const operation_deadline& timeout) {
    auto result_value = base;
    result_value.timeout_ = timeout.timeout();
    return result_value;
}

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

inline void require_db_dialect(db_driver driver) {
    if (driver != db_driver::postgresql && driver != db_driver::mariadb) {
        throw std::invalid_argument("SQL compilation requires an explicit database driver");
    }
}

// MariaDB limits identifiers to 64 characters and PostgreSQL to 63 bytes
// (NAMEDATALEN - 1). Counting characters requires well-formed UTF-8, so an
// ill-formed MariaDB name is rejected rather than measured by guesswork.
[[nodiscard]] inline bool fits_db_identifier_limit(std::string_view name, db_driver driver) noexcept {
    if (driver == db_driver::postgresql) {
        return name.size() <= 63;
    }
    std::size_t characters = 0;
    for (std::size_t i = 0; i < name.size(); ++characters) {
        if (static_cast<unsigned char>(name[i]) < 0x80) {
            ++i;
            continue;
        }
        const auto sequence = ::ruvia::scan_utf8_sequence(name, i);
        if (!sequence.well_formed_) {
            return false;
        }
        i += sequence.length_;
    }
    return characters <= 64;
}

inline void validate_db_identifier(std::string_view name, db_driver driver) {
    require_db_dialect(driver);
    if (name.empty() || name.find('\0') != std::string_view::npos || !fits_db_identifier_limit(name, driver)) {
        throw std::invalid_argument("invalid or overlong database identifier");
    }
}

inline void append_db_identifier(std::string& output, std::string_view name, db_driver driver) {
    validate_db_identifier(name, driver);
    const char quote = driver == db_driver::postgresql ? '"' : '`';
    output.push_back(quote);
    for (auto at = name.find(quote); at != std::string_view::npos; at = name.find(quote)) {
        output.append(name.data(), at + 1);
        output.push_back(quote);
        name.remove_prefix(at + 1);
    }
    output.append(name);
    output.push_back(quote);
}

inline void append_db_qualified_identifier(std::string& output, std::string_view name, db_driver driver) {
    while (true) {
        const auto dot = name.find('.');
        append_db_identifier(output, name.substr(0, dot), driver);
        if (dot == std::string_view::npos) {
            return;
        }
        output.push_back('.');
        name.remove_prefix(dot + 1);
    }
}

inline void append_db_string_literal(std::string& output, std::string_view value, db_driver driver) {
    require_db_dialect(driver);
    if (value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database text cannot contain NUL");
    }
    if (driver == db_driver::mariadb) {
        // Hex text is independent of NO_BACKSLASH_ESCAPES and ANSI_QUOTES.
        // It also avoids allowing connection SQL modes to change DDL values.
        output.reserve(output.size() + 26U + value.size() * 2U);
        output += "CONVERT(X'";
        http_append_lower_hex(output, value);
        output += "' USING utf8mb4)";
        return;
    }
    output.reserve(output.size() + 3U + value.size());
    output += "E'";
    for (auto at = value.find_first_of("'\\"); at != std::string_view::npos;
        at = value.find_first_of("'\\")) {
        output.append(value.data(), at + 1);
        output.push_back(value[at]);
        value.remove_prefix(at + 1);
    }
    output.append(value);
    output.push_back('\'');
}

// Bound doubles travel as float8/DOUBLE, so a literal must keep that type: an
// unadorned 2 is an integer and 2.5 is exact NUMERIC/DECIMAL in both dialects,
// which changes division and overload resolution. PostgreSQL gets an explicit
// cast of a quoted constant (valid in DEFAULT, CHECK, generated columns and
// views, and immune to a preceding '-' forming a comment); MariaDB reads any
// literal with an exponent as DOUBLE.
inline void append_db_double_literal(std::string& output, double value, db_driver driver) {
    require_db_dialect(driver);
    if (driver == db_driver::postgresql) {
        output += "CAST('";
        append_db_number(output, value);
        output += "' AS DOUBLE PRECISION)";
        return;
    }
    const auto start = output.size();
    append_db_number(output, value);
    if (output.find_first_of("eE", start) == std::string::npos) {
        output += "e0";
    }
}

inline void append_db_literal(std::string& output, const db_value& value, db_driver driver) {
    require_db_dialect(driver);
    switch (value.type()) {
        case db_value_type::null:
            output += "NULL";
            return;
        case db_value_type::string:
            append_db_string_literal(output, value.text(), driver);
            return;
        case db_value_type::signed_value:
            append_db_number(output, value.signed_value());
            return;
        case db_value_type::unsigned_value:
            append_db_number(output, value.unsigned_value());
            return;
        case db_value_type::double_value:
            append_db_double_literal(output, value.double_value(), driver);
            return;
        case db_value_type::bool_value:
            output += value.bool_value() ? "TRUE" : "FALSE";
            return;
    }
    std::terminate();
}

enum class db_sql_type_usage : std::uint8_t { column,
    cast };

void append_db_expression(std::string& output, db_expression expression, db_driver driver);

template <typename contains_fn_type>
[[nodiscard]] inline std::string_view select_db_dollar_delimiter(
    std::array<char, 32>& delimiter_buf, contains_fn_type&& contains_delimiter) noexcept {
    std::string_view delimiter = "$ruvia$";
    for (std::uint64_t suffix = 1; contains_delimiter(delimiter); ++suffix) {
        std::string_view("$ruvia").copy(delimiter_buf.data(), 6);
        std::array<char, http_max_unsigned_decimal_size> digits;
        const auto formatted = http_format_unsigned(digits, suffix);
        std::size_t length = 6;
        formatted.copy(delimiter_buf.data() + length, formatted.size());
        length += formatted.size();
        delimiter_buf[length++] = '$';
        delimiter = std::string_view(delimiter_buf.data(), length);
    }
    return delimiter;
}

inline void append_db_dollar_body(std::string& output, std::string_view body) {
    std::array<char, 32> delimiter_buf;
    const auto delimiter = select_db_dollar_delimiter(
        delimiter_buf, [body](std::string_view candidate) noexcept {
            return body.find(candidate) != std::string_view::npos;
        });
    output.reserve(output.size() + delimiter.size() * 2U + body.size());
    output.append(delimiter);
    output.append(body);
    output.append(delimiter);
}

[[nodiscard]] inline std::string_view table_lock_mode_name(db_table_lock_mode mode) {
    switch (mode) {
        case db_table_lock_mode::access_share:
            return "ACCESS SHARE";
        case db_table_lock_mode::row_share:
            return "ROW SHARE";
        case db_table_lock_mode::row_exclusive:
            return "ROW EXCLUSIVE";
        case db_table_lock_mode::share_update_exclusive:
            return "SHARE UPDATE EXCLUSIVE";
        case db_table_lock_mode::share:
            return "SHARE";
        case db_table_lock_mode::share_row_exclusive:
            return "SHARE ROW EXCLUSIVE";
        case db_table_lock_mode::exclusive:
            return "EXCLUSIVE";
        case db_table_lock_mode::access_exclusive:
            return "ACCESS EXCLUSIVE";
    }
    throw std::invalid_argument("unknown table lock mode");
}

// PostgreSQL statements emitted both as migration steps and inside PL/pgSQL bodies.
inline void append_db_lock_table_statement(std::string& output, std::string_view table, db_table_lock_mode mode, bool nowait) {
    const auto mode_name = table_lock_mode_name(mode);
    output.append("LOCK TABLE ");
    append_db_qualified_identifier(output, table, db_driver::postgresql);
    output.append(" IN ");
    output.append(mode_name);
    output.append(" MODE");
    if (nowait) {
        output.append(" NOWAIT");
    }
}
inline void append_db_perform_statement(std::string& output, db_expression expression) {
    output.append("PERFORM ");
    append_db_expression(output, expression, db_driver::postgresql);
}

inline void append_db_type_name(std::string& output, db_data_type type,
    std::string_view custom_name, std::size_t length, unsigned precision, unsigned scale,
    bool array_value, db_driver driver, db_sql_type_usage usage = db_sql_type_usage::column) {
    require_db_dialect(driver);
    const bool pg = driver == db_driver::postgresql;
    if ((array_value || !custom_name.empty()) && !pg) {
        throw std::invalid_argument("array and named database types require PostgreSQL");
    }
    if (!custom_name.empty()) {
        if (type != db_data_type::inferred || length != 0 || precision != 0 || scale != 0) {
            throw std::invalid_argument("a named database type cannot also specify scalar type modifiers");
        }
        append_db_qualified_identifier(output, custom_name, driver);
        if (array_value) {
            output += "[]";
        }
        return;
    }
    if (length != 0 && type != db_data_type::char_value && type != db_data_type::varchar) {
        throw std::invalid_argument("length requires CHAR or VARCHAR");
    }
    if ((precision != 0 || scale != 0) && type != db_data_type::numeric) {
        throw std::invalid_argument("precision and scale require NUMERIC");
    }
    if (scale > precision || (precision > (pg ? 1000U : 65U))) {
        throw std::invalid_argument("invalid database numeric precision or scale");
    }
    const bool maria_cast = !pg && usage == db_sql_type_usage::cast;
    if (type == db_data_type::char_value) {
        if (length == 0) {
            throw std::invalid_argument("CHAR requires a positive length");
        }
        if (pg && length > 10485760) {
            throw std::invalid_argument("PostgreSQL CHAR length exceeds the supported maximum");
        }
        if (!pg && !maria_cast && length > 255) {
            throw std::invalid_argument("MariaDB CHAR length exceeds the supported maximum");
        }
    }
    switch (type) {
        case db_data_type::boolean:
            output += maria_cast ? "UNSIGNED" : "BOOLEAN";
            break;
        case db_data_type::small_int:
            output += maria_cast ? "SIGNED" : "SMALLINT";
            break;
        case db_data_type::integer:
            output += maria_cast ? "SIGNED" : "INTEGER";
            break;
        case db_data_type::big_int:
            output += maria_cast ? "SIGNED" : "BIGINT";
            break;
        case db_data_type::real:
            output += maria_cast ? "DOUBLE" : "REAL";
            break;
        case db_data_type::double_value:
            output += pg ? "DOUBLE PRECISION" : "DOUBLE";
            break;
        case db_data_type::text:
            output += maria_cast ? "CHAR" : "TEXT";
            break;
        case db_data_type::char_value:
            output += "CHAR(";
            append_db_number(output, static_cast<std::uint64_t>(length));
            output.push_back(')');
            break;
        case db_data_type::varchar:
            if (length == 0) {
                throw std::invalid_argument("VARCHAR requires a positive length");
            }
            output += maria_cast ? "CHAR(" : "VARCHAR(";
            append_db_number(output, static_cast<std::uint64_t>(length));
            output.push_back(')');
            break;
        case db_data_type::numeric:
            output += "DECIMAL";
            if (precision != 0) {
                output.push_back('(');
                append_db_number(output, static_cast<std::uint64_t>(precision));
                output.push_back(',');
                append_db_number(output, static_cast<std::uint64_t>(scale));
                output.push_back(')');
            } else if (!pg) {
                // MariaDB's bare DECIMAL is DECIMAL(10,0) and silently rounds.
                throw std::invalid_argument("MariaDB DECIMAL requires an explicit precision");
            }
            break;
        case db_data_type::date:
            output += "DATE";
            break;
        case db_data_type::timestamp:
            // Match PostgreSQL's microsecond precision; bare DATETIME truncates.
            output += pg ? "TIMESTAMP" : "DATETIME(6)";
            break;
        case db_data_type::json:
            if (maria_cast) {
                throw std::invalid_argument("MariaDB does not support CAST AS JSON");
            }
            output += "JSON";
            break;
        case db_data_type::jsonb:
        case db_data_type::uuid:
        case db_data_type::timestamp_tz:
        case db_data_type::interval:
        case db_data_type::inet:
        case db_data_type::cidr:
        case db_data_type::bytea:
            if (!pg) {
                throw std::invalid_argument("selected database type requires PostgreSQL");
            }
            switch (type) {
                case db_data_type::jsonb:
                    output += "JSONB";
                    break;
                case db_data_type::uuid:
                    output += "UUID";
                    break;
                case db_data_type::timestamp_tz:
                    output += "TIMESTAMPTZ";
                    break;
                case db_data_type::interval:
                    output += "INTERVAL";
                    break;
                case db_data_type::inet:
                    output += "INET";
                    break;
                case db_data_type::cidr:
                    output += "CIDR";
                    break;
                case db_data_type::bytea:
                    output += "BYTEA";
                    break;
                default:
                    std::terminate();
            }
            break;
        case db_data_type::array:
        case db_data_type::inferred:
            throw std::invalid_argument("SQL requires an explicit scalar type; use the array flag for arrays");
    }
    if (array_value) {
        output += "[]";
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

constexpr std::size_t no_db_node = std::numeric_limits<std::size_t>::max();

enum class db_node_kind : std::uint8_t {
    sql,
    column,
    star,
    value,
    default_value,
    excluded,
    function,
    coalesce,
    null_if,
    greatest,
    least,
    binary,
    unary,
    between,
    tuple,
    list,
    array,
    any,
    all,
    cast,
    alias,
    case_value,
    exists,
    subquery,
    aggregate,
    filter,
    window,
    within_group,
    extract,
    subscript,
    collate,
    at_time_zone,
};
enum class db_query_kind : std::uint8_t { select,
    values,
    insert,
    update,
    delete_value };
enum class db_source_kind : std::uint8_t { table,
    query,
    function };

struct db_stored_type final {
    db_stored_type() = default;
    db_stored_type(const db_stored_type&) = default;
    db_stored_type& operator=(const db_stored_type&) = default;
    db_stored_type(db_stored_type&&) noexcept = default;
    db_stored_type& operator=(db_stored_type&&) noexcept = default;
    db_stored_type(const db_type_definition& source_value)
        : data_type_(source_value.data_type_),
          custom_name_(source_value.custom_name_),
          length_(source_value.length_),
          precision_(source_value.precision_),
          scale_(source_value.scale_),
          array_(source_value.array_) {}
    db_data_type data_type_{db_data_type::inferred};
    std::string custom_name_;
    std::size_t length_{0};
    unsigned precision_{0};
    unsigned scale_{0};
    bool array_{false};
};
struct db_stored_order final {
    std::size_t expression_{no_db_node};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
struct db_stored_named_argument final {
    db_stored_named_argument(std::string_view name, std::size_t expression)
        : name_(name),
          expression_(expression) {}
    std::string name_;
    std::size_t expression_;
};

struct db_query_node final {
    db_query_node(db_node_kind kind)
        : kind_(kind),
          value_(nullptr) {}
    db_query_node(db_node_kind kind, db_value&& value)
        : kind_(kind),
          value_(std::move(value)) {}
    db_query_node(const db_query_node& source_value)
        : kind_(source_value.kind_),
          value_(db_value(source_value.value_)),
          text_(source_value.text_),
          qualifier_(source_value.qualifier_),
          args_(source_value.args_),
          orders_(source_value.orders_),
          type_(source_value.type_),
          binary_(source_value.binary_),
          unary_(source_value.unary_),
          date_part_(source_value.date_part_),
          left_(source_value.left_),
          right_(source_value.right_),
          query_(source_value.query_),
          flag_(source_value.flag_),
          explicit_alias_(source_value.explicit_alias_),
          frame_(source_value.frame_) {
        named_.reserve(source_value.named_.size());
        for (const auto& argument : source_value.named_) {
            named_.emplace_back(argument.name_, argument.expression_);
        }
    }
    db_query_node(db_query_node&&) noexcept = default;
    db_query_node& operator=(db_query_node&&) = delete;
    db_node_kind kind_;
    db_value value_;
    std::string text_;
    std::string qualifier_;
    std::vector<std::size_t> args_;
    std::vector<db_stored_named_argument> named_;
    std::vector<db_stored_order> orders_;
    db_stored_type type_;
    db_binary_operator binary_{db_binary_operator::equal};
    db_unary_operator unary_{db_unary_operator::not_value};
    db_date_part date_part_{db_date_part::epoch};
    std::size_t left_{no_db_node};
    std::size_t right_{no_db_node};
    std::size_t query_{no_db_node};
    bool flag_{false};
    bool explicit_alias_{false};
    std::optional<db_window_frame_options> frame_{};
};
// AND and OR are associative, so a left-deep chain of one of them (as built
// by and_where/or_where or `p = std::move(p) && q`) is a single nesting level
// for import and compilation. True when `child` continues `parent`'s chain.
[[nodiscard]] inline bool continues_logical_chain(
    const std::vector<db_query_node>& nodes, const db_query_node& parent, std::size_t child) noexcept {
    if (parent.kind_ != db_node_kind::binary || child == no_db_node ||
        (parent.binary_ != db_binary_operator::and_value && parent.binary_ != db_binary_operator::or_value)) {
        return false;
    }
    const auto& node = nodes[child];
    return node.kind_ == db_node_kind::binary && node.binary_ == parent.binary_;
}
struct db_stored_source_column final {
    db_stored_source_column(const db_source_column& source_value)
        : name_(source_value.name_),
          type_(source_value.type_) {}
    db_stored_source_column(const db_stored_source_column&) = default;
    db_stored_source_column& operator=(const db_stored_source_column&) = default;
    db_stored_source_column(db_stored_source_column&&) noexcept = default;
    db_stored_source_column& operator=(db_stored_source_column&&) noexcept = default;
    std::string name_;
    db_stored_type type_;
};
struct db_query_source final {
    db_query_source() = default;
    db_query_source(const db_query_source&) = default;
    db_query_source& operator=(const db_query_source&) = default;
    db_query_source(db_query_source&&) noexcept = default;
    db_query_source& operator=(db_query_source&&) noexcept = default;
    db_source_kind kind_{db_source_kind::table};
    std::string name_;
    std::string alias_;
    std::size_t expression_{no_db_node};
    std::size_t query_{no_db_node};
    bool lateral_{false};
    bool ordinality_{false};
    std::vector<db_stored_source_column> columns_;
};
// Whether a FROM item binds `name`: its alias when present, otherwise its
// table name or, for a schema-qualified table, any dot-aligned suffix of it.
[[nodiscard]] inline bool source_binds_name(const db_query_source& source_value, std::string_view name) noexcept {
    if (!source_value.alias_.empty()) {
        return source_value.alias_ == name;
    }
    const std::string_view table = source_value.name_;
    return table == name ||
           (table.size() > name.size() && table.ends_with(name) && table[table.size() - name.size() - 1] == '.');
}
struct db_stored_join final {
    db_stored_join(db_join_type type, db_query_source source_value, std::size_t on)
        : type_(type),
          source_(std::move(source_value)),
          on_(on) {}
    db_stored_join(const db_stored_join& source_value)
        : type_(source_value.type_),
          source_(source_value.source_),
          on_(source_value.on_),
          using_columns_(source_value.using_columns_) {}
    db_join_type type_;
    db_query_source source_;
    std::size_t on_{no_db_node};
    std::vector<std::string> using_columns_;
};
struct db_stored_assignment final {
    db_stored_assignment(std::string_view column, std::size_t expression)
        : column_(column),
          expression_(expression) {}
    std::string column_;
    std::size_t expression_;
};
struct db_stored_cte final {
    template <class column_range>
    db_stored_cte(std::string_view name, std::size_t query, bool recursive, db_materialization materialization,
        const column_range& columns)
        : name_(name),
          query_(query),
          recursive_(recursive),
          materialization_(materialization) {
        columns_.reserve(std::ranges::size(columns));
        for (const auto& column : columns) {
            columns_.emplace_back(std::string_view(column));
        }
    }
    db_stored_cte(const db_stored_cte& source_value)
        : name_(source_value.name_),
          query_(source_value.query_),
          recursive_(source_value.recursive_),
          materialization_(source_value.materialization_),
          columns_(source_value.columns_) {}
    std::string name_;
    std::size_t query_;
    bool recursive_{false};
    db_materialization materialization_{db_materialization::default_value};
    std::vector<std::string> columns_;
};
struct db_stored_set_operation final {
    db_set_operation operation_;
    std::size_t query_;
};
struct db_stored_conflict final {
    db_stored_conflict() = default;
    db_stored_conflict(const db_stored_conflict&) = default;
    db_stored_conflict& operator=(const db_stored_conflict&) = default;
    db_stored_conflict(db_stored_conflict&&) noexcept = default;
    db_stored_conflict& operator=(db_stored_conflict&&) noexcept = default;
    std::vector<std::string> columns_;
    std::string constraint_;
    std::size_t target_where_{no_db_node};
    std::vector<db_stored_assignment> assignments_;
    std::size_t update_where_{no_db_node};
    bool do_nothing_{false};
    bool any_unique_key_{false};
};
struct db_stored_lock final {
    db_stored_lock(const db_lock_options& source_value)
        : mode_(source_value.mode_),
          nowait_(source_value.nowait_),
          skip_locked_(source_value.skip_locked_) {
        tables_.reserve(source_value.tables_.size());
        for (const auto& table : source_value.tables_) {
            tables_.emplace_back(table);
        }
    }
    db_stored_lock(const db_stored_lock&) = default;
    db_stored_lock& operator=(const db_stored_lock&) = default;
    db_stored_lock(db_stored_lock&&) noexcept = default;
    db_stored_lock& operator=(db_stored_lock&&) noexcept = default;
    db_row_lock mode_;
    bool nowait_{false};
    bool skip_locked_{false};
    std::vector<std::string> tables_;
};

class db_query_storage final {
public:
    db_query_storage() = default;
    db_query_storage(const db_query_storage& source);

    std::vector<db_query_node> nodes_;
    std::vector<db_query> queries_;
    db_query_kind kind_{db_query_kind::select};
    std::string target_;
    std::string target_alias_;
    std::vector<std::string> columns_;
    std::vector<std::size_t> projections_;
    std::vector<std::size_t> groups_;
    std::vector<db_stored_order> orders_;
    bool distinct_{false};
    std::vector<std::size_t> distinct_on_;
    std::vector<std::size_t> returning_;
    std::optional<db_query_source> source_{};
    std::vector<db_stored_join> joins_;
    std::vector<std::vector<std::size_t>> rows_;
    std::vector<db_stored_assignment> assignments_;
    std::size_t insert_query_{no_db_node};
    std::size_t predicate_{no_db_node};
    std::size_t having_{no_db_node};
    std::optional<std::uint64_t> limit_{};
    std::optional<std::uint64_t> offset_{};
    std::optional<db_stored_conflict> conflict_{};
    std::optional<db_stored_lock> lock_{};
    std::vector<db_stored_cte> ctes_;
    std::vector<db_stored_set_operation> set_operations_;

    std::optional<bool> cache_enabled_{};
    std::optional<std::chrono::milliseconds> cache_duration_{};
    std::string cache_id_;
    // The result shape a repository builder derived this query as; with
    // cache_id_ it names the id-keyed entry.
    db_cache_shape cache_shape_{db_cache_shape::rows};

    [[nodiscard]] bool returns_rows() const noexcept {
        return kind_ == db_query_kind::select || kind_ == db_query_kind::values || !returning_.empty();
    }
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Readiness classes a database driver can wait for. A wait completes on the
// first class that becomes ready.
enum class db_socket_readiness : std::uint8_t {
    none = 0,
    read = 1,
    write = 2,
    error = 4,
};

[[nodiscard]] constexpr db_socket_readiness operator|(db_socket_readiness left, db_socket_readiness right) noexcept {
    return static_cast<db_socket_readiness>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool has_db_socket_readiness(db_socket_readiness set, db_socket_readiness value) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(value)) != 0;
}

struct db_socket_wait_result final {
    // The class that completed first; none when the wait failed or was cancelled.
    db_socket_readiness ready_{db_socket_readiness::none};
    std::error_code error_{};
    // The slot deadline guarding this wait expired.
    bool expired_{false};
};

// Ruvia-owned duplicate of a database driver's connection socket, registered
// with ASIO for readiness waits.
//
// MariaDB and libpq own their native socket and close it themselves inside
// their read/write calls when the transport fails (libmariadb end_server,
// libpq pqDropConnection). Registering that handle directly would leave ASIO
// holding a handle the driver may close -- and the OS may reuse -- between two
// waits. A duplicate refers to the same socket object, so it observes the same
// readiness, but its handle belongs to Ruvia: the driver closing its own
// handle can neither invalidate nor be invalidated by it. The duplicate is
// therefore made and registered once per driver socket and kept until the
// pool closes the connection.
//
// The duplicate keeps the underlying connection open after the driver has
// closed its handle. The driver only does that on a transport failure, which
// the pool answers by closing the slot, and closing the slot closes the
// duplicate first so the driver's own close still ends the connection.
struct db_slot_socket final {
    explicit db_slot_socket(asio::io_context& io_context);

    db_slot_socket(const db_slot_socket&) = delete;
    db_slot_socket& operator=(const db_slot_socket&) = delete;

#if defined(_WIN32)
    using native_socket_type = std::uintptr_t;
    using waitable_type = asio::ip::tcp::socket;
    asio::ip::tcp::socket socket_;
    static constexpr native_socket_type invalid_socket = std::numeric_limits<native_socket_type>::max();
#else
    using native_socket_type = asio::posix::stream_descriptor::native_handle_type;
    using waitable_type = asio::posix::stream_descriptor;
    asio::posix::stream_descriptor descriptor_;
    static constexpr native_socket_type invalid_socket = -1;
#endif
    // The driver handle the open duplicate was made from.
    native_socket_type source_{invalid_socket};

    // Any-of readiness wait. The first completion cancels the others, and the
    // awaiting coroutine resumes only after every started handler has run, so
    // no outstanding operation still borrows the awaiter.
    class readiness_awaiter final {
    public:
        readiness_awaiter(db_slot_socket& socket, db_socket_readiness wanted) noexcept
            : socket_(socket),
              wanted_(wanted) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;
        // Rethrows a failure to start a wait.
        [[nodiscard]] db_socket_wait_result await_resume() const;

    private:
        void start(waitable_type::wait_type wait_type, db_socket_readiness readiness);
        void complete(db_socket_readiness readiness, std::error_code error) noexcept;

        db_slot_socket& socket_;
        db_socket_readiness wanted_;
        std::coroutine_handle<> continuation_{};
        db_socket_wait_result result_{};
        std::exception_ptr initiation_failure_{};
        std::uint8_t pending_{0};
        bool completed_{false};
    };

    // Duplicates and registers the driver's socket unless the open duplicate
    // was already made from this handle value. The value is the only identity
    // checked, so a caller must close() before the driver can replace its
    // socket with one that reuses the value.
    [[nodiscard]] std::error_code attach(native_socket_type driver_socket) noexcept;
    [[nodiscard]] readiness_awaiter wait(db_socket_readiness wanted) noexcept {
        return readiness_awaiter(*this, wanted);
    }
    void cancel() noexcept;
    // Cancels pending waits and closes the duplicate, leaving the driver's
    // socket open and unregistered.
    void close() noexcept;

private:
    [[nodiscard]] waitable_type& waitable() noexcept;
};

// Puts a driver-owned socket into non-blocking mode. A driver whose
// asynchronous API suspends on EAGAIN cannot suspend at all while its socket
// blocks, so it runs the whole operation inside the call that was supposed to
// start it.
[[nodiscard]] std::error_code set_db_socket_non_blocking(db_slot_socket::native_socket_type driver_socket) noexcept;

// What a look at an idle pooled connection's socket finds.
enum class db_idle_socket_status : std::uint8_t {
    quiet,
    // Bytes wait to be read.
    pending_data,
    // End of stream or a socket error: the server ended the session.
    closed,
};

// Peeks at most one byte of a driver-owned socket without blocking and
// without taking it from the driver (Pingora's test_reusable_stream). Both
// drivers keep their socket non-blocking: libpq always, MariaDB through
// set_db_socket_non_blocking().
[[nodiscard]] db_idle_socket_status peek_db_idle_socket(db_slot_socket::native_socket_type driver_socket) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

class validated_db_config_view final {
public:
    [[nodiscard]] const db_config& get() const noexcept {
        return *config_;
    }

private:
    friend validated_db_config_view validated_db_config(const db_config& config);

    explicit validated_db_config_view(const db_config& config) noexcept
        : config_(&config) {}

    const db_config* config_;
};

[[nodiscard]] inline constexpr std::uint16_t default_db_port(db_driver driver) noexcept {
    switch (driver) {
        case db_driver::mariadb:
            return 3306;
        case db_driver::postgresql:
            return 5432;
        default:
            return 0;
    }
}

[[nodiscard]] inline std::uint16_t configured_db_port(const db_config& config) noexcept {
    return config.port_.has_value() ? config.port_->value() : default_db_port(config.driver_);
}

inline void validate_db_config(const db_config& config) {
    validate_client_tls_config(config.tls_);
    // Both drivers are configured with key files only; refuse a password they
    // would silently ignore.
    if (config.tls_.identity_.has_value() && !config.tls_.identity_->private_key_password_.empty()) {
        throw std::invalid_argument("database TLS does not support encrypted private keys");
    }
    const auto driver = config.driver_;
    if (driver == db_driver::mariadb && config.tls_.mode_ == client_tls_mode::verify_identity) {
        if (!is_client_ip_address(config.host_) || config.tls_.server_name_.has_value()) {
            throw std::invalid_argument("MariaDB authenticated TLS requires a numeric host with an IP certificate; server_name is unsupported");
        }
    }
    if (driver != db_driver::mariadb && driver != db_driver::postgresql) {
        throw std::invalid_argument("database driver must be selected");
    }
#ifndef RUVIA_ENABLE_MARIADB
    if (driver == db_driver::mariadb) {
        throw std::invalid_argument("MariaDB support is not enabled");
    }
#endif
#ifndef RUVIA_ENABLE_POSTGRESQL
    if (driver == db_driver::postgresql) {
        throw std::invalid_argument("PostgreSQL support is not enabled");
    }
#endif

    validate_client_origin_host(config.host_, "database host must not be empty", "database host is invalid");
    // Both drivers pass these to C APIs as NUL-terminated strings.
    for (const std::string_view value : {std::string_view{config.username_}, std::string_view{config.password_},
             std::string_view{config.database_}}) {
        if (value.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("database credentials must not contain NUL bytes");
        }
    }
    detail::ensure_non_zero_port(configured_db_port(config), "database port must not be zero");
    detail::ensure_positive_size(config.pool_size_per_worker_, "database pool size must be greater than zero");
    detail::ensure_positive_optional_durations("configured database timeouts must be greater than zero",
        config.connect_timeout_, config.read_timeout_, config.write_timeout_, config.query_timeout_,
        config.acquire_timeout_, config.transaction_timeout_);
}

[[nodiscard]] inline validated_db_config_view validated_db_config(const db_config& config) {
    validate_db_config(config);
    return validated_db_config_view(config);
}

validated_db_config_view validated_db_config(db_config&&) = delete;
validated_db_config_view validated_db_config(const db_config&&) = delete;

#ifdef RUVIA_ENABLE_REDIS
struct db_cache_config_storage final {
    template <typename config_type>
    db_cache_config_storage(const config_type& source_value)
        : duration_(source_value.duration_),
          always_enabled_(source_value.always_enabled_),
          ignore_errors_(source_value.ignore_errors_),
          key_prefix_(source_value.key_prefix_) {
        if (duration_.count() <= 0 || key_prefix_.empty()) {
            throw std::invalid_argument("database cache requires a positive duration and nonempty namespace");
        }
    }
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    std::string key_prefix_;
};
#endif

// Worker/app-owned copy of the public startup configuration. Retained runtime
// state is explicitly rebound to its owning PMR domain here.
struct db_config_storage final {
    db_config_storage(const db_config& source_value)
        : db_config_storage(validated_db_config(source_value)) {}

    db_config_storage(validated_db_config_view source_value)
        : db_config_storage(validated_config_tag_type{}, source_value.get()) {}

    db_config_storage(db_config_storage&&) noexcept = default;
    db_config_storage(const db_config_storage& source_value)
        : db_config_storage(validated_config_tag_type{}, source_value) {}

    db_driver driver_{db_driver::unspecified};
    std::string host_;
    std::uint16_t port_{0};
    std::string username_;
    std::string password_;
    client_tls_config_storage tls_;
    std::string database_;
    std::size_t pool_size_per_worker_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> read_timeout_;
    std::optional<timeout_duration> write_timeout_;
    std::optional<timeout_duration> query_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<timeout_duration> transaction_timeout_;
    // Whether a slot whose connection ended connects again on its next use.
    // Off for a pool bound to the sessions it opened at startup (the
    // migrator's): a statement on a lost session then fails rather than run
    // on a new one that lacks the old one's session-scoped state.
    bool reconnect_{true};

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static std::uint16_t resolved_port(const db_config& source) noexcept {
        return configured_db_port(source);
    }
    [[nodiscard]] static std::uint16_t resolved_port(const db_config_storage& source) noexcept {
        return source.port_;
    }
    [[nodiscard]] static bool resolved_reconnect(const db_config&) noexcept {
        return true;
    }
    [[nodiscard]] static bool resolved_reconnect(const db_config_storage& source) noexcept {
        return source.reconnect_;
    }
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const db_config& source) {
        return {source.tls_, client_tls_files::keep_paths};
    }
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const db_config_storage& source) {
        return {source.tls_};
    }

    template <typename source_type>
        requires(std::same_as<source_type, db_config> || std::same_as<source_type, db_config_storage>)
    db_config_storage(validated_config_tag_type, const source_type& source_value)
        : driver_(source_value.driver_),
          host_(source_value.host_),
          port_(resolved_port(source_value)),
          username_(source_value.username_),
          password_(source_value.password_),
          tls_(tls_storage(source_value)),
          database_(source_value.database_),
          pool_size_per_worker_(source_value.pool_size_per_worker_),
          connect_timeout_(source_value.connect_timeout_),
          read_timeout_(source_value.read_timeout_),
          write_timeout_(source_value.write_timeout_),
          query_timeout_(source_value.query_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          transaction_timeout_(source_value.transaction_timeout_),
          reconnect_(resolved_reconnect(source_value)) {}
};

#ifdef RUVIA_ENABLE_REDIS
struct db_query_cache_registration_storage final {
    db_query_cache_registration_storage(const db_query_cache_registration& source_value)
        : redis_alias_(source_value.redis_alias_),
          policy_(source_value.policy_) {
        validate_capability_alias(redis_alias_, "database query cache Redis alias must not be empty");
    }
    std::string redis_alias_;
    db_cache_config_storage policy_;
};
#endif

struct db_definition final {
    using config_storage_type = db_config_storage;
    std::string alias_;
    db_config_storage config_;
#ifdef RUVIA_ENABLE_REDIS
    std::optional<db_query_cache_registration_storage> query_cache_{};
#endif
};

}  // namespace ruvia::detail

// Pipelining of buffered statements outside transactions, shared by the
// drivers whose protocol lets one connection carry several statements at
// once. A statement joins a pipeline group: one leased connection that
// several statements share, each sending its own flight so that a failed
// statement affects no other. Results are read in send order by one member at
// a time, the reader, which hands each finished statement's outcome to its
// member and passes the role on when its own statement completes. Members wait
// in their own coroutine frames; the group's bookkeeping is owned by the
// connection's slot and the pool, because a member that is cancelled leaves
// before its results arrive.
//
// A driver supplies an io_type with its protocol: what a flight and a group
// keep (flight_type, group_state), the statement its caller prepared
// (statement_type), and the hooks that open a group, queue a flight, read
// results without waiting, wait for and exchange I/O, and judge whether a
// connection can be reused once its group dissolves. Everything else --
// admission in arrival order, room, the reader role, deadlines, cancellation
// and connection failure -- is this one implementation.
//
// A statement that changes session state (SET, USE, BEGIN, LOCK, ...) changes
// it for every statement sharing the connection after it; such statements
// belong in a transaction. A group dissolving with its connection left inside
// a transaction block closes that connection.

namespace ruvia::detail {

template <typename driver_type>
class db_pool;
template <typename io_type>
class db_pipeline_state;
template <typename io_type>
struct db_pipeline_record;

// Statements one connection keeps in flight at once. Locally, 16 reached about
// six times the throughput of one statement per round trip on PostgreSQL, 8
// somewhat less and 32 no more; a deeper pipeline only adds queueing latency.
inline constexpr std::size_t db_pipeline_depth = 16;

enum class db_pipeline_phase : std::uint8_t {
    // Holds nothing.
    admitting,
    // Waits in the pool's arrival order for room in a group or a connection.
    parked,
    // Holds a lease and founds a group on it.
    leased,
    // A member holding room for a flight it has not sent yet.
    reserved,
    // A member whose flight is in the group.
    pending,
    // A member whose statement has an outcome, or was abandoned.
    settled,
};

// What woke a waiting member.
enum class db_pipeline_event : std::uint8_t {
    none,
    // Admission: a connection was leased for it, or room in a group reserved.
    leased,
    joined,
    // It reads the group's results now.
    reader,
    // Its statement finished; failure_ tells how.
    completed,
    // Its statement must be sent once more (a stale cached statement).
    retry,
    // Its group's connection failed with failure_.
    failed,
    timed_out,
    cancelled,
    closing,
};

// How the head flight of a group ended, as the driver read it.
struct db_pipeline_outcome final {
    std::exception_ptr failure_{};
    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> insert_id_{};
    // The statement runs once more instead of completing.
    bool retry_{false};
};

// One pipelined statement. It lives in its caller's coroutine frame; the
// group refers to it only while it is a member, through records that forget
// it when it leaves. Construction binds its stop token and makes it findable
// for cancellation; destruction gives back whatever it still holds -- a parked
// position, a lease, room or a flight in a group -- so every exit path of the
// statement shares one cleanup.
template <typename io_type>
class db_pipeline_member final {
public:
    using state_type = db_pipeline_state<io_type>;

    db_pipeline_member(
        state_type& state, db_rows* rows, const stop_token& stop_token_value);
    ~db_pipeline_member();

    db_pipeline_member(const db_pipeline_member&) = delete;
    db_pipeline_member& operator=(const db_pipeline_member&) = delete;

    // Suspends until an event arrives, unless one already has.
    struct wait final {
        db_pipeline_member& member_;

        [[nodiscard]] bool await_ready() const noexcept {
            return member_.event_ != db_pipeline_event::none;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            member_.continuation_ = handle;
        }
        [[nodiscard]] db_pipeline_event await_resume() noexcept {
            return member_.take_event();
        }
    };

    [[nodiscard]] db_pipeline_event take_event() noexcept {
        return std::exchange(event_, db_pipeline_event::none);
    }
    // Its stop token was triggered, even if the cancellation has not been
    // dispatched yet.
    [[nodiscard]] bool stopped() const noexcept {
        return cancel_requested_ || stop_token_.stop_requested();
    }

    state_type& state_;
    // The caller's result for query(); null for execute().
    db_rows* rows_;
    // The caller's options outlive the statement's frame.
    const stop_token& stop_token_;
    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> insert_id_{};
    std::exception_ptr failure_{};
    // The statement deadline, from its first send.
    std::optional<std::chrono::steady_clock::time_point> deadline_{};
    std::coroutine_handle<> continuation_{};
    // The record of its flight while pending.
    db_pipeline_record<io_type>* record_{nullptr};
    // Its place in the pool's arrival order, shared with the callers waiting
    // for a lease.
    std::uint64_t arrival_{0};
    std::size_t slot_{0};
    db_pipeline_phase phase_{db_pipeline_phase::admitting};
    db_pipeline_event event_{db_pipeline_event::none};
    // Set when its cancellation is dispatched, which also unlinks it from the
    // statements a cancellation searches.
    bool cancel_requested_{false};
    bool connecting_{false};
    bool may_retry_{true};
    // The pool's FIFO of parked statements.
    db_pipeline_member* previous_parked_{nullptr};
    db_pipeline_member* next_parked_{nullptr};
    // The pool's cancellable statements, newest first.
    db_pipeline_member* previous_{nullptr};
    db_pipeline_member* next_{nullptr};
    worker_timer_registration admission_timer_;
    std::uint64_t cancellation_id_{0};
    worker_cancellation_registration<worker_cancellation_target<state_type>> cancellation_;
};

// One flight in a group, in send order.
template <typename io_type>
struct db_pipeline_record final {
    // Null once the member abandoned the statement; its results are discarded.
    db_pipeline_member<io_type>* member_{nullptr};
    typename io_type::flight_type flight_{};
    bool returns_rows_{false};
};

// The pipeline group of one connection; it exists while it has members. The
// records form a ring of the flights in send order; the head is the one whose
// results are read next.
template <typename io_type>
struct db_pipeline_group final {
    [[nodiscard]] db_pipeline_record<io_type>& record(std::size_t offset) noexcept {
        return records_[(head_ + offset) % db_pipeline_depth];
    }
    [[nodiscard]] const db_pipeline_record<io_type>& record(std::size_t offset) const noexcept {
        return records_[(head_ + offset) % db_pipeline_depth];
    }
    // Its oldest flight was abandoned: a statement sent now would wait behind
    // one whose caller already gave up on it, so the group takes nobody new
    // until that flight's results have been read.
    [[nodiscard]] bool stalled() const noexcept {
        return pending_ != 0 && records_[head_].member_ == nullptr;
    }

    std::array<db_pipeline_record<io_type>, db_pipeline_depth> records_{};
    // The driver's protocol state of the connection's flights.
    typename io_type::group_state io_{};
    // Why the connection failed, once closing_.
    std::exception_ptr failure_{};
    // When the reader's current socket wait ends at the latest.
    std::optional<std::chrono::steady_clock::time_point> wait_deadline_{};
    // Start of the connection's current stretch without progress -- no socket
    // readiness and no flight completed -- while flights are outstanding. The
    // I/O timeout counts from it whichever member holds the reader role.
    std::optional<std::chrono::steady_clock::time_point> stretch_start_{};
    db_pipeline_member<io_type>* reader_{nullptr};
    std::size_t head_{0};
    std::size_t pending_{0};
    // Room promised to members that have not sent their flight yet.
    std::size_t reserved_{0};
    std::size_t members_{0};
    // The connection failed; the group takes no members and dissolves.
    bool closing_{false};
};

// Per pool: admission into groups, in arrival order with the callers waiting
// for a lease. A statement founds a group on a free connection, else joins
// the group with the most room, else parks in a FIFO served by freed room and
// by released connections. A statement never joins a group ahead of an older
// caller still waiting for a lease: the groups drain to it, and a released
// connection goes to it. A parked statement older than every caller waiting
// for a lease takes a released connection itself, before the lease scheduler
// can hand it on.
template <typename io_type>
class db_pipeline_state final {
public:
    using driver_type = typename io_type::driver_type;
    using pool_type = db_pool<driver_type>;
    using member_type = db_pipeline_member<io_type>;
    using group_type = db_pipeline_group<io_type>;
    using record_type = db_pipeline_record<io_type>;
    using statement_type = typename io_type::statement_type;
    using cancellation_target_type = worker_cancellation_target<db_pipeline_state>;

    db_pipeline_state(pool_type& pool, const worker_ref& worker_value);
    ~db_pipeline_state();

    db_pipeline_state(const db_pipeline_state&) = delete;
    db_pipeline_state& operator=(const db_pipeline_state&) = delete;

    // db_pool hooks.
    [[nodiscard]] bool take_released_slot(std::size_t slot) noexcept;
    void exclusive_waiter_left() noexcept;
    // Fails every parked statement with closing.
    void close() noexcept;
    // worker_cancellation_target hook.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

    [[nodiscard]] const std::shared_ptr<cancellation_target_type>& cancellation_target() const noexcept {
        return cancellation_target_;
    }

    // The statement's way through a group, in order. admit() places it, or
    // parks it to be woken with the event admitted() checks. A statement that
    // leased a connection connects it if needed and founds a group on it.
    [[nodiscard]] db_pipeline_event admit(member_type& member, const operation_deadline& admission_deadline);
    void admitted(db_pipeline_event event) const;
    [[nodiscard]] bool must_connect(member_type& member);
    void connected(member_type& member, const std::exception_ptr& failure);
    void found_group(member_type& member);
    // Queues the member's flight in its reserved room; it becomes the reader
    // when the group has none.
    void send(member_type& member, statement_type& statement);
    [[nodiscard]] bool reads(const member_type& member) const noexcept;
    // Turns the event that ended the member's wait into its outcome: false
    // once it completed, true when it must be sent once more; throws its
    // failure.
    [[nodiscard]] bool retries(member_type& member, db_pipeline_event event) const;

    // Services for the reader. read_buffered() reads the results available
    // without waiting and returns true once the reader's own statement
    // finished, after handing the role on.
    [[nodiscard]] bool read_buffered(member_type& reader);
    // Abandons the pending followers whose deadline passed.
    void expire_followers(member_type& reader, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> earliest_deadline(std::size_t slot) const noexcept;
    // Fails every pending member and closes the connection.
    void fail_group(std::size_t slot, const std::exception_ptr& failure) noexcept;
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    // Services for the driver's hooks.
    [[nodiscard]] pool_type& pool() const noexcept {
        return pool_;
    }
    // Statements this pool runs now, in every phase from admission on.
    [[nodiscard]] std::size_t statements() const noexcept {
        return statements_;
    }
    [[nodiscard]] group_type& group(std::size_t slot) const noexcept;
    // Hands the head flight's outcome to its member, which the reader settles
    // itself and wakes any other; an abandoned flight's outcome is discarded.
    void complete_head(std::size_t slot, member_type& reader, db_pipeline_outcome outcome);
    [[nodiscard]] db_error cancelled() const;

private:
    friend class db_pipeline_member<io_type>;

    // Whether a statement of this arrival may take a connection or room: no
    // older caller still waits for a lease.
    [[nodiscard]] bool may_share(std::uint64_t arrival) const noexcept;
    // Places a statement whose turn it is: a free connection, else room in the
    // group with the most of it that is not stalled.
    [[nodiscard]] db_pipeline_event place(member_type& member);
    [[nodiscard]] std::optional<std::size_t> roomiest_group() const noexcept;
    void join(member_type& member, std::size_t slot) noexcept;
    void park(member_type& member, const operation_deadline& admission_deadline);
    void unpark(member_type& member) noexcept;
    // Serves parked statements in arrival order.
    void dispatch() noexcept;
    // Records what woke a member and posts its resumption.
    void settle(member_type& member, db_pipeline_event event) noexcept;
    void require_usable(std::size_t slot) const;
    void hand_off(member_type& reader) noexcept;
    void abandon(member_type& member) noexcept;
    void clear_flights(group_type& cleared) noexcept;
    void unlink(member_type& member) noexcept;
    void retire(member_type& member) noexcept;
    void leave(member_type& member) noexcept;
    void dissolve(std::size_t slot) noexcept;

    pool_type& pool_;
    const worker_ref& worker_;
    member_type* members_{nullptr};
    std::size_t statements_{0};
    member_type* parked_head_{nullptr};
    member_type* parked_tail_{nullptr};
    std::shared_ptr<cancellation_target_type> cancellation_target_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Names one entry of a db_statement_cache by position and generation, so
// forgetting or replacing an entry never redirects another statement's handle
// to a different statement; a stale handle finds nothing.
struct db_statement_entry final {
    std::size_t index_;
    std::uint64_t generation_;
};

// The server-side prepared statements of one connection, shared by the drivers
// that cache them. A statement is keyed by its SQL and the type codes of its
// parameters, since a server fixes parameter (and with them result) types when
// it prepares. A statement is prepared on its second use: the first runs
// unprepared, so SQL that never repeats (literals, generated lists) costs no
// more than without the cache and does not evict reused statements. Entries
// live as long as the connection; the least recently used one makes room for
// a new one, and its payload goes back to the driver to release on the server.
template <typename payload_type>
class db_statement_cache final {
public:
    static constexpr std::size_t capacity = 128;

    using entry_ref = db_statement_entry;

    struct lookup final {
        // Absent: run the statement unprepared.
        std::optional<entry_ref> statement_{};
        // The statement is new to the connection: prepare it first.
        bool inserted_{false};
        // The payload of an entry evicted to make room.
        std::optional<payload_type> evicted_{};
        // The statement's key_hash().
        std::size_t hash_{0};
        // Not cached, used before, and not inserted only because the caller
        // may not insert it now.
        bool repeated_{false};
    };

    db_statement_cache()
        : entries_() {}

    // Counts a use of the statement. A cached one is returned. One used
    // before is inserted when the caller `may_insert` it now, with the payload
    // make() builds; the whole entry is built before the table changes, so a
    // failed allocation leaves every entry intact. Otherwise the use is
    // remembered and nothing returned.
    template <typename factory_type>
    [[nodiscard]] lookup find(
        std::string_view sql, std::span<const unsigned int> types, bool may_insert, factory_type&& make) {
        const auto hash = key_hash(sql, types);
        ++clock_;
        const auto cached = entry_index_.find(hash, [&](std::size_t index) noexcept {
            const auto& existing = entries_[index];
            return existing.generation_ != 0 && existing.hash_ == hash && existing.sql_ == sql &&
                   std::ranges::equal(existing.types_, types);
        });
        if (cached.has_value()) {
            auto& existing = entries_[*cached];
            existing.last_use_ = clock_;
            return {entry_ref{*cached, existing.generation_}, false, std::nullopt, hash};
        }
        const auto first_use_index = find_first_use(hash);
        if (!first_use_index.has_value()) {
            remember(hash);
            return {std::nullopt, false, std::nullopt, hash};
        }
        if (!may_insert) {
            return {std::nullopt, false, std::nullopt, hash, true};
        }
        const auto free_entry = std::ranges::find(entries_, std::uint64_t{0}, &entry::generation_);
        const bool has_free_entry = free_entry != entries_.end();
        if (!has_free_entry && entries_.size() < capacity && entries_.size() == entries_.capacity()) {
            entries_.reserve(std::min(capacity, std::max<std::size_t>(entries_.size() * 2, 8)));
        }
        std::string key_sql(sql);
        std::vector<unsigned int> key_types(types.begin(), types.end());
        std::optional<payload_type> created_payload;
        if constexpr (std::is_invocable_v<factory_type, std::string_view, std::span<const unsigned int>,
                          std::size_t>) {
            created_payload = make(sql, types, hash);
            if (!created_payload.has_value()) {
                return {std::nullopt, false, std::nullopt, hash, true};
            }
        } else {
            created_payload.emplace(make(sql, types));
        }
        // Nothing below allocates: the key was copied above, `entries_` has
        // room, and every string and vector shares one resource, so swaps and
        // moves only exchange storage.
        const auto next_generation = generation_ + 1;
        auto index = entries_.size();
        std::optional<payload_type> evicted;
        if (has_free_entry || index == capacity) {
            if (has_free_entry) {
                // A forgotten entry is reused before the table grows.
                index = static_cast<std::size_t>(free_entry - entries_.begin());
            } else {
                // Only a full table evicts a statement the server still holds.
                const auto oldest = std::ranges::min_element(entries_, {}, &entry::last_use_);
                index = static_cast<std::size_t>(oldest - entries_.begin());
                entry_index_.erase(index, oldest->hash_, [this](std::size_t position) noexcept { return entries_[position].hash_; });
                evicted.emplace(std::move(oldest->payload_));
            }
            auto& reused = entries_[index];
            reused.sql_.swap(key_sql);
            reused.types_.swap(key_types);
            reused.hash_ = hash;
            reused.last_use_ = clock_;
            reused.generation_ = next_generation;
            reused.payload_ = std::move(*created_payload);
        } else {
            entries_.push_back(
                entry{std::move(key_sql), std::move(key_types), hash, clock_, next_generation, std::move(*created_payload)});
        }
        entry_index_.insert(index, hash);
        clear_first_use(*first_use_index, hash);
        ++generation_;
        return {entry_ref{index, next_generation}, true, std::move(evicted), hash};
    }

    [[nodiscard]] payload_type* payload(entry_ref statement) noexcept {
        auto* found = live(statement);
        return found == nullptr ? nullptr : &found->payload_;
    }

    // The SQL of a live entry, owned by the cache. It stays in place until the
    // next find() or clear().
    [[nodiscard]] std::string_view sql(entry_ref statement) const noexcept {
        if (statement.index_ >= entries_.size() || entries_[statement.index_].generation_ != statement.generation_) {
            return {};
        }
        return entries_[statement.index_].sql_;
    }

    // Drops an entry the server does not hold (a failed prepare) or no longer
    // holds as cached (a stale statement), unless it was replaced since, and
    // hands its payload back. With `use_again` its next use prepares it
    // straight away, as for a statement used before.
    std::optional<payload_type> forget(entry_ref statement, bool use_again) noexcept {
        auto* found = live(statement);
        if (found == nullptr) {
            return std::nullopt;
        }
        const auto old_hash = found->hash_;
        entry_index_.erase(statement.index_, old_hash, [this](std::size_t index) noexcept { return entries_[index].hash_; });
        found->generation_ = 0;
        if (use_again) {
            remember(old_hash);
        }
        return std::move(found->payload_);
    }

    // Calls visit() with the payload of every live entry.
    template <typename visitor_type>
    void for_each(visitor_type&& visit) {
        for (auto& existing : entries_) {
            if (existing.generation_ != 0) {
                visit(existing.payload_);
            }
        }
    }

    // A new connection starts without server-side statements.
    void clear() noexcept {
        entries_.clear();
        entry_index_.clear();
        first_uses_.fill(0);
        first_use_index_.clear();
    }

    // The hash a key is kept under; never zero, which marks a free position.
    [[nodiscard]] static std::size_t key_hash(std::string_view sql, std::span<const unsigned int> types) noexcept {
        auto hash = std::hash<std::string_view>{}(sql) ^ types.size();
        for (const auto type : types) {
            hash = (hash ^ type) * fnv1a_prime;
        }
        return hash == 0 ? 1 : hash;
    }

private:
    struct entry final {
        std::string sql_;
        std::vector<unsigned int> types_;
        std::size_t hash_;
        std::uint64_t last_use_;
        // Zero marks a forgotten entry, free for reuse. Never reused across
        // clear(), so no handle outlives its entry unnoticed.
        std::uint64_t generation_;
        payload_type payload_;
    };

    [[nodiscard]] entry* live(entry_ref statement) noexcept {
        if (statement.index_ >= entries_.size() || entries_[statement.index_].generation_ != statement.generation_) {
            return nullptr;
        }
        return &entries_[statement.index_];
    }

    static constexpr std::size_t index_capacity = 256;
    static_assert((index_capacity & (index_capacity - 1)) == 0 && index_capacity >= capacity * 2);

    // An open-addressing index over at most `capacity` positions, stored as
    // position + 1 so zero marks an empty slot: probed linearly from the
    // hash, deleted by backward shift. `hash_of(position)` reads the hash a
    // stored position was inserted under.
    class probe_index final {
    public:
        template <typename match_type>
        [[nodiscard]] std::optional<std::size_t> find(std::size_t hash, match_type&& match) const noexcept {
            for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
                const auto code = slots_[slot];
                if (code == 0) {
                    return std::nullopt;
                }
                if (const auto position = static_cast<std::size_t>(code - 1); match(position)) {
                    return position;
                }
            }
            return std::nullopt;
        }

        void insert(std::size_t position, std::size_t hash) noexcept {
            std::size_t slot = hash & mask;
            while (slots_[slot] != 0) {
                slot = (slot + 1) & mask;
            }
            slots_[slot] = static_cast<std::uint8_t>(position + 1);
        }

        template <typename hash_of_type>
        void erase(std::size_t position, std::size_t hash, hash_of_type&& hash_of) noexcept {
            const auto target = static_cast<std::uint8_t>(position + 1);
            for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
                const auto code = slots_[slot];
                if (code == 0) {
                    return;
                }
                if (code == target) {
                    std::size_t empty = slot;
                    for (std::size_t next = (slot + 1) & mask; slots_[next] != 0; next = (next + 1) & mask) {
                        const auto ideal = hash_of(static_cast<std::size_t>(slots_[next] - 1)) & mask;
                        if (((next - ideal) & mask) >= ((next - empty) & mask)) {
                            slots_[empty] = slots_[next];
                            empty = next;
                        }
                    }
                    slots_[empty] = 0;
                    return;
                }
            }
        }

        void clear() noexcept {
            slots_.fill(0);
        }

    private:
        static constexpr std::size_t mask = index_capacity - 1;
        std::array<std::uint8_t, index_capacity> slots_{};
    };

    [[nodiscard]] std::optional<std::size_t> find_first_use(std::size_t hash) const noexcept {
        return first_use_index_.find(hash, [&](std::size_t position) noexcept { return first_uses_[position] == hash; });
    }

    void clear_first_use(std::size_t position, std::size_t hash) noexcept {
        first_use_index_.erase(position, hash, [this](std::size_t stored) noexcept { return first_uses_[stored]; });
        first_uses_[position] = 0;
    }

    void remember(std::size_t hash) noexcept {
        if (find_first_use(hash).has_value()) {
            return;
        }
        const auto position = next_first_use_;
        if (first_uses_[position] != 0) {
            first_use_index_.erase(position, first_uses_[position], [this](std::size_t stored) noexcept { return first_uses_[stored]; });
        }
        first_uses_[position] = hash;
        first_use_index_.insert(position, hash);
        next_first_use_ = (next_first_use_ + 1) % first_uses_.size();
    }

    std::vector<entry> entries_;
    probe_index entry_index_;
    // Key hashes of recent first uses; zero marks a free position. A hash
    // collision only prepares a statement on its first use.
    std::array<std::size_t, capacity> first_uses_{};
    probe_index first_use_index_;
    std::size_t next_first_use_{0};
    std::uint64_t clock_{0};
    std::uint64_t generation_{0};
};

}  // namespace ruvia::detail

struct st_mysql;
struct st_mysql_res;
struct ma_charset_info_st;

namespace ruvia::detail {

// Builds the error for a failed driver call from what libmariadb reports for
// it. A statement_failed whose cause ended the connection is reported as
// io_error.
[[nodiscard]] db_error mariadb_error(std::string_view operation, unsigned int native_code, const char* sql_state,
    const char* message, db_error::code_type code);
[[nodiscard]] db_error mysql_error(
    const st_mysql& connection, std::string_view operation, db_error::code_type code);

// A failed command after which the connection cannot carry another one: the
// client library failed or lost the exchange, or the server ended the session
// or read a packet it did not expect. The server answers any other error once
// the command is done, and the connection is ready for the next.
[[nodiscard]] bool mariadb_connection_failure(unsigned int native_code) noexcept;

// MariaDB's C API carries SQL and escaped-value lengths in unsigned long.
// Reject a wider C++ view before any narrowing conversion reaches the driver.
void validate_mariadb_sql_length(std::size_t length);

void free_stored_result(void* result_value) noexcept;

// The query that reads the session's client character set, and the
// character set its stored result names. The latter throws
// std::invalid_argument when libmariadb does not know it.
inline constexpr std::string_view mariadb_charset_query = "SELECT @@character_set_client";
[[nodiscard]] const ma_charset_info_st& mariadb_queried_charset(st_mysql_res& result);
// utf8mb4, the client character set a connection opens with: the one to
// escape for while the session's tracking says it is still in effect.
[[nodiscard]] const ma_charset_info_st& mariadb_utf8mb4_charset() noexcept;

// Writes `sql` with each placeholder replaced by its parameter's literal to
// `output`. A quote is escaped by doubling it, which reads the same in every
// sql_mode and character set; a backslash, in a session that reads backslash
// escapes, by another one, as `charset` (the session's client character set)
// splits the string into characters. The session's status must be current:
// every response before has been read whenever the statement or a string
// parameter has a backslash (mariadb_statement_classification::has_backslash_,
// mariadb_parameter_traits::has_backslash_); `charset` is needed only where
// mariadb_escape_needs_charset() says so. A string parameter with a NUL byte is
// refused: no SQL literal carries one.
void interpolate_sql(st_mysql& connection, std::string_view sql, std::span<const db_value> params,
    std::string& output, const ma_charset_info_st* charset = nullptr);

// What a statement's text tells about how it may run.
struct mariadb_statement_traits final {
    // A cached statement may run prepared: not a CALL, whose OUT parameters
    // come back as an extra result set only in the binary protocol; not a
    // LOAD; not a SET or USE, which change the session every cached
    // statement was prepared in and so would drop their own handle each
    // time they run; not one with RETURNING, whose changes a failure after
    // its result changed shape would hide; and no executable comment, whose
    // content the server runs but this scan cannot see.
    bool preparable_{false};
    // Placeholders, by position, where the text protocol takes only an
    // integer literal (LIMIT, OFFSET, FETCH and ROWS EXAMINED counts).
    std::uint64_t integer_params_{0};
};

[[nodiscard]] mariadb_statement_traits mariadb_statement_traits_of(std::string_view sql) noexcept;

struct mariadb_statement_classification final {
    // A statement that must have the connection to itself: LOAD ... LOCAL INFILE
    // answers its command with a request for the file, then reads the file's
    // content as packets of the same command, so a command sent behind it would
    // be read as file content. A statement led by an executable comment counts as
    // one, since the comment may hide a LOAD.
    bool exclusive_{false};
    // A statement that may run a stored procedure: a CALL, or one hidden behind
    // an executable comment.
    bool calls_routine_{false};
    // Whether `sql` names one of the session tracking variables.
    bool names_session_tracking_{false};
    // Whether `sql` contains a backslash character.
    bool has_backslash_{false};
};

struct mariadb_parameter_traits final {
    bool has_backslash_{false};
    // A backslash right after a byte above 0x7F.
    bool has_ambiguous_backslash_{false};
};

[[nodiscard]] mariadb_statement_classification classify_mariadb_statement(std::string_view sql) noexcept;
[[nodiscard]] mariadb_parameter_traits inspect_mariadb_parameters(std::span<const db_value> params) noexcept;
// Whether escaping string parameters for the session needs its client
// character set. Only an escape a string needs could change its meaning: a
// backslash put in front of its own backslash, in a session that reads
// backslash escapes (its current server status tells). That is safe in every
// character set right after an ASCII byte; right after another byte a
// multibyte character set (GBK, Big5, SJIS) may read the two as one character
// and leave the inserted backslash to escape what follows.
[[nodiscard]] bool mariadb_escape_needs_charset(
    const st_mysql& connection, bool has_ambiguous_backslash) noexcept;

}  // namespace ruvia::detail

struct st_mysql;
struct st_mysql_stmt;

namespace ruvia::detail {

// Storage a bound parameter points at for the length of an execution.
union mariadb_param_value {
    long long integer_;
    double real_;
    signed char flag_;
};

// Storage of one result column between mysql_stmt_fetch() and formatting.
struct mariadb_column final {
    std::vector<char> buffer_;
    unsigned long length_{0};
    char null_{0};
    char error_{0};
};

// One cached statement: its entry_ref once prepared, the placeholders its text
// restricts (mariadb_statement_traits::integer_params_), and the hash of its
// key, for refusing it.
struct mariadb_statement final {
    st_mysql_stmt* handle_{nullptr};
    std::uint64_t integer_params_{0};
    std::size_t key_hash_{0};
};

// Server-side prepared statements of one MariaDB connection, so a repeated
// statement skips the server's parse and travels in the binary protocol
// (db_statement_cache: keyed by SQL and parameter types, prepared on its
// second use, the least recently used one closed for room).
//
// The text protocol is the reference: a statement runs prepared only where
// the binary protocol returns the same field text and errors. So it runs as
// text when the server will not prepare it, when its statement text rules it
// out (mariadb_statement_traits), when a result column is one whose text the
// binary value cannot tell (a YEAR the prepare describes), and for a call whose
// parameters the server would read otherwise than their literals: a negative
// number (a literal's unary minus binds looser than COLLATE, and -? of
// INT64_MIN overflows only as a parameter), an integer above INT64_MAX, a
// string with a NUL byte or a non-finite double (refused by the text
// protocol), and anything but a non-negative integer where the text protocol
// takes only an integer literal (LIMIT ?).
//
// The server parses a statement when it prepares it, under the session's
// sql_mode, character sets, default schema and other variables of that
// moment, and keeps that reading. The session reports every change of those
// variables (mariadb_tracked_variables, tracked with the schema from when the
// connection opens): such a change drops every cached statement, so each is
// prepared anew under the new state; the session's other reports (last_gtid
// after each commit when binary logging is on, autocommit) leave the cache
// alone. A connection whose server does not track its session, whose
// tracking a statement turned off, or whose client or connection character
// set is not the utf8mb4 it opened with (the binary protocol converts
// parameters otherwise than the text protocol does literals) runs nothing
// prepared. A stored procedure may turn tracking off without a report, so
// after a CALL the next prepared execution first reads the tracked list back
// from the server.
//
// The cache owns every statement entry_ref, including those evicted or found
// stale that still wait to be closed on the server, and each entry's SQL,
// which libmariadb reads while a prepare is suspended. After the connection
// was closed, clear() frees the handles without I/O.
class mariadb_statement_cache final {
public:
    using entry_ref = db_statement_entry;

    mariadb_statement_cache();
    ~mariadb_statement_cache();

    mariadb_statement_cache(const mariadb_statement_cache&) = delete;
    mariadb_statement_cache& operator=(const mariadb_statement_cache&) = delete;

    // Counts a use of `sql` with `params`. A statement comes back only when
    // the caller may run it prepared now and it would return what its text
    // returns; another use still counts toward preparing it later.
    [[nodiscard]] std::optional<entry_ref> find(std::string_view sql, std::span<const db_value> params, bool may_prepare);
    // A new connection starts without server-side statements; the old one is
    // closed, so its handles are freed without I/O. Until connected() says
    // otherwise, the session counts as untracked.
    void clear() noexcept;

    // Hears the session changes `connection` reads from the server; false
    // when libmariadb refused the callback. Called before it connects.
    [[nodiscard]] bool observe(st_mysql& connection);
    // The connection opened: whether the server tracks every change of its
    // session for it from now on.
    void connected(bool tracked) noexcept;
    // A statement that may turn session tracking off (it names one of its
    // variables, whose change the server does not report) is about to run.
    void untrack() noexcept;
    // A statement that may run a stored procedure was sent; the procedure
    // may have turned tracking off unreported.
    void routine_called() noexcept;
    // Whether the session's client character set is known to be utf8mb4:
    // tracked, and so current once every response before has been read.
    [[nodiscard]] bool client_utf8mb4() const noexcept;

    [[nodiscard]] mariadb_statement& entry(entry_ref statement);
    void reserve_retired(std::size_t count = 1);
    void retire(st_mysql_stmt* statement) noexcept;
    void refuse(std::size_t hash) noexcept;
    void refuse(entry_ref statement, std::size_t hash) noexcept;
    [[nodiscard]] bool refused(std::size_t hash) const noexcept;
    void invalidate();
    [[nodiscard]] bool usable() const noexcept;
    void lose_tracking() noexcept;

    db_statement_cache<mariadb_statement> statements_;
    std::vector<st_mysql_stmt*> retired_;
    // Key hashes of statements that run as text on this connection; zero
    // marks a free position.
    std::array<std::size_t, db_statement_cache<mariadb_statement>::capacity> refused_{};
    std::size_t next_refused_{0};
    // What the session reported: whether it tracks every variable the cache
    // relies on and the schema, and whether its client and connection
    // character sets are still utf8mb4; and a change since the last find().
    bool tracked_{false};
    bool all_variables_{false};
    bool schema_tracked_{false};
    bool untracked_{false};
    bool client_utf8mb4_{true};
    bool connection_utf8mb4_{true};
    bool changed_{false};
    // A stored procedure ran since the tracked list was last read back.
    bool verify_{false};
    // Scratch of the one execution a connection runs at a time.
    std::vector<unsigned int> types_;
    std::vector<std::max_align_t> binds_;
    std::vector<mariadb_param_value> values_;
    std::vector<mariadb_column> columns_;
    // The bytes the column buffers hold, and those the last result needed.
    std::size_t column_bytes_{0};
    std::size_t column_needed_{0};
};

// The system variables a statement's parse or result depends on, as
// session_track_system_variables lists them, with the two tracking variables
// themselves, so that turning the schema's tracking off or changing this
// list to one that still names it is reported. A change of
// collation_connection is reported as one of character_set_connection.
inline constexpr std::string_view mariadb_tracked_variables =
    "sql_mode,old_mode,character_set_client,character_set_connection,character_set_results,time_zone,"
    "div_precision_increment,default_week_format,lc_time_names,max_sort_length,group_concat_max_len,"
    "session_track_system_variables,session_track_schema";

// Turns on the session tracking the statement cache relies on.
inline constexpr auto mariadb_session_tracking_storage = [] {
    constexpr std::string_view prefix = "SET SESSION session_track_system_variables = '";
    constexpr std::string_view suffix = "', session_track_schema = ON";
    std::array<char, prefix.size() + mariadb_tracked_variables.size() + suffix.size()> statement{};
    auto out = statement.begin();
    for (const auto part : std::array{prefix, mariadb_tracked_variables, suffix}) {
        for (const char byte : part) {
            *out++ = byte;
        }
    }
    return statement;
}();
inline constexpr std::string_view mariadb_session_tracking_setup(mariadb_session_tracking_storage.data(), mariadb_session_tracking_storage.size());

// One prepared execution as a sequence of libmariadb operations: after a
// stored procedure ran, reading back the session's tracked variables;
// closing the statements waiting to be closed, preparing the statement on its
// first prepared use, executing it, and storing each result.
// resume_mariadb_prepared() starts or continues the current operation and
// returns the wait mask its continuation needs; 0 hands the run back to its
// caller, which goes on according to step_.
struct mariadb_prepared_run final {
    enum class step : std::uint8_t {
        // Querying @@session_track_system_variables, then storing its row.
        verifying,
        verified,
        close,
        prepare,
        // The caller confirms the statement still runs, then resumes.
        execute,
        executing,
        storing,
        // The rows failed (the server's error ended them): back to the state
        // after prepare, so that the next execution starts clean.
        resetting,
        // The caller takes the stored rows, then resumes.
        rows,
        next_result,
        // Reading past the rows of a result the run cannot use.
        draining,
        done,
    };

    mariadb_statement_cache* cache_{nullptr};
    mariadb_statement_cache::entry_ref handle_{};
    std::span<const db_value> params_{};
    step step_{step::done};
    // The current step's operation has been started and waits to continue.
    bool started_{false};
    // A entry_ref being closed on the server, or drained.
    st_mysql_stmt* closing_{nullptr};
    // The statement's outcome: its failure, or what its first result reported.
    std::exception_ptr failure_{};
    std::exception_ptr output_failure_{};
    std::uint64_t affected_rows_{0};
    std::uint64_t insert_id_{0};
    bool returned_rows_{false};
    bool extra_row_set_{false};
    // It did not execute because the server no longer holds it as prepared
    // (ER_NEED_REPREPARE, ER_UNKNOWN_STMT_HANDLER). It was dropped from the
    // cache, and a run outside a transaction may go again, preparing it
    // anew. A statement whose result changed shape after it ran is dropped
    // as well, but fails: the server has run it, and running it again would
    // repeat its effects.
    bool stale_{false};
    // The server refused to prepare it, its result is not readable as text,
    // or the session no longer tracks what the cache relies on: run it as
    // text instead.
    bool text_{false};
};

// Starts a run for a statement find() returned.
[[nodiscard]] mariadb_prepared_run start_mariadb_prepared(
    mariadb_statement_cache& cache, mariadb_statement_cache::entry_ref statement, std::span<const db_value> params) noexcept;
// Continues a run with the readiness libmariadb waited for (0 to start the
// current step); returns the next wait mask, or 0 when the caller goes on.
// A failure that leaves the connection out of step is thrown.
[[nodiscard]] int resume_mariadb_prepared(st_mysql& connection, mariadb_prepared_run& run, int ready);
// Keeps the stored rows of the run's first result in `output`, or discards
// them when it is null; then the run resumes.
void take_mariadb_prepared_rows(mariadb_prepared_run& run, db_rows* output);
// Ends a run whose statement was abandoned before it executed.
void abandon_mariadb_prepared(mariadb_prepared_run& run) noexcept;
// What a statement's responses amount to, whichever protocol ran it: its own
// failure, a second row set, a failure to keep its rows, or a row set the
// caller did (not) ask for; null when it succeeded.
[[nodiscard]] std::exception_ptr mariadb_statement_failure(const std::exception_ptr& failure, bool extra_row_set,
    const std::exception_ptr& output_failure, bool returns_rows, bool returned_rows);

}  // namespace ruvia::detail

struct st_mysql_res;
struct ma_charset_info_st;

// MariaDB's part of statement pipelining (db_pipeline.h). A group's
// statements are sent as separate COM_QUERY commands (mysql_send_query) and
// their responses read in order (mysql_read_query_result and
// mysql_store_result). A failed statement does not affect the ones after it:
// the server runs each command on its own.
//
// libmariadb runs one nonblocking operation per connection at a time, and
// accepts a command only once every result before it has been fully read. So
// flights queue in the group and the reader sends them whenever it stands
// between two responses, before it reads the next one: statements that arrive
// while a response is being read go out right after it. The bytes sent ahead
// of unread responses are bounded (mariadb_pipeline_send_budget), so neither
// side can fill its socket buffers with requests while the other waits to
// write responses; a statement larger than the budget is sent once nothing is
// outstanding.
//
// A command is sent ahead of responses only when the statements before it
// cannot change what it means. Its parameters are interpolated when it is
// queued, in text every session reads alike; a statement whose interpolation
// depends on the session -- a backslash in its text or in a string parameter,
// which NO_BACKSLASH_ESCAPES reads either way -- is interpolated and sent once
// every response before it has been read, a SET of sql_mode among them, and
// when its escape depends on the session's client character set
// (mariadb_escape_needs_charset()), that is read from the server first, unless
// session tracking says it is still utf8mb4. A LOAD ... LOCAL INFILE
// (mariadb_statement_classification::exclusive_) goes out alone, and nothing
// follows it until its response has been read.
//
// A statement found in the connection's statement cache runs prepared only
// when it is the only statement its worker runs: the binary protocol has no
// nonblocking way to read an execution's response other than by waiting for
// it, so a prepared statement cannot share the wire with other flights, and
// waiting for each response costs more than the server saves on parsing once
// other statements compete for the worker. Those run as pipelined text.

namespace ruvia::detail {

template <typename driver_type>
class db_pool;
struct mariadb_driver;

inline constexpr std::size_t mariadb_pipeline_send_budget = 64 * 1024;

// One statement's command: its text, kept by its caller until it is sent,
// and the statement as written, for running it prepared or interpolating it
// late.
struct mariadb_flight final {
    // Empty while `deferred_`.
    std::string_view sql_{};
    std::string_view statement_sql_{};
    std::span<const db_value> params_{};
    std::string* interpolated_{nullptr};
    // The bytes it put on the wire, once sent.
    std::size_t size_{0};
    bool sent_{false};
    // Interpolated when sent, once every response before it has been read.
    bool deferred_{false};
    // Sent once every response before it has been read, and followed by
    // nothing until its own response has been read.
    bool exclusive_{false};
    // May run a stored procedure (mariadb_statement_classification::calls_routine_).
    bool routine_{false};
    // Its use was counted in the statement cache: when the reader comes back
    // to it, it goes out as text.
    bool counted_{false};
    // Whether `params_` contains a backslash preceded by a non-ASCII byte.
    bool ambiguous_backslash_{false};
    // The session's client character set, read for a deferred statement
    // whose escape depends on it.
    const ma_charset_info_st* charset_{nullptr};
};

// The operation a group's connection runs, one at a time.
enum class mariadb_operation : std::uint8_t {
    none,
    send,
    read,
    store,
    next_result,
    // Reading the session's client character set for the head statement:
    // the query, then its row.
    charset_query,
    charset_store,
    // A statement running alone in the group as a prepared statement.
    prepared,
};

// The pipeline hooks of MariaDB (see db_pipeline_impl.h).
struct mariadb_pipeline_io final {
    using driver_type = mariadb_driver;
    using pool_type = db_pool<mariadb_driver>;
    using state_type = db_pipeline_state<mariadb_pipeline_io>;
    using member_type = db_pipeline_member<mariadb_pipeline_io>;
    using flight_type = mariadb_flight;

    struct group_state final {
        group_state()
            : send_buffer_() {}

        // Frees a stored result nobody took; called whenever the group's
        // flights clear, which every group does before its slot goes away.
        void reset() noexcept;

        // The operation in progress: the wait mask it suspended with, and,
        // once exchange() finished it, its result.
        mariadb_operation operation_{mariadb_operation::none};
        int status_{0};
        bool done_{false};
        int result_{0};
        st_mysql_res* stored_{nullptr};
        // Records from the head that were sent, or dropped unsent because
        // their statement was abandoned first.
        std::size_t sent_{0};
        std::size_t bytes_in_flight_{0};
        // Commands were sent since the reader last paused for a turn.
        bool batched_{false};
        // The command being sent, copied (or, deferred, interpolated) so that
        // a statement abandoned while its send is suspended leaves libmariadb
        // nothing dangling. Above the send budget it is given back once sent.
        std::string send_buffer_;
        // What has been read of the head's responses.
        std::exception_ptr head_failure_{};
        std::exception_ptr head_output_failure_{};
        std::uint64_t head_affected_rows_{0};
        std::uint64_t head_insert_id_{0};
        bool head_returned_rows_{false};
        bool head_extra_row_set_{false};
        // The head statement's prepared execution, while operation_ is
        // prepared.
        mariadb_prepared_run prepared_{};
    };

    // The statement as its caller wrote it; queue() interpolates the
    // parameters unless that waits for the session (mariadb_flight).
    struct statement_type final {
        std::string_view sql_;
        std::span<const db_value> params_;
        std::string interpolated_;
        mariadb_statement_classification classification_{};
        bool deferred_{false};
        bool ambiguous_backslash_{false};
    };

    [[nodiscard]] static statement_type prepare(std::string_view sql, std::span<const db_value> params);
    [[nodiscard]] static db_slot_socket::native_socket_type socket(pool_type& pool, std::size_t slot);
    static void open(state_type& state, std::size_t slot);
    [[nodiscard]] static bool queue(
        state_type& state, std::size_t slot, member_type& member, statement_type& statement, flight_type& flight);
    [[nodiscard]] static bool advance(state_type& state, std::size_t slot, member_type& reader);
    [[nodiscard]] static db_socket_readiness wanted(pool_type& pool, std::size_t slot) noexcept;
    static void exchange(state_type& state, std::size_t slot, db_socket_readiness ready);
    [[nodiscard]] static bool reusable(state_type& state, std::size_t slot) noexcept;
};

// Per connection: cached prepared statements and the pipeline group sharing
// it.
struct mariadb_session final {
    // Cleared by connect(): a new connection holds no server statements.
    mariadb_statement_cache statements_;
    db_pipeline_group<mariadb_pipeline_io> pipeline_;
};

}  // namespace ruvia::detail

struct pg_conn;
struct pg_result;
// PostgreSQL's part of statement pipelining (db_pipeline.h): a group's
// connection runs in libpq pipeline mode, and each statement sends its own
// flight ending in its own Sync, so the server aborts no statement for another
// one's failure. Flights are queued and flushed as they are sent; the reader
// consumes input and reads results whenever libpq holds them.

namespace ruvia::detail {
// Builds the error for a failed libpq call or result. A statement_failed on a
// connection libpq has marked bad is reported as io_error.
[[nodiscard]] db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type code, const pg_result* result = nullptr);

// Parameters are sent in text format. A double declares float8 so the server
// never infers an integer type from an integral value; every other parameter
// is left untyped (OID 0): the SQL decides its type, and compiled SQL casts
// each placeholder from its value category.
struct postgresql_params final {
    static constexpr std::size_t inline_capacity = 8;
    static constexpr std::size_t numeric_slot_size = 32;

    explicit postgresql_params(std::span<const db_value> params);
    postgresql_params(const postgresql_params&) = delete;
    postgresql_params& operator=(const postgresql_params&) = delete;
    postgresql_params(postgresql_params&&) = delete;
    postgresql_params& operator=(postgresql_params&&) = delete;

    [[nodiscard]] int count() const noexcept {
        return static_cast<int>(count_);
    }

    [[nodiscard]] const char* const* values() const noexcept {
        if (count_ == 0) {
            return nullptr;
        }
        return count_ <= inline_capacity ? inline_values_.data() : heap_values_.data();
    }

    [[nodiscard]] const unsigned int* types() const noexcept {
        if (count_ == 0) {
            return nullptr;
        }
        return count_ <= inline_capacity ? inline_types_.data() : heap_types_.data();
    }

    [[nodiscard]] std::span<const unsigned int> type_span() const noexcept {
        return {types(), count_};
    }

    std::size_t count_{0};
    std::array<char, inline_capacity * numeric_slot_size> inline_encoded_;
    std::array<const char*, inline_capacity> inline_values_;
    std::array<unsigned int, inline_capacity> inline_types_;
    std::vector<std::array<char, numeric_slot_size>> heap_encoded_;
    std::vector<const char*> heap_values_;
    // libpq's Oid is unsigned int; 0 leaves the type to the server.
    std::vector<unsigned int> heap_types_;
};

[[nodiscard]] std::uint64_t postgresql_affected_rows(const pg_result& result_value) noexcept;

// Named server-side statements prepared on one PostgreSQL connection, so a
// repeated statement skips the server's parse and analysis
// (db_statement_cache: keyed by SQL and the parameter types it declares,
// prepared on its second use, least recently used closed for room). A name is
// derived from the key's SHA-256: equal names denote equal statements, so a
// name reaching a server session it was not prepared on (a pooler that shares
// server sessions) can never run a different statement. Every statement in
// flight on a pipelined connection shares the cache.
//
// "ruvia_", 32 lowercase hex digits of the key digest, and a NUL.
using postgresql_statement_name = std::array<char, 39>;
using postgresql_statement_cache = db_statement_cache<postgresql_statement_name>;

[[nodiscard]] postgresql_statement_name make_postgresql_statement_name(
    std::string_view sql, std::span<const unsigned int> types);

template <typename driver_type>
class db_pool;
struct postgresql_driver;

// The commands of one buffered statement flight. A first use runs as the
// unnamed statement. Otherwise a Close of a statement evicted to make room and
// a Close and Parse of a statement new to the connection precede the
// statement. A Sync ends the flight.
struct postgresql_flight final {
    // The cache entry of a named statement.
    std::optional<postgresql_statement_cache::entry_ref> statement_{};
    // Commands queued ahead of the statement.
    std::size_t setup_commands_{0};
    bool parses_{false};
};

// What has been read of one flight's results, up to its Sync.
struct postgresql_flight_results final {
    // Index of the command whose results are being read; each command's
    // results end with a null result, and the Sync result follows the last.
    std::size_t command_{0};
    std::uint64_t affected_rows_{0};
    // The first error of the flight; the server skips the rest up to Sync.
    std::optional<db_error> failure_{};
    // Keeping the rows failed (an allocation in the caller's resource); the
    // flight is still read up to its Sync, so only its statement fails.
    std::exception_ptr output_failure_{};
    // The error is the statement's own rather than its setup's.
    bool statement_failed_{false};
    // The error reports a stale cached statement (26000 / 0A000) ...
    bool stale_{false};
    // ... found while looking it up or revalidating it, before it ran.
    bool stale_before_execution_{false};
    bool returned_rows_{false};
    bool parsed_{false};
};

// The pipeline hooks of PostgreSQL (see db_pipeline_impl.h).
struct postgresql_pipeline_io final {
    using driver_type = postgresql_driver;
    using pool_type = db_pool<postgresql_driver>;
    using state_type = db_pipeline_state<postgresql_pipeline_io>;
    using member_type = db_pipeline_member<postgresql_pipeline_io>;
    using flight_type = postgresql_flight;

    struct group_state final {
        postgresql_flight_results head_results_{};
        // libpq holds output the socket has not accepted yet.
        bool flush_pending_{false};

        void reset() noexcept {
            head_results_ = {};
            flush_pending_ = false;
        }
    };

    // The statement text, kept by its caller, and its encoded parameters.
    struct statement_type final {
        statement_type(std::string_view sql, std::span<const db_value> params)
            : sql_(sql),
              encoded_(params) {}

        std::string_view sql_;
        postgresql_params encoded_;
    };

    [[nodiscard]] static statement_type prepare(std::string_view sql, std::span<const db_value> params);
    // The connection's socket for a wait; throws when it has none.
    [[nodiscard]] static db_slot_socket::native_socket_type socket(pool_type& pool, std::size_t slot);
    static void open(state_type& state, std::size_t slot);
    [[nodiscard]] static bool queue(
        state_type& state, std::size_t slot, member_type& member, statement_type& statement, flight_type& flight);
    [[nodiscard]] static bool advance(state_type& state, std::size_t slot, member_type& reader);
    [[nodiscard]] static db_socket_readiness wanted(pool_type& pool, std::size_t slot) noexcept;
    static void exchange(state_type& state, std::size_t slot, db_socket_readiness ready);
    [[nodiscard]] static bool reusable(state_type& state, std::size_t slot) noexcept;
};

// Per connection: cached statements and the pipeline group sharing it.
struct postgresql_session final {
    // Cleared by connect(): a new connection holds no server statements.
    postgresql_statement_cache statements_;
    db_pipeline_group<postgresql_pipeline_io> pipeline_;
};

}  // namespace ruvia::detail

struct st_mysql;
struct pg_conn;

// One worker-local database pool, shared by every driver. The pool owns lease
// scheduling, cancellation, deadlines, connection lifecycle, and the statement,
// stream and transaction shapes; a driver supplies its connection type, the
// protocol hooks that talk to its client library, and the state it keeps per
// connection (session_type) and per pool (state_type).

namespace ruvia::detail {

using db_resolved_addresses_type = std::vector<std::string>;

[[nodiscard]] db_resolved_addresses_type collect_db_resolved_addresses(
    const client_resolve_result& results, db_driver driver);

// Connector/C accepts comma-separated hosts and performs failover only while
// opening the transport. IPv6 literals in a multi-host list must be bracketed
// so their colons are not parsed as a per-host port separator; a lone IPv6
// literal remains unbracketed because Connector/C only invokes that list parser
// when a comma is present.
[[nodiscard]] std::string make_mariadb_resolved_host_list(
    std::span<const std::string> addresses);

struct postgresql_resolved_host_list final {
    std::string hosts_;
    std::string addresses_;
};

// libpq requires host and hostaddr lists to contain the same number of items.
// Repeating the logical hostname preserves TLS/GSS/password-file identity while
// the numeric hostaddr list suppresses libpq's blocking DNS lookup.
[[nodiscard]] postgresql_resolved_host_list make_postgresql_resolved_host_list(
    std::string_view logical_host, std::span<const std::string> addresses);

enum class db_deadline_kind : std::uint8_t {
    resolve,
    socket,
    // A driver-requested timed pause without a socket wait (MariaDB connect).
    sleep,
};

// What a driver hook reports after starting an unbuffered statement.
struct db_stream_start final {
    db_stream_start() = default;

    // Driver-owned result handle handed to the stream (may be null).
    void* result_{nullptr};
    std::vector<std::string> column_names_;
    // False when the statement produced no row set; the slot is still clean.
    bool returns_rows_{true};
};

// How a stream ended. Either way the driver has read the statement to its end,
// released its result handle, and left the connection idle.
enum class db_stream_end : std::uint8_t {
    exhausted,
    // The statement produced no row set, which only its first result tells.
    no_row_set,
};

// One read of a stream: its next row, or how it ended.
using db_stream_step = std::variant<db_row, db_stream_end>;

template <typename driver_type>
class db_pool;

// The configured port as a NUL-terminated buffer, the form both asio's
// resolver and libpq take it in.
[[nodiscard]] std::array<char, 6> format_db_port(std::uint16_t port);

// The driver hooks. Statement hooks run on a connected, leased slot under a
// deadline already constrained by the query timeout and, on a transaction or
// stream lease, by the lease deadline. `sql` views are
// NUL-terminated and contain no other NUL, and string parameters own their
// NUL-terminated storage for the entire hook call: raw SQL is checked once
// where it enters (prepare_db_statement), compiled SQL is built that way, and
// parameters are checked where they are encoded for the client library. Hooks
// throw on failure. A statement_failed db_error or a std::invalid_argument
// promises that the command has been drained and the connection is idle -- a
// hook that cannot get there closes the slot before throwing -- so the pool
// keeps the connection; any other failure closes it.
//
// Outside transactions, query() and execute() run through the driver's
// pipeline (pipeline_io_type, db_pipeline.h), which shares connections between
// callers; state_type is that pipeline's per-pool state and session_type keeps
// the group of each connection. The state may keep a lease that release_slot()
// gives back for a caller of its own (take_released_slot() returns true),
// hears when a caller stops waiting for a lease, and is closed by close_now().
// Before any lease is reassigned, reusable_connection() must confirm that the
// drained connection is outside a transaction and safe for unrelated callers.
// idle_connection_alive() looks at a connection that sat in the pool, without
// blocking and before anything is sent on it: false when the server ended the
// session meanwhile, so the pool reconnects instead.
//
// Stream hooks are protocol steps only: the pool owns a stream's cancellation,
// deadline and the release of its slot. A stream's `result` handle is the
// driver's, kept in the slot (db_slot::stream_result_); a hook that frees it
// sets it to null, and after a hook throws the pool discards what is left of
// it (discard_stream()) and closes the slot.
struct mariadb_driver final {
    using connection_type = st_mysql;
    using pool_type = db_pool<mariadb_driver>;
    using session_type = mariadb_session;
    using pipeline_io_type = mariadb_pipeline_io;
    using state_type = db_pipeline_state<pipeline_io_type>;
    static constexpr db_driver driver = db_driver::mariadb;
    static constexpr std::string_view name{"MariaDB"};

    static task<void> connect(pool_type& pool, std::size_t slot, const operation_deadline& deadline);
    static void close_connection(connection_type& connection) noexcept;
    [[nodiscard]] static bool reusable_connection(connection_type& connection) noexcept;
    [[nodiscard]] static bool idle_connection_alive(connection_type& connection) noexcept;
    static task<db_rows> query(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_exec_result> execute(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_stream_start> start_stream(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_stream_step> read_stream_row(pool_type& pool, std::size_t slot, void*& result,
        std::vector<db_field>& fields, std::vector<std::string>& column_names, const operation_deadline& deadline);
    // Ends a stream before its rows ran out; on return the connection is idle.
    static task<void> close_stream(pool_type& pool, std::size_t slot, void*& result, const operation_deadline& deadline);
    static void discard_stream(void* result) noexcept;
};

struct postgresql_driver final {
    using connection_type = pg_conn;
    using pool_type = db_pool<postgresql_driver>;
    using session_type = postgresql_session;
    using pipeline_io_type = postgresql_pipeline_io;
    using state_type = db_pipeline_state<pipeline_io_type>;
    static constexpr db_driver driver = db_driver::postgresql;
    static constexpr std::string_view name{"PostgreSQL"};

    static task<void> connect(pool_type& pool, std::size_t slot, const operation_deadline& deadline);
    static void close_connection(connection_type& connection) noexcept;
    [[nodiscard]] static bool reusable_connection(connection_type& connection) noexcept;
    [[nodiscard]] static bool idle_connection_alive(connection_type& connection) noexcept;
    static task<db_rows> query(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_exec_result> execute(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_stream_start> start_stream(pool_type& pool, std::size_t slot, std::string_view sql,
        std::span<const db_value> params, const operation_deadline& deadline);
    static task<db_stream_step> read_stream_row(pool_type& pool, std::size_t slot, void*& result,
        std::vector<db_field>& fields, std::vector<std::string>& column_names, const operation_deadline& deadline);
    // Ends a stream before its rows ran out; on return the connection is idle
    // or closed.
    static task<void> close_stream(pool_type& pool, std::size_t slot, void*& result, const operation_deadline& deadline);
    static void discard_stream(void* result) noexcept;
};

// One pooled connection. The driver owns the native socket; db_slot_socket
// waits on a Ruvia-owned duplicate of it. Pending Asio handlers, deadline
// continuations and pipeline records address the slot, so it never moves.
template <typename driver_type>
struct db_slot final {
    db_slot(asio::io_context& io_context, const worker_ref& worker)
        : resolver_(worker),
          wait_socket_(io_context) {}

    ~db_slot() {
        if (wait_active_) {
            std::terminate();
        }
    }

    db_slot(const db_slot&) = delete;
    db_slot& operator=(const db_slot&) = delete;

    client_host_resolver resolver_;
    typename driver_type::connection_type* connection_{nullptr};
    db_slot_socket wait_socket_;
    // Driver state of the current connection.
    [[no_unique_address]] typename driver_type::session_type session_;
    // A sleep wait parks here until its deadline or a close resumes it.
    std::coroutine_handle<> deadline_continuation_{};
    // The driver result of the stream holding this slot, null otherwise. The
    // pool keeps it so an expiring lease can discard it before closing.
    void* stream_result_{nullptr};
    // Whole-lease deadline (db_config::transaction_timeout_) of the
    // transaction or stream holding this slot; unbounded for any other lease.
    operation_deadline lease_timeout_{std::nullopt};
    bool connected_{false};
    // Shutdown may request closure while an asynchronous wait still borrows
    // this slot and its socket. The transport stays alive until that wait
    // resumes and the driving coroutine performs the final close; destroying
    // it immediately would leave the queued Asio handler dangling.
    bool wait_active_{false};
    bool close_requested_{false};
    // An operation of the transaction or stream holding this slot is running.
    // Its deadline already ends no later than lease_timeout_, so the lease
    // timer leaves it to that deadline.
    bool lease_busy_{false};
    // Fires at lease_timeout_ to close the connection of a lease its holder
    // keeps idle. Declared last so it is unregistered before anything it
    // reaches is destroyed.
    worker_timer_registration lease_timer_;
};

// Marks a slot as borrowed by a suspended wait for the wait's duration.
template <typename driver_type>
class db_slot_active_wait_guard final {
public:
    explicit db_slot_active_wait_guard(db_slot<driver_type>& slot) noexcept
        : slot_(slot) {
        if (slot_.wait_active_) {
            std::terminate();
        }
        slot_.wait_active_ = true;
    }
    db_slot_active_wait_guard(const db_slot_active_wait_guard&) = delete;
    db_slot_active_wait_guard& operator=(const db_slot_active_wait_guard&) = delete;
    ~db_slot_active_wait_guard() {
        slot_.wait_active_ = false;
    }

private:
    db_slot<driver_type>& slot_;
};

template <typename driver_type>
class db_pool final {
public:
    using slot_type = db_slot<driver_type>;

    struct slot_policy final {
        using owner_type = db_pool;
        using deadline_kind_type = db_deadline_kind;
        [[noreturn]] static void throw_acquire_failure(const db_pool& pool, worker_wait_status status);
        [[noreturn]] static void throw_aborted(const db_pool& pool, pool_slot_abort_reason reason);
        static void expire_slot(db_pool& pool, std::size_t slot, db_deadline_kind kind) noexcept;
        static void cancel_slot(db_pool& pool, std::size_t slot) noexcept;
    };
    using leases_type = slot_pool<slot_policy>;

    db_pool(asio::io_context& io_context, const worker_ref& worker_value, db_config_storage config);
    db_pool(asio::io_context&, worker_ref&&, db_config_storage) = delete;
    ~db_pool();

    db_pool(const db_pool&) = delete;
    db_pool& operator=(const db_pool&) = delete;

    [[nodiscard]] task<void> connect();
    void close_now() noexcept;

    // The only execution path for raw SQL, compiled queries and transactions.
    // Outside a transaction, a driver that pipelines statements shares
    // connections between query() and execute() callers. `io_metrics` is the
    // request's counter block, or null outside a request.
    //
    // A transaction (begin_transaction() to commit/rollback) and a stream
    // (stream() until its rows run out or it is closed) each hold one lease
    // under config().transaction_timeout_: every operation on it runs under
    // the earlier of its own deadline and the lease deadline, and a lease
    // whose deadline passes between operations has its connection closed by
    // the lease timer; its next operation fails with timeout and gives the
    // slot back.
    task<db_rows> query(std::string sql, std::vector<db_value> params, operation_options options,
        request_io_metrics* io_metrics = nullptr);
    task<db_exec_result> execute(std::string sql, std::vector<db_value> params, operation_options options,
        request_io_metrics* io_metrics = nullptr);
    task<db_stream_result> stream(std::string sql, std::vector<db_value> params, operation_options options);
    task<std::optional<db_row>> read_stream_row(std::size_t slot, std::vector<db_field>& fields,
        std::vector<std::string>& column_names,
        const operation_options& options);
    task<void> close_stream(std::size_t slot, const operation_options& options);
    void abort_stream(std::size_t slot) noexcept;
    task<db_rows> query_on_transaction_slot(std::size_t slot, std::string sql,
        std::vector<db_value> params, const operation_options& options);
    task<db_exec_result> execute_on_transaction_slot(std::size_t slot, std::string sql,
        std::vector<db_value> params, const operation_options& options);
    task<db_transaction> begin_transaction(operation_options operation_options_value, db_transaction_options transaction_options);
    task<void> commit_transaction(
        std::size_t slot, const operation_options& options);
    task<void> rollback_transaction(
        std::size_t slot, const operation_options& options);
    void abort_transaction(std::size_t slot) noexcept;

    // Services for driver hooks.
    [[nodiscard]] const db_config_storage& config() const noexcept {
        return config_;
    }
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] slot_type& slot(std::size_t index) noexcept {
        return *slots_[index];
    }
    // Lease services for driver hooks. A driver never waits for a lease: the
    // pool's own waits (lease_acquire) are the only callers the lease scheduler
    // queues, so arrival order covers every one of them.
    [[nodiscard]] bool closing() const noexcept {
        return leases_.closing();
    }
    void throw_if_aborted(std::size_t index) const {
        leases_.throw_if_aborted(index);
    }
    // Starts an operation on a leased slot: clears a cancellation its previous
    // operation left, while closing stays permanent.
    void reset_abort(std::size_t index) noexcept {
        leases_.reset_abort(index);
    }
    // Leases a free slot without waiting; nullopt while every slot is leased
    // or the pool is closing.
    [[nodiscard]] std::optional<std::size_t> try_lease() {
        return leases_.try_acquire();
    }
    [[nodiscard]] typename driver_type::state_type& state() noexcept {
        return state_;
    }
    // Arrival order shared by every caller that waits for a connection: a
    // driver stamps its own waiters with it to keep their order against the
    // callers waiting for a lease.
    [[nodiscard]] std::uint64_t next_arrival() noexcept {
        return ++arrivals_;
    }
    // The arrival of the oldest caller still waiting for a lease, if any. One
    // whose stop was requested no longer counts: the lease scheduler drops it
    // at once, while the resumption that unlists it may come a turn later.
    [[nodiscard]] std::optional<std::uint64_t> oldest_exclusive_arrival() const noexcept;
    // Gives a lease back, unless the driver state keeps it for a caller of its
    // own. Every lease this pool hands out ends here, and so does the lease
    // deadline of a transaction or stream.
    void release_slot(std::size_t index) noexcept;
    // Runs once per new lease, before its first statement: closes the leased
    // slot's pooled connection when the server ended the session while it sat
    // idle, so the next connect_slot() replaces it and nothing is sent on a
    // dead connection.
    void retire_dead_idle_connection(std::size_t index) noexcept;
    // Connects a leased slot unless it is connected; a failure closes it. A
    // pool that does not reconnect (db_config_storage::reconnect_) fails here
    // for a slot whose connection ended.
    task<void> connect_slot(std::size_t index, const operation_deadline& timeout);
    // Closes the slot's connection now, or -- while a wait still borrows it --
    // wakes that wait so the driving coroutine finishes the close.
    void close_slot(std::size_t index) noexcept;
    task<db_resolved_addresses_type> resolve_host(std::size_t index, const operation_deadline& deadline);
    // One wait on a leased slot: readiness of the driver's current socket, or
    // a timed pause without a socket (MariaDB connect), under an optional
    // per-wait timeout. Not a coroutine: await_ready() registers the socket and
    // arms the slot deadline, await_suspend() hands the coroutine to the
    // readiness wait or the deadline, and await_resume() classifies the
    // outcome -- so a wait allocates nothing. Cancellation, closing, operation
    // timeout, and wait failures are thrown as db_error; slot-deadline expiry
    // and socket errors are returned for the driver to interpret, and a pause
    // always returns expired. Once the slot is connected, the socket's
    // registration persists across waits.
    class slot_wait final {
    public:
        slot_wait(db_pool& pool, std::size_t index, db_deadline_kind kind, db_slot_socket::native_socket_type native,
            db_socket_readiness wanted, std::optional<std::chrono::steady_clock::time_point> wait_deadline,
            const operation_deadline& operation_deadline_value) noexcept;
        ~slot_wait();

        slot_wait(const slot_wait&) = delete;
        slot_wait& operator=(const slot_wait&) = delete;

        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;
        [[nodiscard]] db_socket_wait_result await_resume();

    private:
        db_pool& pool_;
        std::size_t index_;
        db_deadline_kind kind_;
        db_slot_socket::native_socket_type native_;
        std::optional<std::chrono::steady_clock::time_point> wait_deadline_;
        const operation_deadline& operation_deadline_;
        db_slot_socket::readiness_awaiter socket_wait_;
        // The slot is marked borrowed between arming and resumption.
        bool borrowed_{false};
    };

    [[nodiscard]] slot_wait wait_socket(std::size_t index, db_slot_socket::native_socket_type native,
        db_socket_readiness wanted, std::optional<std::chrono::steady_clock::time_point> wait_deadline,
        const operation_deadline& operation_deadline_value) noexcept {
        return slot_wait(*this, index, db_deadline_kind::socket, native, wanted, wait_deadline, operation_deadline_value);
    }
    [[nodiscard]] slot_wait sleep(std::size_t index, std::optional<std::chrono::steady_clock::time_point> wait_deadline,
        const operation_deadline& operation_deadline_value) noexcept {
        return slot_wait(*this, index, db_deadline_kind::sleep, db_slot_socket::invalid_socket,
            db_socket_readiness::none, wait_deadline, operation_deadline_value);
    }
    [[nodiscard]] db_error timed_out() const;
    [[nodiscard]] db_error closing_error() const;

private:
    template <typename result_type>
    using statement_hook_type = task<result_type> (*)(db_pool&, std::size_t, std::string_view,
        std::span<const db_value>, const operation_deadline&);

    // A caller waiting for a lease, listed in arrival order for the duration
    // of its acquire.
    class exclusive_wait final {
    public:
        exclusive_wait(db_pool& pool, const stop_token& stop) noexcept;
        exclusive_wait(const exclusive_wait&) = delete;
        exclusive_wait& operator=(const exclusive_wait&) = delete;
        ~exclusive_wait();

    private:
        friend class db_pool;

        db_pool& pool_;
        const stop_token& stop_;
        std::uint64_t arrival_;
        exclusive_wait* previous_;
        exclusive_wait* next_{nullptr};
    };

    // The pool's only wait for a lease: stamped and listed before the lease
    // scheduler queues it, unlisted once its acquire returns. The awaitable
    // lives for its co_await expression, so it adds no coroutine frame.
    class lease_acquire final {
    public:
        lease_acquire(db_pool& pool, const operation_deadline& timeout, const stop_token& stop)
            : pool_(pool),
              waiting_(pool, stop),
              acquire_(pool.leases_.acquire(timeout, pool.config_.acquire_timeout_, stop)) {}
        lease_acquire(const lease_acquire&) = delete;
        lease_acquire& operator=(const lease_acquire&) = delete;

        [[nodiscard]] bool await_ready() {
            return acquire_.await_ready();
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            return acquire_.await_suspend(continuation);
        }
        [[nodiscard]] std::size_t await_resume() {
            const auto index = acquire_.await_resume();
            pool_.retire_dead_idle_connection(index);
            return index;
        }

    private:
        db_pool& pool_;
        exclusive_wait waiting_;
        typename leases_type::acquire_awaiter acquire_;
    };

    template <typename result_type>
    task<result_type> run_on_transaction_slot(std::size_t index, std::string sql,
        std::vector<db_value> params, const operation_options& options,
        statement_hook_type<result_type> statement);
    task<void> execute_control(std::size_t index, std::string_view sql,
        const operation_deadline& timeout);
    task<void> finish_transaction(std::size_t index, std::string_view command, const operation_options& options);
    task<void> retire_failed_transaction_slot(std::size_t index,
        const operation_deadline& timeout, const std::exception_ptr& failure);
    task<void> open_slot(std::size_t index, const operation_deadline& timeout);
    // Starts the lease deadline of a transaction or stream on its fresh lease,
    // with its first operation running.
    void start_lease(std::size_t index);
    // Starts one operation on a transaction or stream lease: reports an
    // aborted slot, then a lease whose deadline has passed.
    void enter_lease_operation(std::size_t index);
    // Leaves the lease held and idle after an operation succeeded; throws
    // timeout when the lease deadline passed meanwhile, so the operation fails
    // and its caller retires the lease.
    void leave_lease_operation(std::size_t index);
    // The earlier of an operation's own deadline and its lease deadline.
    [[nodiscard]] operation_deadline lease_operation_timeout(
        std::size_t index, const operation_deadline& timeout) const noexcept;
    // Lease timer expiry: closes an idle lease's connection, so the server
    // ends its transaction and releases its locks while the holder keeps the
    // lease.
    void expire_idle_lease(std::size_t index) noexcept;
    [[nodiscard]] db_error lease_timed_out() const;
    // Discards what a failed stream left, closes its connection and gives its
    // lease back.
    void fail_stream(std::size_t index, typename leases_type::cancellation_guard& cancellation) noexcept;
    // Every lease names a slot of this pool; any other index breaks the lease
    // lifecycle, which nothing can unwind from.
    void require_slot_index(std::size_t index) const noexcept;

    asio::io_context& io_context_;
    db_config_storage config_;
    const worker_ref& worker_;
    // Declared before the lease pool: pending deadline timers address slots.
    // Each slot is allocated once and stays put while the pool lives.
    std::vector<std::unique_ptr<slot_type>> slots_;
    leases_type leases_;
    std::uint64_t arrivals_{0};
    exclusive_wait* oldest_exclusive_{nullptr};
    exclusive_wait* newest_exclusive_{nullptr};
    // Declared after the lease pool, whose leases it borrows.
    [[no_unique_address]] typename driver_type::state_type state_;
};

#ifdef RUVIA_ENABLE_MARIADB
extern template class db_pipeline_state<mariadb_pipeline_io>;
extern template class db_pool<mariadb_driver>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
extern template class db_pipeline_state<postgresql_pipeline_io>;
extern template class db_pool<postgresql_driver>;
#endif

// Every pool operation dispatches on the configured backend; reaching the end
// means the build has no driver for it.
[[noreturn]] inline void throw_unavailable_db_backend() {
    throw std::logic_error("database backend is not available");
}

// db_pool_ref_type is a closed backend set. Keep its single checked dispatch here so
// handle, stream, transaction, and registry operations cannot drift into
// subtly different null or unavailable-backend behavior.
template <typename visitor_type>
decltype(auto) visit_db_pool([[maybe_unused]] const db_pool_ref_type& pool, [[maybe_unused]] visitor_type&& visitor) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<db_pool<mariadb_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<visitor_type>(visitor)(**client);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<db_pool<postgresql_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<visitor_type>(visitor)(**client);
    }
#endif
    throw_unavailable_db_backend();
}

// Destruction and immediate close paths cannot report an empty backend. They
// deliberately ignore it while preserving the same closed-set dispatch.
template <typename visitor_type>
void visit_db_pool_if_present([[maybe_unused]] const db_pool_ref_type& pool, [[maybe_unused]] visitor_type&& visitor) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<db_pool<mariadb_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<visitor_type>(visitor)(**client);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<db_pool<postgresql_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<visitor_type>(visitor)(**client);
    }
#endif
}

[[nodiscard]] inline db_driver db_pool_driver(const db_pool_ref_type& pool) {
    return visit_db_pool(pool, [](const auto& client) noexcept { return client.config().driver_; });
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Every result shape, so cache-id removal can name each entry of one id.
inline constexpr std::array db_cache_shapes{
    db_cache_shape::rows, db_cache_shape::one, db_cache_shape::exists, db_cache_shape::count};

// The key part naming each result shape, indexed by its value. An id-keyed
// entry stores the caller's id and this tag as separate key parts, so no user
// id can spell another id's shape.
inline constexpr std::array<std::string_view, db_cache_shapes.size()> db_cache_shape_tags{
    "rows", "one", "exists", "count"};

[[nodiscard]] constexpr std::string_view db_cache_shape_tag(db_cache_shape shape) noexcept {
    return db_cache_shape_tags[static_cast<std::size_t>(shape)];
}

// The key prefix of one alias's cache entries: everything that decides what a
// cached result means (namespace, alias, driver, server, database, user and
// TLS server name) digested into one key part.
[[nodiscard]] std::string db_cache_prefix(std::string_view name_space, std::string_view alias,
    const db_config_storage& config);

[[nodiscard]] std::string encode_db_cache_rows(const db_rows& rows);
[[nodiscard]] db_rows decode_db_cache_rows(std::string bytes_value);
// Key functions take the immutable prefix already built for this cache owner.
// Append all result shapes for one id, sharing the id digest.
void append_db_cache_id_keys(std::vector<std::string>& keys, std::string_view prefix, std::string_view id);
// Keyed by id and shape when the query names an id, otherwise by its SQL and parameters.
[[nodiscard]] std::string db_cache_key(std::string_view prefix, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver);

// The deferred database statement behind a cached step. On a cache hit it is
// destroyed without starting; on a miss it runs once on the plan's backend.
class db_cache_query final {
public:
    db_cache_query(const db_query_backend& backend, std::string sql, std::vector<db_value> params) noexcept
        : backend_(backend),
          sql_(std::move(sql)),
          params_(std::move(params)) {}

    db_cache_query(const db_cache_query&) = delete;
    db_cache_query& operator=(const db_cache_query&) = delete;
    db_cache_query(db_cache_query&&) noexcept = default;
    db_cache_query& operator=(db_cache_query&&) = delete;

    // The returned task owns the statement and the options it runs under.
    [[nodiscard]] task<db_rows> operator()(operation_options options) &&;

private:
    db_query_backend backend_;
    std::string sql_;
    std::vector<db_value> params_;
};

// One database alias's Redis-backed query-result cache (Redis builds only).
// Worker-local: concurrent misses of one key on this worker share one fill.
#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state final {
public:
    // `prefix` is the alias's db_cache_prefix().
    db_query_cache_state(const redis_session& redis, const db_cache_config_storage& config, std::string prefix);
    ~db_query_cache_state();
    void close_now() noexcept;
    [[nodiscard]] std::optional<std::string> key(
        const db_query& query, const db_statement& statement, db_driver driver);
    // Serves the step from the cache, or runs `database` and stores its rows.
    [[nodiscard]] task<db_rows> wrap(std::optional<std::chrono::milliseconds> duration, std::string key,
        db_cache_query database, operation_options options, operation_deadline deadline);
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] task<void> remove(
        std::span<const std::string_view> ids, request_io_metrics* io_metrics, operation_options options);
    [[nodiscard]] task<void> clear(request_io_metrics* io_metrics, operation_options options);

private:
    // One read-through fill in flight: the caller that missed first (the
    // leader) runs the statement; later misses of the same full key wait for
    // its encoded result, or for its end without one, and then look again.
    // Shared, so a waiter still reads it after the leader's frame is gone.
    struct fill final {
        explicit fill(std::string_view key)
            : key_(key) {}

        std::string key_;
        // Requested once the fill ends, with or without a result.
        stop_source done_;
        std::optional<std::string> result_;
        // A waiter joined, so the leader publishes its result in memory.
        bool awaited_{false};
    };
    using fill_pointer = std::shared_ptr<fill>;
    class fill_lead;

    void require_open() const;
    task<db_rows> query(std::string key, std::chrono::milliseconds duration, db_cache_query database,
        operation_options options, operation_deadline deadline);
    // Ends the fill if it is still the one listed for its key. Later misses
    // of that key no longer join it.
    void unlist_fill(const fill& ended) noexcept;
    // After remove() or clear(), a fill that started before must not answer
    // callers that arrive after: they read the database themselves.
    void unlist_fills(std::span<const std::string> keys) noexcept;
    void unlist_all_fills() noexcept;

    bool closed_{false};
    stop_source stop_;
    const worker_ref& worker_;
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    // The store's reply limit: a GET of a larger entry fails on every hit, so
    // such an entry is never written.
    std::optional<std::size_t> max_entry_reply_bytes_;
    std::string prefix_;
    redis_session redis_;
    std::vector<fill_pointer> fills_;
};
#endif

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)

namespace ruvia {
class redis_session;
}

namespace ruvia::detail {

class redis_registry;
struct request_io_metrics;
#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif

class db_registry final {
public:
    db_registry(asio::io_context& io_context, const worker_ref& worker_value,
        const db_config& default_config);
    db_registry(asio::io_context& io_context, const worker_ref& worker_value,
        std::span<const db_definition> databases, redis_registry* redis = nullptr);
#ifdef RUVIA_ENABLE_REDIS
    db_registry(asio::io_context& io_context, const worker_ref& worker_value,
        const db_config& config,
        const redis_session& redis, const db_cache_config& policy);
#endif
    ~db_registry();

    db_registry(const db_registry&) = delete;
    db_registry& operator=(const db_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] db_session get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;
    [[nodiscard]] db_session get(
        std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

    template <typename driver_type>
    using pool_owner_ptr_type = std::unique_ptr<db_pool<driver_type>>;
#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    using pool_owner_type = std::variant<pool_owner_ptr_type<mariadb_driver>, pool_owner_ptr_type<postgresql_driver>>;
#elif defined(RUVIA_ENABLE_MARIADB)
    using pool_owner_type = std::variant<pool_owner_ptr_type<mariadb_driver>>;
#else
    using pool_owner_type = std::variant<pool_owner_ptr_type<postgresql_driver>>;
#endif

private:
    void add(asio::io_context& io_context, const worker_ref& worker, db_config_storage config);

    struct entry_type final {
        explicit entry_type(pool_owner_type pool) noexcept;
        pool_owner_type pool_;
#ifdef RUVIA_ENABLE_REDIS
        std::unique_ptr<db_query_cache_state> cache_;
#endif
    };

#ifdef RUVIA_ENABLE_REDIS
    void attach_cache(std::size_t index, const worker_ref& worker_value,
        const redis_session& redis, const db_cache_config_storage& policy,
        const db_config_storage& config, std::string_view alias);
#endif
    [[nodiscard]] db_session session(
        const entry_type& entry, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

    std::vector<entry_type> entries_;
    named_capability_index alias_index_;
#ifdef RUVIA_ENABLE_REDIS
    mutable ::ruvia::operation_scope cache_scope_;
#endif
};

}  // namespace ruvia::detail

#endif

// Definitions of db_pipeline.h, included after db_pool.h by the driver
// translation units that instantiate a pipeline and by db_pool.cpp, which runs
// statements through it (run_pipelined).
//
// The io_type hooks, all static, each on a connected, leased slot of a group:
//   statement_type prepare(std::string_view sql, std::span<const db_value>)
//       Validates and prepares what queue() needs, before admission.
//   void open(state_type&, std::size_t slot)
//       Readies a connection for a new group; a failure closes it.
//   bool queue(state_type&, std::size_t slot, member_type&, statement_type&, flight_type&)
//       Queues the member's flight. A failure that leaves the protocol out of
//       step fails the group before it is thrown. True when a waiting reader
//       must look again.
//   bool advance(state_type&, std::size_t slot, member_type& reader)
//       One step without waiting: sends, reads or finishes a flight, handing
//       a finished one to complete_head(). False when it needs a wait first.
//   db_socket_readiness wanted(pool_type&, std::size_t slot)
//       What the next wait waits for; none pauses the reader for one worker
//       turn instead, after which advance() is asked again.
//   db_slot_socket::native_socket_type socket(pool_type&, std::size_t slot)
//       The connection's socket for that wait; throws when it has none.
//   void exchange(state_type&, std::size_t slot, db_socket_readiness ready)
//       Continues the protocol after a readiness wake.
//   bool reusable(state_type&, std::size_t slot) noexcept
//       Whether a dissolving group's protocol exchange is complete, with no
//       unread results. The pool checks transaction state before reassigning it.
// group_state has reset(), called when its group's flights are cleared.

namespace ruvia::detail {

// db_config's read and write timeouts bound waiting without socket progress in
// that direction; waiting while output is pending waits to write.
[[nodiscard]] inline std::optional<timeout_duration> db_io_timeout(
    const db_config_storage& config, db_socket_readiness wanted) noexcept {
    return has_db_socket_readiness(wanted, db_socket_readiness::write) ? config.write_timeout_ : config.read_timeout_;
}

// A socket error ends the connection. A cancelled wait does not: it expired,
// or it was woken to be armed again.
inline void check_db_socket_wait(const db_socket_wait_result& result, db_driver driver, std::string_view name) {
    if (result.error_ && result.error_ != asio::error::operation_aborted) {
        throw db_error(db_error::code_type::io_error, driver,
            std::string(name).append(" socket wait failed: ").append(error_code_text(result.error_)),
            result.error_.value());
    }
}

// One socket wait on a slot held exclusively by a driver operation, bounded by
// the operation deadline and optional driver I/O timeout.
template <typename driver_type>
struct db_driver_socket_wait final {
    using pool_type = db_pool<driver_type>;
    pool_type& pool_;
    typename pool_type::slot_wait wait_;
    bool throw_on_expiry_{true};

    [[nodiscard]] bool await_ready() {
        return wait_.await_ready();
    }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
        return wait_.await_suspend(handle);
    }
    [[nodiscard]] db_socket_wait_result resume_raw() {
        const auto wait_result = wait_.await_resume();
        if (wait_result.expired_ && throw_on_expiry_) {
            throw pool_.timed_out();
        }
        return wait_result;
    }
    void await_resume() {
        check_db_socket_wait(resume_raw(), driver_type::driver, driver_type::name);
    }
};

template <typename io_type>
db_pipeline_member<io_type>::db_pipeline_member(
    state_type& state, db_rows* rows, const stop_token& stop_token_value)
    : state_(state),
      rows_(rows),
      stop_token_(stop_token_value),
      cancellation_(state.cancellation_target(), cancellation_id_) {
    cancellation_.arm(stop_token_);
    // Linked once binding cannot fail; a stop delivered while binding is seen
    // through stopped(). Newest first: a stop source runs its newest callback
    // first, so stopping every statement at once (a client closing) finds each
    // one at the front.
    next_ = state_.members_;
    if (next_ != nullptr) {
        next_->previous_ = this;
    }
    state_.members_ = this;
    ++state_.statements_;
}

template <typename io_type>
db_pipeline_member<io_type>::~db_pipeline_member() {
    state_.retire(*this);
}

template <typename io_type>
db_pipeline_state<io_type>::db_pipeline_state(pool_type& pool, const worker_ref& worker_value)
    : pool_(pool),
      worker_(worker_value),
      cancellation_target_(make_worker_cancellation_target(*this, worker_value)) {}

template <typename io_type>
db_pipeline_state<io_type>::~db_pipeline_state() {
    cancellation_target_->detach(*this);
    // Started statements complete before their pool is destroyed.
    if (members_ != nullptr || parked_head_ != nullptr) {
        std::terminate();
    }
}

template <typename io_type>
typename db_pipeline_state<io_type>::group_type& db_pipeline_state<io_type>::group(std::size_t slot) const noexcept {
    return pool_.slot(slot).session_.pipeline_;
}

template <typename io_type>
db_error db_pipeline_state<io_type>::cancelled() const {
    return db_error(db_error::code_type::cancelled, driver_type::driver, "database operation cancelled");
}

template <typename io_type>
bool db_pipeline_state<io_type>::may_share(std::uint64_t arrival) const noexcept {
    const auto oldest = pool_.oldest_exclusive_arrival();
    return !oldest.has_value() || arrival < *oldest;
}

// A resume from inside the reader, a stop callback or a timer would re-enter
// the woken statement; it is deferred to the worker instead. Taking the
// continuation makes each suspension resumable exactly once.
template <typename io_type>
void db_pipeline_state<io_type>::settle(member_type& member, db_pipeline_event event) noexcept {
    member.event_ = event;
    if (auto continuation = std::exchange(member.continuation_, {})) {
        worker_.defer_resume(continuation);
    }
}

template <typename io_type>
std::optional<std::size_t> db_pipeline_state<io_type>::roomiest_group() const noexcept {
    std::optional<std::size_t> roomiest;
    std::size_t lowest_load = db_pipeline_depth;
    for (std::size_t index = 0; index < pool_.config().pool_size_per_worker_; ++index) {
        const auto& candidate = group(index);
        const auto load = candidate.pending_ + candidate.reserved_;
        if (candidate.members_ != 0 && !candidate.closing_ && !candidate.stalled() && pool_.slot(index).connected_ &&
            load < lowest_load) {
            roomiest = index;
            lowest_load = load;
        }
    }
    return roomiest;
}

template <typename io_type>
void db_pipeline_state<io_type>::join(member_type& member, std::size_t slot) noexcept {
    auto& joined = group(slot);
    ++joined.members_;
    ++joined.reserved_;
    member.slot_ = slot;
    member.phase_ = db_pipeline_phase::reserved;
}

template <typename io_type>
db_pipeline_event db_pipeline_state<io_type>::place(member_type& member) {
    if (!may_share(member.arrival_)) {
        return db_pipeline_event::none;
    }
    if (const auto slot = pool_.try_lease()) {
        member.slot_ = *slot;
        member.phase_ = db_pipeline_phase::leased;
        return db_pipeline_event::leased;
    }
    if (const auto slot = roomiest_group()) {
        join(member, *slot);
        return db_pipeline_event::joined;
    }
    return db_pipeline_event::none;
}

// Parked statements go first, so an arrival only places itself when nobody
// is parked.
template <typename io_type>
db_pipeline_event db_pipeline_state<io_type>::admit(member_type& member, const operation_deadline& admission_deadline) {
    if (pool_.closing()) {
        throw pool_.closing_error();
    }
    if (member.stopped()) {
        throw cancelled();
    }
    member.arrival_ = pool_.next_arrival();
    if (parked_head_ == nullptr) {
        if (const auto event = place(member); event != db_pipeline_event::none) {
            return event;
        }
    }
    park(member, admission_deadline);
    return db_pipeline_event::none;
}

template <typename io_type>
void db_pipeline_state<io_type>::admitted(db_pipeline_event event) const {
    switch (event) {
        case db_pipeline_event::leased:
        case db_pipeline_event::joined:
            return;
        case db_pipeline_event::timed_out:
            pool_type::slot_policy::throw_acquire_failure(pool_, worker_wait_status::timed_out);
        case db_pipeline_event::cancelled:
            throw cancelled();
        case db_pipeline_event::closing:
            throw pool_.closing_error();
        default:
            std::terminate();
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::park(member_type& member, const operation_deadline& admission_deadline) {
    if (const auto deadline_value = admission_deadline.deadline()) {
        if (*deadline_value <= std::chrono::steady_clock::now()) {
            pool_type::slot_policy::throw_acquire_failure(pool_, worker_wait_status::timed_out);
        }
        // A stopped timer queue arms nothing; the statement still leaves
        // through admission, its stop token or close().
        (void)worker_.schedule_timer(
            member.admission_timer_, *deadline_value, [this, &member](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && member.phase_ == db_pipeline_phase::parked) {
                    unpark(member);
                    member.phase_ = db_pipeline_phase::admitting;
                    settle(member, db_pipeline_event::timed_out);
                }
            });
    }
    member.previous_parked_ = parked_tail_;
    member.next_parked_ = nullptr;
    if (parked_tail_ != nullptr) {
        parked_tail_->next_parked_ = &member;
    } else {
        parked_head_ = &member;
    }
    parked_tail_ = &member;
    member.phase_ = db_pipeline_phase::parked;
}

// Leaves the phase to the caller, which knows where the statement goes next.
template <typename io_type>
void db_pipeline_state<io_type>::unpark(member_type& member) noexcept {
    member.admission_timer_.cancel();
    if (member.previous_parked_ != nullptr) {
        member.previous_parked_->next_parked_ = member.next_parked_;
    } else {
        parked_head_ = member.next_parked_;
    }
    if (member.next_parked_ != nullptr) {
        member.next_parked_->previous_parked_ = member.previous_parked_;
    } else {
        parked_tail_ = member.previous_parked_;
    }
    member.previous_parked_ = nullptr;
    member.next_parked_ = nullptr;
}

template <typename io_type>
void db_pipeline_state<io_type>::dispatch() noexcept {
    while (parked_head_ != nullptr && !pool_.closing()) {
        auto& head = *parked_head_;
        const auto event = place(head);
        if (event == db_pipeline_event::none) {
            return;
        }
        unpark(head);
        settle(head, event);
    }
}

// A released connection goes to the oldest parked statement unless an older
// caller waits for it in the lease scheduler. Room would have admitted that
// statement already.
template <typename io_type>
bool db_pipeline_state<io_type>::take_released_slot(std::size_t slot) noexcept {
    if (parked_head_ == nullptr || pool_.closing() || !may_share(parked_head_->arrival_)) {
        return false;
    }
    auto& head = *parked_head_;
    unpark(head);
    head.slot_ = slot;
    head.phase_ = db_pipeline_phase::leased;
    settle(head, db_pipeline_event::leased);
    return true;
}

template <typename io_type>
void db_pipeline_state<io_type>::exclusive_waiter_left() noexcept {
    dispatch();
}

template <typename io_type>
void db_pipeline_state<io_type>::close() noexcept {
    while (parked_head_ != nullptr) {
        auto& head = *parked_head_;
        unpark(head);
        head.phase_ = db_pipeline_phase::admitting;
        settle(head, db_pipeline_event::closing);
    }
}

// Every lease a statement founds a group on passes here first.
template <typename io_type>
bool db_pipeline_state<io_type>::must_connect(member_type& member) {
    pool_.reset_abort(member.slot_);
    if (member.stopped()) {
        throw cancelled();
    }
    pool_.retire_dead_idle_connection(member.slot_);
    member.connecting_ = !pool_.slot(member.slot_).connected_;
    return member.connecting_;
}

// A stop while connecting closed the slot, which ended the connect.
template <typename io_type>
void db_pipeline_state<io_type>::connected(member_type& member, const std::exception_ptr& failure) {
    member.connecting_ = false;
    if (failure == nullptr) {
        return;
    }
    if (member.stopped()) {
        throw cancelled();
    }
    std::rethrow_exception(failure);
}

template <typename io_type>
void db_pipeline_state<io_type>::found_group(member_type& member) {
    io_type::open(*this, member.slot_);
    auto& founded = group(member.slot_);
    founded.members_ = 1;
    founded.reserved_ = 1;
    member.phase_ = db_pipeline_phase::reserved;
    dispatch();
}

template <typename io_type>
void db_pipeline_state<io_type>::require_usable(std::size_t slot) const {
    const auto& checked = group(slot);
    if (checked.closing_) {
        std::rethrow_exception(checked.failure_);
    }
    pool_.throw_if_aborted(slot);
    if (!pool_.slot(slot).connected_) {
        throw pool_.closing_error();
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::send(member_type& member, statement_type& statement) {
    if (member.stopped()) {
        throw cancelled();
    }
    if (member.deadline_.has_value() && *member.deadline_ <= std::chrono::steady_clock::now()) {
        throw pool_.timed_out();
    }
    const auto index = member.slot_;
    auto& slot = pool_.slot(index);
    auto& sending = slot.session_.pipeline_;
    require_usable(index);
    typename io_type::flight_type flight{};
    const bool wake = io_type::queue(*this, index, member, statement, flight);
    auto& record = sending.record(sending.pending_);
    record = {&member, std::move(flight), member.rows_ != nullptr};
    ++sending.pending_;
    --sending.reserved_;
    member.record_ = &record;
    member.phase_ = db_pipeline_phase::pending;
    if (sending.reader_ == nullptr) {
        sending.reader_ = &member;
        return;
    }
    if (!slot.wait_active_) {
        return;
    }
    // The waiting reader looks again when the driver asks for it, or when
    // this statement's deadline comes first.
    const bool earlier = member.deadline_.has_value() &&
                         (!sending.wait_deadline_.has_value() || *member.deadline_ < *sending.wait_deadline_);
    if (wake || earlier) {
        slot.wait_socket_.cancel();
    }
}

template <typename io_type>
bool db_pipeline_state<io_type>::reads(const member_type& member) const noexcept {
    return group(member.slot_).reader_ == &member;
}

template <typename io_type>
bool db_pipeline_state<io_type>::retries(member_type& member, db_pipeline_event event) const {
    switch (event) {
        case db_pipeline_event::completed:
            if (member.failure_ != nullptr) {
                std::rethrow_exception(member.failure_);
            }
            return false;
        case db_pipeline_event::retry:
            return true;
        case db_pipeline_event::failed:
            std::rethrow_exception(member.failure_);
        case db_pipeline_event::timed_out:
            throw pool_.timed_out();
        case db_pipeline_event::cancelled:
            throw cancelled();
        default:
            std::terminate();
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::complete_head(std::size_t slot, member_type& reader, db_pipeline_outcome outcome) {
    auto& completing = group(slot);
    auto& record = completing.records_[completing.head_];
    completing.stretch_start_.reset();
    auto* member = record.member_;
    record = {};
    completing.head_ = (completing.head_ + 1) % db_pipeline_depth;
    --completing.pending_;
    if (member == nullptr) {
        dispatch();
        return;
    }
    member->record_ = nullptr;
    auto event = db_pipeline_event::completed;
    if (outcome.retry_) {
        // The retry keeps the room it frees, and goes to the tail.
        member->may_retry_ = false;
        member->phase_ = db_pipeline_phase::reserved;
        ++completing.reserved_;
        event = db_pipeline_event::retry;
    } else {
        member->phase_ = db_pipeline_phase::settled;
        member->affected_rows_ = outcome.affected_rows_;
        member->insert_id_ = outcome.insert_id_;
        member->failure_ = std::move(outcome.failure_);
    }
    if (member == &reader) {
        member->event_ = event;
    } else {
        settle(*member, event);
    }
    if (event != db_pipeline_event::retry) {
        dispatch();
    }
}

template <typename io_type>
bool db_pipeline_state<io_type>::read_buffered(member_type& reader) {
    const auto slot = reader.slot_;
    require_usable(slot);
    while (reader.event_ == db_pipeline_event::none) {
        if (!io_type::advance(*this, slot, reader)) {
            return false;
        }
    }
    // Results already available reach their members before the role passes
    // on; a failure among them no longer concerns the reader's own outcome.
    auto& reading = group(slot);
    try {
        while (reading.pending_ != 0 && !reading.closing_ && io_type::advance(*this, slot, reader)) {
        }
    } catch (...) {
        fail_group(slot, std::current_exception());
    }
    hand_off(reader);
    return true;
}

template <typename io_type>
void db_pipeline_state<io_type>::expire_followers(
    member_type& reader, std::chrono::steady_clock::time_point now) noexcept {
    auto& reading = group(reader.slot_);
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        auto* member = reading.record(offset).member_;
        if (member != nullptr && member != &reader && member->event_ == db_pipeline_event::none &&
            member->deadline_.has_value() && *member->deadline_ <= now) {
            abandon(*member);
            settle(*member, db_pipeline_event::timed_out);
        }
    }
}

template <typename io_type>
std::optional<std::chrono::steady_clock::time_point> db_pipeline_state<io_type>::earliest_deadline(
    std::size_t slot) const noexcept {
    const auto& reading = group(slot);
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        const auto* member = reading.record(offset).member_;
        if (member != nullptr && member->event_ == db_pipeline_event::none && member->deadline_.has_value() &&
            (!earliest.has_value() || *member->deadline_ < *earliest)) {
            earliest = member->deadline_;
        }
    }
    return earliest;
}

template <typename io_type>
void db_pipeline_state<io_type>::hand_off(member_type& reader) noexcept {
    auto& reading = group(reader.slot_);
    if (reading.reader_ != &reader) {
        return;
    }
    reading.reader_ = nullptr;
    for (std::size_t offset = 0; offset < reading.pending_; ++offset) {
        auto* member = reading.record(offset).member_;
        if (member != nullptr && member != &reader && member->event_ == db_pipeline_event::none) {
            reading.reader_ = member;
            settle(*member, db_pipeline_event::reader);
            return;
        }
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::abandon(member_type& member) noexcept {
    if (member.record_ != nullptr) {
        member.record_->member_ = nullptr;
        member.record_ = nullptr;
    }
    member.phase_ = db_pipeline_phase::settled;
}

template <typename io_type>
void db_pipeline_state<io_type>::clear_flights(group_type& cleared) noexcept {
    for (std::size_t offset = 0; offset < cleared.pending_; ++offset) {
        cleared.record(offset) = {};
    }
    cleared.io_.reset();
    cleared.wait_deadline_.reset();
    cleared.stretch_start_.reset();
    cleared.reader_ = nullptr;
    cleared.head_ = 0;
    cleared.pending_ = 0;
}

// The first failure is the group's; a designated reader that has not resumed
// yet learns it instead of its role.
template <typename io_type>
void db_pipeline_state<io_type>::fail_group(std::size_t slot, const std::exception_ptr& failure) noexcept {
    auto& failing = group(slot);
    if (!failing.closing_) {
        failing.closing_ = true;
        failing.failure_ = failure;
    }
    for (std::size_t offset = 0; offset < failing.pending_; ++offset) {
        auto* member = failing.record(offset).member_;
        if (member == nullptr) {
            continue;
        }
        member->record_ = nullptr;
        member->phase_ = db_pipeline_phase::settled;
        if (member->event_ == db_pipeline_event::none || member->event_ == db_pipeline_event::reader) {
            member->failure_ = failing.failure_;
            settle(*member, db_pipeline_event::failed);
        }
    }
    clear_flights(failing);
    pool_.close_slot(slot);
}

template <typename io_type>
void db_pipeline_state<io_type>::unlink(member_type& member) noexcept {
    if (member.previous_ != nullptr) {
        member.previous_->next_ = member.next_;
    } else {
        members_ = member.next_;
    }
    if (member.next_ != nullptr) {
        member.next_->previous_ = member.previous_;
    }
    member.previous_ = nullptr;
    member.next_ = nullptr;
}

// A cancelled statement leaves the search, so stopping every statement at once
// costs one step each.
template <typename io_type>
void db_pipeline_state<io_type>::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0) {
        return;
    }
    auto* member = members_;
    while (member != nullptr && member->cancellation_id_ != cancellation_id) {
        member = member->next_;
    }
    if (member == nullptr) {
        return;
    }
    unlink(*member);
    member->cancel_requested_ = true;
    switch (member->phase_) {
        case db_pipeline_phase::parked:
            unpark(*member);
            member->phase_ = db_pipeline_phase::admitting;
            settle(*member, db_pipeline_event::cancelled);
            return;
        case db_pipeline_phase::leased:
            if (member->connecting_) {
                pool_.close_slot(member->slot_);
            }
            return;
        case db_pipeline_phase::pending: {
            auto& slot = pool_.slot(member->slot_);
            if (slot.session_.pipeline_.reader_ == member) {
                // The reader leaves its wait and passes the role on.
                if (slot.wait_active_) {
                    slot.wait_socket_.cancel();
                }
            } else if (member->event_ == db_pipeline_event::none) {
                abandon(*member);
                settle(*member, db_pipeline_event::cancelled);
            }
            return;
        }
        // Running, or about to: it sees stopped() at its next step.
        case db_pipeline_phase::admitting:
        case db_pipeline_phase::reserved:
        case db_pipeline_phase::settled:
            return;
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::retire(member_type& member) noexcept {
    --statements_;
    if (!member.cancel_requested_) {
        unlink(member);
    }
    switch (member.phase_) {
        case db_pipeline_phase::admitting:
            break;
        case db_pipeline_phase::parked:
            unpark(member);
            break;
        case db_pipeline_phase::leased:
            member.phase_ = db_pipeline_phase::admitting;
            pool_.release_slot(member.slot_);
            break;
        case db_pipeline_phase::reserved:
            --group(member.slot_).reserved_;
            leave(member);
            break;
        case db_pipeline_phase::pending:
            hand_off(member);
            abandon(member);
            leave(member);
            break;
        case db_pipeline_phase::settled:
            hand_off(member);
            leave(member);
            break;
    }
}

template <typename io_type>
void db_pipeline_state<io_type>::leave(member_type& member) noexcept {
    member.phase_ = db_pipeline_phase::admitting;
    if (--group(member.slot_).members_ != 0) {
        dispatch();
        return;
    }
    dissolve(member.slot_);
}

// The last member gives the connection back. A failed or incomplete protocol
// exchange closes it here; release_slot() checks transaction state before
// either scheduler hands it to another caller.
template <typename io_type>
void db_pipeline_state<io_type>::dissolve(std::size_t slot) noexcept {
    auto& connection_slot = pool_.slot(slot);
    auto& dissolving = connection_slot.session_.pipeline_;
    if (dissolving.closing_ || !connection_slot.connected_ || !io_type::reusable(*this, slot)) {
        pool_.close_slot(slot);
    }
    clear_flights(dissolving);
    dissolving.failure_ = nullptr;
    dissolving.reserved_ = 0;
    dissolving.closing_ = false;
    pool_.release_slot(slot);
}

// Reads the group's results for one statement while it holds the reader role,
// from taking the role to the statement's outcome. A stop or the statement's
// deadline abandons the statement, which passes the role on as it leaves; any
// other failure while reading fails the connection for every member.
template <typename io_type>
class db_pipeline_reader final {
public:
    using clock_type = std::chrono::steady_clock;
    using state_type = db_pipeline_state<io_type>;
    using pool_type = typename state_type::pool_type;
    using member_type = typename state_type::member_type;

    db_pipeline_reader(pool_type& pool, member_type& member) noexcept
        : pool_(pool),
          member_(member) {}

    db_pipeline_reader(const db_pipeline_reader&) = delete;
    db_pipeline_reader& operator=(const db_pipeline_reader&) = delete;

    // One socket wait; a failure of the wait itself fails the connection.
    // Without a socket wait it pauses for one worker turn instead: the driver
    // has queued nothing to wait for yet, and statements arriving in this
    // turn should go out with what it just sent.
    class socket_wait final {
    public:
        explicit socket_wait(db_pipeline_reader& reader) noexcept
            : reader_(reader) {}
        socket_wait(db_pipeline_reader& reader, std::size_t slot, db_slot_socket::native_socket_type native,
            db_socket_readiness wanted, std::optional<clock_type::time_point> deadline) noexcept
            : reader_(reader) {
            wait_.emplace(reader.pool_, slot, db_deadline_kind::socket, native, wanted, deadline, reader.unbounded_);
        }
        socket_wait(const socket_wait&) = delete;
        socket_wait& operator=(const socket_wait&) = delete;

        [[nodiscard]] bool await_ready() {
            if (!wait_.has_value()) {
                return false;
            }
            try {
                return wait_->await_ready();
            } catch (...) {
                reader_.fail(std::current_exception());
            }
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
            if (!wait_.has_value()) {
                reader_.pool_.state().worker().defer_resume(handle);
                return true;
            }
            return wait_->await_suspend(handle);
        }
        void await_resume() {
            if (!wait_.has_value()) {
                return;
            }
            try {
                reader_.waited(wait_->await_resume());
            } catch (...) {
                reader_.fail(std::current_exception());
            }
        }

    private:
        db_pipeline_reader& reader_;
        std::optional<typename pool_type::slot_wait> wait_;
    };

    // Reads what is available; true once the statement has its outcome.
    [[nodiscard]] bool read() {
        auto& state = pool_.state();
        if (member_.stopped()) {
            throw state.cancelled();
        }
        const auto now = clock_type::now();
        if (member_.deadline_.has_value() && *member_.deadline_ <= now) {
            throw pool_.timed_out();
        }
        state.expire_followers(member_, now);
        try {
            return state.read_buffered(member_);
        } catch (...) {
            fail(std::current_exception());
        }
    }

    // Waits for what the driver needs until the earliest deadline of a
    // pending member or the I/O timeout of the direction waited for, counted
    // from the connection's last progress.
    [[nodiscard]] socket_wait wait() {
        try {
            const auto slot = member_.slot_;
            auto& group = pool_.state().group(slot);
            const auto wanted = io_type::wanted(pool_, slot);
            if (wanted == db_socket_readiness::none) {
                return socket_wait(*this);
            }
            const auto now = clock_type::now();
            if (!group.stretch_start_.has_value()) {
                group.stretch_start_ = now;
            }
            io_deadline_.reset();
            if (const auto io_timeout = db_io_timeout(pool_.config(), wanted)) {
                io_deadline_ = saturating_deadline(
                    *group.stretch_start_, io_timeout->value());
            }
            auto until = pool_.state().earliest_deadline(slot);
            if (io_deadline_.has_value() && (!until.has_value() || *io_deadline_ < *until)) {
                until = io_deadline_;
            }
            group.wait_deadline_ = until;
            return socket_wait(*this, slot, io_type::socket(pool_, slot), wanted, until);
        } catch (...) {
            fail(std::current_exception());
        }
    }

private:
    // A cancelled wait (a stop, a member joining with an earlier deadline or
    // a driver asking to look again) returns to read(), which looks again.
    void waited(const db_socket_wait_result& result) {
        auto& state = pool_.state();
        auto& group = state.group(member_.slot_);
        group.wait_deadline_.reset();
        check_db_socket_wait(result, io_type::driver_type::driver, io_type::driver_type::name);
        if (result.ready_ != db_socket_readiness::none) {
            group.stretch_start_.reset();
            io_type::exchange(state, member_.slot_, result.ready_);
        } else if (result.expired_ && io_deadline_.has_value() && clock_type::now() >= *io_deadline_) {
            throw pool_.timed_out();
        }
    }

    [[noreturn]] void fail(const std::exception_ptr& failure) {
        auto& state = pool_.state();
        state.fail_group(member_.slot_, failure);
        if (member_.stopped()) {
            throw state.cancelled();
        }
        std::rethrow_exception(member_.failure_ != nullptr ? member_.failure_ : failure);
    }

    pool_type& pool_;
    member_type& member_;
    const operation_deadline unbounded_{std::nullopt};
    std::optional<clock_type::time_point> io_deadline_{};
};

// One buffered statement outside a transaction, as a member of a pipeline
// group: admission, a connection of its own when it founds a group, its
// flight, then its outcome -- read by itself while it holds the reader role,
// else handed over by the reader. Every wait is an awaiter on this frame, so a
// statement costs no coroutine frame beyond this one unless it connects. The
// member's destructor gives back whatever it holds on every exit path.
template <typename io_type, typename result_type>
task<result_type> run_pipelined(typename db_pipeline_state<io_type>::pool_type& pool, std::string sql,
    std::vector<db_value> params, operation_options options,
    request_io_metrics* io_metrics) {
    using member_type = db_pipeline_member<io_type>;
    constexpr bool returns_rows = std::is_same_v<result_type, db_rows>;
    scoped_request_io_timer timer(io_metrics, request_io_kind::db);
    auto statement = io_type::prepare(sql, std::span<const db_value>(params));
    const operation_deadline timeout(options.timeout_);
    auto& pipeline = pool.state();
    auto output = db_result_access::make_result();
    member_type member(pipeline, returns_rows ? &output : nullptr, options.stop_token_);

    // Waiting for admission is bounded like acquiring a lease.
    if (pipeline.admit(member, timeout.constrained_by(pool.config().acquire_timeout_)) == db_pipeline_event::none) {
        pipeline.admitted(co_await typename member_type::wait{member});
    }
    if (member.phase_ == db_pipeline_phase::leased) {
        if (pipeline.must_connect(member)) {
            std::exception_ptr failure;
            try {
                co_await pool.connect_slot(member.slot_, timeout);
            } catch (...) {
                failure = std::current_exception();
            }
            pipeline.connected(member, failure);
        }
        pipeline.found_group(member);
    }

    member.deadline_ = timeout.constrained_by(pool.config().query_timeout_).deadline();
    for (;;) {
        pipeline.send(member, statement);
        auto event = db_pipeline_event::none;
        while (event == db_pipeline_event::none) {
            if (pipeline.reads(member)) {
                db_pipeline_reader<io_type> reader(pool, member);
                while (!reader.read()) {
                    co_await reader.wait();
                }
                event = member.take_event();
            } else if ((event = co_await typename member_type::wait{member}) == db_pipeline_event::reader) {
                event = db_pipeline_event::none;
            }
        }
        if (!pipeline.retries(member, event)) {
            break;
        }
    }
    if constexpr (returns_rows) {
        co_return output;
    } else {
        co_return db_result_access::make_exec_result(member.affected_rows_, member.insert_id_);
    }
}

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_REDIS)

struct redisReader;
struct redisReadTask;
struct redisReplyObjectFunctions;

#ifdef RUVIA_ENABLE_DATABASE
#endif
#ifdef RUVIA_ENABLE_DATABASE
#endif

namespace ruvia::detail {

// Serializes a command into RESP multi-bulk form, appending directly to the
// reused connection write buffer. Both overloads avoid the extra heap
// allocation + copy that hiredis' redisFormatCommandArgv would impose; the
// pmr::string overload also lets owned-argument paths skip building an
// intermediate string_view vector.
[[nodiscard]] std::size_t resp_header_serialized_size(std::size_t arg_count);
[[nodiscard]] std::size_t resp_bulk_serialized_size(std::size_t arg_bytes);
void add_resp_serialized_size(std::size_t& current, std::size_t amount);
void append_resp_header(std::string& output, std::size_t arg_count);
void append_resp_bulk(std::string& output, std::string_view arg);
void serialize_resp_command(std::string& output, std::span<const std::string_view> args);
void serialize_resp_command(std::string& output, std::span<const std::string> args);
void serialize_resp_command(
    std::string& output, std::string_view first, std::span<const std::string_view> rest);
void append_resp_command(std::string& output, std::span<const std::string_view> args);
void append_resp_command(std::string& output, std::span<const std::string> args);
void append_resp_command(
    std::string& output, std::string_view first, std::span<const std::string_view> rest);
// Throws std::invalid_argument when args is empty, or std::length_error when
// the complete RESP command size cannot be represented by std::size_t.
[[nodiscard]] std::size_t resp_command_serialized_size(std::span<const std::string_view> args);
[[nodiscard]] std::size_t resp_command_serialized_size(std::span<const std::string> args);
[[nodiscard]] std::size_t resp_command_serialized_size(
    std::string_view first, std::span<const std::string_view> rest);
[[nodiscard]] const char* hiredis_reader_error(const redisReader& reader_value) noexcept;

// The only RESP reply construction path: hiredis' reader calls these object
// functions while parsing. The root lives in the connection's builder;
// payload and nested storage use the resource of the read in progress. One
// builder serves one reader for its whole life and enforces the configured
// reply budget: total array elements (one per three reply bytes) and array
// nesting depth. A rejected or failed node makes the reader fail; the reader
// then frees the partial tree through the builder.
class redis_reply_builder final {
public:
    redis_reply_builder(std::optional<std::size_t> max_reply_bytes, std::size_t max_depth) noexcept;

    redis_reply_builder(const redis_reply_builder&) = delete;
    redis_reply_builder& operator=(const redis_reply_builder&) = delete;

    // Creates a reader whose replies this builder constructs; the builder must
    // outlive it. Throws redis_error when hiredis cannot allocate the reader.
    [[nodiscard]] redisReader* create_reader();

    // Starts reading one reply: its owned payload uses `resource` and the
    // per-reply budget is restored.
    void begin() noexcept;

    // After the reader failed, reports a rejection raised by this builder:
    // the budget's redis_error or the exception a node construction threw.
    // Returns when the failure was the reader's own protocol error.
    void throw_if_rejected();
    void clear() noexcept;
    // Takes ownership of a complete reply returned by redisReaderGetReply.
    [[nodiscard]] static redis_value take(void* reply) noexcept;

private:
    enum class rejection_type : std::uint8_t { none,
        elements,
        depth,
        failed };

    struct root_slot final {
        root_slot(redis_reply_builder& owner, redis_value&& value) noexcept
            : owner_(&owner),
              value_(std::move(value)) {}
        redis_reply_builder* owner_;
        redis_value value_;
    };

    template <typename factory_type>
    void* place(const redisReadTask* task_value, factory_type&& factory) noexcept;

    static void* create_string(const redisReadTask* task_value, char* data, std::size_t size) noexcept;
    static void* create_array(const redisReadTask* task_value, std::size_t elements) noexcept;
    static void* create_integer(const redisReadTask* task_value, long long value) noexcept;
    static void* create_double(const redisReadTask* task_value, double value, char* data, std::size_t size) noexcept;
    static void* create_nil(const redisReadTask* task_value) noexcept;
    static void* create_bool(const redisReadTask* task_value, int value) noexcept;
    static void free_object(void* reply) noexcept;

    static redisReplyObjectFunctions functions_;

    // One root may be under construction until hiredis completes or rejects it.
    std::optional<root_slot> root_;
    redisReader* reader_{nullptr};
    std::optional<std::size_t> max_reply_bytes_;
    std::size_t maximum_elements_;
    std::size_t remaining_elements_;
    std::size_t max_depth_;
    rejection_type rejection_{rejection_type::none};
    std::exception_ptr failure_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::size_t redis_int_scratch_size = 20;
inline constexpr std::size_t redis_score_scratch_size = 64;

[[nodiscard]] inline std::string_view format_redis_int(
    std::span<char, redis_int_scratch_size> scratch, std::int64_t value) noexcept {
    const auto end = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value).ptr;
    return {scratch.data(), static_cast<std::size_t>(end - scratch.data())};
}

// Redis accepts "+inf"/"-inf" as sorted-set scores and range bounds, and
// replies them back, so the client round-trips infinities. NaN has no Redis
// spelling and is rejected.
[[nodiscard]] inline std::string_view format_redis_score(
    std::span<char, redis_score_scratch_size> scratch, double value) {
    if (std::isinf(value)) {
        return value > 0 ? "+inf" : "-inf";
    }
    if (!std::isfinite(value)) {
        throw std::invalid_argument("redis sorted set score must not be NaN");
    }
    const auto [ptr, ec] = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("redis sorted set score is invalid");
    }
    return {scratch.data(), static_cast<std::size_t>(ptr - scratch.data())};
}

// Encodes the arguments `enumerate_arguments(emit)` produces, in order, as one
// RESP command. The enumeration runs twice, once to size the request and once
// to append it, so the argument layout is written in exactly one place.
template <typename enumerate_type>
[[nodiscard]] redis_owned_command encode_redis_command(
    const enumerate_type& enumerate_arguments) {
    std::size_t arg_count = 0;
    std::size_t bulk_bytes = 0;
    enumerate_arguments([&](std::string_view arg) {
        ++arg_count;
        add_resp_serialized_size(bulk_bytes, resp_bulk_serialized_size(arg.size()));
    });
    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, bulk_bytes);
    redis_owned_command command{std::string()};
    command.wire_.reserve(size);
    append_resp_header(command.wire_, arg_count);
    enumerate_arguments([&](std::string_view arg) { append_resp_bulk(command.wire_, arg); });
    return command;
}

[[nodiscard]] redis_owned_command make_owned_redis_command(
    std::initializer_list<std::string_view> args);

[[nodiscard]] bool redis_set_returns_previous(redis_set_previous_value_policy policy);

[[nodiscard]] redis_owned_command redis_command_with_keys(std::string_view command,
    std::span<const std::string_view> keys);
[[nodiscard]] redis_owned_command redis_mset_args(
    std::span<const std::pair<std::string_view, std::string_view>> items);
[[nodiscard]] redis_owned_command redis_set_args(std::string_view key,
    std::string_view value, const redis_set_options& options);
[[nodiscard]] redis_owned_command redis_hset_fields_args(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value);
[[nodiscard]] redis_owned_command redis_command_with_key_fields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields_value);
[[nodiscard]] redis_owned_command redis_eval_args(std::string_view command,
    std::string_view script, std::span<const std::string_view> keys,
    std::span<const std::string_view> argv);
[[nodiscard]] redis_owned_command redis_blocking_pop_args(std::string_view command,
    std::span<const std::string_view> keys, redis_block_wait wait);
[[nodiscard]] redis_owned_command redis_x_read_group_args(std::string_view group,
    std::string_view consumer, std::span<const redis_stream_read_view> streams,
    const redis_x_read_group_options& options);

}  // namespace ruvia::detail

namespace ruvia::detail {

struct redis_types_access final {
    [[nodiscard]] static redis_set_result set_result(bool applied,
        std::optional<std::string> previous = std::nullopt) {
        return redis_set_result(applied, std::move(previous));
    }

    [[nodiscard]] static constexpr redis_scan_cursor scan_cursor(std::uint64_t value) noexcept {
        return redis_scan_cursor(value);
    }

    [[nodiscard]] static constexpr std::uint64_t cursor_value(redis_scan_cursor cursor_value) noexcept {
        return cursor_value.value_;
    }

    [[nodiscard]] static redis_key_value key_value(
        std::string&& key, std::string&& value) noexcept {
        return redis_key_value(std::move(key), std::move(value));
    }

    [[nodiscard]] static redis_scored_value scored_value(
        std::string&& value, double score) noexcept {
        return redis_scored_value(std::move(value), score);
    }

    [[nodiscard]] static redis_scan_result scan_result() {
        return redis_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<std::string>& values(
        redis_scan_result& result_value) noexcept {
        return result_value.values_;
    }

    [[nodiscard]] static redis_hash_scan_result hash_scan_result() {
        return redis_hash_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_hash_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<redis_key_value>& entries(
        redis_hash_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_z_scan_result z_scan_result() {
        return redis_z_scan_result{};
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_z_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::vector<redis_scored_value>& entries(
        redis_z_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_stream_entry stream_entry(
        std::string&& id) noexcept {
        return redis_stream_entry(std::move(id));
    }

    [[nodiscard]] static std::vector<redis_key_value>& fields(redis_stream_entry& entry_value) noexcept {
        return entry_value.fields_;
    }

    [[nodiscard]] static redis_stream_read_result stream_read_result(
        std::string&& stream) noexcept {
        return redis_stream_read_result(std::move(stream));
    }

    [[nodiscard]] static std::vector<redis_stream_entry>& entries(
        redis_stream_read_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_x_read_group_result xread_group_result() {
        return redis_x_read_group_result{};
    }

    [[nodiscard]] static std::vector<redis_stream_read_result>& streams(
        redis_x_read_group_result& result_value) noexcept {
        return result_value.streams_;
    }

    [[nodiscard]] static constexpr redis_ttl ttl(redis_ttl_state state_value,
        std::optional<std::chrono::milliseconds> remaining = std::nullopt) noexcept {
        return redis_ttl(state_value, remaining);
    }

    [[nodiscard]] static redis_pubsub_event pubsub_confirmation(redis_pubsub_confirmation_kind kind,
        std::optional<std::string_view> name, std::int64_t subscription_count) noexcept {
        return redis_pubsub_event(redis_pubsub_confirmation(kind, name, subscription_count));
    }
    [[nodiscard]] static redis_pubsub_event pubsub_message(std::string_view channel,
        std::optional<std::string_view> pattern, std::string_view payload) noexcept {
        return redis_pubsub_event(redis_pubsub_message(channel, pattern, payload));
    }

    [[nodiscard]] static redis_value null_value() {
        return redis_value::null_value();
    }

    [[nodiscard]] static redis_value string_value(
        std::string_view value) {
        return redis_value::string_value(value);
    }

    [[nodiscard]] static redis_value error_value(
        std::string_view value) {
        return redis_value::error_value(value);
    }

    [[nodiscard]] static redis_value integer_value(
        std::int64_t value) {
        return redis_value::integer_value(value);
    }

    [[nodiscard]] static redis_value array_value(std::size_t capacity) {
        return redis_value::array_value(capacity);
    }

    [[nodiscard]] static std::vector<redis_value>& elements(redis_value& value) noexcept {
        return value.array_;
    }

    [[nodiscard]] static std::string& text(redis_value& value) noexcept {
        return value.string_;
    }

    // The resource every allocation of `value` comes from.
};

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

// Consumes reply storage when its resource matches; otherwise owns a copy in
// the destination resource. Never returns a borrow into the discarded reply.
[[nodiscard]] std::string take_redis_string(redis_value&& value);
[[nodiscard]] bool redis_value_integer_bool(const redis_value& value);
[[nodiscard]] std::span<redis_value> redis_value_array(redis_value& value);
void throw_if_redis_transaction_reply_error(const redis_value& value, std::size_t index);

// Reply decoders. Those taking only (redis_value&&, memory_resource*) are
// mappers for map_redis_command; results own storage in `resource`.
[[nodiscard]] std::optional<std::string> parse_redis_optional_string(
    redis_value&& value);
[[nodiscard]] std::int64_t parse_redis_integer(redis_value&& value);
[[nodiscard]] bool parse_redis_integer_bool(redis_value&& value);
// Accept only the OK / PONG status reply, compared case-insensitively.
void parse_redis_ok(redis_value&& value);
void parse_redis_pong(redis_value&& value);
[[nodiscard]] std::vector<bool> parse_redis_bool_array(redis_value&& value);
// Every element must be a string; a nil element is a protocol error.
[[nodiscard]] std::vector<std::string> parse_redis_string_array(
    redis_value&& value);
[[nodiscard]] std::vector<std::optional<std::string>> parse_redis_optional_string_array(
    redis_value&& value);
[[nodiscard]] std::vector<redis_scored_value> parse_redis_scored_array(
    redis_value&& value);
[[nodiscard]] redis_scan_result parse_redis_scan_result(redis_value&& value);
[[nodiscard]] redis_hash_scan_result parse_redis_hash_scan_result(
    redis_value&& value);
[[nodiscard]] redis_z_scan_result parse_redis_z_scan_result(redis_value&& value);
[[nodiscard]] std::optional<redis_key_value> parse_redis_blocking_pop_reply(
    redis_value&& value);
[[nodiscard]] std::optional<redis_x_read_group_result> parse_redis_x_read_group_reply(
    redis_value&& value);

[[nodiscard]] double parse_redis_double(std::string_view value, std::string_view context_value);
[[nodiscard]] std::optional<double> parse_redis_optional_double(redis_value&& value, std::string_view context_value);
[[nodiscard]] std::vector<redis_key_value> parse_redis_key_value_array(
    redis_value&& value, std::string_view context_value);
[[nodiscard]] redis_set_result parse_redis_set_reply(
    redis_value&& value, const redis_set_options& options);
// TTL/PTTL: -2 is a missing key, -1 a key without expiry.
[[nodiscard]] redis_ttl parse_redis_ttl(redis_value&& value, bool seconds_precision);
// A pushed reply of a subscribed connection. Views borrow `value`.
[[nodiscard]] redis_pubsub_event parse_redis_pubsub_event(const redis_value& value);
// The reply to an argument-less PING on a subscription connection.
[[nodiscard]] bool is_redis_subscription_pong(const redis_value& value);

}  // namespace ruvia::detail

namespace ruvia::detail {

inline void validate_redis_config(const redis_config& config) {
    validate_client_tls_config(config.tls_);
    validate_client_origin_host(config.host_, "redis host must not be empty", "redis host is invalid");
    detail::ensure_non_zero_port(config.port_.value(), "redis port must not be zero");
    detail::ensure_positive_size(config.pool_size_per_worker_, "redis pool size must be greater than zero");
    detail::ensure_positive_size(
        config.blocking_pool_size_per_worker_, "redis blocking pool size must be greater than zero");
    detail::ensure_positive_optional_durations("configured redis timeouts must be greater than zero",
        config.connect_timeout_, config.command_timeout_, config.acquire_timeout_);
    detail::ensure_positive_size(config.max_array_depth_, "redis max array depth must be greater than zero");
    detail::ensure_positive_optional_size(
        config.max_reply_bytes_, "configured redis reply byte limit must be greater than zero");
    validate_client_tcp_config(config.tcp_);
    detail::ensure_positive_optional_duration(
        config.subscription_ping_interval_, "redis subscription ping interval must be greater than zero");
}

// PMR-owned runtime form of the ordinary public redis_config value.
struct redis_config_storage final {
    redis_config_storage(const redis_config& source_value)
        : redis_config_storage(
              validated_config_tag_type{}, validate(source_value)) {}

    redis_config_storage(const redis_config_storage& source_value)
        : redis_config_storage(validated_config_tag_type{}, source_value) {}

    std::string host_;
    std::uint16_t port_;
    std::string username_;
    std::string password_;
    client_tls_config_storage tls_;
    std::uint32_t database_;
    std::size_t pool_size_per_worker_;
    std::size_t blocking_pool_size_per_worker_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> command_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<std::size_t> max_reply_bytes_;
    std::size_t max_array_depth_;
    client_tcp_config tcp_;
    std::optional<timeout_duration> subscription_ping_interval_;

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static const redis_config& validate(const redis_config& source_value) {
        validate_redis_config(source_value);
        return source_value;
    }

    [[nodiscard]] static std::uint16_t resolved_port(const redis_config& source) noexcept {
        return source.port_.value();
    }
    [[nodiscard]] static std::uint16_t resolved_port(const redis_config_storage& source) noexcept {
        return source.port_;
    }

    // Public configuration parses the TLS files every pool's context
    // installs; allocator-rebound copies share them.
    template <typename source_type>
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const source_type& source_value) {
        if constexpr (std::same_as<source_type, redis_config>) {
            return {source_value.tls_, client_tls_files::parse};
        } else {
            return {source_value.tls_};
        }
    }

    // Public configuration validation and allocator-rebound copies enter the
    // same owning construction path; no intermediate string copies or views.
    template <typename source_type>
        requires(std::same_as<source_type, redis_config> ||
                    std::same_as<source_type, redis_config_storage>)
    redis_config_storage(
        validated_config_tag_type, const source_type& source_value)
        : host_(source_value.host_),
          port_(resolved_port(source_value)),
          username_(source_value.username_),
          password_(source_value.password_),
          tls_(tls_storage(source_value)),
          database_(source_value.database_),
          pool_size_per_worker_(source_value.pool_size_per_worker_),
          blocking_pool_size_per_worker_(source_value.blocking_pool_size_per_worker_),
          connect_timeout_(source_value.connect_timeout_),
          command_timeout_(source_value.command_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          max_reply_bytes_(source_value.max_reply_bytes_),
          max_array_depth_(source_value.max_array_depth_),
          tcp_(source_value.tcp_),
          subscription_ping_interval_(source_value.subscription_ping_interval_) {}
};

using redis_definition_type = named_capability_definition<redis_config_storage>;

}  // namespace ruvia::detail

struct redisReader;

namespace ruvia::detail {

struct redis_reader_deleter {
    void operator()(redisReader* reader) const noexcept;
};

class redis_reply_builder;

inline constexpr std::size_t redis_read_buffer_bytes = 8192;
// Serialized request capacity retained after a write's transport borrow ends.
// Larger buffers return to their PMR resource, including on failure.
inline constexpr std::size_t redis_retained_write_buffer_bytes = std::size_t{64} * 1024;

// A credential request (AUTH) is wiped from the write buffer once written, so
// the retained buffer never holds a password after the write completes.
enum class redis_request_content : bool { ordinary,
    credential };

// One Redis connection: the shared outbound transport plus the RESP request
// and reply buffers. Pool slots and subscriptions each own one. Address-stable:
// the transport's pending I/O and timers and the reader's object functions
// borrow it. The owner serializes requests into write_buffer(), sends them
// with write_request() and reads replies one at a time; a subscription keeps
// reading pushed replies without writing.
//
// Every failed transport result is mapped to redis_error here. An owner
// abort (cancelled or closing) is recorded on the transport and checked
// after each wait, so cancellation and shutdown never surface as I/O errors;
// an aborted result without an owner reason is an expired deadline.
class redis_connection final {
public:
    redis_connection(asio::io_context& io_context, asio::ssl::context& tls_context, const worker_ref& worker_value,
        const redis_config_storage& config, client_wire_counters& counters);
    redis_connection(asio::io_context&, asio::ssl::context&, worker_ref&&, const redis_config_storage&,
        client_wire_counters&) = delete;
    ~redis_connection();

    redis_connection(const redis_connection&) = delete;
    redis_connection& operator=(const redis_connection&) = delete;

    [[nodiscard]] bool connected() const noexcept {
        return connected_;
    }

    // Connects when closed. A connected connection whose peer closed it or
    // sent unsolicited bytes would desynchronize the next reply, so it is
    // replaced. The optional operation deadline bounds establishment together
    // with connect_timeout; AUTH and SELECT replies also honor command_timeout.
    task<void> connect(const operation_deadline* operation_timeout_value,
        std::optional<timeout_duration> command_timeout);
    // Serialized request storage: append RESP commands, then write_request().
    [[nodiscard]] std::string& write_buffer() noexcept {
        return write_buffer_;
    }
    // Sends and then releases the serialized request, wiping it first when it
    // carries a credential, whether or not the write succeeded.
    task<void> write_request(
        const operation_deadline& timeout, redis_request_content content = redis_request_content::ordinary);
    // Reads one complete reply, allocating its nodes from `resource`. Throws
    // redis_error(io_error) when the connection is or becomes closed. A
    // non-null `input_time` receives the time of every socket read that
    // delivered bytes, so a subscription's idle clock restarts on partial
    // replies too.
    task<redis_value> read_reply(const operation_deadline& timeout,
        std::chrono::steady_clock::time_point* input_time = nullptr);

    void close() noexcept;
    // Cancellation/shutdown: records the reason so a resolve completing after
    // this point starts no connect or handshake, then closes the connection.
    void abort(client_abort_reason reason) noexcept;
    // Throws the redis_error of a recorded owner abort (cancelled or closing).
    void throw_if_aborted() const;

private:
    void ensure_reader();
    task<void> authenticate(const operation_deadline& connect_timeout,
        std::optional<timeout_duration> command_timeout);
    // One connection-setup command (AUTH, SELECT) and its reply.
    task<redis_value> exchange_control_command(
        std::span<const std::string_view> args, operation_deadline deadline, redis_request_content content);

    const redis_config_storage& config_;
    client_tcp_transport transport_;
    std::string write_buffer_;
    std::array<char, redis_read_buffer_bytes> read_buffer_;
    // Created on first connect; the reader's object functions address the
    // builder, so it outlives every reader.
    std::unique_ptr<redis_reply_builder> reply_builder_;
    std::unique_ptr<redisReader, redis_reader_deleter> reader_;
    std::size_t reply_bytes_{0};
    bool connected_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

struct redis_command_args_view final {
    std::span<const std::string> args_;
};

class redis_pool final {
public:
    // `tls_context` is borrowed from the owning runtime and shared by every
    // pool of the alias; it is configured only when TLS is enabled.
    redis_pool(asio::io_context& io_context, const redis_config_storage& config, asio::ssl::context& tls_context,
        std::optional<timeout_duration> command_timeout, std::size_t pool_size,
        const worker_ref& worker_value);
    redis_pool(asio::io_context&, redis_config_storage&&, asio::ssl::context&, std::optional<timeout_duration>,
        std::size_t, const worker_ref&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, const worker_ref&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, worker_ref&&) = delete;
    redis_pool(asio::io_context&, const redis_config_storage&, asio::ssl::context&,
        std::optional<timeout_duration>, std::size_t, const worker_ref&&) = delete;
    ~redis_pool();

    redis_pool(const redis_pool&) = delete;
    redis_pool& operator=(const redis_pool&) = delete;

    task<void> connect();
    void close_now() noexcept;
    // `reply_timeout` starts when the command is written, so a server-side
    // block window is not consumed by slot acquisition or a lazy connect.
    // `io_metrics` is the request's counter block, or null outside a request.
    task<redis_value> execute_owned(redis_owned_command command, operation_options options = {},
        std::optional<timeout_duration> reply_timeout = std::nullopt,
        request_io_metrics* io_metrics = nullptr, std::optional<operation_deadline> deadline = std::nullopt);
    task<redis_value> execute_owned(std::vector<std::string> args, operation_options options = {},
        std::optional<timeout_duration> reply_timeout = std::nullopt,
        request_io_metrics* io_metrics = nullptr, std::optional<operation_deadline> deadline = std::nullopt);
    // The caller keeps `args` alive until the operation completes. `deadline`
    // is copied, so repeated commands share the operation's absolute bound.
    task<redis_value> execute_borrowed(redis_command_args_view args,
        operation_options options, operation_deadline deadline);
    task<std::vector<redis_value>> execute_pipeline(
        std::vector<redis_owned_command> commands, operation_options options, bool wrap_transaction = false);

private:
    // Every deadline belongs to a connection's transport; the slot pool only
    // leases slots and routes cancellation.
    struct slot_policy final {
        using owner_type = redis_pool;
        enum class deadline_kind_type : std::uint8_t { none };

        [[noreturn]] static void throw_acquire_failure(const redis_pool& pool, worker_wait_status status);
        [[noreturn]] static void throw_aborted(const redis_pool& pool, pool_slot_abort_reason reason);
        static void expire_slot(redis_pool&, std::size_t, deadline_kind_type) noexcept {}
        static void cancel_slot(redis_pool& pool, std::size_t slot) noexcept;
    };
    using slot_pool_type = slot_pool<slot_policy>;
    using connection_owner_type = std::unique_ptr<redis_connection>;

    template <typename result_type, typename command_source_type>
    task<result_type> execute_commands(command_source_type source, operation_options options,
        std::optional<timeout_duration> reply_timeout,
        std::optional<operation_deadline> deadline, request_io_metrics* io_metrics = nullptr,
        bool wrap_transaction = false);

    const redis_config_storage& config_;
    std::optional<timeout_duration> command_timeout_;
    client_wire_counters wire_counters_;
    slot_pool_type slots_;
    // Declared after the slot pool, whose worker every transport borrows.
    std::vector<connection_owner_type> connections_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class redis_subscription_set;

// The connection behind one redis_subscription. Address-stable and
// worker-affine; the set links every live connection so shutdown and the
// subscription's stop token can abort it, and the connection unlinks itself
// when destroyed. Closing is terminal: a failed, aborted or cancelled
// connection never reconnects.
//
// While read() waits, subscription_ping_interval drives an idle probe: an
// interval without input sends PING unless a request written since the last
// input still awaits its reply, and a further interval without input closes
// the connection with reason timeout. The probe timer and every PING write
// belong to the read that started them and end before it returns.
class redis_subscription_connection final {
public:
    redis_subscription_connection(redis_subscription_set& owner, operation_options options);
    ~redis_subscription_connection();

    redis_subscription_connection(const redis_subscription_connection&) = delete;
    redis_subscription_connection& operator=(const redis_subscription_connection&) = delete;

    // Writes one subscription command, connecting first when needed. The
    // write is bound by the operation timeout and command_timeout, and waits
    // for a PING write of the idle probe that holds the transport's write lane.
    task<void> send(redis_owned_command command);
    // Reads the next pushed reply, consuming PING replies. Views of the
    // returned event borrow current_reply_ until the next read() releases it.
    // `operation` tightens the read deadline; its stop request closes the
    // connection and fails this read with cancelled.
    task<std::optional<redis_pubsub_event>> read(operation_options operation);
    void abort(client_abort_reason reason) noexcept;

    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] bool has_active_operations() const noexcept {
        return read_active_ || write_active_;
    }

private:
    friend class redis_subscription_set;

    using clock_type = std::chrono::steady_clock;

    void throw_if_closed() const;
    void fail() noexcept;
    // Idle probe of one read(); `ping_writes` is that read's scope and
    // `read_timeout` its overall deadline, both borrowed until
    // end_idle_probe().
    void begin_idle_probe(task_scope& ping_writes, const operation_deadline& read_timeout) noexcept;
    void end_idle_probe() noexcept;
    void arm_idle_probe(clock_type::time_point deadline) noexcept;
    void idle_probe_expired() noexcept;
    // Records the exception being handled for the read, then closes.
    void fail_idle_probe() noexcept;
    // Bounded by `timeout`: one interval, capped by the joining read's deadline.
    task<void> write_ping(operation_deadline timeout);
    static void cancel_read(void* connection) noexcept;

    redis_subscription_set& owner_;
    operation_options options_;
    redis_connection connection_;
    redis_value current_reply_;
    redis_subscription_connection* previous_{nullptr};
    redis_subscription_connection* next_{nullptr};
    std::uint64_t cancellation_id_{0};
    bool read_active_{false};
    bool write_active_{false};
    // The first subscribe() has authenticated the connection.
    bool ready_{false};
    bool closed_{false};
    // A PING write holds the transport's write lane; send() waits on
    // ping_written_ until it ends.
    bool ping_writing_{false};
    // PING replies still owed by the server, consumed by read() in wire order.
    std::size_t pending_pings_{0};
    // The read()'s scope its PING writes join and that read's deadline; null
    // while no read waits.
    task_scope* ping_writes_{nullptr};
    const operation_deadline* read_timeout_{nullptr};
    // Idle clock: the latest received bytes, the start of the waiting read,
    // the latest request written (a command or PING whose reply is owed when
    // it is newer than the input) and the start of the current probe.
    clock_type::time_point last_input_{};
    clock_type::time_point read_started_{};
    clock_type::time_point last_request_{};
    std::optional<clock_type::time_point> probe_started_{};
    // An internal probe failure no caller observed yet; the waiting read
    // rethrows it.
    std::exception_ptr probe_failure_{};
    worker_signal ping_written_;
    // Why the connection closed: none for a failed operation, timeout for an
    // unanswered idle probe.
    client_abort_reason closed_reason_{client_abort_reason::none};
    // Armed only while a read() waits and cancelled before it returns; its
    // completion addresses this connection.
    worker_timer_registration idle_probe_timer_;
    // Routes the stop request of the waiting read().
    operation_interrupt_source read_stops_;
    // Declared last: it is armed with the stop token once the connection is
    // linked and is released before any other member is destroyed.
    worker_cancellation_registration<worker_cancellation_target<redis_subscription_set>> cancellation_;
};

// The live Pub/Sub connections of one alias on one worker. Owned by
// redis_client_runtime next to the command pools; close_now() aborts every
// connection, and each connection is joined by the scope that owns its
// redis_subscription. Destroying the set with live connections terminates.
class redis_subscription_set final {
public:
    redis_subscription_set(asio::io_context& io_context, asio::ssl::context& tls_context,
        const redis_config_storage& config, const worker_ref& worker_value);
    redis_subscription_set(asio::io_context&, asio::ssl::context&, const redis_config_storage&, worker_ref&&) = delete;
    ~redis_subscription_set();

    redis_subscription_set(const redis_subscription_set&) = delete;
    redis_subscription_set& operator=(const redis_subscription_set&) = delete;

    using connection_owner_type =
        std::unique_ptr<redis_subscription_connection>;

    // A closed set opens connections that are already closing, so a late
    // subscription observes shutdown instead of connecting.
    [[nodiscard]] connection_owner_type open(operation_options options);
    void close_now() noexcept;

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    // worker_cancellation_target hook; runs on the worker.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

private:
    friend class redis_subscription_connection;

    void link(redis_subscription_connection& connection) noexcept;
    void unlink(redis_subscription_connection& connection) noexcept;

    asio::io_context& io_context_;
    asio::ssl::context& tls_context_;
    const redis_config_storage& config_;
    worker_ref worker_;
    client_wire_counters wire_counters_;
    std::shared_ptr<worker_cancellation_target<redis_subscription_set>> cancellation_target_;
    redis_subscription_connection* head_{nullptr};
    bool closing_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The single backend owner used by standalone clients and application registrations.
// Both pools and the Pub/Sub connections borrow this stable owner's normalized
// configuration, TLS context and worker.
class redis_client_runtime final {
public:
    redis_client_runtime(asio::io_context& io_context, const worker_ref& worker_value,
        redis_config_storage config);
    redis_client_runtime(asio::io_context&, worker_ref&&, redis_config_storage) = delete;
    ~redis_client_runtime();

    redis_client_runtime(const redis_client_runtime&) = delete;
    redis_client_runtime& operator=(const redis_client_runtime&) = delete;

    [[nodiscard]] task<void> connect();
    void close_now() noexcept;
    // An application-registry handle; `io_metrics` is the request's counter
    // block, or null outside a request.
    [[nodiscard]] redis_session session(::ruvia::operation_scope& scope, request_io_metrics* io_metrics);
    // A standalone client handle, which never belongs to a request.
    [[nodiscard]] redis_session session(::ruvia::operation_scope& scope, operation_options options);

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] const redis_config_storage& config() const noexcept {
        return config_;
    }
    // The configuration of the runtime behind an active handle, for a
    // consumer that sizes its own use of that store (the DB query cache).
    [[nodiscard]] static const redis_config_storage& handle_config(const redis_session& handle) noexcept {
        return handle.runtime_->config();
    }
    [[nodiscard]] redis_pool& general() noexcept {
        return general_;
    }
    [[nodiscard]] redis_pool& blocking() noexcept {
        return blocking_;
    }
    [[nodiscard]] redis_subscription_set& subscriptions() noexcept {
        return subscriptions_;
    }

private:
    redis_config_storage config_;
    worker_ref worker_;
    // One client TLS context per alias, borrowed by every connection of both
    // pools and every subscription; configured only when TLS is enabled.
    asio::ssl::context tls_context_;
    redis_pool general_;
    redis_pool blocking_;
    redis_subscription_set subscriptions_;
};

}  // namespace ruvia::detail

#ifdef RUVIA_ENABLE_REDIS

namespace ruvia::detail {

class redis_registry final {
public:
    redis_registry(asio::io_context& io_context,
        std::span<const redis_definition_type> redis, worker_ref worker_value);
    ~redis_registry();

    redis_registry(const redis_registry&) = delete;
    redis_registry& operator=(const redis_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] redis_session get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;
    [[nodiscard]] redis_session get(
        std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

private:
    using entry_type = std::unique_ptr<redis_client_runtime>;

    std::vector<entry_type> runtimes_;
    named_capability_index alias_index_;
};

}  // namespace ruvia::detail

#endif

#endif
#pragma once

#ifdef RUVIA_ENABLE_REDIS
#endif
#ifdef RUVIA_ENABLE_REDIS
#endif

namespace ruvia::detail {

class router;

// What a registered route runs, as one closed set of alternatives. Each
// alternative carries the handler's invoke thunk together with the route
// metadata only that shape may have, so a route cannot claim a streaming or
// websocket mode while holding a buffered handler. The handler's target is
// worker-bound and lives in each worker's route_table, never here.

class route_endpoint;

class buffered_route_endpoint final {
public:
    [[nodiscard]] route_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] detail::request_body_mode request_body_mode() const noexcept {
        return request_body_mode_;
    }

private:
    friend class route_endpoint;

    buffered_route_endpoint(route_handler::invoke_type invoke,
        detail::request_body_mode request_body_mode_value) noexcept
        : invoke_(invoke),
          request_body_mode_(request_body_mode_value) {}

    route_handler::invoke_type invoke_;
    detail::request_body_mode request_body_mode_;
};

class response_stream_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] detail::request_body_mode request_body_mode() const noexcept {
        return request_body_mode_;
    }

private:
    friend class route_endpoint;

    explicit response_stream_route_endpoint(route_stream_handler::invoke_type invoke,
        detail::request_body_mode request_body_mode_value = detail::request_body_mode::buffered) noexcept
        : invoke_(invoke),
          request_body_mode_(request_body_mode_value) {}

    route_stream_handler::invoke_type invoke_;
    detail::request_body_mode request_body_mode_;
};

class tunnel_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }
    [[nodiscard]] std::string_view protocol() const noexcept {
        return protocol_;
    }
    [[nodiscard]] const http_tunnel_route_config& config() const noexcept {
        return config_;
    }

private:
    friend class route_endpoint;
    tunnel_route_endpoint(route_stream_handler::invoke_type invoke,
        std::string_view protocol, http_tunnel_route_config config)
        : invoke_(invoke),
          protocol_(protocol),
          config_(config) {}
    route_stream_handler::invoke_type invoke_;
    std::string protocol_;
    http_tunnel_route_config config_;
};

class websocket_route_endpoint final {
public:
    [[nodiscard]] route_stream_handler::invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] std::span<const std::string_view> subprotocols() const noexcept {
        return subprotocols_;
    }

    [[nodiscard]] websocket_handshake_validation_options handshake_validation() const noexcept {
        return {.supported_subprotocols_ = subprotocols_, .require_subprotocol_ = require_subprotocol_};
    }

    [[nodiscard]] const websocket_lifecycle_options& lifecycle() const noexcept {
        return lifecycle_;
    }

    [[nodiscard]] const websocket_deflate_config& deflate() const noexcept {
        return deflate_;
    }
    [[nodiscard]] protocol_byte_limit message_limit(std::size_t server_limit) const {
        const auto local = max_message_bytes_.maximum();
        return protocol_byte_limit::limited(local && *local < server_limit ? *local : server_limit);
    }

private:
    friend class route_endpoint;

    websocket_route_endpoint(route_stream_handler::invoke_type invoke,
        const websocket_route_config& options)
        : invoke_(invoke),
          require_subprotocol_(options.require_subprotocol_),
          lifecycle_(options.lifecycle_),
          deflate_(options.deflate_),
          max_message_bytes_(options.max_message_bytes_) {
        if (!options.subprotocols_.empty()) {
            std::size_t total_bytes = 0;
            for (const auto& subprotocol : options.subprotocols_) {
                total_bytes += subprotocol.size();
            }
            subprotocol_storage_.reserve(total_bytes);
            subprotocols_.reserve(options.subprotocols_.size());
            for (const auto& subprotocol : options.subprotocols_) {
                const auto offset = subprotocol_storage_.size();
                subprotocol_storage_.insert(
                    subprotocol_storage_.end(), subprotocol.begin(), subprotocol.end());
                subprotocols_.emplace_back(
                    subprotocol_storage_.data() + offset, subprotocol.size());
            }
        }
    }

    route_stream_handler::invoke_type invoke_;
    std::vector<char> subprotocol_storage_;
    std::vector<std::string_view> subprotocols_;
    bool require_subprotocol_;
    websocket_lifecycle_options lifecycle_;
    websocket_deflate_config deflate_;
    protocol_byte_limit max_message_bytes_;
};

// Startup-built endpoint contract, validated once when the process route plan
// is compiled. The handler shape and its only legal route metadata live in the
// same alternative.
class route_endpoint final {
public:
    route_endpoint(const route_endpoint&) = delete;
    route_endpoint& operator=(const route_endpoint&) = delete;
    route_endpoint(route_endpoint&&) noexcept = default;
    route_endpoint& operator=(route_endpoint&&) = delete;

    // Handlers come from the controller macros' bind()/bind_stream(), which
    // always supply an invoker; body modes and stream kinds are the builder's
    // own enumerators.
    [[nodiscard]] static route_endpoint buffered(route_handler::invoke_type invoke,
        request_body_mode request_body_mode_value) {
        return route_endpoint(buffered_route_endpoint(invoke, request_body_mode_value));
    }

    [[nodiscard]] static route_endpoint response_stream(
        route_stream_handler::invoke_type invoke,
        request_body_mode request_body_mode_value = request_body_mode::buffered) {
        return route_endpoint(response_stream_route_endpoint(invoke, request_body_mode_value));
    }

    [[nodiscard]] static route_endpoint websocket(route_stream_handler::invoke_type invoke, websocket_route_config options) {
        options.lifecycle_ = normalize_websocket_lifecycle_options(options.lifecycle_);
        validate_websocket_deflater_config(options.deflate_.deflater_);
        ruvia::websocket_subprotocol_set subprotocols;
        for (const auto& subprotocol : options.subprotocols_) {
            if (!subprotocols.append(subprotocol)) {
                throw std::invalid_argument(
                    "websocket subprotocols must contain at most 64 unique HTTP tokens");
            }
        }
        if (options.require_subprotocol_ && options.subprotocols_.empty()) {
            throw std::invalid_argument("a websocket route requiring a subprotocol must list subprotocols");
        }
        return route_endpoint(
            websocket_route_endpoint(invoke, options));
    }

    [[nodiscard]] static http_tunnel_route_config validate_tunnel_config(
        bool upgrade_protocol, bool udp, http_tunnel_route_config config) {
        if (udp) {
            config.datagrams_ = true;
        }
        if (config.datagrams_ && !upgrade_protocol) {
            throw std::invalid_argument("HTTP Datagrams require an upgrade-protocol tunnel route");
        }
        if (config.peer_transport_fin_timeout_.value() <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("CONNECT peer transport FIN timeout must be greater than zero");
        }
        return config;
    }

    [[nodiscard]] static route_endpoint tunnel(route_stream_handler::invoke_type invoke, std::string_view protocol, http_tunnel_route_config config) {
        if (!protocol.empty() && !is_valid_http_header_name(protocol)) {
            throw std::invalid_argument("CONNECT protocol must be an HTTP token");
        }
        config = validate_tunnel_config(!protocol.empty(), protocol == "connect-udp", config);
        return route_endpoint(
            tunnel_route_endpoint(invoke, protocol, config));
    }

    [[nodiscard]] const tunnel_route_endpoint* tunnel() const& noexcept {
        return std::get_if<tunnel_route_endpoint>(&value_);
    }
    const tunnel_route_endpoint* tunnel() const&& = delete;

    [[nodiscard]] const buffered_route_endpoint* buffered() const& noexcept {
        return std::get_if<buffered_route_endpoint>(&value_);
    }
    [[nodiscard]] const buffered_route_endpoint* buffered() const&& = delete;

    [[nodiscard]] const response_stream_route_endpoint* response_stream() const& noexcept {
        return std::get_if<response_stream_route_endpoint>(&value_);
    }
    [[nodiscard]] const response_stream_route_endpoint* response_stream() const&& = delete;

    [[nodiscard]] const websocket_route_endpoint* websocket() const& noexcept {
        return std::get_if<websocket_route_endpoint>(&value_);
    }
    [[nodiscard]] const websocket_route_endpoint* websocket() const&& = delete;

    [[nodiscard]] ruvia::detail::request_body_mode request_body_mode() const noexcept {
        if (const auto* endpoint = buffered()) {
            return endpoint->request_body_mode();
        }
        if (const auto* endpoint = response_stream()) {
            return endpoint->request_body_mode();
        }
        return request_body_mode::buffered;
    }

    // The streaming handler thunk, or null for a buffered-response endpoint.
    [[nodiscard]] route_stream_handler::invoke_type stream_invoke() const noexcept {
        if (const auto* endpoint = response_stream()) {
            return endpoint->invoke();
        }
        if (const auto* endpoint = websocket()) {
            return endpoint->invoke();
        }
        if (const auto* endpoint = tunnel()) {
            return endpoint->invoke();
        }
        return nullptr;
    }

private:
    using value_type =
        std::variant<buffered_route_endpoint, response_stream_route_endpoint, websocket_route_endpoint, tunnel_route_endpoint>;

    template <typename endpoint_type>
    explicit route_endpoint(endpoint_type endpoint) noexcept
        : value_(std::move(endpoint)) {}

    value_type value_;
};

class route_entry final {
public:
    struct init_type final {
        std::size_t index_{0};
        // Registration-order slot of the controller whose instance is the
        // handler target on every worker.
        std::size_t controller_{0};
        http_known_method method_;
        // Non-empty only for an extension-method route, where it is the exact,
        // case-sensitive wire token (RFC 9110 9.1) and `method` is unknown.
        // Known methods keep the enum as their identity so the routing fast
        // path never compares strings.
        std::string_view method_token_{};
        std::string_view path_;
        route_endpoint endpoint_;
        bool dynamic_{false};
        // 0 = no route-declared ceiling; the server default applies.
        std::size_t max_request_body_bytes_{0};
    };

    route_entry(init_type init)
        : index_(init.index_),
          controller_(init.controller_),
          method_(init.method_),
          method_token_(init.method_token_),
          path_(init.path_),
          endpoint_(std::move(init.endpoint_)),
          dynamic_(init.dynamic_),
          max_request_body_bytes_(init.max_request_body_bytes_) {}
    route_entry(const route_entry&) = delete;
    route_entry& operator=(const route_entry&) = delete;
    route_entry(route_entry&&) noexcept = default;
    route_entry& operator=(route_entry&&) = delete;

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }

    [[nodiscard]] std::size_t controller() const noexcept {
        return controller_;
    }

    [[nodiscard]] http_known_method method() const noexcept {
        return method_;
    }

    // Empty unless this is an extension-method route.
    [[nodiscard]] std::string_view method_token() const noexcept {
        return method_token_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] const route_endpoint& endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] bool dynamic() const noexcept {
        return dynamic_;
    }

    // A ceiling declared by one of this route's middlewares, or 0 for none.
    // Read before the body is accepted, so it bounds what is buffered rather
    // than what a handler later sees.
    [[nodiscard]] std::size_t max_request_body_bytes() const noexcept {
        return max_request_body_bytes_;
    }

    [[nodiscard]] std::span<const std::string_view> param_names() const noexcept {
        return param_names_;
    }

    [[nodiscard]] std::size_t middleware_offset() const noexcept {
        return middleware_offset_;
    }

    [[nodiscard]] std::size_t middleware_count() const noexcept {
        return middleware_count_;
    }

    [[nodiscard]] bool has_middleware() const noexcept {
        return middleware_count_ != 0;
    }

    // True when at least one frame in this route's range is a path-scoped
    // middleware whose scope covers only part of the route's request paths;
    // dispatch then checks the request path before running such a frame.
    [[nodiscard]] bool has_conditional_middleware() const noexcept {
        return conditional_middleware_;
    }

private:
    friend class router;

    std::size_t index_;
    std::size_t controller_;
    http_known_method method_;
    std::string method_token_;
    std::string path_;
    route_endpoint endpoint_;
    bool dynamic_{false};
    std::size_t max_request_body_bytes_{0};
    std::span<const std::string_view> param_names_{};
    std::size_t middleware_offset_{0};
    std::size_t middleware_count_{0};
    bool conditional_middleware_{false};
};
class route_table;

// The one immutable route plan of a process run, compiled once at startup and
// shared by every worker: every route with its endpoint metadata, limits and
// parameter names, the middleware frame layout, the lookup indices, and the
// error/not-found fallbacks. Workers bind only their own handler targets and
// middleware instances to it (route_table), by route and frame index.
class compiled_route_plan final {
public:
    compiled_route_plan()
        : routes_(),
          middleware_instances_(),
          middleware_frame_instances_(),
          middleware_scopes_(),
          param_names_(),
          extension_route_indices_(),
          extension_paths_(),
          extension_methods_(),
          connect_route_indices_(),
          connect_protocols_(),
          static_slots_(),
          dynamic_node_arena_(),
          url_patterns_(),
          prefix_error_handlers_(),
          prefix_not_found_handlers_() {}

    compiled_route_plan(const compiled_route_plan&) = delete;
    compiled_route_plan& operator=(const compiled_route_plan&) = delete;
    compiled_route_plan(compiled_route_plan&&) = delete;
    compiled_route_plan& operator=(compiled_route_plan&&) = delete;

private:
    friend class route_table;
    friend class router;

    static constexpr std::size_t routable_method_count = 7;
    static constexpr std::size_t no_route_index = std::numeric_limits<std::size_t>::max();

    struct static_route_slot final {
        std::uint64_t hash_{};
        std::size_t route_index_{no_route_index};
    };

    struct extension_path_type final {
        std::string_view path_;
        std::span<const std::string_view> methods_;
    };

    struct dynamic_node_type;

    struct dynamic_static_child_type final {
        std::string_view segment_;
        dynamic_node_type* node_{nullptr};
    };

    struct dynamic_node_type final {
        dynamic_node_type()
            : static_children_() {}

        std::vector<dynamic_static_child_type> static_children_;
        dynamic_node_type* param_child_{nullptr};
        std::size_t route_index_{no_route_index};
        std::size_t wildcard_route_index_{no_route_index};
    };

    struct connect_protocol_index_type final {
        explicit connect_protocol_index_type(std::string_view value)
            : protocol_(value) {}
        std::string protocol_;
        dynamic_node_type root_;
    };

    // Normalized prefix, deepest first: selection is a first-match scan.
    template <typename handler_type>
    struct prefix_handler_type final {
        prefix_handler_type(std::string_view prefix_value,
            handler_type handler_value)
            : prefix_(prefix_value),
              handler_(handler_value) {}

        std::string prefix_;
        handler_type handler_{nullptr};
    };

    std::vector<route_entry> routes_;
    // One descriptor per middleware instance a worker materializes. Frames
    // name their instance by index, so an app-wide middleware is one instance
    // shared by every route of a worker.
    std::vector<controller_middleware_descriptor> middleware_instances_;
    std::vector<std::size_t> middleware_frame_instances_;
    // Parallel to middleware_frame_instances_: the path scope a conditional
    // frame checks at dispatch, or empty for an unconditional frame. Only
    // routes flagged has_conditional_middleware() read it. Views borrow the
    // registration-owned use_at prefixes, which outlive every plan.
    std::vector<std::string_view> middleware_scopes_;
    // A contiguous block at the end of the frames: the app-wide middlewares
    // that declared they also run when no route matched.
    std::size_t unmatched_middleware_offset_{0};
    std::size_t unmatched_middleware_count_{0};
    std::vector<std::string_view> param_names_;
    // Sorted by case-sensitive method token, then canonical path.
    std::vector<std::size_t> extension_route_indices_;
    // Sorted distinct canonical paths, naming every registered token for
    // 405/OPTIONS Allow in registration order. The spans slice
    // extension_methods_ and borrow the routes' own path and token storage.
    std::vector<extension_path_type> extension_paths_;
    std::vector<std::string_view> extension_methods_;
    std::vector<std::size_t> connect_route_indices_;
    std::vector<connect_protocol_index_type> connect_protocols_;
    std::vector<static_route_slot> static_slots_;
    std::array<dynamic_node_type, routable_method_count> dynamic_roots_;
    std::vector<dynamic_node_type> dynamic_node_arena_;
    // Distinct registered route patterns, sorted, so url_for finds its pattern
    // by binary search.
    std::vector<std::string_view> url_patterns_;
    http_error_handler_ref error_handler_{nullptr};
    http_not_found_handler_ref not_found_handler_{nullptr};
    std::vector<prefix_handler_type<http_error_handler_ref>> prefix_error_handlers_;
    std::vector<prefix_handler_type<http_not_found_handler_ref>> prefix_not_found_handlers_;
    std::uint32_t static_method_mask_{0};
    std::uint32_t dynamic_method_mask_{0};
    std::size_t static_slot_mask_{};
    bool has_route_rate_limit_{false};
};

using compiled_route_plan_ptr_type =
    std::unique_ptr<compiled_route_plan>;

}  // namespace ruvia::detail

// Lightweight, self-contained route-resolution result types. A materialized
// resolution is exactly one of resolved, method-not-allowed, not-found, or
// rejected; the payload for another alternative is not observable.

namespace ruvia::detail {

inline constexpr std::size_t max_route_params = 16;
class route_entry;

class route_match final {
public:
    constexpr route_match() noexcept = default;

    constexpr route_match(const route_match& other) noexcept
        : param_count_(other.param_count_) {
        for (std::size_t i = 0; i < param_count_; ++i) {
            param_values_[i] = other.param_values_[i];
        }
    }

    constexpr route_match& operator=(const route_match& other) noexcept {
        param_count_ = other.param_count_;
        for (std::size_t i = 0; i < param_count_; ++i) {
            param_values_[i] = other.param_values_[i];
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::string_view> values() const& noexcept {
        return std::span<const std::string_view>(param_values_.data(), param_count_);
    }
    [[nodiscard]] std::span<const std::string_view> values() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return param_count_;
    }

    void clear() noexcept {
        param_count_ = 0;
    }

    void truncate(std::size_t count) noexcept {
        param_count_ = count <= param_count_ ? count : param_count_;
    }

    [[nodiscard]] bool add(std::string_view value) noexcept {
        if (param_count_ >= param_values_.size()) {
            return false;
        }

        param_values_[param_count_++] = value;
        return true;
    }

private:
    std::array<std::string_view, max_route_params> param_values_{};
    std::size_t param_count_{0};
};

class route_not_found final {
private:
    friend class route_resolution;
    constexpr route_not_found() noexcept = default;
};

// A target that names no resource at all (rootless or dot-segment path): no
// route and no document-root file may answer it, only the not-found path.
class route_rejected final {
private:
    friend class route_resolution;
    constexpr route_rejected() noexcept = default;
};

class route_method_not_allowed final {
public:
    [[nodiscard]] constexpr std::uint32_t allowed_methods() const noexcept {
        return allowed_methods_;
    }
    // The distinct extension tokens registered on the resource, for Allow.
    // Borrowed from the compiled route plan.
    [[nodiscard]] constexpr std::span<const std::string_view> extension_methods() const noexcept {
        return extension_methods_;
    }

private:
    friend class route_resolution;

    constexpr route_method_not_allowed(
        std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept
        : allowed_methods_(allowed_methods),
          extension_methods_(extension_methods) {}

    std::uint32_t allowed_methods_;
    std::span<const std::string_view> extension_methods_;
};

class resolved_route final {
public:
    // Process-invariant route data from the compiled plan.
    [[nodiscard]] const route_entry& route() const noexcept {
        return *route_;
    }

    // The resolving worker's handler. handler() requires a buffered-response
    // endpoint; stream_handler() a response-stream, websocket or tunnel one.
    [[nodiscard]] route_handler handler() const noexcept;
    [[nodiscard]] route_stream_handler stream_handler() const noexcept;

    // Static routes carry an empty match; dynamic routes carry their captured
    // values in the same value object. Callers never need a nullable match side
    // channel.
    [[nodiscard]] const route_match& match() const& noexcept {
        return match_;
    }
    [[nodiscard]] const route_match& match() const&& = delete;

private:
    friend class route_resolution;

    resolved_route(const route_entry& route, void* handler_target) noexcept
        : route_(&route),
          handler_target_(handler_target) {}

    resolved_route(const route_entry& route, void* handler_target, const route_match& match) noexcept
        : route_(&route),
          handler_target_(handler_target),
          match_(match) {}

    const route_entry* route_;
    void* handler_target_;
    route_match match_;
};

class route_resolution final {
public:
    route_resolution() noexcept
        : value_(route_not_found{}) {}

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, void* handler_target) noexcept {
        return route_resolution(resolved_route(route, handler_target));
    }

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, void* handler_target, const route_match& match) noexcept {
        return route_resolution(resolved_route(route, handler_target, match));
    }

    // The mask spans only classified methods, so it can be empty for a resource
    // that has extension-method routes. A non-empty `extension_methods` alone
    // still makes the resource exist, so an extension-only path answers 405
    // with an Allow built from its tokens instead of collapsing to 404.
    [[nodiscard]] static route_resolution method_not_allowed(
        std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept {
        if (allowed_methods == 0 && extension_methods.empty()) {
            return route_resolution();
        }
        return route_resolution(route_method_not_allowed(allowed_methods, extension_methods));
    }

    [[nodiscard]] static route_resolution reject() noexcept {
        return route_resolution(route_rejected{});
    }

    [[nodiscard]] const resolved_route* resolved() const& noexcept {
        return std::get_if<resolved_route>(&value_);
    }
    [[nodiscard]] const resolved_route* resolved() const&& = delete;

    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const& noexcept {
        return std::get_if<route_method_not_allowed>(&value_);
    }
    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const&& = delete;

    [[nodiscard]] const route_rejected* rejected() const& noexcept {
        return std::get_if<route_rejected>(&value_);
    }
    [[nodiscard]] const route_rejected* rejected() const&& = delete;

private:
    using value_type = std::variant<route_not_found, route_method_not_allowed, resolved_route, route_rejected>;

    template <typename alternative_type>
    explicit route_resolution(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class route_table;

struct middleware_chain final {
    using terminal_type = callback_ref<task<void>(context&)>;
    using failure_type = task<void> (*)(const route_table&, context&, std::exception_ptr);

    const route_table* table_{nullptr};
    std::span<const route_middleware> frames_;
    std::span<const std::string_view> scopes_;
    terminal_type terminal_;
    failure_type failure_{nullptr};
};

[[nodiscard]] task<void> run_chain(const middleware_chain& chain, context& context_value);
struct route_request_view final {
    http_known_method known_method_{http_known_method::unknown};
    std::string_view method_{};
    std::string_view path_{};
    std::string_view connect_target_{};
    std::string_view extended_protocol_{};
};

class controller_store;
class router;

// The framework's own answer to a failed request, consulting no application
// error handler: the status an http_error or protocol failure carries, else a
// generic 500 that names nothing internal. An inbound transport failure
// propagates instead.
struct default_exception_response final {
    http_response response_;
    // The failure maps to no HTTP error: a fault for the caller to report.
    bool unexpected_;
};
[[nodiscard]] default_exception_response make_default_exception_response(const std::exception_ptr& exception);

// One worker's binding of the process route plan: the worker's controller
// instance behind every route and the worker's middleware instance behind
// every frame, indexed exactly like the plan. Everything else -- lookup,
// endpoint metadata, limits, fallbacks -- is read from the shared plan.
class route_table final {
public:
    // The plan and the controllers must outlive the table. Middleware
    // instances are created here and destroyed with the table.
    route_table(const compiled_route_plan& plan, const controller_store& controllers);
    route_table(const compiled_route_plan& plan, std::span<void* const> targets);
    ~route_table();
    route_table(const route_table&) = delete;
    route_table& operator=(const route_table&) = delete;
    route_table(route_table&&) = delete;
    route_table& operator=(route_table&&) = delete;
    // Owner thread, before this worker admits its first routed listener.
    // Both plans and controller stores retain their existing lifetimes.
    void swap(route_table& other) noexcept;

    [[nodiscard]] bool has_route_rate_limit() const noexcept {
        return plan_->has_route_rate_limit_;
    }
    // Builds a request path from a registered route pattern: ":name" segments
    // take the next value (percent-encoded, non-empty), a trailing "*" takes
    // the final value (slashes preserved, may be empty). The pattern is the
    // route's identity -- an unregistered pattern, a value-count mismatch, a
    // "."/".." value, or a pattern starting with an empty segment (a "//"
    // network-path reference) is a programming error and throws
    // std::invalid_argument. A root "*" value's leading slash becomes "%2F".
    [[nodiscard]] std::string url_for(std::string_view pattern, std::span<const std::string_view> values) const;
    [[nodiscard]] route_resolution resolve(const http_request& request) const noexcept;
    [[nodiscard]] route_resolution resolve(route_request_view request) const noexcept;

    // Whether ANY route in the table uses this exact token. RFC 9110 15.5.6
    // makes 405 conditional on the method being "known by the origin server",
    // so a token nobody registered is 501 no matter what the target path holds.
    [[nodiscard]] bool recognizes_method_token(std::string_view method_token) const noexcept;
    // Canonical buffered-response application dispatch for unresolved requests
    // and callers that need it to own the route context. An unresolved route
    // first consults the configured document root, then falls through to
    // 404/405/OPTIONS handling. Any failure escaping the routing machinery
    // becomes an error response, except an inbound transport failure, which
    // propagates. Connection persistence and wire framing remain the HTTP/1
    // driver's responsibility. Pass document_root_binding::none() when no root
    // is configured, or the worker's configured(generation) lease otherwise.
    // The caller owns response and keeps it alive until dispatch completes;
    // successful completion assigns the selected final response directly.
    task<void> dispatch_buffered_response(const http_request& request,
        const route_resolution& resolution, document_root_binding document_root,
        const context_services& services, static_file_selection_mode static_file_mode,
        http_response& response) const;
    // The one matched-route invocation every transport awaits: builds the
    // context in this frame, runs the route chain, applies the response policy
    // for routes without middleware, finalizes a failure, and falls back to the
    // request-only handler when the context cannot be built or finalization
    // fails. `body_failure`, when given, is a request-body failure the transport
    // records outside the handler's reads (HTTP/2 DATA): a handler that returns
    // without surfacing it gets that failure instead. Returns whether the
    // response accounts for that failure. The context dies before this
    // returns, so body completion and the request arena outlive it.
    [[nodiscard]] task<bool> dispatch_resolved_route(const http_request& request,
        const resolved_route& resolved, const context_services& services,
        const std::optional<http_request_body_failure>* body_failure, http_response& response) const;
    task<http_response> handle_error(const http_request& request,
        http_error_info error, const context_services& services) const;
    task<http_response> handle_exception(const http_request& request,
        std::exception_ptr exception, const context_services& services) const;
    // Absence means the bound output handled the request; a value is the one
    // buffered response produced before a response-stream commit.
    task<std::optional<http_response>> dispatch_response_stream(const http_request& request,
        const resolved_route& route, response_stream_writer& response_stream,
        const context_services& services) const;
    // A CONNECT tunnel or WebSocket upgrade: `handler` is the terminal that
    // commits the upgrade. Absence means the terminal took the request.
    task<std::optional<http_response>> dispatch_upgrade(const http_request& request,
        const resolved_route& route, const route_stream_handler& handler,
        const context_services& services) const;
    // The not-found handler that applies to `path`: the tightest prefix scope
    // covering it, else the plan-wide one; null when neither is configured.
    [[nodiscard]] http_not_found_handler_ref not_found_handler_for(std::string_view path) const noexcept;
    // Reuses the transport's already-bound context and response writer.
    task<std::optional<http_response>> dispatch_bound_stream(const resolved_route& resolved,
        context& context_value, route_stream_handler handler) const;
    task<http_response> dispatch_bound_buffered(const resolved_route& resolved, context& context_value,
        const std::optional<http_request_body_failure>* body_failure = nullptr) const;

private:
    friend class router;

    [[nodiscard]] task<http_response> invoke_route(const resolved_route& resolved, context& context) const;
    // Failed buffered/stream invocations preserve only the explicit policy;
    // discarded representation fields never enter the replacement error.
    // If finalization fails, destroy the context before the request-only fallback.
    [[nodiscard]] task<http_response> finalize_failure(context& context, std::exception_ptr exception) const;

    [[nodiscard]] route_resolution resolve(
        http_known_method method, std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolve_connect(std::string_view protocol, std::string_view target) const noexcept;
    // Extension-method routing, kept off every enum-indexed structure. The
    // request's exact token is compared against a small cold list, which costs
    // a known-method request nothing: the HTTP/1 driver only reaches it when
    // classify_http_method() returned unknown.
    [[nodiscard]] route_resolution resolve_extension_method(
        std::string_view method_token, std::string_view path) const noexcept;
    // Distinct tokens of the extension routes on `path`, precomputed in the
    // plan; empty when none is registered there.
    [[nodiscard]] std::span<const std::string_view> extension_methods_for(std::string_view path) const noexcept;
    [[nodiscard]] route_resolution resolved(const route_entry& route) const noexcept;
    [[nodiscard]] route_resolution resolved(const route_entry& route, const route_match& match) const noexcept;

    static constexpr std::size_t routable_method_count = compiled_route_plan::routable_method_count;
    static constexpr std::size_t no_route_index = compiled_route_plan::no_route_index;
    using dynamic_node_type = compiled_route_plan::dynamic_node_type;
    using dynamic_static_child_type = compiled_route_plan::dynamic_static_child_type;

    [[nodiscard]] static std::size_t method_index(http_known_method method) noexcept;
    [[nodiscard]] static bool is_routable_method(http_known_method method) noexcept;
    [[nodiscard]] static bool supports_head_fallback(const route_entry& route) noexcept;
    [[nodiscard]] static std::uint64_t path_hash(std::string_view path) noexcept;
    [[nodiscard]] static std::uint64_t route_hash(
        http_known_method method, std::uint64_t hash) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node(
        const dynamic_node_type& node, std::string_view path, route_match& match) noexcept;
    [[nodiscard]] static std::size_t find_dynamic_node_no_params(
        const dynamic_node_type& node, std::string_view path) noexcept;
    [[nodiscard]] static const dynamic_static_child_type* find_dynamic_static_child(
        const dynamic_node_type& node, std::string_view segment) noexcept;

    [[nodiscard]] const route_entry* find_static_route(
        http_known_method method, std::string_view path, std::uint64_t hash) const noexcept;
    [[nodiscard]] const route_entry* find_dynamic_route(
        http_known_method method, std::string_view path, route_match& match) const noexcept;
    [[nodiscard]] std::uint32_t allowed_methods(std::string_view path, std::uint64_t hash,
        std::uint32_t excluded_mask = 0, std::uint32_t initial_mask = 0) const noexcept;
    // The handler is copied into the coroutine frame: callers may pass one
    // bound from the resolved route as a temporary.
    [[nodiscard]] task<std::optional<http_response>> dispatch_stream_route(const http_request& request,
        const resolved_route& route, route_stream_handler handler,
        const context_services& services, response_stream_writer* response_stream = nullptr) const;

    // A route's own frames and, only when some are conditional, their scopes.
    [[nodiscard]] middleware_chain route_chain(const route_entry& route) const noexcept;
    // Runs `frames` around `terminal`, whose response becomes the result; the
    // completion check shared by matched buffered routes and the unmatched
    // chain.
    [[nodiscard]] task<http_response> respond_through_chain(
        middleware_chain chain, context& context, route_handler terminal) const;
    // Runs the unmatched-request middleware chain around `terminal`, which
    // produces the 404/405/501 response. Falls straight through to the terminal
    // when nothing declared itself for unmatched requests.
    [[nodiscard]] task<http_response> run_unmatched_chain(context& context, route_handler terminal) const;
    [[nodiscard]] static task<void> contain_failure(
        const route_table& table, context& context, std::exception_ptr exception);
    [[nodiscard]] task<http_response> handle_not_found(
        const http_request& request, const context_services& services) const;
    [[nodiscard]] task<http_response> handle_exception(
        context& context, std::exception_ptr exception) const;
    [[nodiscard]] task<http_response> handle_exception(context& context, std::exception_ptr exception,
        http_error_handler_ref error_handler) const;
    [[nodiscard]] task<http_response> handle_exception(const http_request& request,
        std::exception_ptr exception, const context_services& services,
        http_error_handler_ref error_handler) const;

    [[nodiscard]] http_error_handler_ref error_handler_for(std::string_view path) const noexcept;
    explicit route_table(const compiled_route_plan& plan);
    void build_middleware_instances();

    const compiled_route_plan* plan_;
    // Worker-bound, indexed like the plan: the controller instance behind each
    // route, the middleware instances this table owns, and each frame bound to
    // its instance.
    std::vector<void*> handler_targets_;
    std::vector<void*> middleware_instances_;
    std::vector<route_middleware> middleware_frames_;
};

}  // namespace ruvia::detail

namespace ruvia {

[[nodiscard]] inline context make_route_context(const http_request& request,
    const detail::resolved_route& resolved, const detail::context_services& services) {
    const auto& route = resolved.route();
    const auto values = resolved.match().values();
    return detail::context_access::make(request, route.path(), route.param_names().data(),
        values.data(), values.size(), reinterpret_cast<std::uintptr_t>(&route), services);
}

}  // namespace ruvia

namespace ruvia::detail {

class app_configuration;

// Builds the process route plan once per run: every controller instance in
// `controllers` (one per registrar, registrar order) declares its routes, then
// `configuration` applies the app-wide fallbacks and middleware. Route
// conflicts and invalid registrations throw here, before any worker starts.
[[nodiscard]] compiled_route_plan_ptr_type compile_route_plan(
    std::span<const controller_registrar> registrars, const controller_store& controllers,
    const app_configuration& configuration);

// Startup builder shared by controller declarations and owned public routes.
class router final {
public:
    router();
    ~router();

    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

    void set_error_handler(http_error_handler_ref handler) noexcept;
    void set_not_found_handler(http_not_found_handler_ref handler) noexcept;
    // Copies normalized, de-duplicated prefixes into the plan, deepest first.
    // Callback views borrow their app_configuration owners until the plan retires.
    void set_prefix_error_handlers(std::span<const std::pair<std::string, callback_registration<http_error_handler>>> handlers);
    void set_prefix_not_found_handlers(std::span<const std::pair<std::string, callback_registration<http_not_found_handler>>> handlers);
    // Application-wide middleware, prepended to every route's chain. Each
    // descriptor becomes exactly one instance per worker, serving all routes.
    void set_global_middlewares(std::span<const controller_middleware_descriptor> descriptors);

    // Runs every registrar against its controller instance, in slot order.
    void declare_controllers(
        std::span<const controller_registrar> registrars, const controller_store& controllers);
    // The single registration path. `handler_target` must be the instance of
    // the controller currently declaring its routes. `method_token` is non-empty
    // only for an extension method, with `method` unknown.
    void register_endpoint(http_known_method method, std::string_view method_token, std::string_view path,
        route_endpoint endpoint, void* handler_target,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    // Explicit, owned handler slots use the same registration and compilation
    // path without a controller declaration in progress.
    void register_owned_endpoint(std::size_t slot, http_known_method method, std::string_view method_token,
        std::string_view path, route_endpoint endpoint,
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);

    [[nodiscard]] compiled_route_plan_ptr_type compile();

private:
    // A route's own (controller + route) middleware descriptors, as a slice of
    // route_middleware_descriptors_. Parallel to the plan's routes.
    struct route_middleware_range_type final {
        std::size_t offset_{0};
        std::size_t count_{0};
    };

    void validate_route_target(http_known_method method, std::string_view method_token, std::string_view path,
        const route_endpoint& endpoint) const;
    void validate_no_dynamic_route_conflict() const;
    [[nodiscard]] std::size_t append_route_middlewares(
        std::span<const controller_middleware_descriptor> controller_middlewares,
        std::span<const controller_middleware_descriptor> route_middlewares);
    void build_middleware_frames();
    void build_static_index();
    void build_static_method_mask();
    void build_extension_index();
    void build_dynamic_routes();
    void append_dynamic_param_name(route_entry& route, std::string_view name);
    void insert_dynamic(compiled_route_plan::dynamic_node_type& root, route_entry& route);
    void build_url_patterns();

    [[nodiscard]] static bool is_dynamic_path(std::string_view path) noexcept;

    compiled_route_plan_ptr_type plan_;
    std::vector<std::uint64_t> route_hashes_;
    std::vector<controller_middleware_descriptor> route_middleware_descriptors_;
    std::vector<route_middleware_range_type> route_middleware_ranges_;
    std::vector<controller_middleware_descriptor> global_middleware_descriptors_;
    std::size_t controller_slot_{0};
    void* controller_target_{nullptr};
};

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP2)

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

#endif

// Shared admission for HTTP/1 and in-process subrequests. The HTTP/1 session
// reads and frames messages; admission decides coding, client identity,
// expectations, rate limits, body ceilings and endpoint kind.

namespace ruvia::detail {

class http1_deferred_access_log;
class route_table;

// The deployment limits that shape admission. Servers build it from their
// options; a subrequest builds it from its parent (no rate limit and no
// target-origin check: the parent request already passed both).
struct http_request_admission_policy final {
    std::optional<std::size_t> max_stream_body_bytes_{};
    std::size_t max_buffered_body_bytes_{};
    bool apply_rate_limit_{true};
    // RFC 9110 §7.4: answer 421 when the connection cannot carry the target
    // (decide_http_target_origin against the services' transport facts).
    bool verify_target_origin_{true};
    [[nodiscard]] static http_request_admission_policy server(const http_server_options& options) noexcept;
};

struct http_request_admission_input final {
    const http_request& request_;
    const route_resolution& resolution_;
    // The parser or subrequest supplies the response-coding selection.
    http_response_coding_selection coding_;
    // Request arena for copied rejection diagnostics.
    // The protocol's Expect plan, when it defers the rejection to Web.
    const http_server_expectation_plan* expectation_{};
    // Start of the request for the access-log duration.
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
    // The arrival instant rate-limit decisions use, pinned by an owner that
    // replays requests at chosen instants (test_app); empty reads the steady
    // clock at each decision.
    std::optional<std::chrono::steady_clock::time_point> pinned_arrival_{};
};

// Effective request-body ceiling. A route may only tighten the deployment limit.
[[nodiscard]] protocol_byte_limit http_request_body_limit(const route_resolution& resolution,
    request_body_mode mode, const http_request_admission_policy& policy) noexcept;

// A server-layer rejection decided before any handler ran. The diagnostic is
// borrowed from static storage or the request arena.
class http_request_rejection final {
public:
    [[nodiscard]] static constexpr http_request_rejection error(http_error_info error) noexcept {
        return http_request_rejection(error, std::nullopt);
    }

    [[nodiscard]] static http_request_rejection rate_limited(rate_limit_rejection rejection) noexcept;

    [[nodiscard]] constexpr const http_error_info& error() const& noexcept RUVIA_LIFETIMEBOUND {
        return error_;
    }
    const http_error_info& error() const&& = delete;

    [[nodiscard]] constexpr const rate_limit_rejection* rate_limit() const& noexcept RUVIA_LIFETIMEBOUND {
        return rate_limit_ ? &*rate_limit_ : nullptr;
    }
    const rate_limit_rejection* rate_limit() const&& = delete;

private:
    constexpr http_request_rejection(http_error_info error, std::optional<rate_limit_rejection> rate_limit) noexcept
        : error_(error),
          rate_limit_(rate_limit) {}

    http_error_info error_;
    std::optional<rate_limit_rejection> rate_limit_;
};

// Answers a rejection through the application's error handler, adding the
// rate-limit headers when that is why the request was refused. The lazy task
// owns this snapshot; its diagnostic text still borrows static/request storage.
[[nodiscard]] task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request, http_request_rejection rejection, const context_services& services);

// Result of the synchronous first preparation pass. ready() is null only when
// a blocking-pool encode or the application error handler must be awaited
// through http_response_tail::finish_prepare().
class http_response_preparation final {
public:
    [[nodiscard]] const prepared_application_response* ready() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<prepared_application_response>(&state_);
    }
    const prepared_application_response* ready() const&& = delete;

    http_response_preparation(const http_response_preparation&) = default;
    http_response_preparation(http_response_preparation&&) = default;
    http_response_preparation& operator=(const http_response_preparation&) = delete;
    http_response_preparation& operator=(http_response_preparation&&) = delete;

private:
    friend class http_response_tail;

    struct deferred_type final {
        http_response_coding_policy policy_;
        buffered_response_recovery recovery_;
        // Without a saved recovery step, encode on the blocking pool first.
        std::optional<buffered_response_recovery_step> pending_step_;
    };

    explicit http_response_preparation(prepared_application_response ready) noexcept
        : state_(ready) {}
    explicit http_response_preparation(http_response_coding_policy policy) noexcept
        : state_(deferred_type{policy, {}, std::nullopt}) {}
    http_response_preparation(http_response_coding_policy policy, buffered_response_recovery recovery,
        buffered_response_recovery_step pending_step) noexcept
        : state_(deferred_type{policy, recovery, pending_step}) {}

    std::variant<prepared_application_response, deferred_type> state_;
};

// The post-response tail: final representation preparation (CORS,
// compression, 406/500 recovery through the application error handler) and
// the access-log record of the committed status.
class http_response_tail final {
public:
    http_response_tail(const http_request& request, http_response_coding_policy coding_policy,
        ip_address remote_address, ip_address client_address,
        std::chrono::steady_clock::time_point start) noexcept
        : request_(&request),
          coding_policy_(coding_policy),
          remote_address_(remote_address),
          client_address_(client_address),
          start_(start) {}

    [[nodiscard]] const http_response_coding_policy& coding_policy() const& noexcept RUVIA_LIFETIMEBOUND {
        return coding_policy_;
    }
    const http_response_coding_policy& coding_policy() const&& = delete;

    [[nodiscard]] request_io_metrics& io_metrics() const& noexcept RUVIA_LIFETIMEBOUND {
        return io_metrics_;
    }
    request_io_metrics& io_metrics() const&& = delete;

    // A generated terminal response must not inherit the negotiated coding
    // promise (and must not be transformed again).
    void disable_response_coding() noexcept {
        coding_policy_ = http_response_coding_policy::disabled();
    }
    void bind_addresses(ip_address remote_address, ip_address client_address) noexcept {
        remote_address_ = remote_address;
        client_address_ = client_address;
    }
    // The listener's Alt-Svc value (RFC 7838 §3), added to every prepared
    // response that does not carry its own; it borrows listener storage.
    void bind_alternative_service(std::string_view value) noexcept {
        alternative_service_ = value;
    }
    // The CORS policy the response streams and buffered responses of this
    // request apply; null when none does.
    [[nodiscard]] static const http_cors_policy* cors(const http_server_options& options) noexcept {
        return options.cors_.has_value() ? &*options.cors_ : nullptr;
    }

    // All arguments borrow the current request owner through completion. Only
    // an uncommitted response may enter. The common response (no blocking-pool
    // encode, no recovery) completes here without a coroutine frame.
    [[nodiscard]] http_response_preparation prepare(
        http_response& response, const http_server_options& options, const worker_ref& worker) const;

    // Completes a preparation whose ready() is null, with the arguments given
    // to prepare(). The application error handler runs at most once here.
    [[nodiscard]] task<prepared_application_response> finish_prepare(const http_response_preparation& preparation,
        http_response& response, const http_server_options& options, const route_table& routes, const context_services& services) const;

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response,
        const http_server_options& options, const worker_ref& worker, const route_table& routes, const context_services& services) const {
        const auto first = prepare(response, options, worker);
        if (const auto* ready = first.ready()) {
            co_return *ready;
        }
        co_return co_await finish_prepare(first, response, options, routes, services);
    }

    [[nodiscard]] task<prepared_application_response> prepare_or_finish(http_response& response,
        const http_server_options& options, const route_table& routes,
        const context_services& services) const {
        return prepare_or_finish(response, options, services.worker(), routes, services);
    }

    // Records a terminal outcome whose final status was committed. A single
    // null check when no access log is configured.
    void record_access(const access_log_sink& access_log, http_status_code status) const noexcept;

    // Keeps the record of a pipelined response held back for a coalesced
    // write whose head completes at output offset `head_end`; the log reports
    // it once that write commits the head. A no-op without an access log.
    void defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred, std::size_t head_end,
        http_status_code status) const;

private:
    const http_request* request_;
    http_response_coding_policy coding_policy_;
    ip_address remote_address_{};
    ip_address client_address_{};
    std::chrono::steady_clock::time_point start_;
    std::string_view alternative_service_;
    mutable request_io_metrics io_metrics_{};
};

enum class http_request_endpoint : std::uint8_t {
    // Rejections: answer rejection() through respond_http_request_rejection.
    rejection,
    // Admitted endpoints (resolved() is non-null for all but buffered).
    websocket,
    tunnel,
    response_stream,
    stream_body,
    buffered,
};

class http_request_admission final {
public:
    [[nodiscard]] http_request_endpoint endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] const http_request_rejection* rejection() const& noexcept RUVIA_LIFETIMEBOUND {
        return rejection_ ? &*rejection_ : nullptr;
    }
    const http_request_rejection* rejection() const&& = delete;

    // Null for unresolved requests (404/405/OPTIONS/document root).
    [[nodiscard]] const resolved_route* resolved() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolved_;
    }
    const resolved_route* resolved() const&& = delete;

    // CONNECT-UDP (RFC 9298) tunnel; meaningful for endpoint() == tunnel.
    [[nodiscard]] bool udp_tunnel() const noexcept {
        return udp_tunnel_;
    }

    // The connection's request services after applying admission policy.
    [[nodiscard]] const context_services& services() const& noexcept RUVIA_LIFETIMEBOUND {
        return services_;
    }
    const context_services& services() const&& = delete;

    [[nodiscard]] protocol_byte_limit body_limit() const noexcept {
        return body_limit_;
    }

    [[nodiscard]] static_file_selection_mode static_file_mode() const noexcept {
        return services_.precompressed_static_files() ? static_file_selection_mode::precompressed
                                                      : static_file_selection_mode::identity_only;
    }

    [[nodiscard]] http_response_tail& tail() & noexcept RUVIA_LIFETIMEBOUND {
        return tail_;
    }
    http_response_tail& tail() && = delete;

    http_request_admission(const http_request_admission& other) noexcept
        : tail_(other.tail_),
          services_(other.services_),
          rejection_(other.rejection_),
          resolved_(other.resolved_),
          body_limit_(other.body_limit_),
          endpoint_(other.endpoint_),
          udp_tunnel_(other.udp_tunnel_) {
        if (services_.io_metrics() == &other.tail_.io_metrics()) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }

    http_request_admission& operator=(const http_request_admission& other) noexcept {
        if (this != &other) {
            tail_ = other.tail_;
            services_ = other.services_;
            if (services_.io_metrics() == &other.tail_.io_metrics()) {
                services_.bind_io_metrics(&tail_.io_metrics());
            }
            rejection_ = other.rejection_;
            resolved_ = other.resolved_;
            body_limit_ = other.body_limit_;
            endpoint_ = other.endpoint_;
            udp_tunnel_ = other.udp_tunnel_;
        }
        return *this;
    }

    http_request_admission(http_request_admission&& other) noexcept
        : tail_(std::move(other.tail_)),
          services_(other.services_),
          rejection_(std::move(other.rejection_)),
          resolved_(other.resolved_),
          body_limit_(other.body_limit_),
          endpoint_(other.endpoint_),
          udp_tunnel_(other.udp_tunnel_) {
        if (services_.io_metrics() == &other.tail_.io_metrics()) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }

    http_request_admission& operator=(http_request_admission&& other) noexcept {
        if (this != &other) {
            tail_ = std::move(other.tail_);
            services_ = other.services_;
            if (services_.io_metrics() == &other.tail_.io_metrics()) {
                services_.bind_io_metrics(&tail_.io_metrics());
            }
            rejection_ = std::move(other.rejection_);
            resolved_ = other.resolved_;
            body_limit_ = other.body_limit_;
            endpoint_ = other.endpoint_;
            udp_tunnel_ = other.udp_tunnel_;
        }
        return *this;
    }

private:
    friend http_request_admission admit_http_request(const http_request_admission_input& input,
        const http_request_admission_policy& policy, const context_services& services);

    http_request_admission(http_response_tail tail, const context_services& services) noexcept
        : tail_(tail),
          services_(services) {
        if (services_.io_metrics() == nullptr) {
            services_.bind_io_metrics(&tail_.io_metrics());
        }
    }
    http_response_tail tail_;
    context_services services_;
    std::optional<http_request_rejection> rejection_;
    const resolved_route* resolved_{};
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http_request_endpoint endpoint_{http_request_endpoint::buffered};
    bool udp_tunnel_{false};
};

// Runs the admission policy once for one request. `services` are the
// connection's request services (stop token, interim output, trailers, ...).
// Admission borrows the input only during this synchronous call and owns its
// request-local service view; binding it never changes the connection's view.
[[nodiscard]] http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, const context_services& services);

// Admits a buffered in-memory request and dispatches it through the route
// table or the rejection handler.
[[nodiscard]] task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes,
    const http_request_admission_input& input, const http_request_admission_policy& policy,
    const context_services& services, http_response& response);

}  // namespace ruvia::detail

namespace ruvia {
class context;  // only used as context* in a type-erased bind thunk; web supplies the definition
}

namespace ruvia::detail {

// A normal response writer may reject a tunnel but may never establish one.
// Establishment transfers ownership through the protocol's acceptance driver.
inline void validate_response_stream_status(const http_request& request, const http_response& response) {
    if (request.known_method() == http_known_method::connect && response.status().is_successful()) {
        throw std::logic_error("a 2xx response to CONNECT establishes the tunnel; use accept_tunnel()");
    }
    if (response.status() == http_status::switching_protocols) {
        throw std::logic_error("a 101 response requires accept_tunnel() or accept_websocket()");
    }
}

// Plain forwarding functions, not coroutines: each returns the sink's own task,
// so a streamed chunk costs exactly the sink's frame and no wrapper frame.
template <typename sink>
task<stream_commit_result> response_stream_commit_thunk(void* target) {
    return static_cast<sink*>(target)->commit();
}

template <typename sink>
task<void> response_stream_write_thunk(void* target, std::string_view chunk) {
    return static_cast<sink*>(target)->write(chunk);
}

template <typename sink>
task<void> response_stream_end_thunk(void* target, std::span<const http_header_view> trailers) {
    return static_cast<sink*>(target)->end(trailers);
}

template <typename sink>
task<timer_sleep_result> response_stream_sleep_thunk(
    void* target, std::chrono::milliseconds duration, const stop_token& stop_token_value) {
    return static_cast<sink*>(target)->sleep(duration, stop_token_value);
}

template <typename sink>
bool response_stream_aborted_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->aborted();
}

template <typename sink>
void response_stream_bind_context_thunk(void* target, context* context_value) {
    static_cast<sink*>(target)->bind_context(context_value);
}

template <typename sink>
void response_stream_release_context_thunk(void* target) noexcept {
    static_cast<sink*>(target)->release_context();
}

template <typename sink>
bool response_stream_committed_thunk(void* target) noexcept {
    return static_cast<sink*>(target)->committed();
}

template <typename sink>
[[nodiscard]] response_stream_writer make_response_stream_writer(sink& sink_value) noexcept {
    return streaming_access::make_response_stream_writer(&sink_value, sink_value.output_interrupts(), &response_stream_commit_thunk<sink>, &response_stream_write_thunk<sink>,
        &response_stream_end_thunk<sink>, &response_stream_sleep_thunk<sink>,
        &response_stream_bind_context_thunk<sink>, &response_stream_release_context_thunk<sink>,
        &response_stream_committed_thunk<sink>, &response_stream_aborted_thunk<sink>);
}

class response_stream_completed final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;

    explicit constexpr response_stream_completed(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
};

class response_stream_aborted_before_commit final {
private:
    friend class response_stream_dispatch_result;
    constexpr response_stream_aborted_before_commit() noexcept = default;
};

class response_stream_aborted_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

private:
    friend class response_stream_dispatch_result;
    explicit constexpr response_stream_aborted_after_commit(http_status_code status) noexcept
        : status_(status) {}
    http_status_code status_;
};

// The handler failed after its head was already on the wire. The status is what
// the peer was told; the exception is why it will never receive the rest. The
// status alone cannot be reported to anyone -- it says 200 -- so the exception
// travels with it to the transport, which is the last owner able to report it.
class response_stream_failed_after_commit final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // Never null.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend class response_stream_dispatch_result;

    response_stream_failed_after_commit(http_status_code status, std::exception_ptr exception) noexcept
        : status_(status),
          exception_(std::move(exception)) {}

    http_status_code status_;
    std::exception_ptr exception_;
};

class response_stream_route_response final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_route_response(http_response response) noexcept
        : response_(std::move(response)) {}

    http_response response_;
};

class response_stream_recovered_failure final {
public:
    [[nodiscard]] http_response take_response() && noexcept {
        return std::move(response_);
    }

private:
    friend class response_stream_dispatch_result;

    explicit response_stream_recovered_failure(http_response response) noexcept
        : response_(std::move(response)) {}

    http_response response_;
};

// Each route terminal is one alternative. Commit-bearing alternatives own the
// exact wire status; response-bearing alternatives own the movable response.
// No separate outcome discriminator is needed.
class response_stream_dispatch_result final {
public:
    [[nodiscard]] static response_stream_dispatch_result make_completed(
        http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_completed(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_aborted_before_commit() noexcept {
        return response_stream_dispatch_result(response_stream_aborted_before_commit{});
    }

    [[nodiscard]] static response_stream_dispatch_result make_aborted_after_commit(http_status_code status) noexcept {
        return response_stream_dispatch_result(response_stream_aborted_after_commit(status));
    }

    [[nodiscard]] static response_stream_dispatch_result make_failed_after_commit(
        http_status_code status, std::exception_ptr exception) noexcept {
        return response_stream_dispatch_result(
            response_stream_failed_after_commit(status, std::move(exception)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_route_response(
        http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_route_response(std::move(response)));
    }

    [[nodiscard]] static response_stream_dispatch_result make_recovered_failure(
        http_response response) noexcept {
        return response_stream_dispatch_result(response_stream_recovered_failure(std::move(response)));
    }

    [[nodiscard]] const response_stream_completed* completed() const& noexcept {
        return std::get_if<response_stream_completed>(&value_);
    }
    const response_stream_completed* completed() const&& = delete;

    [[nodiscard]] const response_stream_aborted_before_commit* aborted_before_commit() const& noexcept {
        return std::get_if<response_stream_aborted_before_commit>(&value_);
    }
    const response_stream_aborted_before_commit* aborted_before_commit() const&& = delete;

    [[nodiscard]] const response_stream_aborted_after_commit* aborted_after_commit() const& noexcept {
        return std::get_if<response_stream_aborted_after_commit>(&value_);
    }
    const response_stream_aborted_after_commit* aborted_after_commit() const&& = delete;

    [[nodiscard]] const response_stream_failed_after_commit* failed_after_commit() const& noexcept {
        return std::get_if<response_stream_failed_after_commit>(&value_);
    }
    const response_stream_failed_after_commit* failed_after_commit() const&& = delete;

    [[nodiscard]] response_stream_route_response* route_response() & noexcept {
        return std::get_if<response_stream_route_response>(&value_);
    }
    response_stream_route_response* route_response() && = delete;

    [[nodiscard]] response_stream_recovered_failure* recovered_failure() & noexcept {
        return std::get_if<response_stream_recovered_failure>(&value_);
    }
    response_stream_recovered_failure* recovered_failure() && = delete;

    [[nodiscard]] std::optional<http_status_code> committed_status() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = aborted_after_commit()) {
            return value->status();
        }
        if (const auto* value = failed_after_commit()) {
            return value->status();
        }
        return std::nullopt;
    }

private:
    using value_type = std::variant<response_stream_completed, response_stream_aborted_before_commit,
        response_stream_aborted_after_commit, response_stream_failed_after_commit,
        response_stream_route_response, response_stream_recovered_failure>;

    template <typename alternative_type>
    explicit response_stream_dispatch_result(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

// Only called once sink_value.committed() holds.
template <typename sink>
[[nodiscard]] http_status_code committed_response_stream_status(const sink& sink_value) {
    return sink_value.commit_plan()->response_status();
}

struct response_stream_no_peer_abort final {
    [[nodiscard]] constexpr bool operator()() const noexcept {
        return false;
    }
};

// Drives a response-stream route over an already-constructed sink. Connection
// persistence remains a transport concern when a buffered response is returned.
template <typename sink, typename peer_aborted_type = response_stream_no_peer_abort>
task<response_stream_dispatch_result> dispatch_response_stream_with(sink& sink_value, const route_table& routes_value,
    const http_request& request, const resolved_route& route,
    const context_services& services, peer_aborted_type peer_aborted = {}) {
    auto response_stream = make_response_stream_writer(sink_value);

    std::exception_ptr exception;
    try {
        auto result_value = co_await routes_value.dispatch_response_stream(
            request, route, response_stream, services);
        if (services.connection_aborted() || services.request_reset() || peer_aborted()) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_aborted_after_commit(
                committed_response_stream_status(sink_value));
        }
        if (!result_value.has_value()) {
            if (!sink_value.committed()) {
                throw std::logic_error("handled response stream has no committed protocol plan");
            }
            co_return response_stream_dispatch_result::make_completed(
                committed_response_stream_status(sink_value));
        }
        if (sink_value.committed()) {
            throw std::logic_error("buffered stream result followed a committed response head");
        }
        co_return response_stream_dispatch_result::make_route_response(std::move(*result_value));
    } catch (const inbound_transport_error&) {
        if (!services.request_reset() && (services.connection_aborted() || peer_aborted())) {
            if (!sink_value.committed()) {
                co_return response_stream_dispatch_result::make_aborted_before_commit();
            }
            co_return response_stream_dispatch_result::make_aborted_after_commit(
                committed_response_stream_status(sink_value));
        }
        exception = std::current_exception();
    } catch (...) {
        exception = std::current_exception();
    }
    if (services.connection_aborted() || services.request_reset()) {
        auto result = sink_value.committed()
                          ? response_stream_dispatch_result::make_aborted_after_commit(
                                committed_response_stream_status(sink_value))
                          : response_stream_dispatch_result::make_aborted_before_commit();
        // The transport is already closed, but a real callback failure still
        // belongs to its configured outlet. Snapshot before invoking user code.
        const auto failure = services.connection_failure();
        const auto remote = services.remote_address();
        failure.invoke(remote, exception);
        co_return result;
    }

    if (sink_value.committed()) {
        // Past the point of no return: the status cannot be changed and no
        // error body can be appended, so the exception is handed to the
        // transport rather than dropped with the connection.
        co_return response_stream_dispatch_result::make_failed_after_commit(
            committed_response_stream_status(sink_value), std::move(exception));
    }
    auto response = co_await routes_value.handle_exception(request, exception, services);
    co_return response_stream_dispatch_result::make_recovered_failure(std::move(response));
}

}  // namespace ruvia::detail

// listener_dispatch::request_stream: what a request stream connection resolves
// every request to in place of the route table, and the one run of the
// application's request_stream callback every transport reaches through it.

namespace ruvia::detail {

class route_entry;

// A request stream route runs no middleware and declares no body limit. A
// tunnel request -- CONNECT, extended CONNECT, or an HTTP/1.1 Upgrade without
// content -- resolves to an upgrade route whose callback commits the tunnel
// itself (accept_tunnel()); any other request to a response stream route with
// a streamed body.
[[nodiscard]] route_resolution resolve_request_stream_route(bool tunnel) noexcept;

// Whether `route` is the tunnel route resolve_request_stream_route() yields.
[[nodiscard]] bool is_request_stream_tunnel_route(const route_entry& route) noexcept;

// Whether an HTTP/1 request of a request stream connection is a tunnel
// request; `content` is whether it carries request content.
[[nodiscard]] bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept;

// Runs the connection's callback for a tunnel request in the context its
// upgrade route built. `accept` commits the success head and establishes the
// tunnel; a refusal writes through the protocol's ordinary response stream,
// which the upgrade driver must bind before dispatching this context.
[[nodiscard]] task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept,
    const request_stream_websocket_accept* accept_websocket = nullptr);

}  // namespace ruvia::detail

namespace ruvia::detail {

// The single writer of every HTTP/1 final head that hands the connection to
// another protocol: a WebSocket or CONNECT-UDP 101 and a CONNECT 2xx. RFC 9110
// section 7.8: when the head is 101 (Switching Protocols) and the request
// carried Expect: 100-continue, the HTTP-owned
// http_request_expectations::continue_before_switching_protocols() decision
// puts a 100 (Continue) head before it in the same write, even when no content
// follows. Any other status is written alone. `head` is the complete encoded
// final head; it must stay valid until the returned task completes.
template <typename stream_type>
[[nodiscard]] task<std::error_code> write_http1_upgrade_response_head(stream_type& stream,
    const http_request_expectations& expectations, http_status_code status, std::string_view head,
    connection_scanner::entry& scanner_entry) {
    constexpr std::size_t continue_head_capacity = 64;
    std::array<char, continue_head_capacity> continue_bytes;
    std::string_view continue_head;
    if (status == http_status::switching_protocols && expectations.continue_before_switching_protocols()) {
        const auto prepared = prepare_http1_interim_response_head(
            http_interim_response_head(http_status::continue_value), std::span<char>(continue_bytes));
        const auto* prepared_head = std::get_if<prepared_http1_interim_response>(&prepared);
        if (prepared_head == nullptr) {
            throw std::logic_error("cannot prepare HTTP/1 100 Continue head");
        }
        continue_head = prepared_head->head();
    }
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(continue_head), asio::buffer(head)};
    const connection_scanner::entry::write_scope write_stall_clock(scanner_entry);
    const auto written = co_await ruvia::async_asio<std::size_t>([&stream, &buffers, &scanner_entry](auto handler) {
        asio::async_write(stream, buffers, connection_write_progress(scanner_entry), std::move(handler));
    });
    co_return written.error_code();
}

// The context retains header storage through handshake construction. The
// protocol plan copies the views before this temporary pointer table is freed.
[[nodiscard]] inline inline_http_header_views<> websocket_response_headers(context& context_value) {
    return inline_http_header_views<>(
        context_access::handshake_headers(context_value));
}

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
// The shared HTTP/1 upgrade-head writer applies the request `expectations`
// (100 Continue before 101, RFC 9110 section 7.8).
template <typename stream_type>
task<std::error_code> write_websocket_handshake(stream_type& stream,
    const websocket_server_handshake& handshake, const http_request_expectations& expectations, connection_scanner::entry& scanner_entry) {
    http_response_head_buffer head;
    handshake.for_each_response_part([&](std::string_view part) {
        head.append(part);
    });
    co_return co_await write_http1_upgrade_response_head(
        stream, expectations, http_status::switching_protocols, head.view(), scanner_entry);
}

}  // namespace ruvia::detail

// The shared shape of every upgrade-capable route (CONNECT / CONNECT-UDP
// tunnels and WebSocket upgrades) across HTTP/1 and HTTP/2: run the
// application chain with a terminal that commits the upgrade and
// owns the resulting session; once a session exists it is always finished,
// otherwise a handler failure propagates and an uncommitted response falls
// back to the ordinary buffered tail.

namespace ruvia::detail {

[[nodiscard]] inline websocket_connection_options make_websocket_session_connection_options(
    const http_server_options& options, websocket_compression negotiated,
    const websocket_deflate_config& deflate, protocol_byte_limit receive_limit) noexcept {
    // Directions without context takeover borrow the worker's codecs, which
    // outlive every connection, so they allocate no codec state here.
    return websocket_connection_options{
        .receive_message_limit_ = receive_limit,
        .compression_ = negotiated,
        .deflater_ = deflate.deflater_,
        .shared_deflate_ = &options.codecs_->websocket_deflate(),
    };
}

// Selects the refusal response the server sends after the callback returns.
[[nodiscard]] inline websocket_rejection reject_request_stream_websocket(
    context& context_value, const websocket_handshake_failure& failure) {
    const auto error = failure.protocol_error();
    auto response = context_value.error(copy_http_protocol_error_info(error));
    failure.apply_required_response_headers(response);
    const websocket_rejection rejection{response.status(), error.code()};
    context_access::set_response(context_value, std::move(response));
    return rejection;
}

struct websocket_accept_access final {
    [[nodiscard]] static websocket_accept_result accepted(websocket& session) noexcept {
        return websocket_accept_result(session);
    }
    [[nodiscard]] static websocket_accept_result rejected(websocket_rejection rejection) noexcept {
        return websocket_accept_result(rejection);
    }
};

// `datagrams` is the route's http_tunnel_route_config::datagrams_, which
// CONNECT-UDP (`udp`) always sets.
[[nodiscard]] inline http_response prepare_tunnel_upgrade_response_head(
    context& context_value, http_protocol_version version, bool udp, bool datagrams) {
    auto head_response = context_access::streaming_head(context_value);
    if (udp) {
        auto negotiated = prepare_http_connect_udp_response(std::move(head_response), version);
        if (negotiated.index() != 0) {
            throw std::invalid_argument("invalid CONNECT-UDP response metadata");
        }
        head_response = std::move(std::get<0>(negotiated));
    } else if (datagrams && head_response.status().is_successful()) {
        // RFC 9297 section 3.4: the route's protocol uses the Capsule Protocol,
        // so its successful response says so with exactly one ?1.
        head_response.header_stable_view("Capsule-Protocol", "?1");
    }
    return head_response;
}

// `establish(context&) -> task<void>` commits the upgrade response head,
// emplaces `session` and runs the route's stream handler on it.
// `finish(session_type&, std::exception_ptr) -> task<void>` closes an
// established session and reports a handler failure; it always runs once a
// session exists, before any handler exception is surfaced.
// Returns a buffered response only when the application answered without
// upgrading; an established or abandoned upgrade returns std::nullopt.
template <typename session_type, typename establish_type, typename finish_type>
[[nodiscard]] task<std::optional<http_response>> run_http_upgrade_route(
    const route_table& routes, const http_request& request, const resolved_route& resolved,
    const context_services& services, std::optional<session_type>& session, establish_type& establish,
    finish_type& finish) {
    const auto terminal = callback_access::bind<task<void>(context&)>(establish);
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await routes.dispatch_upgrade(request, resolved, terminal, services);
    } catch (...) {
        failure = std::current_exception();
    }
    if (session.has_value()) {
        co_await finish(*session, failure);
        co_return std::nullopt;
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return buffered;
}

template <typename stream_type>
class websocket_upgrade_commit final {
public:
    static constexpr bool native_stream = websocket_session_connection<stream_type>::native_stream;
    using stream_argument_type = typename websocket_session_connection<stream_type>::stream_argument_type;

    websocket_upgrade_commit(stream_argument_type stream, websocket_compression compression,
        std::string_view subprotocol)
        : stream_(hold_stream(std::forward<stream_argument_type>(stream))),
          compression_(compression) {
        try {
            subprotocol_.assign(subprotocol);
        } catch (...) {
            abort();
            throw;
        }
    }
    [[nodiscard]] stream_argument_type release_stream() noexcept {
        if constexpr (native_stream) {
            return *stream_;
        } else {
            return std::move(stream_);
        }
    }

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::string&& release_subprotocol() noexcept {
        return std::move(subprotocol_);
    }

    void abort() noexcept {
        if constexpr (native_stream) {
            websocket_session_connection<stream_type>::close_stream(*stream_);
        } else {
            stream_.abort();
        }
    }

private:
    using held_stream_type = std::conditional_t<native_stream, stream_type*, stream_type>;

    [[nodiscard]] static held_stream_type hold_stream(stream_argument_type stream) noexcept(native_stream) {
        if constexpr (native_stream) {
            return &stream;
        } else {
            return std::move(stream);
        }
    }

    held_stream_type stream_;
    websocket_compression compression_{};
    std::string subprotocol_;
};

// A committed upgrade, or the refusal its handshake check selected.
template <typename stream_type>
using websocket_upgrade_attempt = std::variant<websocket_upgrade_commit<stream_type>, websocket_rejection>;

template <typename transport_type, typename commit_upgrade_type, typename commit_websocket_type>
struct tunnel_upgrade_session_operation final {
    using session_type = http_tunnel_session<transport_type>;

    const http_request& request_;
    const resolved_route& resolved_;
    const tunnel_route_endpoint& endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    inbound_buffer_account& inbound_;
    bool udp_;
    std::string_view pending_;
    commit_upgrade_type& commit_upgrade_;
    commit_websocket_type& commit_websocket_;
    std::optional<session_type>& session_;
    http_tunnel_route_config config_{endpoint_.config()};
    std::optional<websocket_session_connection<transport_type>> websocket_session_{};
    struct websocket_borrow final {
        explicit websocket_borrow(websocket_session_connection<transport_type>& connection)
            : value_(make_websocket_facade(connection)) {}
        websocket value_;
    };
    std::optional<websocket_borrow> websocket_{};

    // Commits the upgrade head the context's draft describes and opens the
    // session over the transport that head hands over.
    [[nodiscard]] task<void> establish(context& context_value) {
        auto head_response = prepare_tunnel_upgrade_response_head(
            context_value, request_.protocol_version(), udp_, config_.datagrams_);
        std::optional<transport_type> transport;
        if constexpr (requires { commit_upgrade_(context_value, head_response).operator co_await(); }) {
            transport.emplace(co_await commit_upgrade_(context_value, head_response));
        } else {
            transport.emplace(commit_upgrade_(context_value, head_response));
        }
        try {
            session_.emplace(std::move(*transport), services_.worker(), &inbound_,
                config_.datagrams_, pending_);
        } catch (...) {
            transport->abort();
            throw;
        }
        // A synchronous commit leaves no co_await in this instantiation;
        // co_return keeps the body a coroutine for every transport.
        co_return;
    }

    [[nodiscard]] task<http_tunnel*> accept_request_stream_tunnel(context& context_value, http_tunnel_route_config config) {
        config_ = route_endpoint::validate_tunnel_config(
            !request_.protocol().empty() || request_.known_method() != http_known_method::connect, udp_, config);
        co_await establish(context_value);
        scanner_entry_.enter_long_lived();
        co_return &session_->tunnel();
    }

    [[nodiscard]] task<websocket_accept_result> accept_request_stream_websocket(
        context& context_value, const websocket_route_endpoint& websocket_endpoint) {
        const auto headers = websocket_response_headers(context_value);
        auto attempt = co_await commit_websocket_(context_value, websocket_endpoint, headers);
        if (const auto* rejection = std::get_if<websocket_rejection>(&attempt)) {
            co_return websocket_accept_access::rejected(*rejection);
        }
        auto* committed = std::get_if<websocket_upgrade_commit<transport_type>>(&attempt);
        try {
            websocket_session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_,
                websocket_endpoint.lifecycle(),
                protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_),
                pending_,
                make_websocket_session_connection_options(options_, committed->compression(), websocket_endpoint.deflate(),
                    websocket_endpoint.message_limit(options_.server_.max_websocket_message_bytes_)),
                committed->release_subprotocol(), &inbound_);
        } catch (...) {
            committed->abort();
            throw;
        }
        websocket_.emplace(*websocket_session_);
        scanner_entry_.enter_long_lived();
        co_return websocket_accept_access::accepted(websocket_->value_);
    }

    [[nodiscard]] task<void> operator()(context& context_value) {
        if (is_request_stream_tunnel_route(resolved_.route())) {
            auto accept = [this, &context_value](http_status_code, http_tunnel_route_config config) {
                return accept_request_stream_tunnel(context_value, config);
            };
            const auto accept_ref = callback_access::bind<task<http_tunnel*>(http_status_code, http_tunnel_route_config)>(accept);
            auto accept_websocket = [this, &context_value](const websocket_route_endpoint& endpoint) {
                return accept_request_stream_websocket(context_value, endpoint);
            };
            const auto websocket_accept_ref =
                callback_access::bind<task<websocket_accept_result>(const websocket_route_endpoint&)>(accept_websocket);
            std::exception_ptr failure;
            try {
                co_await serve_request_stream_tunnel(context_value, accept_ref, &websocket_accept_ref);
            } catch (...) {
                failure = std::current_exception();
            }
            // Retire borrowed operations before draining the owned connection,
            // exactly as the route handler's stack facade does.
            websocket_.reset();
            if (websocket_session_) {
                co_await finish_websocket_session(*websocket_session_, failure,
                    options_.connection_failure_, services_.remote_address());
            } else if (failure) {
                std::rethrow_exception(failure);
            }
            co_return;
        }
        co_await establish(context_value);
        co_await invoke_tunnel_handler(
            *session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_tunnel_session(session_value, std::move(failure),
            options_.connection_failure_, services_.remote_address(),
            scanner_entry_, config_.peer_transport_fin_timeout_);
    }
};

template <typename transport_type, typename commit_upgrade_type, typename commit_websocket_type>
[[nodiscard]] task<std::optional<http_response>> execute_tunnel_upgrade_session(
    const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, const http_server_options& options,
    connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound, bool udp,
    std::string_view pending, commit_upgrade_type commit_upgrade, commit_websocket_type commit_websocket) {
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    using operation_type = tunnel_upgrade_session_operation<transport_type, commit_upgrade_type, commit_websocket_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .request_ = request,
        .resolved_ = resolved,
        .endpoint_ = endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .udp_ = udp,
        .pending_ = pending,
        .commit_upgrade_ = commit_upgrade,
        .commit_websocket_ = commit_websocket,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(
        routes, request, resolved, services, session, operation, operation);
}

template <typename stream_type, typename commit_upgrade_type>
struct websocket_upgrade_session_operation final {
    using session_type = websocket_session_connection<stream_type>;

    const resolved_route& resolved_;
    const websocket_route_endpoint& websocket_endpoint_;
    const context_services& services_;
    const http_server_options& options_;
    connection_scanner::entry& scanner_entry_;
    inbound_buffer_account& inbound_;
    std::string_view pending_frames_;
    commit_upgrade_type& commit_upgrade_;
    std::optional<session_type>& session_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        const auto response_headers_value = websocket_response_headers(context_value);
        std::optional<websocket_upgrade_commit<stream_type>> committed;
        if constexpr (requires { commit_upgrade_(context_value, response_headers_value).operator co_await(); }) {
            committed.emplace(co_await commit_upgrade_(context_value, response_headers_value));
        } else {
            committed.emplace(commit_upgrade_(context_value, response_headers_value));
        }
        try {
            session_.emplace(committed->release_stream(), services_.worker(), scanner_entry_,
                websocket_endpoint_.lifecycle(),
                protocol_byte_limit::limited(options_.server_.max_websocket_message_bytes_),
                pending_frames_,
                make_websocket_session_connection_options(
                    options_, committed->compression(), websocket_endpoint_.deflate(),
                    websocket_endpoint_.message_limit(options_.server_.max_websocket_message_bytes_)),
                committed->release_subprotocol(), &inbound_);
        } catch (...) {
            committed->abort();
            throw;
        }
        co_await invoke_websocket_handler(
            *session_, scanner_entry_, resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_websocket_session(session_value, std::move(failure),
            options_.connection_failure_, services_.remote_address());
    }
};

template <typename stream_type, typename commit_upgrade_type>
[[nodiscard]] task<std::optional<http_response>> execute_websocket_upgrade_session(
    const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& services, const http_server_options& options,
    connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound,
    std::string_view pending_frames, commit_upgrade_type commit_upgrade) {
    const auto& websocket_endpoint = *resolved.route().endpoint().websocket();
    using operation_type = websocket_upgrade_session_operation<stream_type, commit_upgrade_type>;
    std::optional<typename operation_type::session_type> session;
    operation_type operation{
        .resolved_ = resolved,
        .websocket_endpoint_ = websocket_endpoint,
        .services_ = services,
        .options_ = options,
        .scanner_entry_ = scanner_entry,
        .inbound_ = inbound,
        .pending_frames_ = pending_frames,
        .commit_upgrade_ = commit_upgrade,
        .session_ = session,
    };
    co_return co_await run_http_upgrade_route(
        routes, request, resolved, services, session, operation, operation);
}

}  // namespace ruvia::detail

namespace ruvia {

class context;

namespace detail {

class response_stream_state final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return commit_plan() != nullptr;
    }

    [[nodiscard]] bool ended() const noexcept {
        return std::holds_alternative<ended_type>(state_);
    }

    [[nodiscard]] bool aborted() const noexcept {
        return std::holds_alternative<aborted_before_commit_type>(state_) ||
               std::holds_alternative<aborted_after_commit_type>(state_);
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        if (const auto* value = std::get_if<body_open_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<ended_type>(&state_)) {
            return &value->plan_;
        }
        if (const auto* value = std::get_if<aborted_after_commit_type>(&state_)) {
            return &value->plan_;
        }
        return nullptr;
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    void bind_context(context* context_value) {
        if (!std::holds_alternative<unbound_type>(state_)) {
            throw std::logic_error("response stream context is already bound");
        }
        if (context_value == nullptr) {
            throw std::invalid_argument("response stream context binding is incomplete");
        }
        state_.emplace<bound_type>(context_value);
    }

    void release_context() noexcept {
        if (std::holds_alternative<bound_type>(state_)) {
            state_.emplace<detached_type>();
        }
    }

    [[nodiscard]] context& bound_context() const {
        const auto* bound = std::get_if<bound_type>(&state_);
        if (bound == nullptr) {
            if (committed()) {
                throw std::logic_error("response stream is already committed");
            }
            throw std::logic_error("response stream context is not bound");
        }
        return *bound->context_;
    }

    void mark_committed(http_response_stream_commit_plan plan) {
        if (committed() || aborted()) {
            throw std::logic_error("response stream is already committed");
        }
        switch (plan.head_disposition()) {
            case http_response_stream_head_disposition::body_open:
                state_.emplace<body_open_type>(plan);
                break;
            case http_response_stream_head_disposition::message_ended:
                state_.emplace<ended_type>(plan);
                break;
        }
    }

    void mark_ended() {
        if (ended()) {
            return;
        }
        if (auto* value = std::get_if<body_open_type>(&state_)) {
            auto plan = value->plan_;
            state_.emplace<ended_type>(plan);
            return;
        }
        throw std::logic_error("response stream is not committed");
    }

    void mark_aborted() noexcept {
        if (aborted()) {
            return;
        }
        if (const auto* committed_plan = commit_plan()) {
            // A body-suppressed commit is already logically ended, but its head
            // can still fail during transport delivery. Preserve that commit.
            const auto plan = *committed_plan;
            state_.emplace<aborted_after_commit_type>(plan);
            return;
        }
        state_.emplace<aborted_before_commit_type>();
    }

    void ensure_body_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        if (!std::holds_alternative<body_open_type>(state_)) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    // Trailers are optional metadata (RFC 9110 6.5): framing that cannot carry
    // them discards them, so only a status that can never carry content is a
    // handler error here. The caller writes trailers only if trailers_available().
    void ensure_trailers_allowed() const {
        if (aborted()) {
            throw std::logic_error("response stream is aborted");
        }
        if (ended()) {
            throw std::logic_error("response stream is already ended");
        }
        const auto* plan = commit_plan();
        if (plan == nullptr || !plan->status_permits_trailers()) {
            throw std::logic_error("response status does not permit trailers");
        }
    }

private:
    struct unbound_type final {};

    struct detached_type final {};

    struct bound_type final {
        explicit bound_type(context* bound_context) noexcept
            : context_(bound_context) {}

        context* context_;
    };

    struct body_open_type final {
        explicit body_open_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct ended_type final {
        explicit ended_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    struct aborted_before_commit_type final {};

    struct aborted_after_commit_type final {
        explicit aborted_after_commit_type(http_response_stream_commit_plan commit_plan) noexcept
            : plan_(commit_plan) {}

        http_response_stream_commit_plan plan_;
    };

    using state_type = std::variant<unbound_type, bound_type, detached_type, body_open_type, ended_type,
        aborted_before_commit_type, aborted_after_commit_type>;

    state_type state_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

// Owns the response-coding lifecycle of HTTP/1 and HTTP/2 stream sinks.
// The transport only delivers encoded slices; it never reconstructs whether a
// coding was selected, whether the body is suppressed, or whether an encoder
// exists. The encoder itself is borrowed from the worker's codecs for the
// stream's lifetime, not constructed per response.
class http_streaming_response_compression final {
public:
    // Application input encoded per step. It bounds the encoded buffer and the
    // codec work between two transport writes independently of the size of
    // one application write.
    static constexpr std::size_t input_slice_bytes = 16 * 1024;

    // `compression` is the server's compression policy, or nullptr when the
    // server compresses nothing.
    http_streaming_response_compression(worker_codecs& codecs,
        http_response_coding_selection selection, const compression_config* compression) noexcept
        : codecs_(codecs),
          selection_(selection),
          compression_(compression) {}
    http_streaming_response_compression(worker_codecs&&,
        http_response_coding_selection, const compression_config*) = delete;

    http_streaming_response_compression(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression& operator=(const http_streaming_response_compression&) = delete;
    http_streaming_response_compression(http_streaming_response_compression&&) = delete;
    http_streaming_response_compression& operator=(http_streaming_response_compression&&) = delete;

    // Applies the representation headers before the protocol-owned stream head
    // is committed. A forbidden identity fallback, including a negotiation the
    // selection already reports as failed, is rejected here, while the encoder
    // itself is delayed until activate() has a final body plan.
    void prepare(const http_request& request, http_response& response) {
        if (!std::holds_alternative<unprepared_type>(state_)) {
            throw std::logic_error("streaming response compression is already prepared");
        }
        const auto request_method = request.known_method();
        const auto decision = prepare_response_compression(selection_, request_method, response,
            http_response_compression_source::stream,
            compression_ != nullptr ? http_response_coding_availability::identity_and_compression
                                    : http_response_coding_availability::identity_only);
        if (decision != http_response_compression_decision::encode || !stream_length_allows_encoding(response)) {
            if (http_response_coding_fallback_forbidden(selection_, request_method, response, &request)) {
                throw http_error(http_not_acceptable_coding_error());
            }
            state_.emplace<inactive_type>();
            return;
        }

        response.apply_content_encoding(http_content_coding_token(selection_.coding()));
        state_.emplace<pending_type>();
    }

    // Binds encoder ownership to the protocol body plan. Body-suppressed
    // responses retain their selected representation metadata but never create
    // an encoder for bytes that the protocol will not send.
    void activate(http_response_body_plan body_plan) {
        if (std::holds_alternative<inactive_type>(state_)) {
            return;
        }
        const auto* pending = std::get_if<pending_type>(&state_);
        if (pending == nullptr) {
            throw std::logic_error("streaming response compression is not pending");
        }
        if (body_plan.body_suppressed()) {
            state_.emplace<inactive_type>();
            return;
        }
        state_.emplace<active_type>(codecs_.acquire_stream_encoder(selection_.coding()));
    }

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<active_type>(state_);
    }

    // A stream-head transaction may prepare representation metadata before the
    // protocol layer accepts the head. If that later preparation fails, the
    // sink is terminal before any body byte can be retried; keep the encoder
    // lifecycle in the same terminal state instead of leaving pending/inactive
    // behind for a second commit attempt.
    void abort() noexcept {
        encoded_chunk_.clear();
        state_.emplace<failed_type>();
    }

    // The one slicing loop of both protocols: encodes one application write in
    // input_slice_bytes steps and hands every non-empty encoded slice to
    // `deliver` (the sink's framed write, returning task<void>). Only the last
    // slice flushes, so each application write ends with exactly one codec
    // flush, which keeps SSE events prompt without flushing every slice.
    template <typename deliver_type>
    task<void> write(std::string_view chunk, deliver_type deliver) {
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            const auto count = (std::min)(input_slice_bytes, chunk.size() - offset);
            const auto slice = chunk.substr(offset, count);
            offset += count;
            encode_slice(slice, offset == chunk.size());
            if (!encoded_chunk_.empty()) {
                co_await deliver(std::string_view(encoded_chunk_));
            }
            release_output();
        }
    }

    void finish() {
        encoded_chunk_.clear();
        if (std::holds_alternative<finished_type>(state_)) {
            return;
        }
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            throw std::logic_error("streaming response encoder is not active");
        }
        try {
            active_state->lease_.encoder().finish(encoded_chunk_);
            state_.emplace<finished_type>();
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    [[nodiscard]] std::string_view output() const& noexcept {
        return encoded_chunk_;
    }
    std::string_view output() const&& = delete;

    // A sent chunk no longer has a transport borrower. Keep ordinary stream
    // buffers reusable without retaining unusually large writes for the life
    // of a long-running handler.
    void release_output() {
        constexpr std::size_t retained_bytes = 64 * 1024;
        http_clear_string_retaining_small(encoded_chunk_, retained_bytes);
    }

private:
    struct unprepared_type final {};
    // Identity output and protocol-suppressed bodies both need no encoder.
    struct inactive_type final {};
    struct pending_type final {};
    struct finished_type final {};
    struct failed_type final {};

    struct active_type final {
        explicit active_type(stream_encoder_lease lease) noexcept
            : lease_(std::move(lease)) {}

        stream_encoder_lease lease_;
    };

    using state_type = std::variant<unprepared_type, inactive_type, pending_type, finished_type, failed_type, active_type>;

    // A length the head declares sizes the stream like a buffered body; an
    // undeclared length leaves the decision to the negotiated coding.
    [[nodiscard]] bool stream_length_allows_encoding(const http_response& response) const noexcept {
        const auto declared = declared_content_length(response);
        return !declared.has_value() || declared_length_permits_encoding(selection_, *compression_, *declared);
    }

    void encode_slice(std::string_view input, bool flush) {
        auto* active_state = std::get_if<active_type>(&state_);
        if (active_state == nullptr) {
            encoded_chunk_.clear();
            throw std::logic_error("streaming response encoder is not active");
        }
        encoded_chunk_.clear();
        try {
            active_state->lease_.encoder().write(input, encoded_chunk_, flush);
        } catch (...) {
            fail_encoder(*active_state);
            throw;
        }
    }

    // A codec that failed mid-stream is never returned to the worker pool.
    void fail_encoder(active_type& active_state) noexcept {
        active_state.lease_.discard();
        abort();
    }

    worker_codecs& codecs_;
    http_response_coding_selection selection_;
    const compression_config* compression_;
    std::string encoded_chunk_;
    state_type state_;
};

}  // namespace ruvia::detail

namespace ruvia {
class context;
}

namespace ruvia::detail {

// Inactivity belongs to the connection; request bodies and output waits have
// independent per-stream deadlines.
[[nodiscard]] inline connection_scanner::phase multiplexed_inactivity_phase(bool peer_settings_received,
    bool header_block_in_progress, bool stream_active, bool tunnel_active) noexcept {
    if (!peer_settings_received || header_block_in_progress) {
        return connection_scanner::phase::reading_initial;
    }
    if (tunnel_active) {
        return connection_scanner::phase::long_lived;
    }
    return stream_active ? connection_scanner::phase::idle : connection_scanner::phase::awaiting_stream;
}

class multiplexed_request_body_deadline final {
public:
    void open(std::int64_t now_ms) noexcept {
        open_ = true;
        opened_ms_ = now_ms;
        progress_ms_ = now_ms;
    }
    void progress(std::int64_t now_ms) noexcept {
        progress_ms_ = now_ms;
    }
    void close() noexcept {
        open_ = false;
    }
    [[nodiscard]] bool overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return open_ &&
               ((inactivity.has_value() && now_ms - progress_ms_ >= inactivity->value().count()) ||
                   (completion.has_value() && now_ms - opened_ms_ >= completion->value().count()));
    }

private:
    std::int64_t opened_ms_{0};
    std::int64_t progress_ms_{0};
    bool open_{false};
};

// A timeout drain must end the connection at its bound; an ordinary drain only
// finalizes GOAWAY. Protocol owners keep their own wake/close ordering.
class multiplexed_drain_timer final {
public:
    void request_timeout() noexcept {
        timeout_drain_ = true;
    }
    [[nodiscard]] bool needed(bool terminated, bool final_boundary) const noexcept {
        return !timer_.registered() && !terminated && (!final_boundary || timeout_drain_);
    }
    void mark_expired() noexcept {
        expired_ = timeout_drain_;
    }
    [[nodiscard]] bool expired() const noexcept {
        return expired_;
    }
    void cancel() noexcept {
        timer_.cancel();
    }

    template <typename duration_type, typename expired_type, typename failure_type>
    void arm(const worker_ref& worker_value, duration_type bound,
        expired_type on_expired, failure_type on_failure) noexcept {
        try {
            const auto status = worker_value.schedule_timer(timer_,
                deadline_after(bound), [on_expired](worker_timer_outcome outcome) {
                    // Cancellation belongs to worker shutdown, which closes the session.
                    if (outcome == worker_timer_outcome::expired) {
                        on_expired();
                    }
                });
            if (status == worker_timer_schedule_status::worker_stopping) {
                on_expired();
            }
        } catch (...) {
            on_failure();
        }
    }

private:
    bool timeout_drain_{false};
    bool expired_{false};
    // Unregister before the state captured by its completion can retire.
    worker_timer_registration timer_;
};

// Connection-owned terminal state shared by every Web capability derived from
// one multiplexed HTTP/2 or HTTP/3 session. Stream waits that must end on
// termination combine their own stop token with token().
class multiplexed_session_termination final {
public:
    multiplexed_session_termination() noexcept = default;
    multiplexed_session_termination(const multiplexed_session_termination&) = delete;
    multiplexed_session_termination& operator=(const multiplexed_session_termination&) = delete;

    [[nodiscard]] bool terminated() const noexcept {
        return static_cast<bool>(error_);
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return error_;
    }

    [[nodiscard]] bool terminate(std::error_code error) noexcept {
        if (terminated()) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::connection_aborted);
        if (stop_source_) {
            stop_source_->request_stop();
        }
        return true;
    }

    // Allocated on first use; already stopped once the session terminated.
    [[nodiscard]] stop_token token() {
        if (!stop_source_) {
            stop_source_.emplace();
            if (terminated()) {
                stop_source_->request_stop();
            }
        }
        return stop_source_->token();
    }

private:
    std::error_code error_;
    std::optional<stop_source> stop_source_;
};

// The one asynchronous wake primitive of a dispatched multiplexed stream. Body
// readers and writers waiting for send window or output budget share it and
// always re-check their own readiness after a wake.
class multiplexed_stream_signal final {
public:
    class output_wait_guard final {
    public:
        explicit output_wait_guard(multiplexed_stream_signal& signal) noexcept
            : signal_(signal) {
            signal_.set_waiting_output(true);
        }
        ~output_wait_guard() noexcept {
            signal_.set_waiting_output(false);
        }
        output_wait_guard(const output_wait_guard&) = delete;
        output_wait_guard& operator=(const output_wait_guard&) = delete;

    private:
        multiplexed_stream_signal& signal_;
    };

    multiplexed_stream_signal(const worker_ref& worker_value, multiplexed_session_termination& termination)
        : signal_(worker_value),
          termination_(termination) {}
    multiplexed_stream_signal(worker_ref&&, multiplexed_session_termination&) = delete;

    void wake() noexcept {
        signal_.notify();
    }

    // The stream ended abnormally (peer reset, local reset): every wait ends.
    void stop() noexcept {
        stopped_ = true;
        if (stop_source_) {
            stop_source_->request_stop();
        }
        wake();
    }

    [[nodiscard]] bool stopped() const noexcept {
        return stopped_ || termination_.terminated();
    }

    [[nodiscard]] stop_token terminal_stop_token() {
        if (!stop_source_) {
            stop_source_.emplace();
        }
        if (stopped()) {
            stop_source_->request_stop();
        }
        return stop_source_->token();
    }

    [[nodiscard]] bool terminated() const noexcept {
        return termination_.terminated();
    }

    [[nodiscard]] std::error_code terminal_error() const noexcept {
        return termination_.error();
    }

    [[nodiscard]] multiplexed_session_termination& termination() & noexcept RUVIA_LIFETIMEBOUND {
        return termination_;
    }
    multiplexed_session_termination& termination() && = delete;

    // Spans one whole wait for peer window / acknowledgements to free output
    // capacity; the connection scanner times it against write_timeout_.
    void set_waiting_output(bool waiting) noexcept {
        waiting_output_ = waiting;
        output_wait_observed_ms_.reset();
        output_wait_expired_ = false;
    }

    [[nodiscard]] bool waiting_output() const noexcept {
        return waiting_output_;
    }

    // True once per wait, when it has been observed for `timeout` in the
    // scanner's clock. The first observation starts the clock.
    [[nodiscard]] bool output_wait_expired(std::int64_t now_ms, timeout_duration timeout) noexcept {
        if (!waiting_output_ || output_wait_expired_) {
            return false;
        }
        if (!output_wait_observed_ms_.has_value()) {
            output_wait_observed_ms_ = now_ms;
        }
        output_wait_expired_ = now_ms - *output_wait_observed_ms_ >= timeout.value().count();
        return output_wait_expired_;
    }

    [[nodiscard]] worker_signal::wait_operation wait() {
        return signal_.wait();
    }

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_.worker();
    }
    const worker_ref& worker() const&& = delete;

private:
    worker_signal signal_;
    multiplexed_session_termination& termination_;
    std::optional<stop_source> stop_source_;
    bool stopped_{false};
    bool waiting_output_{false};
    bool output_wait_expired_{false};
    std::optional<std::int64_t> output_wait_observed_ms_;
};

// Streams of one multiplexed connection that may open or read a file body on
// the blocking pool at once; the rest wait holding no read buffer and no pool
// task.
inline constexpr std::size_t multiplexed_file_read_slots = 4;

struct multiplexed_buffered_response_write_result final {
    // The final status, once the response head was submitted.
    std::optional<http_status_code> committed_status_{};
    // Why a file body failed. Without committed_status_ nothing of the
    // response was sent and the stream still awaits one.
    std::optional<response_file_failure> file_failure_{};
};

class multiplexed_file_read_limiter final {
public:
    class slot final {
    public:
        slot(slot&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)) {}
        ~slot() noexcept {
            if (owner_ != nullptr) {
                auto* const owner = std::exchange(owner_, nullptr);
                --owner->active_;
                owner->released_.notify();
            }
        }

        slot(const slot&) = delete;
        slot& operator=(const slot&) = delete;
        slot& operator=(slot&&) = delete;

    private:
        friend class multiplexed_file_read_limiter;

        explicit slot(multiplexed_file_read_limiter& owner) noexcept
            : owner_(&owner) {}

        multiplexed_file_read_limiter* owner_;
    };

    explicit multiplexed_file_read_limiter(
        const worker_ref& worker_value, std::size_t max_slots = multiplexed_file_read_slots) noexcept
        : released_(worker_value),
          max_slots_(max_slots) {}
    multiplexed_file_read_limiter(worker_ref&&, std::size_t = multiplexed_file_read_slots) = delete;
    multiplexed_file_read_limiter(const multiplexed_file_read_limiter&) = delete;
    multiplexed_file_read_limiter& operator=(const multiplexed_file_read_limiter&) = delete;

    // Admission and its return obligation travel as one move-only value.
    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<slot>> acquire(stream_ended_predicate stream_ended) {
        while (active_ >= max_slots_) {
            co_await released_.wait();
            if (stream_ended()) {
                co_return std::nullopt;
            }
        }
        ++active_;
        co_return std::optional<slot>(slot(*this));
    }

    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<multiplexed_buffered_response_write_result>> open_file_body(
        std::optional<response_file_source>& source, const http_response& response,
        const response_file_io& file_io, stream_ended_predicate stream_ended) {
        source.emplace(response, file_io);
        bool opened = false;
        {
            const auto held = co_await acquire(stream_ended);
            if (!held) {
                co_return multiplexed_buffered_response_write_result{};
            }
            opened = co_await source->open_first();
        }
        if (stream_ended()) {
            co_return multiplexed_buffered_response_write_result{};
        }
        if (!opened) {
            co_return multiplexed_buffered_response_write_result{.file_failure_ = *source->failure()};
        }
        co_return std::nullopt;
    }

private:
    worker_signal released_;
    std::size_t max_slots_;
    std::size_t active_{0};
};

// Shared state and lifecycle for HTTP/2 and HTTP/3 streaming response sinks.
// Derived sinks supply the protocol-specific head submission, encoded DATA
// write, terminal finish, and abort check.
template <typename derived_type, typename stream_type>
class multiplexed_response_stream_sink_base {
public:
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted() || (!state_.ended() && (derived().stream_aborted() || signal_.terminated()));
    }

    // Routes the stop requests of the writer's output operations.
    [[nodiscard]] operation_interrupt_source& output_interrupts() & noexcept RUVIA_LIFETIMEBOUND {
        return output_interrupts_;
    }
    operation_interrupt_source& output_interrupts() && = delete;

    task<stream_commit_result> commit() {
        if (state_.committed()) {
            throw std::logic_error("response stream is already committed");
        }
        co_await commit_head(http_response_trailer_intent::none);
        co_return state_.ended() ? stream_commit_result::completed : stream_commit_result::body;
    }

    void bind_context(context* context_value) {
        interim_output_ = context_value != nullptr ? context_access::interim_output(*context_value) : nullptr;
        state_.bind_context(context_value);
    }

    void release_context() noexcept {
        state_.release_context();
    }

    task<void> write(std::string_view chunk) {
        throw_if_terminated();
        throw_if_body_failed();
        state_.ensure_body_allowed();
        if (chunk.empty()) {
            co_return;
        }
        if (compression_.active()) {
            try {
                co_await compression_.write(
                    chunk, [this](std::string_view encoded) { return write_encoded(encoded); });
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_return;
        }
        co_await write_encoded(chunk);
    }

    task<void> write_encoded(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        try {
            co_await derived().write_encoded_data(chunk);
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
    }

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        // Connection termination wakes the sleep through the combined token and
        // then fails it; the caller's own stop still reports stop_requested.
        const stop_group stop_owner(stop_token_value, signal_.termination().token());
        const auto result = co_await sleep_for(signal_.worker(), duration, stop_owner.token());
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::sleep_terminated_error_message);
        }
        if (derived().stream_aborted()) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), derived_type::sleep_reset_error_message);
        }
        co_return result;
    }

    task<void> end(std::span<const http_header_view> trailers) {
        // An empty terminal call is idempotent once the content ended.
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }
        throw_if_terminated();
        throw_if_body_failed();

        const auto trailer_section = validate_http_response_trailers(trailers);
        const auto trailer_intent = response_trailer_intent(trailer_section);
        if (!state_.committed()) {
            co_await commit_head(trailer_intent);
        }
        throw_if_body_failed();
        if (state_.ended()) {
            co_return;
        }
        if (!trailer_section.empty()) {
            state_.ensure_trailers_allowed();
        }
        if (compression_.active()) {
            try {
                compression_.finish();
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_await write_encoded(compression_.output());
            compression_.release_output();
        }
        try {
            if constexpr (std::is_void_v<decltype(derived().finish_stream_response(trailer_section))>) {
                derived().finish_stream_response(trailer_section);
            } else {
                co_await derived().finish_stream_response(trailer_section);
            }
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
        state_.mark_ended();
    }

protected:
    multiplexed_response_stream_sink_base(stream_type& stream, multiplexed_stream_signal& signal, http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding, const compression_config* compression,
        const http_cors_policy* cors) noexcept
        : stream_(stream),
          signal_(signal),
          request_method_(request_method),
          cors_(cors),
          compression_(codecs, response_coding, compression) {}

    void throw_if_terminated() const {
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
        }
    }

    void throw_if_body_failed() {
        if (const auto& body_failure = stream_.body_failure(); body_failure.has_value()) {
            if (state_.committed()) {
                state_.mark_aborted();
            }
            if (*body_failure == http_request_body_failure::buffer_limit) {
                throw inbound_buffer_limit_error{};
            }
            throw http_request_body_protocol_error(*body_failure);
        }
    }

    stream_type& stream_;
    multiplexed_stream_signal& signal_;
    response_stream_state state_;
    http_interim_response_output* interim_output_{};
    http_known_method request_method_{http_known_method::unknown};
    const http_cors_policy* cors_;
    http_streaming_response_compression compression_;
    operation_interrupt_source output_interrupts_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }

    task<void> commit_head(http_response_trailer_intent trailer_intent) {
        throw_if_terminated();
        throw_if_body_failed();
        try {
            if (context_access::output_committed(state_.bound_context())) {
                throw std::logic_error("the request's tunnel is already established");
            }
            auto response = co_await context_access::commit_streaming_head(state_.bound_context());
            validate_response_stream_status(context_access::request(state_.bound_context()), response);
            if (cors_ != nullptr) {
                (void)cors_->apply_response_headers(
                    context_access::request(state_.bound_context()), response);
            }
            throw_if_terminated();
            throw_if_body_failed();
            compression_.prepare(context_access::request(state_.bound_context()), response);
            const auto commit_body_plan = plan_http_response_body(request_method_, response.status());
            compression_.activate(commit_body_plan);
            const auto submitted_head = derived().submit_streaming_head(std::move(response), trailer_intent);
            if (interim_output_ != nullptr) {
                interim_output_->commit_final();
            }
            stream_.mark_response_committed();
            state_.mark_committed(submitted_head);
            derived().wake_writer();
        } catch (...) {
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
    }
};

// Shared streaming request-body reader loop for HTTP/2 and HTTP/3 streams.
// Derived readers provide protocol-specific body failure, reset check, chunk
// extraction (returning flow-control credit), and end-of-body check.
template <typename derived_type>
class multiplexed_request_body_reader_base {
public:
    // An interrupted read leaves queued chunks and flow-control credit for the
    // next read.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) {
        operation_interrupt interrupt(interrupts_, signal_.worker(), operation, &signal_, &wake_signal,
            "request body read");
        throw_if_body_failed();
        if (auto* continue_output = std::exchange(continue_output_, nullptr)) {
            co_await continue_output->send_continue();
        }
        for (;;) {
            interrupt.throw_if_interrupted();
            throw_if_body_failed();
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
            }
            if (signal_.stopped() || derived().stream_aborted()) {
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), derived_type::reset_error_message);
            }
            const auto chunk = derived().pop_chunk();
            if (!chunk.empty()) {
                co_return ::ruvia::as_bytes(chunk);
            }
            if (derived().body_ended()) {
                co_return std::nullopt;
            }
            co_await signal_.wait();
        }
    }

protected:
    multiplexed_request_body_reader_base(
        multiplexed_stream_signal& signal, http_interim_response_output* continue_output) noexcept
        : signal_(signal),
          continue_output_(continue_output) {}

    multiplexed_stream_signal& signal_;
    http_interim_response_output* continue_output_;
    operation_interrupt_source interrupts_;

private:
    static void wake_signal(void* raw) noexcept {
        static_cast<multiplexed_stream_signal*>(raw)->wake();
    }
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
    void throw_if_body_failed() const {
        if (const auto& failure = derived().body_failure(); failure.has_value()) {
            if (*failure == http_request_body_failure::buffer_limit) {
                throw inbound_buffer_limit_error{};
            }
            throw http_request_body_protocol_error(*failure);
        }
    }
};

// Shared duplex CONNECT tunnel read loop and WebSocket write disposition
// forwarding for HTTP/2 and HTTP/3 streams.
template <typename derived_type>
class multiplexed_tunnel_transport_base {
public:
    template <typename buffer_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer) {
        return read_more(buffer, []() noexcept {}, asio::cancellation_slot{});
    }

    // A cancellation emitted on `slot` wakes the wait; `check` runs before
    // every wait and may throw to abandon it. Input that has not been
    // consumed stays queued for the next read_more().
    template <typename buffer_type, typename check_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer, check_type check, asio::cancellation_slot slot) {
        const wake_assignment assignment(signal_, slot);
        for (;;) {
            derived().require_live();
            if (derived().consume_tunnel_chunk(buffer)) {
                co_return true;
            }
            if (derived().receive_ended()) {
                co_return false;
            }
            derived().require_live();
            check();
            co_await signal_.wait();
        }
    }

protected:
    explicit multiplexed_tunnel_transport_base(multiplexed_stream_signal& signal) noexcept
        : signal_(signal) {}

    // While assigned, a cancellation emitted on the slot wakes this stream's waits.
    class wake_assignment final {
    public:
        wake_assignment(multiplexed_stream_signal& signal, asio::cancellation_slot slot)
            : slot_(slot) {
            if (slot_.is_connected()) {
                slot_.assign([&signal](asio::cancellation_type) noexcept { signal.wake(); });
            }
        }
        ~wake_assignment() {
            if (slot_.is_connected()) {
                slot_.clear();
            }
        }
        wake_assignment(const wake_assignment&) = delete;
        wake_assignment& operator=(const wake_assignment&) = delete;

    private:
        asio::cancellation_slot slot_;
    };

    multiplexed_stream_signal& signal_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
};

// `request_stream`: the connection's listener is a request stream one, whose
// every request streams its body (a CONNECT is a tunnel request).
[[nodiscard]] inline std::pair<route_resolution, request_body_mode> resolve_multiplexed_stream_route(
    const route_table& routes, bool request_stream, std::string_view method_token, std::string_view target_path,
    std::string_view authority, std::string_view protocol) noexcept {
    const auto method = classify_http_method(method_token);
    if (request_stream) {
        return {resolve_request_stream_route(method == http_known_method::connect), request_body_mode::stream};
    }
    const auto path = target_path.substr(0, target_path.find('?'));
    const auto extended_protocol = http_ascii_equals_ignore_case(protocol, "websocket")
                                       ? std::string_view("websocket")
                                       : protocol;
    auto resolution = routes.resolve(route_request_view{
        method, method_token, path, authority, extended_protocol});
    auto body_mode = request_body_mode::buffered;
    if (const auto* resolved = resolution.resolved()) {
        body_mode = resolved->route().endpoint().request_body_mode();
        if ((method == http_known_method::connect && resolved->route().endpoint().websocket() != nullptr) ||
            resolved->route().endpoint().tunnel() != nullptr) {
            body_mode = request_body_mode::stream;
        }
    }
    return {std::move(resolution), body_mode};
}

[[nodiscard]] inline task<http_response> respond_http_request_body_failure(const route_table& routes,
    const http_request& request, http_request_body_failure failure,
    const context_services& services, http_response_tail& tail) {
    tail.disable_response_coding();
    return respond_http_request_rejection(routes, request,
        http_request_rejection::error(
            [&] { const auto err = http_request_body_protocol_error(failure); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()),
        services);
}

// Completes a multiplexed response-stream route result. Returns true when the
// stream already terminated (peer abort before commit or committed head), or
// false when `response` holds a buffered route/recovered response to finish.
template <typename reset_type>
[[nodiscard]] inline bool complete_multiplexed_response_stream_dispatch(
    response_stream_dispatch_result& result_value, bool request_body_failed,
    const http_server_options& options, std::string_view remote_address,
    http_response_tail& tail, http_response& response, bool& body_failure_handled,
    reset_type&& reset_stream) {
    if (result_value.aborted_before_commit() != nullptr) {
        return true;
    }
    if (const auto committed_status = result_value.committed_status()) {
        if (const auto* failed = result_value.failed_after_commit()) {
            reset_stream(request_body_failed);
            if (!request_body_failed) {
                options.connection_failure_.invoke(remote_address, failed->exception());
            }
        } else if (request_body_failed) {
            reset_stream(true);
        }
        tail.record_access(options.access_log_, *committed_status);
        return true;
    }
    if (auto* route_response = result_value.route_response()) {
        response = std::move(*route_response).take_response();
    } else if (auto* recovered = result_value.recovered_failure()) {
        body_failure_handled = request_body_failed;
        response = std::move(*recovered).take_response();
    } else {
        throw std::logic_error("response stream dispatch returned no terminal alternative");
    }
    return false;
}

template <typename stream_id_type, typename writer_type, typename reset_type>
[[nodiscard]] task<void> finish_multiplexed_buffered_response(stream_id_type stream_id,
    const std::optional<http_request_body_failure>& body_failure, bool body_failure_handled,
    http_response& response, const http_request& request, const route_table& routes, const context_services& request_services,
    const http_server_options& options, std::string_view remote_address,
    http_response_tail& tail, writer_type& writer, reset_type reset_stream) {
    if (body_failure.has_value() && !body_failure_handled) {
        body_failure_handled = true;
        response = co_await respond_http_request_body_failure(
            routes, request, *body_failure, request_services, tail);
    }
    auto preparation = co_await tail.prepare_or_finish(
        response, options, routes, request_services);
    if (body_failure.has_value() && !body_failure_handled) {
        response = co_await respond_http_request_body_failure(
            routes, request, *body_failure, request_services, tail);
        preparation = co_await tail.prepare_or_finish(
            response, options, routes, request_services);
    }
    if (request_services.get_stop_token().stop_requested()) {
        reset_stream(true);
        co_return;
    }
    auto written = co_await writer.write(stream_id, response, preparation.write_plan_);
    if (written.file_failure_ && !written.committed_status_) {
        // Nothing of the response was sent: the stream gets the error
        // response instead of a reset.
        options.connection_failure_.invoke(remote_address, written.file_failure_->fault());
        tail.disable_response_coding();
        response = co_await routes.handle_exception(
            request, written.file_failure_->response_error(), request_services);
        preparation = co_await tail.prepare_or_finish(
            response, options, routes, request_services);
        written = co_await writer.write(stream_id, response, preparation.write_plan_);
        if (written.file_failure_ && !written.committed_status_) {
            reset_stream(false);
        }
    }
    if (written.file_failure_) {
        options.connection_failure_.invoke(remote_address, written.file_failure_->fault());
    }
    if (written.committed_status_) {
        tail.record_access(options.access_log_, *written.committed_status_);
    }
}

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP3)

// Server-chosen QUIC connection IDs (RFC 9000 §5.1) that route themselves
// without exposing a linkable worker prefix on the wire (RFC 9000 §9.5).
// Every worker of a POSIX HTTP/3 listener binds the UDP port with
// SO_REUSEPORT (on Linux steered in the kernel by SO_ATTACH_REUSEPORT_CBPF),
// and on Windows the acceptor receives every datagram. Each connection ID
// this server issues names the worker that owns the connection, masked and
// authenticated by the listener's shared secret and the ID's random suffix so
// that sibling IDs of one connection share no fixed bytes and a client-chosen
// ID only passes the tag check by chance (one in 2^16):
//
//   bytes 0..1   (owner worker index) ^ mask, big endian
//   bytes 2..3   tag: keyed 16-bit authentication tag over worker index and random bytes
//   bytes 4..19  random (16 CSPRNG bytes)
//
// A datagram whose destination ID names another worker is steered to it (by
// the kernel cBPF filter on Linux, or handed through the worker directory).
// Any other unknown ID (a client's first Initial) is hashed by its connection
// ID and the listener secret.

namespace ruvia::detail {

inline constexpr std::size_t http3_connection_id_length = 20;
using http3_secret = std::array<std::uint8_t, 32>;

// 32-bit mixing primes shared with the Linux SO_ATTACH_REUSEPORT_CBPF program.
inline constexpr std::uint32_t http3_cid_mix_prime_1 = 0x85ebca6bu;
inline constexpr std::uint32_t http3_cid_mix_prime_2 = 0xc2b2ae35u;

[[nodiscard]] constexpr std::uint16_t read_http3_be16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] constexpr std::uint32_t read_http3_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

// A connection ID as a map key: up to NGTCP2_MAX_CIDLEN bytes.
struct http3_cid_key final {
    std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> bytes_{};
    std::uint8_t size_{0};

    [[nodiscard]] static http3_cid_key from(std::span<const std::uint8_t> value) noexcept {
        http3_cid_key key;
        key.size_ = static_cast<std::uint8_t>(value.size() < key.bytes_.size() ? value.size() : key.bytes_.size());
        std::memcpy(key.bytes_.data(), value.data(), key.size_);
        return key;
    }

    [[nodiscard]] static http3_cid_key from(const ngtcp2_cid& cid) noexcept {
        return from(std::span<const std::uint8_t>(cid.data, cid.datalen));
    }

    friend bool operator==(const http3_cid_key& left, const http3_cid_key& right) noexcept {
        return left.size_ == right.size_ && std::memcmp(left.bytes_.data(), right.bytes_.data(), left.size_) == 0;
    }
};

struct http3_cid_key_hash final {
    [[nodiscard]] std::size_t operator()(const http3_cid_key& key) const noexcept;
};

// Fills `cid` with a fresh ID owned by `worker_index`. False when the CSPRNG
// failed.
[[nodiscard]] bool make_http3_connection_id(
    const http3_secret& secret, std::size_t worker_index, ngtcp2_cid& cid) noexcept;

// The worker a destination connection ID names, or nullopt when it is not an
// ID of this listener (wrong length, bad tag) or names no running worker.
[[nodiscard]] std::optional<std::size_t> http3_connection_id_owner(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;
// The worker that opens the connection whose client Initial carries the
// client-chosen destination ID `cid`: a hash of it keyed by the listener
// secret. Retransmissions of that Initial carry the same ID, so they reach
// the same worker without per-connection acceptor state, and a client that
// does not know the secret cannot choose its worker.
[[nodiscard]] std::size_t http3_initial_worker(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;

// The stateless reset token of `cid` (RFC 9000 §10.3.2), derived from the
// listener secret so every worker computes the same token.
[[nodiscard]] bool make_http3_stateless_reset_token(
    const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

// The largest UDP payload (65535 minus the UDP header): a receive buffer of
// this size truncates no datagram.
inline constexpr std::size_t http3_max_udp_payload_bytes = 65527;

// A receive error that concerns a single datagram, not the socket: ICMP
// errors of earlier sends (Windows reports them on later receives) and
// truncation. The socket keeps receiving.
[[nodiscard]] inline bool http3_ignorable_receive_error(const std::error_code& error) noexcept {
    return error == asio::error::connection_refused || error == asio::error::connection_reset ||
           error == asio::error::message_size || error == asio::error::network_unreachable ||
           error == asio::error::host_unreachable;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class http3_endpoint;

#if defined(_WIN32)
class windows_acceptor;
#endif

// One datagram handed to a worker that did not receive it. It owns its bytes
// because it crosses threads.
struct http3_handoff_datagram final {
    std::vector<std::byte> bytes_;
    asio::ip::udp::endpoint remote_;
    std::uint32_t ecn_{0};
    asio::ip::udp::endpoint local_;
    std::uint32_t interface_index_{};
};

// Slot identities outlive their worker-local endpoint. Another worker can
// still own a connection issued under that slot's secret after this one has
// released its last connection and socket.
struct http3_endpoint_registration final {
    http3_endpoint* endpoint_;
    asio::ip::udp::endpoint local_;
    std::shared_ptr<const http3_secret> secret_;
    std::size_t listener_index_;
};

// Datagrams in flight from one producer to one worker, product limit. A
// datagram past it is dropped, as UDP may drop it anyway; QUIC loss recovery
// retransmits what matters.
inline constexpr std::size_t http3_max_datagrams_in_flight = 256;

// One handed-off datagram's place in its in-flight bound: reserved before the
// datagram is posted, released when the posted job ran or was abandoned, on
// whichever thread that is.
class http3_in_flight_token final {
public:
    // Empty when the bound is full.
    [[nodiscard]] static http3_in_flight_token reserve(std::atomic<std::size_t>& count) noexcept {
        if (count.fetch_add(1, std::memory_order_relaxed) >= http3_max_datagrams_in_flight) {
            count.fetch_sub(1, std::memory_order_relaxed);
            return http3_in_flight_token(nullptr);
        }
        return http3_in_flight_token(&count);
    }
    http3_in_flight_token(http3_in_flight_token&& other) noexcept
        : count_(std::exchange(other.count_, nullptr)) {}
    http3_in_flight_token(const http3_in_flight_token&) = delete;
    http3_in_flight_token& operator=(const http3_in_flight_token&) = delete;
    http3_in_flight_token& operator=(http3_in_flight_token&&) = delete;
    ~http3_in_flight_token() {
        if (count_ != nullptr) {
            count_->fetch_sub(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return count_ != nullptr;
    }

private:
    explicit http3_in_flight_token(std::atomic<std::size_t>* count) noexcept
        : count_(count) {}

    std::atomic<std::size_t>* count_;
};

// The HTTP/3 rendezvous of every worker of one application run. A datagram
// for a connection a worker owns (see http3_connection_id.h) is handed to
// that worker here when another thread received it: on POSIX another worker
// whose SO_REUSEPORT socket the kernel chose, on Windows the acceptor that
// receives every datagram. It travels on the owner's bounded worker queue,
// at most http3_max_datagrams_in_flight per owner at a time, and is dropped
// when either bound is full or the owner no longer serves.
class http3_worker_directory final {
public:
    using receive_type = void (*)(void* target, std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;

    explicit http3_worker_directory(std::size_t worker_count);
    http3_worker_directory(const http3_worker_directory&) = delete;
    ~http3_worker_directory();
    http3_worker_directory& operator=(const http3_worker_directory&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return slots_.size();
    }

    // Construction thread, before any worker serves -- for a directory
    // application::add_listener() creates, before any worker uses it.
    void bind_worker(std::size_t index, const worker_ref& worker_value);
    // The owning worker's thread: opens and closes its receiving side.
    void attach(std::size_t index, void* target, receive_type receive) noexcept;
    void detach(std::size_t index) noexcept;

    // Worker-local endpoint registrations share packet routing across removed
    // and re-added listeners bound to the same UDP endpoint.
    void register_endpoint(std::size_t worker_index, http3_endpoint& endpoint,
        const asio::ip::udp::endpoint& local, std::shared_ptr<const http3_secret> secret,
        std::size_t listener_index);
    void unregister_endpoint(std::size_t worker_index, http3_endpoint& endpoint) noexcept;
    [[nodiscard]] std::span<const http3_endpoint_registration> endpoints(std::size_t worker_index) const noexcept {
        return slots_[worker_index]->endpoints_;
    }

#if !defined(_WIN32)
    // The first call for `listener_index` binds every worker's SO_REUSEPORT
    // socket in worker index order 0..N-1 -- or takes `inherited`'s UDP
    // sockets, in that order already, when it holds them -- and attaches
    // SO_ATTACH_REUSEPORT_CBPF on Linux; each call assigns `worker_index`'s
    // socket to `socket`.
    void open_listener_socket(std::size_t worker_index, std::size_t listener_index,
        const asio::ip::udp::endpoint& configured_endpoint, const http3_secret& secret,
        inherited_listener_sockets* inherited, asio::ip::udp::socket& socket, std::optional<bool> ipv6_only);
    // Closes the sockets of `listener_index` no worker claimed, for a listener
    // whose addition was rolled back.
    void close_unclaimed_listener_sockets(std::size_t listener_index) noexcept;
#endif
    // Any thread. False when the datagram was dropped.
    [[nodiscard]] bool hand_off(std::size_t owner, std::size_t listener_index,
        std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index,
        std::uint32_t ecn = 0) noexcept;

private:
    struct slot final {
        worker_ref worker_;
        // Read and written only on the owning worker's thread.
        void* target_{nullptr};
        receive_type receive_{nullptr};
        std::atomic<std::size_t> in_flight_{0};
        std::vector<http3_endpoint_registration> endpoints_{};
    };

    std::vector<std::unique_ptr<slot>> slots_;
    std::mutex bind_mutex_;
    std::vector<std::vector<int>> listener_sockets_;
};

// Where one worker's HTTP/3 endpoints sit among the application's workers:
// the directory (null without an HTTP/3 listener) and this worker's index in
// it, which the connection IDs it issues carry.
struct http3_worker_placement final {
    http3_worker_directory* directory_;
    std::size_t worker_index_;
#if defined(_WIN32)
    // Sends this worker's datagrams from the listener's one UDP socket.
    windows_acceptor* acceptor_;
#endif
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class http3_session;
class route_table;
class worker_capabilities;

// The worker-owned services every HTTP/3 connection of a worker borrows.
// Each outlives the worker's connections, which their listeners' task scopes
// join.
struct http3_worker_services final {
    asio::io_context& io_;
    const worker_ref& worker_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    connection_scanner& scanner_;
    connection_count& connection_count_;
    connection_id_sequence& connection_ids_;
    connection_registry& registry_;
    const stop_token& stop_token_;
    const http_server_worker_state& state_;
    std::atomic<std::size_t>& refused_;
    // Where this worker sits among the application's workers. Its directory
    // (and on Windows its acceptor) is set whenever an endpoint exists.
    http3_worker_placement placement_;
};

// One worker's QUIC endpoint of one HTTP/3 listener: the table from
// connection ID to this worker's connections, connection admission, packet
// steering and the datagram path.
//
// On POSIX the endpoint owns a UDP socket bound with SO_REUSEPORT. On Windows
// the application's acceptor owns the listener's only socket: every datagram
// arrives here handed over through the directory, and every datagram this
// worker sends leaves through the acceptor's bounded queue, so every worker
// serves HTTP/3 alike.
//
// A datagram is delivered to the connection its destination ID names in the
// table. Otherwise an ID that names another worker (http3_connection_id.h)
// is handed to it through the directory; an unknown ID with a valid client
// Initial opens a connection on this worker, bounded like TCP by
// max_connections_per_worker_ (the connection idle longest is evicted
// first); an unsupported version gets Version Negotiation (RFC 9000 §6); a
// short header naming a retired ID of this worker gets a rate-limited
// Stateless Reset (RFC 9000 §10.3); any other datagram is dropped.
class http3_endpoint final {
public:
    // `listener_index` names this listener's endpoint on every worker (and in
    // the Windows acceptor), for datagrams handed between them. Its receive
    // loop and connections run in `tasks`, its listener's scope.
    http3_endpoint(const http3_worker_services& services, http_server_session_config& session,
        asio::ip::udp::endpoint endpoint, std::size_t listener_index, task_scope& tasks, connection_count& listener_connections);
    ~http3_endpoint();
    http3_endpoint(const http3_endpoint&) = delete;
    http3_endpoint& operator=(const http3_endpoint&) = delete;

    // POSIX binds the UDP socket (SO_REUSEADDR and SO_REUSEPORT), or claims
    // it from `inherited` when that holds the service's sockets; Windows,
    // whose acceptor binds it, only prepares the QUIC backend.
    void open(inherited_listener_sockets* inherited);
    // Starts admitting connections, and on POSIX receiving.
    void start() noexcept;
    // The listener was removed: no connection is admitted any more, while
    // established ones are served. After the last one ends the POSIX socket
    // closes; on Windows the acceptor learns that this worker no longer
    // needs the listener's socket.
    void retire() noexcept;
    // The worker stops: no connection is admitted any more, and the POSIX
    // socket closes. Its connections were already closed by the scanner.
    void close() noexcept;
#if !defined(_WIN32)
    // A dup() of the open socket, for application::duplicate_listener_sockets().
    [[nodiscard]] native_socket duplicate_socket();
#endif

    // A datagram another thread received for this worker.
    void receive_handoff(http3_handoff_datagram& datagram) noexcept;

    // Session interface.
    [[nodiscard]] const http3_worker_services& services() const noexcept {
        return services_;
    }
    [[nodiscard]] http_server_session_config& session_config() const noexcept {
        return session_;
    }
    [[nodiscard]] const http3_secret& secret() const noexcept {
        return *session_.http3_->secret_;
    }
    [[nodiscard]] const asio::ip::udp::endpoint& local_endpoint() const noexcept {
        return local_endpoint_;
    }
    // Sends one datagram without blocking. On POSIX would_block means the
    // socket's send buffer is full (then wait with wait_writable()); on
    // Windows the datagram is queued to the acceptor, or lost when that
    // bounded queue is full (no_buffer_space).
    [[nodiscard]] std::error_code send(const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
#if !defined(_WIN32)
    // Completes once the socket can send again, or with the error that ended
    // waiting (the socket closed).
    [[nodiscard]] task<std::error_code> wait_writable();
#endif
    // False when another connection already holds the ID.
    [[nodiscard]] bool register_connection_id(const http3_cid_key& key, http3_session& session);
    void unregister_connection_id(const http3_cid_key& key) noexcept;

private:
    using session_pointer = std::unique_ptr<http3_session>;

#if !defined(_WIN32)
    task<void> run_receive_loop();
    worker_signal activated_;
#endif
    task<void> serve(session_pointer session);
    void receive(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void accept(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_version_negotiation(const ngtcp2_version_cid& header, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_retry(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote,
        std::size_t max_packet_bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_stateless_connection_close(const ngtcp2_pkt_hd& header, const asio::ip::udp::endpoint& remote,
        std::uint64_t error_code, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_stateless_reset(std::span<const std::uint8_t> destination_cid, std::size_t trigger_bytes,
        const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    [[nodiscard]] bool consume_stateless_reset_budget() noexcept;
    // A retiring endpoint whose last connection ended.
    void release_after_last_connection() noexcept;

    const http3_worker_services& services_;
    http_server_session_config& session_;
    asio::ip::udp::endpoint configured_endpoint_;
    asio::ip::udp::endpoint local_endpoint_;
    std::size_t listener_index_;
    task_scope& tasks_;
    connection_count& listener_connections_;
#if !defined(_WIN32)
    asio::ip::udp::socket socket_;
    // One received datagram at a time; a UDP payload never exceeds it.
    std::vector<std::byte> receive_buffer_;
#endif
    std::unordered_map<http3_cid_key, http3_session*, http3_cid_key_hash> connections_;
    std::chrono::steady_clock::time_point stateless_reset_refill_at_{};
    std::size_t stateless_reset_tokens_{32};
    // Sessions serve() is running.
    std::size_t sessions_{0};
    // Between open() and close(): datagrams are processed.
    bool open_{false};
    bool admitting_{false};
    bool registered_{false};
    bool retiring_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Resumes the coroutine on a later turn of its worker, letting other
// connections run between bursts of one connection's output.
struct worker_turn final {
    explicit worker_turn(const worker_ref& worker_value) noexcept
        : worker_(worker_value) {}
    worker_turn(worker_ref&&) = delete;
    const worker_turn* operator&() const = delete;
    worker_turn* operator&() = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const noexcept {
        worker_.defer_resume(continuation);
    }
    void await_resume() const noexcept {}

    const worker_ref& worker_;
};

struct free_tls final {
    void operator()(SSL* tls) const noexcept {
        SSL_free(tls);
    }
};

struct free_crypto final {
    void operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept {
        ngtcp2_crypto_ossl_ctx_del(crypto);
    }
};

inline void fill_random(std::uint8_t* destination, std::size_t size, const ngtcp2_rand_ctx*) noexcept {
    // ngtcp2 cannot fail this callback; packet protection without randomness
    // must not continue.
    if (!fill_random_bytes(std::span<unsigned char>(destination, size))) {
        std::terminate();
    }
}

// The ngtcp2_crypto_ossl adapter callbacks both roles share; the client adds
// client_initial_ and recv_retry_, the server recv_client_initial_.
[[nodiscard]] inline http3_quic_crypto_callbacks make_http3_quic_crypto_callbacks() noexcept {
    return {
        .recv_crypto_data_ = ngtcp2_crypto_recv_crypto_data_cb,
        .encrypt_ = ngtcp2_crypto_encrypt_cb,
        .decrypt_ = ngtcp2_crypto_decrypt_cb,
        .hp_mask_ = ngtcp2_crypto_hp_mask_cb,
        .update_key_ = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx_ = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx_ = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data_ = ngtcp2_crypto_get_path_challenge_data2_cb,
        .version_negotiation_ = ngtcp2_crypto_version_negotiation_cb,
        .rand_ = fill_random,
    };
}

[[nodiscard]] inline ngtcp2_tstamp http3_quic_now() noexcept {
    return static_cast<ngtcp2_tstamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] inline ngtcp2_duration http3_quic_duration(timeout_duration value) noexcept {
    const auto milliseconds = static_cast<std::uint64_t>((std::max)(value.value().count(), std::int64_t{0}));
    return milliseconds > UINT64_MAX / NGTCP2_MILLISECONDS ? UINT64_MAX : milliseconds * NGTCP2_MILLISECONDS;
}

[[nodiscard]] inline ngtcp2_duration http3_quic_duration(const std::optional<timeout_duration>& value) noexcept {
    return value.has_value() ? http3_quic_duration(*value) : 0;
}

[[nodiscard]] inline std::chrono::steady_clock::time_point http3_quic_steady_time(ngtcp2_tstamp timestamp) noexcept {
    using nanoseconds = std::chrono::nanoseconds;
    if (timestamp >= static_cast<ngtcp2_tstamp>((std::numeric_limits<nanoseconds::rep>::max)())) {
        return (std::chrono::steady_clock::time_point::max)();
    }
    return std::chrono::steady_clock::time_point(
        std::chrono::ceil<std::chrono::steady_clock::duration>(nanoseconds(static_cast<nanoseconds::rep>(timestamp))));
}

[[nodiscard]] inline std::chrono::nanoseconds http3_quic_nanoseconds(ngtcp2_duration duration) noexcept {
    return std::chrono::nanoseconds(
        static_cast<std::int64_t>((std::min)(duration, static_cast<ngtcp2_duration>(INT64_MAX))));
}

[[nodiscard]] inline ngtcp2_path http3_quic_path(
    asio::ip::udp::endpoint& local, asio::ip::udp::endpoint& remote) noexcept {
    return {
        .local = {local.data(), static_cast<ngtcp2_socklen>(local.size())},
        .remote = {remote.data(), static_cast<ngtcp2_socklen>(remote.size())},
        .user_data = nullptr,
    };
}

[[nodiscard]] inline std::optional<asio::ip::udp::endpoint> http3_udp_endpoint(
    const ngtcp2_addr& address) noexcept {
    asio::ip::udp::endpoint endpoint;
    if (address.addr == nullptr || address.addrlen == 0 || address.addrlen > endpoint.capacity()) {
        return std::nullopt;
    }
    std::memcpy(endpoint.data(), address.addr, address.addrlen);
    endpoint.resize(address.addrlen);
    return endpoint;
}
}  // namespace ruvia::detail

// Web-side state of one HTTP/3 request stream. The QUIC/HTTP/3 core owns
// framing, QPACK and flow control; this runtime owns what a route needs: the
// validated head, the request body as the route consumes it (buffered or
// streamed, with flow-control credit returned only as bytes are consumed), the
// trailers, and the wake/stop signal a dispatched handler waits on.

namespace ruvia::detail {

enum class http3_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
    buffer_limit,
};

// Tunnel bytes taken together with the receive credit they hold; the reader
// drops the credit once it consumed the bytes.
struct http3_tunnel_chunk final {
    std::string bytes_;
    std::optional<http3_received_data_credit> credit_;
};

class http3_stream final {
public:
    http3_stream(http3_request_head head, route_resolution resolution, request_body_mode body_mode,
        inbound_buffer_account* inbound = nullptr) noexcept
        : head_(std::move(head)),
          resolution_(std::move(resolution)),
          body_mode_(body_mode),
          body_reservation_(inbound),
          active_reservation_(inbound) {}

    http3_stream(const http3_stream&) = delete;
    http3_stream& operator=(const http3_stream&) = delete;

    [[nodiscard]] std::int64_t id() const noexcept {
        return head_.stream_id();
    }
    [[nodiscard]] const http3_request_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http3_request_head& head() const&& = delete;
    [[nodiscard]] const route_resolution& resolution() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolution_;
    }
    const route_resolution& resolution() const&& = delete;
    [[nodiscard]] request_body_mode body_mode() const noexcept {
        return body_mode_;
    }
    [[nodiscard]] http_request_trailers& trailers() & noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    http_request_trailers& trailers() && = delete;
    [[nodiscard]] const std::optional<http_priority>& priority() const& noexcept RUVIA_LIFETIMEBOUND {
        return priority_;
    }
    const std::optional<http_priority>& priority() const&& = delete;
    void set_priority(std::optional<http_priority> priority) noexcept {
        priority_ = priority;
    }
    void defer_dispatch() noexcept {
        dispatch_pending_ = true;
    }
    [[nodiscard]] bool dispatch_pending() const noexcept {
        return dispatch_pending_ && !dispatched();
    }
    void defer_continue() noexcept {
        continue_pending_ = true;
    }
    [[nodiscard]] bool take_continue_pending() noexcept {
        return std::exchange(continue_pending_, false);
    }

    void store_datagram(std::string_view payload, std::size_t byte_limit) {
        if ((datagrams_ && datagrams_->size() >= 64) || datagram_bytes_ > byte_limit ||
            payload.size() > byte_limit - datagram_bytes_) {
            return;  // RFC 9221 permits loss; never backpressure reliable DATA.
        }
        try {
            if (!datagrams_) {
                datagrams_.emplace();
            }
            datagrams_->emplace_back(payload);
            datagram_bytes_ += payload.size();
        } catch (const inbound_buffer_limit_error&) {
            return;
        }
        if (signal_) {
            signal_->wake();
        }
    }
    [[nodiscard]] std::optional<std::string> pop_datagram() {
        if (!datagrams_ || datagrams_->empty()) {
            return std::nullopt;
        }
        auto payload = std::move(datagrams_->front());
        datagrams_->pop_front();
        datagram_bytes_ -= payload.size();
        return payload;
    }
    [[nodiscard]] http3_tunnel_chunk take_tunnel_bytes() {
        http3_tunnel_chunk result{std::string{}, std::exchange(body_credit_, std::nullopt)};
        result.bytes_.swap(body_);
        body_reservation_.clear();
        return result;
    }

    // Buffered body: the whole content, borrowed by the request at dispatch;
    // its credit returns as it is stored. Streaming body and tunnel: the bytes
    // queued for the reader, holding their credit until it takes them.
    [[nodiscard]] http3_body_store_status store(std::string_view data, http3_received_data_credit&& credit,
        protocol_byte_limit total_limit, std::size_t backlog_limit) {
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return http3_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream &&
            (body_.size() > backlog_limit || data.size() > backlog_limit - body_.size())) {
            return http3_body_store_status::backlog_overflow;
        }
        try {
            inbound_append(body_, body_reservation_, data);
        } catch (const inbound_buffer_limit_error&) {
            return http3_body_store_status::buffer_limit;
        }
        received_bytes_ += data.size();
        if (body_mode_ == request_body_mode::stream && credit.valid()) {
            if (body_credit_) {
                body_credit_->merge(std::move(credit));
            } else {
                body_credit_.emplace(std::move(credit));
            }
        }
        return http3_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    std::string_view buffered_body() const&& = delete;

    // Streaming reader: hands out every queued byte as one chunk, valid until
    // the next pop(). The previous chunk's flow-control credit returns now;
    // true when it held any, so the caller can let the writer send it.
    [[nodiscard]] std::pair<std::string_view, bool> pop() & RUVIA_LIFETIMEBOUND {
        const bool returned = active_credit_.has_value();
        active_credit_.reset();
        if (body_credit_) {
            active_credit_.emplace(std::move(*body_credit_));
            body_credit_.reset();
        }
        inbound_clear_retaining_small(active_chunk_, active_reservation_);
        active_chunk_.swap(body_);
        active_reservation_.swap(body_reservation_);
        return {active_chunk_, returned};
    }
    std::pair<std::string_view, bool> pop() && = delete;

    // The handler finished: the content's storage goes back to the
    // connection's inbound budget, and credit for content received but never
    // read returns so a stopped reader cannot strand connection-level credit.
    // True when any credit returned.
    bool discard_body() noexcept {
        const bool returned = body_credit_.has_value() || active_credit_.has_value();
        body_credit_.reset();
        active_credit_.reset();
        inbound_discard(body_, body_reservation_);
        inbound_discard(active_chunk_, active_reservation_);
        datagrams_.reset();
        datagram_bytes_ = 0;
        return returned;
    }

    void mark_body_failure(http_request_body_failure failure) noexcept {
        if (!body_failure_.has_value()) {
            body_failure_ = failure;
        }
    }
    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_failure_;
    }
    const std::optional<http_request_body_failure>& body_failure() const&& = delete;

    // The request content ended (the peer's FIN, or no content at all).
    void mark_request_ended() noexcept {
        request_ended_ = true;
        request_body_deadline_.close();
    }
    [[nodiscard]] bool request_ended() const noexcept {
        return request_ended_;
    }

    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_deadline_.open(now_ms);
    }
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_deadline_.progress(now_ms);
    }
    void mark_request_body_closed() noexcept {
        request_body_deadline_.close();
    }
    // The request body is still open and went `inactivity` without DATA, or
    // `completion` since it opened (request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return request_body_deadline_.overdue(now_ms, inactivity, completion);
    }

    void mark_tunnel() noexcept {
        tunnel_ = true;
    }
    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

    void mark_response_committed() noexcept {
        response_committed_ = true;
    }
    [[nodiscard]] bool response_committed() const noexcept {
        return response_committed_;
    }

    // The peer reset the stream, or this side did: no response byte may follow.
    void mark_aborted() noexcept {
        aborted_ = true;
        if (signal_) {
            signal_->stop();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }

    // The QUIC stream closed in both directions.
    void mark_closed() noexcept {
        closed_ = true;
    }
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        const worker_ref& worker_value, multiplexed_session_termination& termination) {
        if (signal_ || handler_done_) {
            return nullptr;
        }
        signal_.emplace(worker_value, termination);
        if (aborted_) {
            signal_->stop();
        }
        return &*signal_;
    }
    [[nodiscard]] bool dispatched() const noexcept {
        return signal_.has_value();
    }
    [[nodiscard]] multiplexed_stream_signal* signal() & noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    multiplexed_stream_signal* signal() && = delete;
    [[nodiscard]] const multiplexed_stream_signal* signal() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    const multiplexed_stream_signal* signal() const&& = delete;

    void mark_handler_done() noexcept {
        handler_done_ = true;
    }
    [[nodiscard]] bool handler_done() const noexcept {
        return handler_done_;
    }

private:
    http3_request_head head_;
    route_resolution resolution_;
    request_body_mode body_mode_;
    http_request_trailers trailers_;
    std::optional<http_priority> priority_{};
    // Received request content: the whole body (buffered), or the queue the
    // reader drains (streaming). active_chunk_ backs the last popped view.
    std::string body_;
    inbound_buffer_reservation body_reservation_;
    std::string active_chunk_;
    inbound_buffer_reservation active_reservation_;
    // Receive credit of body_ and active_chunk_ (streaming body or tunnel).
    std::optional<http3_received_data_credit> body_credit_;
    std::optional<http3_received_data_credit> active_credit_;
    std::optional<std::deque<std::string>> datagrams_;
    std::size_t datagram_bytes_{0};
    std::size_t received_bytes_{0};
    std::optional<http_request_body_failure> body_failure_{};
    std::optional<multiplexed_stream_signal> signal_;
    multiplexed_request_body_deadline request_body_deadline_;
    bool request_ended_{false};
    bool tunnel_{false};
    bool response_committed_{false};
    bool aborted_{false};
    bool closed_{false};
    bool handler_done_{false};
    bool dispatch_pending_{false};
    bool continue_pending_{false};
};

}  // namespace ruvia::detail

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {

class http3_session;
class http3_stream;
enum class http3_data_write_status : std::uint8_t {
    completed,
    // The stream was reset (by either side) or stopped.
    stream_aborted,
    // The connection ended.
    terminated,
};

// Submits all of `bytes` as DATA of a stream, at most the stream's output
// budget at a time (http3_connection::submit_data): when the budget is
// spent it waits for the peer's acknowledgements, timed by the session
// against write_timeout_. A Content-Length overrun throws std::length_error.
[[nodiscard]] task<http3_data_write_status> write_http3_stream_data(
    http3_session& session, std::int64_t stream_id, multiplexed_stream_signal& signal, std::string_view bytes);

// Writes a buffered or file response on one request stream: the head with its
// write plan's Content-Length, then the content within the stream's output
// budget. A file body is opened and validated before the head without being
// read; each later chunk is read only once output budget is free, at most that
// much, while holding one of multiplexed_file_read_slots through its submission. A
// stream it cannot complete is reset through the session.
class http3_buffered_response_writer final {
public:
    http3_buffered_response_writer(http3_session& session, response_file_io file_io) noexcept;

    http3_buffered_response_writer(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer& operator=(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer(http3_buffered_response_writer&&) = delete;
    http3_buffered_response_writer& operator=(http3_buffered_response_writer&&) = delete;

    [[nodiscard]] task<multiplexed_buffered_response_write_result> write(std::int64_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);

private:
    [[nodiscard]] task<multiplexed_buffered_response_write_result> write_file_body(
        std::int64_t stream_id, multiplexed_stream_signal& signal, response_file_source& source,
        http_status_code committed_status);
    [[nodiscard]] bool stream_ended(std::int64_t stream_id) noexcept;

    http3_session& session_;
    response_file_io file_io_;
    multiplexed_file_read_limiter file_reads_;
};

}  // namespace ruvia::detail

struct ngtcp2_crypto_ossl_ctx;

namespace ruvia::detail {

class http3_endpoint;
struct http3_worker_services;

// Per-stream DATA a connection holds between submission and the peer's
// acknowledgement (http3_connection_options::max_stream_output_bytes_).
inline constexpr std::size_t http3_stream_output_bytes = 256 * 1024;
// Largest UDP payload this server sends: fits a 1500-byte Ethernet MTU behind
// IPv6, UDP and common tunnel headers, without path MTU discovery.
inline constexpr std::size_t http3_max_datagram_bytes = 1350;

// One HTTP/3 connection on the worker that accepted it: its TLS session, its
// QUIC/HTTP/3 state (http3_quic_connection), its one QUIC timer, its writer
// and the dispatch of its request streams through the shared request spine
// (admission, routes, response tail), as the HTTP/2 session engine does.
//
// The endpoint feeds datagrams in with receive(); run() is the connection's
// task: it writes every datagram the connection produces, re-arms the timer to
// expiry(), and returns once the connection has ended and every handler has
// finished. All of it runs on the owning worker.
//
// Lifecycle: open -> (graceful drain on the request cap or a connection
// timeout: shutdown-notice GOAWAY, a bounded round trip, final GOAWAY,
// CONNECTION_CLOSE(H3_NO_ERROR) once no stream is left) -> closing period
// (RFC 9000 §10.2, three probe timeouts absorbing late packets) -> ended. A
// worker stop, eviction or a timeout past its grace ends it at once with
// CONNECTION_CLOSE(H3_NO_ERROR).
class http3_session final {
public:
    // Accepts the connection the client Initial `initial` opens (RFC 9000
    // §7.2): a server connection ID owned by this worker, the client's chosen
    // ID as original_destination_connection_id.
    http3_session(http3_endpoint& endpoint, connection_slot slot, const ngtcp2_pkt_hd& initial,
        const asio::ip::udp::endpoint& remote, const ngtcp2_cid& original_dcid,
        std::optional<ngtcp2_cid> retry_scid, std::span<const std::uint8_t> retry_token,
        connection_lifecycle lifecycle);
    ~http3_session();

    http3_session(const http3_session&) = delete;
    http3_session& operator=(const http3_session&) = delete;
    http3_session(http3_session&&) = delete;
    http3_session& operator=(http3_session&&) = delete;

    // One datagram for this connection, from `remote`.
    void receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    [[nodiscard]] task<void> run();
    // Ends the connection now: CONNECTION_CLOSE(H3_NO_ERROR), streams
    // cancelled, no closing period. The scanner's close action.
    void close_now() noexcept;

    // Stream I/O for the response writers, the streaming sink and the request
    // body reader of this connection's handlers.
    [[nodiscard]] http3_connection& codec() noexcept {
        return connection_->http3();
    }
    [[nodiscard]] http3_stream* find_stream(std::int64_t stream_id) noexcept;
    // Queues the stream's output for the writer.
    void wake_writer() noexcept;
    // Abandons both directions of a request stream (RFC 9114 §4.1.1) and
    // stops every wait of its handler.
    void reset_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] std::string_view remote_address() const& noexcept RUVIA_LIFETIMEBOUND {
        return remote_address_;
    }
    std::string_view remote_address() const&& = delete;
    [[nodiscard]] http3_quic_status submit_datagram(std::int64_t stream_id, std::span<const std::byte> payload);

private:
    using stream_pointer = std::unique_ptr<http3_stream>;

    enum class lifecycle_type : std::uint8_t {
        open,
        // The QUIC connection closed (either side, or idle): late packets
        // are absorbed until the closing period elapses.
        closing,
        ended,
    };

    static ngtcp2_conn* native_connection(ngtcp2_crypto_conn_ref* reference) noexcept;
    static int new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
        std::size_t size, void* user_data) noexcept;
    static int remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) noexcept;

    [[nodiscard]] static ngtcp2_tstamp now() noexcept;
    [[nodiscard]] ngtcp2_path path(asio::ip::udp::endpoint& remote) noexcept;
    void register_connection_id(const ngtcp2_cid& cid);
    void unregister_connection_ids() noexcept;

    // Connection events.
    void process_events() noexcept;
    void drain_events();
    void start_application();
    void on_request_head(http3_request_head_event& event);
    void on_data(http3_data_event& event);
    void on_datagram(const http3_datagram_event& event);
    void on_message_end(const http3_message_end_event& event);
    void on_stream_end(std::int64_t stream_id);
    void on_stream_abort(std::int64_t stream_id) noexcept;
    void on_stream_close(const http3_stream_close_event& event) noexcept;
    void wake_output_waiters() noexcept;

    // Requests.
    [[nodiscard]] bool admit_stream(std::int64_t stream_id);
    [[nodiscard]] task<void> dispatch_one(std::int64_t stream_id);
    void finish_stream_handler(std::int64_t stream_id) noexcept;
    void remove_stream_if_done(std::int64_t stream_id) noexcept;
    void set_inactivity_phase() noexcept;
    void expire_stream_deadlines(std::int64_t now_ms) noexcept;

    // Output and timers.
    [[nodiscard]] task<void> flush();
    [[nodiscard]] task<void> send_datagram();
    void arm_timer() noexcept;
    void timer_fired(worker_timer_outcome outcome) noexcept;
    void enter_closing() noexcept;
    void terminate(std::error_code error) noexcept;

    // Graceful drain (RFC 9114 §5.2).
    void request_drain() noexcept;
    void begin_drain() noexcept;
    void arm_drain_timer() noexcept;
    void drain_timer_expired() noexcept;
    void finish_drain_if_ready() noexcept;

    http3_endpoint& endpoint_;
    const http3_worker_services& services_;
    connection_slot slot_;
    connection_lifecycle connection_lifecycle_;
    // conn_info::connection_id() of this connection.
    std::uint64_t connection_id_;
    asio::ip::udp::endpoint local_;
    // The actual destination address of the Initial, including wildcard-bound
    // listeners. TLS identity selection borrows this stable address.
    asio::ip::address tls_local_address_;
    asio::ip::udp::endpoint remote_;
    // The sender of the datagram being read, for its ngtcp2 path.
    asio::ip::udp::endpoint path_remote_;
    asio::ip::udp::endpoint path_local_;
    std::uint32_t path_interface_index_;
    std::string remote_address_;
    // Present only with an on_tls_client_hello observer; tls_ borrows it, so
    // it is declared before tls_.
    std::string local_address_;
    std::optional<tls_client_hello_observation> client_hello_;
    // Keys this connection holds in the endpoint's table.
    std::vector<http3_cid_key> connection_ids_;
    std::unique_ptr<SSL, free_tls> tls_;
    std::array<unsigned char, 32> tls_resumption_context_{};
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref connection_reference_{};
    // Destroyed first: ngtcp2 still references the TLS objects above.
    std::optional<http3_quic_connection> connection_;
    http3_quic_datagram datagram_;
    std::string client_certificate_;
    std::optional<tls_served_certificate> served_certificate_;
    inbound_buffer_account inbound_buffers_;
    std::optional<context_services> base_services_;
    http_request_admission_policy admission_policy_;
    multiplexed_session_termination termination_;
    worker_signal write_signal_;
    worker_signal handler_finished_;
    // Declared after the inbound buffers their bodies borrow.
    std::unordered_map<std::int64_t, stream_pointer> streams_;
    http3_buffered_response_writer buffered_response_writer_;
    std::size_t active_handler_tasks_{0};
    std::size_t accepted_request_heads_{0};
    std::size_t tunnel_streams_{0};
    lifecycle_type lifecycle_{lifecycle_type::open};
    bool drain_begun_{false};
    // The deadline the expiry timer is armed for, in ngtcp2 time.
    ngtcp2_tstamp armed_expiry_{UINT64_MAX};
    connection_scanner::entry scanner_entry_;
    std::optional<connection_scanner::guard> scanner_guard_;
    connection_scanner::periodic_check_registration scanner_checks_;
    // Declared last: unregistered before the state their completions touch.
    multiplexed_drain_timer drain_timer_;
    worker_timer_registration timer_;
};

}  // namespace ruvia::detail

#endif

#if defined(RUVIA_ENABLE_HTTP3)
#endif

namespace ruvia::detail {

class http1_request_buffer_completion;
struct access_log_sink;

// Initial read capacity for an active connection. Growth is bounded by pending
// input, and destruction returns the buffer to the worker resource on idle.
inline constexpr std::size_t initial_read_buffer_bytes = std::size_t{4} * 1024;

// Bytes of the connection-resident buffer an idle connection reads into while
// it holds no work set. Sized so a typical request line arrives in one read;
// longer heads finish in the regular read loop after the work set is
// re-acquired. It lives in every connection's frame, so growing it to cover
// whole browser heads (400-800 bytes with cookies) costs that much per idle
// connection to save one immediately-completing read per such request.
inline constexpr std::size_t idle_resident_read_bytes = 256;

// A connection's buffered request input with its growth and trim policy.
// Consumption advances the window; reads use the writable tail first and
// compact only when the consumed prefix covers the remaining bytes.
class connection_read_buffer final {
public:
    connection_read_buffer() noexcept;

    // Unconsumed bytes, starting with the current request.
    [[nodiscard]] std::string_view pending() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    std::string_view pending() const&& = delete;
    [[nodiscard]] bool empty() const noexcept {
        return storage_.empty();
    }

    // Room for the next socket read, bounded by max_http_header_bytes minus
    // pending bytes. Uses spare storage first, growing only when none remains.
    // Empty when pending bytes already fill the header limit.
    [[nodiscard]] std::span<char> prepare_read() & RUVIA_LIFETIMEBOUND;
    std::span<char> prepare_read() && = delete;
    // Appends `bytes` read into the span prepare_read() returned.
    void commit_read(std::size_t bytes) noexcept {
        storage_.commit(bytes);
    }

    // Drops the first `bytes` pending bytes (a served request).
    void consume(std::size_t bytes) noexcept;
    // Replaces the pending bytes with a copy of `bytes`, which never aliases
    // this buffer (an idle landing pad or a body runtime's over-read stash).
    void assign(std::string_view bytes);
    // Applies a reusable request's buffer completion. Installing a handed-over
    // pipeline can grow the storage, so this is the one completion step that
    // may allocate.
    void complete_request(const http1_request_buffer_completion& completion);

    // Gives storage that a handed-over pipeline grew past max_http_header_bytes
    // back once the pending bytes fit the initial size.
    void trim();

private:
    http_io_buffer storage_;
};

// Access-log fields of pipelined responses held back for a coalesced write.
// The request storage they come from is reused before that write, so the
// logged fields are copied; nothing is kept without an access log.
class http1_deferred_access_log final {
public:
    http1_deferred_access_log() noexcept;

    // Keeps the record of a response whose head completes at output offset
    // `head_end`. Duration is measured when the record is reported.
    void defer(std::size_t head_end, const http_request& request, ip_address client_address,
        std::chrono::steady_clock::time_point start, http_status_code status,
        const request_io_metrics& io_metrics = {});
    // Reports every kept response whose head lies within the first `written`
    // output bytes (in output order), then forgets all of them.
    void complete(const access_log_sink& access_log, ip_address remote_address, std::size_t written) noexcept;
    // Forgets every kept response and releases grown storage.
    void reset() noexcept;

private:
    struct entry_type final {
        std::size_t head_end_;
        std::size_t text_begin_;
        std::size_t method_bytes_;
        std::size_t path_bytes_;
        ip_address client_address_;
        std::chrono::steady_clock::time_point start_;
        request_io_metrics io_metrics_{};
        http_known_method known_method_;
        http_protocol_version protocol_version_;
        http_status_code status_;
    };

    std::vector<entry_type> entries_;
    std::string text_;
};

// Heavy request storage exists only while the connection is serving requests.
// A pipelined burst reuses it; idle connections retain only their session frame.
// The work set and its configured initial arena block share one allocation.
struct connection_work_set final {
    connection_work_set() noexcept;

    connection_read_buffer read_buffer_;
    http_response_head_buffer response_head_;
    http1_deferred_access_log deferred_access_;
    http1_server_request_parse_state parsed_;
    route_resolution route_resolution_;
};

[[nodiscard]] std::unique_ptr<connection_work_set> make_connection_work_set();

// Owns one admitted socket and its connection_slot as one linear value. The
// slot is acquired only after socket configuration succeeds, then moves into
// the lazy session task before task_scope::spawn. If coroutine allocation or
// spawn throws, destroying that cold ownership chain closes the socket first
// and releases the count second.
class accepted_connection_lease final {
public:
    accepted_connection_lease(asio::ip::tcp::socket socket, connection_count& count,
        connection_lifecycle lifecycle) noexcept
        : slot_(count),
          socket_(std::move(socket)),
          lifecycle_(std::move(lifecycle)) {
        lifecycle_.bind_transport(connection_transport::tcp(socket_));
    }

    accepted_connection_lease(const accepted_connection_lease&) = delete;
    accepted_connection_lease& operator=(const accepted_connection_lease&) = delete;
    accepted_connection_lease(accepted_connection_lease&& other) noexcept
        : slot_(std::move(other.slot_)),
          socket_(std::move(other.socket_)),
          lifecycle_(std::move(other.lifecycle_)) {
        lifecycle_.bind_transport(connection_transport::tcp(socket_));
    }
    accepted_connection_lease& operator=(accepted_connection_lease&&) = delete;

    ~accepted_connection_lease() {
        // Session bodies may close their socket before deferred frame retirement.
        // Keep the count until retirement, and close cold tasks or early exits here.
        if (socket_.is_open()) {
            std::error_code ignored;
            socket_.close(ignored);
        }
    }

    [[nodiscard]] asio::ip::tcp::socket& socket() & noexcept {
        return socket_;
    }
    asio::ip::tcp::socket& socket() && = delete;
    [[nodiscard]] connection_lifecycle& lifecycle() noexcept {
        return lifecycle_;
    }

private:
    // Declared first: released after the socket closes.
    connection_slot slot_;
    asio::ip::tcp::socket socket_;
    connection_lifecycle lifecycle_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The connection is closing, so no bytes from this request can be reused.
class http1_request_buffer_discarded final {
private:
    friend class http1_session_request_completion;

    constexpr http1_request_buffer_discarded() noexcept = default;
};

// The session still owns an unshifted read buffer and must remove exactly this
// request prefix before parsing the next pipelined request.
class http1_request_buffer_compaction final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_request_buffer_compaction(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}

    std::size_t consumed_bytes_;
};

// A request-body runtime over-read the next pipelined request into its own
// buffer, which dies with the route, and handed those bytes over in
// request-scoped storage that outlives the completion. The session installs
// them at its single buffer-cleanup point, once the response is written and
// the access log is recorded. A suffix still in the read buffer is a
// compaction instead.
class http1_request_buffer_pipeline_restore final {
public:
    [[nodiscard]] constexpr std::string_view pipeline() const noexcept {
        return pipeline_;
    }

private:
    friend class http1_session_request_completion;

    explicit constexpr http1_request_buffer_pipeline_restore(std::string_view pipeline) noexcept
        : pipeline_(pipeline) {}

    std::string_view pipeline_;
};

class http1_request_buffer_completion final {
public:
    [[nodiscard]] constexpr const http1_request_buffer_discarded* discarded() const& noexcept {
        return std::get_if<http1_request_buffer_discarded>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_discarded* discarded() const&& = delete;

    [[nodiscard]] constexpr const http1_request_buffer_compaction* compaction() const& noexcept {
        return std::get_if<http1_request_buffer_compaction>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_compaction* compaction() const&& = delete;

    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore()
        const& noexcept {
        return std::get_if<http1_request_buffer_pipeline_restore>(&value_);
    }
    [[nodiscard]] constexpr const http1_request_buffer_pipeline_restore* pipeline_restore() const&& =
        delete;

private:
    friend class http1_session_request_completion;

    using value_type = std::variant<http1_request_buffer_discarded, http1_request_buffer_compaction,
        http1_request_buffer_pipeline_restore>;

    template <typename alternative_type>
    explicit constexpr http1_request_buffer_completion(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

// A buffered response still needs the session's scatter-gather writer.
class http1_buffered_response_ready final {
private:
    friend class http1_session_request_completion;

    constexpr http1_buffered_response_ready() noexcept = default;
};

// A response-stream route already committed its final head. Only this
// alternative exposes the exact wire status used by access logging.
class http1_committed_stream_response final {
public:
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // The handler failed after the head was on the wire. A close-delimited or
    // TLS body ended by a clean close would read as complete (RFC 9112 §9.8),
    // so the connection is aborted rather than closed gracefully.
    [[nodiscard]] constexpr bool truncated() const noexcept {
        return truncated_;
    }

private:
    friend class http1_session_request_completion;

    constexpr http1_committed_stream_response(http_status_code status, bool truncated) noexcept
        : status_(status),
          truncated_(truncated) {}

    http_status_code status_;
    bool truncated_;
};

// A route's buffered response broke the HTTP/1 final-response rules, so it
// cannot be committed. The session replaces it through the exception handler
// on a closing connection before preparing the response for the wire.
class http1_rejected_final_response final {
public:
    [[nodiscard]] http1_response_head_error exception() const noexcept {
        return error_;
    }

private:
    friend class http1_session_request_completion;

    explicit http1_rejected_final_response(::ruvia::http1_response_head_error error) noexcept
        : error_(std::move(error)) {}

    ::ruvia::http1_response_head_error error_;
};

// One terminal result for a dispatched HTTP/1 request. Wire ownership,
// connection reuse, and read-buffer cleanup are committed together so the
// session never reconstructs them from an optional status plus scalar flags.
class http1_session_request_completion final {
public:
    [[nodiscard]] static http1_session_request_completion make_buffered_closing(
        http1_request_connection_plan connection_plan) noexcept {
        if (connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_unrestored(
        http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_buffered_pipeline_restore(
        http1_request_connection_plan connection_plan, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_buffered_response_ready{}, connection_plan,
            http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream(
        http1_request_connection_plan connection_plan, http_status_code status,
        std::size_t consumed_bytes) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status, false), connection_plan,
            unshifted_buffer_completion(connection_plan, consumed_bytes));
    }

    [[nodiscard]] static http1_session_request_completion make_committed_stream_pipeline_restore(
        http1_request_connection_plan connection_plan, http_status_code status, borrowed_text pipeline) noexcept {
        if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
            std::terminate();
        }
        return http1_session_request_completion(http1_committed_stream_response(status, false), connection_plan,
            http1_request_buffer_completion(http1_request_buffer_pipeline_restore(pipeline.view())));
    }

    [[nodiscard]] static http1_session_request_completion make_truncated_stream(
        http1_request_connection_plan connection_plan, http_status_code status) noexcept {
        return http1_session_request_completion(http1_committed_stream_response(status, true),
            connection_plan.require_close(), http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] static http1_session_request_completion make_rejected_final_response(
        http1_request_connection_plan connection_plan, ::ruvia::http1_response_head_error error) noexcept {
        return http1_session_request_completion(http1_rejected_final_response(std::move(error)),
            connection_plan.require_close(), http1_request_buffer_completion(http1_request_buffer_discarded{}));
    }

    [[nodiscard]] constexpr const http1_buffered_response_ready* buffered_response() const& noexcept {
        return std::get_if<http1_buffered_response_ready>(&value_);
    }
    [[nodiscard]] constexpr const http1_buffered_response_ready* buffered_response() const&& = delete;

    [[nodiscard]] constexpr const http1_committed_stream_response* committed_stream() const& noexcept {
        return std::get_if<http1_committed_stream_response>(&value_);
    }
    [[nodiscard]] constexpr const http1_committed_stream_response* committed_stream() const&& = delete;

    [[nodiscard]] constexpr const http1_rejected_final_response* rejected_final_response() const& noexcept {
        return std::get_if<http1_rejected_final_response>(&value_);
    }
    [[nodiscard]] constexpr const http1_rejected_final_response* rejected_final_response() const&& = delete;

    [[nodiscard]] constexpr http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

    [[nodiscard]] constexpr const http1_request_buffer_completion& buffer_completion() const& noexcept {
        return buffer_completion_;
    }
    [[nodiscard]] constexpr const http1_request_buffer_completion& buffer_completion() const&& = delete;

    // A buffered response can be replaced by a pre-commit policy error after
    // representation preparation (for example a forbidden identity fallback).
    // Rebind the final response plan without losing the exact read-buffer
    // cleanup alternative already established by request-body dispatch.
    [[nodiscard]] http1_session_request_completion with_buffered_connection_plan(
        http1_request_connection_plan connection_plan) const& noexcept {
        if (std::get_if<http1_buffered_response_ready>(&value_) == nullptr) {
            std::terminate();
        }
        if (buffer_completion_.discarded() != nullptr) {
            return make_buffered_closing(connection_plan.require_close());
        }
        if (const auto* compaction = buffer_completion_.compaction()) {
            return make_buffered_unrestored(connection_plan, compaction->consumed_bytes());
        }
        if (const auto* pipeline = buffer_completion_.pipeline_restore()) {
            if (connection_plan.disposition() == http1_close_policy::close_after_response) {
                return make_buffered_closing(connection_plan);
            }
            return make_buffered_pipeline_restore(connection_plan, pipeline->pipeline());
        }
        std::terminate();
    }

private:
    using value_type = std::variant<http1_buffered_response_ready, http1_committed_stream_response,
        http1_rejected_final_response>;

    [[nodiscard]] static http1_request_buffer_completion unshifted_buffer_completion(
        http1_request_connection_plan connection_plan, std::size_t consumed_bytes) noexcept {
        if (connection_plan.disposition() == http1_close_policy::close_after_response) {
            return http1_request_buffer_completion(http1_request_buffer_discarded{});
        }
        return http1_request_buffer_completion(http1_request_buffer_compaction(consumed_bytes));
    }

    template <typename alternative_type>
    http1_session_request_completion(alternative_type alternative, http1_request_connection_plan connection_plan,
        http1_request_buffer_completion buffer_completion) noexcept
        : value_(std::move(alternative)),
          connection_plan_(connection_plan),
          buffer_completion_(std::move(buffer_completion)) {}

    value_type value_;
    http1_request_connection_plan connection_plan_;
    http1_request_buffer_completion buffer_completion_;
};

}  // namespace ruvia::detail

#if defined(_WIN32)
#endif

namespace ruvia {
class worker_runtime;
}

namespace ruvia::detail {

using tcp_socket_type = asio::ip::tcp::socket;
class accepted_connection_lease;
class context_services;
template <typename stream_type>
struct http1_connection_session;
class route_table;
struct server_connection_fields;
class worker_capabilities;

// What serve_requests hands back to run_stream when the connection stays open:
// no input is left buffered (the next request is awaited without a work set),
// or a complete prior-knowledge HTTP/2 preface is pending in the work set's
// read buffer (RFC 9113 §3.3).
enum class http1_serve_handoff : std::uint8_t {
    await_request,
    cleartext_http2,
};

#if !defined(_WIN32)
// One worker's dup()s of a listener's sockets; udp_ is invalid without an
// HTTP/3 service.
struct listener_socket_duplicates final {
    native_socket tcp_{invalid_native_socket};
    native_socket udp_{invalid_native_socket};
};
#endif

// Worker-local listeners, connection resources and session assembly. The
// surrounding runtime owns lifecycle phases and the scope joining every task.
// Admission runs open -> stop, and stopping closes every connection at once:
// there is no drain. On POSIX each worker binds every TCP listener with
// SO_REUSEPORT and accepts on its own event loop; on Windows the
// application's acceptor accepts and hands each socket over (adopt_socket).
// A TLS listener with an HTTP/3 service also gets a QUIC endpoint
// (http3_endpoint), which admits and stops the same way.
//
// Listener slots are numbered alike on every worker (and in the Windows
// acceptor) and never reused within a run, so a hand-over queued for a
// removed listener cannot reach another. A listener added while running is
// prepared (bound, not admitting) on every worker before any opens it; a
// removed one stops admitting at once and retires -- slot emptied, its
// session configuration destroyed -- once every task it owns has ended.
class worker_connections final {
public:
    worker_connections(asio::io_context& io, const worker_ref& worker_value,
        const route_table& routes_value, worker_capabilities& capabilities,
        http_server_options& options, const stop_token& stop_token_value,
        http_server_worker_state& state_value, task_scope& tasks,
        std::span<const http_server_listener_definition> listeners, worker_failure_sink failure, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );
    worker_connections(const worker_connections&) = delete;
    worker_connections& operator=(const worker_connections&) = delete;
    void prepare();
    void open_admission() noexcept;
    // Worker thread, once the worker state left running: stops admission and
    // closes every connection at once.
    void stop() noexcept;
    void retire_tls() noexcept;
    // Worker thread: listener `listener_index` -- TLS over TCP and its
    // HTTP/3 service alike -- selects every later handshake's identity from
    // `identities`.
    void adopt_tls_identities(
        std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept;
    // Worker thread, the controls of application::add_listener(): prepares
    // `definition` in slot `index` as prepare() would -- TLS contexts, bound
    // or adopted sockets, the HTTP/3 endpoint in slot `http3_index` -- without
    // admitting, or throws with the slot left empty.
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition,
        bool externally_accepted = false
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory = nullptr
#endif
    );
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition, bool externally_accepted,
        prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory = nullptr
#endif
    );
    // 唤醒准备阶段已经创建的接入任务；提交不再分配监听资源。
    void open_listener(std::size_t index) noexcept;
    // Closes the sockets of a listener prepared but never opened and retires
    // it; an empty slot is left alone.
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept;
    // application::remove_listener(): stops admitting listener `index` and
    // closes its TCP socket at once, holding `receipt` until its accept loop
    // exited. Its connections are served on; its HTTP/3 endpoint stops
    // admitting and closes its socket after its last connection.
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt, listener_remove_options options = {}) noexcept;
#if !defined(_WIN32)
    // application::duplicate_listener_sockets(): this worker's sockets of
    // listener `index`, duplicated. Throws std::logic_error when it no longer
    // admits.
    [[nodiscard]] listener_socket_duplicates duplicate_listener(std::size_t index);
#endif
    // Takes an already accepted native socket, including ownership on failure.
    // It first binds that socket to this worker's execution context.
    void adopt_connection(std::size_t index, native_socket socket, adopt_options options = {});
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] std::shared_ptr<connection_count> listener_connections(const asio::ip::tcp::endpoint& endpoint) const;
    // Owning worker only; false for an unknown or retired connection ID.
    [[nodiscard]] bool close_connection(std::uint64_t id) noexcept;
#if defined(_WIN32)
    // This worker as the acceptor's hand-over target.
    [[nodiscard]] windows_acceptor::business_worker acceptor_target() noexcept;
    void bind_acceptor(windows_acceptor& acceptor) noexcept {
#if defined(RUVIA_ENABLE_HTTP3)
        http3_placement_.acceptor_ = &acceptor;
        http3_services_.placement_.acceptor_ = &acceptor;
#else
        (void)acceptor;
#endif
    }
#endif

private:
    struct listener final {
        listener(asio::io_context& io, const worker_ref& worker_value,
            const http_server_listener_definition& definition,
            prepared_http_server_session session = {})
            : session_owner_(std::move(session)),
              session_(session_owner_ ? *session_owner_ : inline_session_.emplace(definition)),
              endpoint_(definition.endpoint_),
              tasks_(worker_value),
              retired_(worker_value),
              activated_(worker_value)
#if !defined(_WIN32)
              ,
              socket_(io),
              inherited_(definition.inherited_sockets_)
#endif
        {
#if defined(_WIN32)
            (void)io;
#endif
        }

        prepared_http_server_session session_owner_;
        std::optional<http_server_session_config> inline_session_;
        http_server_session_config& session_;
        asio::ip::tcp::endpoint endpoint_;
        // Its accept loop, sessions and HTTP/3 endpoint tasks, all joined
        // before the listener is destroyed: they borrow session_.
        task_scope tasks_;
        // Notified once removal or the worker's stop ended admission.
        worker_signal retired_;
        worker_signal activated_;
        // 取消准备持有至该监听的全部任务已 join；强制移除不等待调用它的会话自身。
        std::shared_ptr<void> retirement_receipt_;
#if defined(RUVIA_ENABLE_HTTP3)
        std::optional<std::size_t> http3_index_;
#endif
#if !defined(_WIN32)
        asio::ip::tcp::acceptor socket_;
        // Until preparation claimed this worker's sockets.
        std::shared_ptr<inherited_listener_sockets> inherited_;
        // A removal's, until the accept loop exited.
        std::shared_ptr<void> removal_receipt_;
        bool accepting_{false};
#endif
        // From preparation on, a task awaits retired_ to retire the listener.
        bool prepared_{false};
        // From opening until removal or the worker's stop.
        bool admitting_{false};
        bool externally_accepted_{false};
    };
#if defined(RUVIA_ENABLE_HTTP3)
    void add_http3_endpoint(std::size_t listener_index, std::size_t http3_index);
    void receive_http3_handoff(std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;
#endif
    // Builds the listener's TLS contexts, binds or adopts its sockets and
    // starts the task that retires it.
    void prepare_listener_resources(std::size_t index);
    void start_listener(std::size_t index) noexcept;
    void close_listener_sockets(listener& configured) noexcept;
    // Awaits the listener's retired_, joins its tasks, then empties its slot.
    task<void> retire_listener(std::size_t index);
#if defined(_WIN32)
    // The worker thread: binds a socket the acceptor accepted to this
    // worker's io_context (its first IOCP association) and serves it.
    void adopt_socket(std::size_t listener_index, windows_accepted_socket accepted);
#else
    void close_listeners() noexcept;
    task<void> run_accept_loop(std::size_t index);
#endif
    void report_accept_failure(std::exception_ptr failure, bool fatal) noexcept;
    task<void> rotate_tls_ticket_keys();
    void accept_socket(std::size_t index, tcp_socket_type socket, adopt_options options = {});
    task<void> prepare_connection(std::size_t index, tcp_socket_type socket,
        std::vector<std::byte> prefix, std::optional<ip_endpoint> remote);
    void admit_socket(std::size_t index, tcp_socket_type socket, std::optional<ip_endpoint> remote,
        std::vector<std::byte> prefix, std::size_t bytes_read = 0);
    // `remote` is the peer accept_socket() read before the socket moved into
    // the lease; absent when the socket no longer knew its peer.
    task<void> run_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix);
    task<void> run_tls_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix);
    void report_session_failure(
        tcp_socket_type& socket, std::string_view remote_address, std::exception_ptr failure) noexcept;
    template <typename stream_type>
    task<http_connection_close> run_stream(http_server_session_config& listener_value, stream_type& stream, tcp_socket_type& socket, context_services services);
#if defined(RUVIA_ENABLE_HTTP2)
    template <typename stream_type>
    task<http_connection_close> run_http2(stream_type& stream, tcp_socket_type& socket, context_services services);
#endif
    // A listener_dispatch::connection listener's connection: application::on_connection
    // owns it until its task completes. `fields` names everything but the
    // local endpoint, which this reads from `socket`.
    template <typename stream_type>
    task<void> serve_connection(stream_type& stream, tcp_socket_type& socket, server_connection_fields fields);
    template <typename stream_type>
    task<std::variant<http1_serve_handoff, http_connection_close>> serve_requests(
        http1_connection_session<stream_type>& session, connection_work_set& work_set);
    asio::io_context& io_;
    const worker_ref& worker_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    const stop_token& stop_token_;
    http_server_worker_state& state_;
    task_scope& tasks_;
    worker_failure_sink failure_;
    // This worker's position among the application's workers: the inherited
    // socket of each listener it claims.
    std::size_t worker_index_;
    // Indexed by listener slot; empty once a listener retired or failed to
    // prepare.
    std::vector<std::unique_ptr<listener>> listeners_;
    connection_scanner scanner_;
    // Caller-thread statistics and the Windows acceptor's balancing read it.
    connection_count connection_count_;
    struct listener_gauge final {
        listener_gauge(const asio::ip::tcp::endpoint& endpoint, connection_count& total)
            : endpoint_(endpoint),
              count_(&total) {}
        asio::ip::tcp::endpoint endpoint_;
        connection_count count_;
        std::atomic<bool> serving_{false};
        std::atomic<bool> published_{false};
    };
    void prepare_listener_gauge(std::size_t index, const asio::ip::tcp::endpoint& endpoint);
    mutable std::mutex gauges_mutex_;
    std::vector<std::shared_ptr<listener_gauge>> gauges_{};
    connection_id_sequence connection_ids_;
    connection_registry registry_;
    std::atomic<std::size_t> refused_{};
    std::atomic<std::size_t> failures_{};
    std::atomic<std::size_t> accept_failures_{};
    bool rotating_ticket_keys_{false};
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_placement http3_placement_;
    http3_worker_services http3_services_;
    bool http3_attached_{false};
    // Indexed by HTTP/3 slot, like listeners_. Declared last: destroyed
    // before every service its connections borrow.
    std::vector<std::unique_ptr<http3_endpoint>> http3_endpoints_;
#endif
};

}  // namespace ruvia::detail

namespace ruvia {
class env;
}  // namespace ruvia

namespace ruvia::detail {

class context_services;
struct connection_failure_sink;

struct worker_capability_definitions final {
#ifdef RUVIA_ENABLE_DATABASE
    std::span<const db_definition> databases_{};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::span<const redis_definition_type> redis_{};
#endif
    std::span<const worker_state_definition> worker_states_{};
    std::span<const http_client_definition_type> http_clients_{};
};

struct worker_capability_options final {
    std::optional<rate_limit_rule> default_rate_limit_{};
    route_rate_limit_presence route_rate_limits_{route_rate_limit_presence::absent};
    std::size_t rate_limit_capacity_{default_rate_limit_capacity_per_worker};
    std::size_t max_decoded_body_bytes_{default_max_buffered_body_bytes};
    http_client_worker_config http_client_worker_{};
    const env* env_{nullptr};
    const http_client_ip_extractor* trusted_proxies_{nullptr};
    bool precompressed_static_files_{false};
    // The server write_timeout_ bounding a file response's open on the pool.
    std::optional<timeout_duration> file_io_timeout_{};
    // The worker's connection failure outlet; required.
    const connection_failure_sink* connection_failure_{nullptr};
};

// Owns every application capability attached to one worker. Construction,
// startup, request exposure, and shutdown all follow this single ownership
// boundary; none of these registries is process-global or shared by workers.
// start()/stop() are the one worker bring-up and retirement chain, shared by
// the server worker and test_app. Both run on the owning worker.
class worker_capabilities final {
public:
    worker_capabilities(asio::io_context& io_context, const worker_ref& worker_value,
        worker_capability_definitions definitions,
        worker_capability_options options);
    worker_capabilities(asio::io_context&, worker_ref&&,
        worker_capability_definitions, worker_capability_options) = delete;

    worker_capabilities(const worker_capabilities&) = delete;
    worker_capabilities& operator=(const worker_capabilities&) = delete;

    // Creates the per-worker state, then connects remote capabilities. A
    // failure retires everything this call started before rethrowing.
    [[nodiscard]] task<void> start();
    // Interrupts capability I/O; idempotent. Retirement completes in stop().
    void close_now() noexcept;
    // Closes capability I/O, joins in-flight client work, then destroys the
    // per-worker state. Safe after a failed or skipped start().
    [[nodiscard]] task<void> stop();

    [[nodiscard]] context_services make_context_services(const stop_token& stop_token);
    context_services make_context_services(stop_token&&) = delete;
    [[nodiscard]] worker_client_registry_view client_registries() noexcept;
    [[nodiscard]] const worker_state_registry& worker_states() const noexcept;

    // Owner thread, together with adoption of the first routed table.
    void adopt_route_rate_limiter(rate_limiter& prepared) noexcept {
        rate_limiter_.adopt_route_storage(prepared);
    }

private:
    const worker_ref& worker_;
#ifdef RUVIA_ENABLE_REDIS
    redis_registry redis_;
#endif
#ifdef RUVIA_ENABLE_DATABASE
    db_registry databases_;
#endif
    http_client_worker_services http_client_services_;
    http_client_registry http_clients_;
    worker_state_registry worker_states_;
    rate_limiter rate_limiter_;
    worker_capability_options options_;
    context_worker_services worker_services_;
};

}  // namespace ruvia::detail

namespace ruvia {

class http_client;
class worker_ref;
#ifdef RUVIA_ENABLE_DATABASE
class db_client;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_client;
#endif

namespace detail {

// Caller-owned standalone clients a router lends to the requests it
// dispatches. Every entry borrows its client, which must run on the router's
// worker and outlive every dispatch; aliases are unique per client kind. The
// set is sealed with its router, so request-time lookup only reads it.
class borrowed_clients final {
public:
    borrowed_clients();
    borrowed_clients(const borrowed_clients&) = delete;
    borrowed_clients& operator=(const borrowed_clients&) = delete;

    [[nodiscard]] bool empty() const noexcept;

    void bind_http_client(const worker_ref& worker, std::string_view alias, const http_client& client);
    [[nodiscard]] const http_client* http_clients(std::string_view alias) const noexcept;

#ifdef RUVIA_ENABLE_DATABASE
    void bind_database(const worker_ref& worker, std::string_view alias, const db_client& client);
    [[nodiscard]] const db_client* database(std::string_view alias) const noexcept;
#endif

#ifdef RUVIA_ENABLE_REDIS
    void bind_redis(const worker_ref& worker, std::string_view alias, const redis_client& client);
    [[nodiscard]] const redis_client* redis(std::string_view alias) const noexcept;
#endif

private:
    template <typename client_type>
    struct alias_entry final {
        std::string alias_;
        const client_type* client_;
    };

    std::vector<alias_entry<http_client>> http_clients_;
#ifdef RUVIA_ENABLE_DATABASE
    std::vector<alias_entry<db_client>> databases_;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::vector<alias_entry<redis_client>> redis_;
#endif
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

enum class app_lifecycle_state : std::uint8_t {
    stopped,
    preparing,
    starting,
    running,
    stopping,
};

enum class app_stop_request : std::uint8_t {
    ignored,
    // Startup was in progress: the runtime stops at once, rolling it back.
    startup,
    // The app was serving: the run() owner stops admission, then the runtime.
    serving,
};

// application owns the mutex protecting this state machine. application::run() is the sole
// lifecycle owner; other threads may only request the monotonic transition to
// stopping. A stop requested while stopped is latched and consumed by the next run.
class app_lifecycle final {
public:
    [[nodiscard]] bool active() const noexcept {
        return state_ != app_lifecycle_state::stopped;
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ == app_lifecycle_state::stopping;
    }

    // Serving, with no stop requested yet.
    [[nodiscard]] bool running() const noexcept {
        return state_ == app_lifecycle_state::running;
    }

    [[nodiscard]] bool begin_run() noexcept {
        if (state_ != app_lifecycle_state::stopped) {
            return false;
        }
        state_ = stop_pending_ ? app_lifecycle_state::stopping : app_lifecycle_state::preparing;
        stop_pending_ = false;
        return true;
    }

    [[nodiscard]] bool publish_runtime() noexcept {
        if (state_ != app_lifecycle_state::preparing) {
            return false;
        }
        state_ = app_lifecycle_state::starting;
        return true;
    }

    [[nodiscard]] bool mark_running() noexcept {
        if (state_ != app_lifecycle_state::starting) {
            return false;
        }
        state_ = app_lifecycle_state::running;
        return true;
    }

    [[nodiscard]] app_stop_request request_stop() noexcept {
        switch (state_) {
            case app_lifecycle_state::preparing:
            case app_lifecycle_state::starting:
                state_ = app_lifecycle_state::stopping;
                return app_stop_request::startup;
            case app_lifecycle_state::running:
                state_ = app_lifecycle_state::stopping;
                return app_stop_request::serving;
            case app_lifecycle_state::stopped:
                stop_pending_ = true;
                return app_stop_request::ignored;
            case app_lifecycle_state::stopping:
                return app_stop_request::ignored;
        }
        return app_stop_request::ignored;
    }

    void complete_run() noexcept {
        state_ = app_lifecycle_state::stopped;
    }

private:
    app_lifecycle_state state_{app_lifecycle_state::stopped};
    bool stop_pending_{};
};

struct app_runtime_graph;

struct app_document_root_config final {
    explicit app_document_root_config(static_root_config_storage configured_static_options)
        : static_options_(std::move(configured_static_options)) {}

    http_native_path_string_type root_;
    static_root_config_storage static_options_;
    std::chrono::milliseconds refresh_interval_{};
    static_root_precompression_options precompression_;
    static_root_resident_options resident_;
};

struct app_state final {
    app_state();
    ~app_state();

    std::vector<http_server_listener_definition> listeners_;
    callback_registration<access_log_callback> access_log_callback_;
    callback_registration<connection_failure_callback> connection_failure_callback_;
    callback_registration<tls_client_hello_callback> tls_client_hello_callback_;
    callback_registration<http2_input_callback> http2_input_callback_;
#if defined(RUVIA_ENABLE_HTTP2)
    callback_registration<http2_frame_callback> http2_frame_callback_;
    callback_registration<http2_headers_callback> http2_headers_callback_;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    callback_registration<http3_frame_callback> http3_frame_callback_;
    callback_registration<http3_field_callback> http3_field_callback_;
#endif
    callback_registration<connection_admission_callback> connection_admission_callback_;
    callback_registration<connection_open_callback> connection_open_callback_;
    callback_registration<connection_close_callback> connection_close_callback_;
    callback_registration<connection_callback> connection_callback_;
    callback_registration<request_stream_callback> request_stream_callback_;
    http_server_options options_{};
    std::optional<app_document_root_config> document_root_config_;
    app_configuration configuration_;
    std::optional<blocking_pool_options> blocking_pool_{std::in_place};
    std::vector<callback_registration<app_hook>> on_start_hooks_;
    std::vector<callback_registration<app_hook>> on_stop_hooks_;
#ifdef RUVIA_ENABLE_DATABASE
    std::vector<db_definition> databases_;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::vector<redis_definition_type> redis_;
#endif
    std::vector<http_client_definition_type> http_clients_;
    env env_;
    std::unique_ptr<app_runtime_graph> runtime_;

    mutable std::mutex mutex_;
    std::condition_variable lifecycle_changed_;
    app_lifecycle lifecycle_;
    // Controls never hold a mutex across suspension. The synchronous run()
    // owner keeps the published runtime and frozen configuration alive until
    // every admitted control (including its blocking preparation) has joined.
    runtime_control_gate runtime_control_gate_;
    std::size_t runtime_controls_{};
};

inline void ensure_app_not_running(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}

template <typename configure_type>
application& mutate_stopped_app(
    application& app, app_state& state_value, const char* running_message, configure_type&& configure) {
    std::lock_guard lock(state_value.mutex_);
    ensure_app_not_running(state_value.lifecycle_.active(), running_message);
    std::forward<configure_type>(configure)(state_value);
    return app;
}

void run_app(application& app, app_state& state_value);
// The one stop chain for a published runtime: closes admission on the acceptor
// and every worker. The caller holds state_value.mutex_; run_app owns retirement.
void request_runtime_stop(app_state& state_value) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct env_variable final {
    std::string name_;
    std::string value_;
};

struct env_state final {
    std::vector<env_variable> variables_;
    bool loaded_{false};
};

[[nodiscard]] std::vector<env_variable> read_dotenv_entries(
    std::istream& input, const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path dotenv_executable_directory();
// prefix followed by path in UTF-8. path::string() narrows a Windows wide path
// through the ANSI code page, which throws or yields non-UTF-8 error text.
[[nodiscard]] std::string dotenv_path_message(std::string_view prefix, const std::filesystem::path& path);

}  // namespace ruvia::detail

namespace ruvia::detail {

class worker_capabilities;
class worker_state_registry;

// The endpoint behind every web_worker_ref. Core owns admission: posts go
// through worker_ref::post_payload and accepted jobs start only while that
// worker accepts and its stop token is clear. Started jobs run in the worker's
// task_scope, which the server worker joins before retiring capabilities.
//
// Escaped handles share this object so their terminal queries (valid,
// accepting, id, stats) remain safe after the server is gone. The borrowed
// server state below is touched only by jobs started on the worker; core
// abandons every other accepted job without running it.
class web_worker_dispatch final {
public:
    using job_type = move_only_function<ruvia::task<void>(web_worker_context&)>;

    web_worker_dispatch(ruvia::worker_runtime& runtime,
        worker_capabilities& capabilities, task_scope& jobs, const stop_token& stop_token_value);

    web_worker_dispatch(const web_worker_dispatch&) = delete;
    web_worker_dispatch& operator=(const web_worker_dispatch&) = delete;

    [[nodiscard]] static web_worker_ref make_ref(std::shared_ptr<web_worker_dispatch> dispatch) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_post_result post(job_type job);
    [[nodiscard]] web_worker_stats stats() const noexcept;

    // Reconciles the outstanding reservation of an accepted job that core
    // destroys without running. Public only for the reservation deleter.
    void abandon() noexcept;

private:
    void start(job_type job);
    [[nodiscard]] ruvia::task<void> run(job_type job);

    // A stable copy: queries stay valid after the server's runtime is gone.
    worker_ref worker_;
    ruvia::worker_runtime& runtime_;
    worker_client_registry_view client_registries_;
    const worker_state_registry& worker_states_;
    task_scope& jobs_;
    const stop_token& stop_token_;
    std::atomic_uint64_t accepted_{0};
    std::atomic_uint64_t queue_full_{0};
    std::atomic_uint64_t worker_stopping_{0};
    std::atomic_uint64_t completed_{0};
    std::atomic_uint64_t failed_count_{0};
    std::atomic_size_t outstanding_{0};
};

}  // namespace ruvia::detail

namespace ruvia {
class worker_ref;
}  // namespace ruvia

namespace ruvia::detail {

struct http_server_options;

// The application's one document-root refresher. The first registered worker
// walks the root, reads small files into the snapshot and precompresses changed files
// once per interval; every worker adopts the published immutable snapshot on
// its own thread. The runtime graph owns this object, so it outlives every
// worker and their joined refresh tasks.
class document_root_publisher final {
public:
    // Builds the initial snapshot; a failure here fails application startup.
    document_root_publisher(const std::filesystem::path& root, const static_root_config_storage& config,
        std::chrono::milliseconds interval, static_root_precompression_options precompression,
        static_root_resident_options resident, std::size_t worker_count);
    ~document_root_publisher();

    document_root_publisher(const document_root_publisher&) = delete;
    document_root_publisher& operator=(const document_root_publisher&) = delete;

    [[nodiscard]] std::chrono::milliseconds interval() const noexcept {
        return interval_;
    }

    [[nodiscard]] std::size_t failures() const noexcept {
        return failures_.load(std::memory_order_relaxed);
    }

    // Counts a failed refresh pass or adoption and reports its cause through
    // the rate-limited unhandled-failure sink; the last complete snapshot
    // stays published.
    void record_failure(std::exception_ptr failure) noexcept;

    // One rebuild pass on the refresher. A changed tree is published; a
    // failed pass is counted and the last complete snapshot stays published.
    [[nodiscard]] task<void> refresh(
        const worker_ref& worker, const http_server_worker_state& state, const stop_token& stop = {});

private:
    friend class static_root_runtime;

    using publication_owner =
        std::unique_ptr<document_root_snapshot>;

    struct mailbox final {
        ~mailbox();

        // Exchange transfers exclusive ownership of the node, not a borrowed
        // shared_ptr source. Exactly one exchanging thread reclaims each node.
        std::atomic<document_root_snapshot*> pending_{nullptr};
    };

    static_assert(std::atomic<document_root_snapshot*>::is_always_lock_free);

    // The coordinator registers every worker before starting any of them.
    // The fixed mailbox storage then outlives all workers and joined tasks.
    // The first mailbox identifies the one refresher.
    [[nodiscard]] mailbox& register_worker() noexcept;

    void publish(document_root_snapshot snapshot);
    void commit_publication(document_root_snapshot snapshot,
        std::vector<publication_owner>& publication) noexcept;

    // The configured path, not the snapshot's canonical one: a refresh must
    // re-resolve symlinks so a retargeted root (atomic deploy) is followed.
    std::filesystem::path root_;
    std::chrono::milliseconds interval_;
    static_root_precompression_options precompression_;
    static_root_resident_options resident_;
    // Only the first registered worker reads or replaces current_ after startup.
    document_root_snapshot current_;
    std::vector<mailbox> mailboxes_;
    std::size_t registered_workers_{0};
    std::atomic<std::size_t> failures_{0};
};

// One worker's adopted document-root generations. Request bindings lease the
// current generation with a plain counter; a superseded generation stays until
// its leases drain, so a long request pins only the snapshot it borrows. The
// worker joins refresh() before teardown.
class static_root_runtime final {
public:
    static_root_runtime(http_server_options& options, const worker_ref& worker_value,
        const http_server_worker_state& state_value);
    ~static_root_runtime();
    static_root_runtime(const static_root_runtime&) = delete;
    static_root_runtime& operator=(const static_root_runtime&) = delete;

    [[nodiscard]] bool enabled() const noexcept {
        return publisher_ != nullptr;
    }

    // Once per interval while the worker admits work: reclaim drained
    // generations, run the application's rebuild if this worker claimed it,
    // then adopt the published snapshot.
    [[nodiscard]] task<void> refresh(const stop_token& stop = {});

private:
    using generation_owner =
        std::unique_ptr<document_root_generation>;

    void adopt_published();

    http_server_options& options_;
    document_root_publisher* publisher_;
    document_root_publisher::mailbox* mailbox_;
    const worker_ref& worker_;
    const http_server_worker_state& state_;
    // Retained locally if preparing a worker generation fails; a later
    // publication supersedes it instead of accumulating missed snapshots.
    document_root_publisher::publication_owner pending_;
    generation_owner current_;
    std::vector<generation_owner> retired_;
    const bool refreshes_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class route_table;
class validated_http_server_configuration;
class web_worker_dispatch;

// One server worker: its core worker_runtime plus the HTTP domain state that
// runtime's hooks compose. Core owns the generic lifecycle (launch, ready/serve
// barrier, stop admission, finalization, join, first failure); application
// sequences every independently listening worker through one core worker_group.
class web_worker_runtime final {
public:
    // `pool` is the application pool the worker's event loop borrows for
    // every offload and client host lookup; null when the application
    // disabled it. It must outlive the worker. `worker_index` is this
    // worker's position among the application's workers.
    web_worker_runtime(const validated_http_server_configuration& configuration, const route_table& routes_value,
        worker_capability_definitions capabilities, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );
    ~web_worker_runtime();

    web_worker_runtime(const web_worker_runtime&) = delete;
    web_worker_runtime& operator=(const web_worker_runtime&) = delete;

    [[nodiscard]] ruvia::worker_runtime& runtime() & noexcept {
        return runtime_;
    }
    ruvia::worker_runtime& runtime() && = delete;
    [[nodiscard]] stop_token cancellation() const noexcept {
        return stop_token_;
    }
    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] std::shared_ptr<connection_count> listener_connections(const asio::ip::tcp::endpoint& endpoint) const {
        return connections_.listener_connections(endpoint);
    }
    [[nodiscard]] web_worker_ref web_worker() const;
    // Owner thread, before admitting this run's first routed listener.
    void adopt_route_rate_limiter(rate_limiter& prepared) noexcept {
        capabilities_.adopt_route_rate_limiter(prepared);
    }
    // Owner thread only, from a control posted to runtime(): listener
    // `listener_index` -- TLS over TCP and its HTTP/3 service alike --
    // selects every later handshake's identity from `identities`.
    void adopt_tls_identities(
        std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept {
        connections_.adopt_tls_identities(listener_index, std::move(identities));
    }
    // Owner thread only, from controls posted to runtime(): the listener
    // transactions of application::add_listener() and remove_listener()
    // (see worker_connections).
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition, prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory
#endif
    ) {
        connections_.prepare_listener(index, http3_index, definition, false, std::move(session)
#if defined(RUVIA_ENABLE_HTTP3)
                                                                                 ,
            directory
#endif
        );
    }
    void open_listener(std::size_t index) noexcept {
        connections_.open_listener(index);
    }
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept {
        connections_.discard_listener(index, std::move(receipt));
    }
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt, listener_remove_options options = {}) noexcept {
        connections_.remove_listener(index, std::move(receipt), options);
    }
    [[nodiscard]] bool close_connection(std::uint64_t connection_id) noexcept {
        return connections_.close_connection(connection_id);
    }
#if !defined(_WIN32)
    [[nodiscard]] listener_socket_duplicates duplicate_listener(std::size_t index) {
        return connections_.duplicate_listener(index);
    }
#endif
#if defined(_WIN32)
    // This worker as the Windows acceptor's hand-over target.
    [[nodiscard]] windows_acceptor::business_worker acceptor_target() noexcept {
        return connections_.acceptor_target();
    }
#endif

private:
    web_worker_runtime(std::span<const http_server_listener_definition> listeners,
        const route_table& routes_value, worker_capability_definitions capabilities,
        http_server_options validated_options, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );

    void serve_on_context();
    void stop_admission_on_context() noexcept;
    void finalize_on_context() noexcept;
    task<void> run_worker();
    ruvia::worker_runtime runtime_;
    asio::io_context& io_context_;
    worker_signal finalize_signal_;
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    const route_table& routes_;
    // Every connection's inbound buffers charge this worker-wide budget.
    inbound_buffer_budget inbound_budget_;
    // Borrowed through options_.codecs_ by requests and connections, which
    // all end before it is destroyed.
    worker_codecs codecs_;
    task_scope background_tasks_;
    http_server_worker_state worker_state_{http_server_worker_state::fresh};
    http_server_options options_;
    static_root_runtime static_roots_;
    worker_capabilities capabilities_;
    worker_connections connections_;
    // Shared only with escaped public handles: their terminal queries outlive
    // this worker. Jobs run in background_tasks_ and are joined by run_worker.
    std::shared_ptr<web_worker_dispatch> web_worker_dispatch_;
    // Atomic because stats() reads them from the caller's thread while this
    // worker updates them. Relaxed: they are counters, and publish nothing.
    std::atomic<std::size_t> worker_failures_{0};
};

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP3)
#endif
#if defined(_WIN32)
#endif

namespace ruvia::detail {

struct app_worker_slot final {
    app_worker_slot(controller_store configured_controllers,
        std::unique_ptr<route_table> configured_routes,
        std::unique_ptr<web_worker_runtime> configured_runtime)
        : controllers_(std::move(configured_controllers)),
          routes_(std::move(configured_routes)),
          runtime_(std::move(configured_runtime)) {}

    app_worker_slot(app_worker_slot&&) noexcept = default;
    app_worker_slot& operator=(app_worker_slot&&) noexcept = default;
    app_worker_slot(const app_worker_slot&) = delete;
    app_worker_slot& operator=(const app_worker_slot&) = delete;

    // Destruction is intentionally reversed: runtime, routes, controllers.
    controller_store controllers_;
    std::unique_ptr<route_table> routes_;
    std::unique_ptr<web_worker_runtime> runtime_;
};

struct app_runtime_graph final {
    app_runtime_graph() = default;

    // Declared before workers so it is destroyed after suspended worker tasks.
    std::unique_ptr<blocking_pool> blocking_pool_;
    // The one document-root refresher; null without a document root. Workers
    // borrow it and adopt its snapshots, so it is declared before them.
    std::unique_ptr<document_root_publisher> document_root_;
    // Every worker's route_table binds its instances to this one immutable plan.
    compiled_route_plan_ptr_type route_plan_;
    // A non-routed run keeps its original empty plan alive if its first routed
    // listener is added while serving; a stopping worker may still borrow it.
    compiled_route_plan_ptr_type empty_route_plan_;
    bool routes_enabled_{false};
#if defined(RUVIA_ENABLE_HTTP3)
    // Hands QUIC datagrams to the worker that owns their connection; null
    // without an HTTP/3 listener. Declared before the workers and the
    // acceptor, whose queued hand-overs reference it.
    std::unique_ptr<http3_worker_directory> http3_directory_;
#endif
#if defined(_WIN32)
    // The one thread that owns every listener and hands sockets and datagrams
    // to the workers. Declared before them: they send their datagrams
    // through it until they joined, so it is destroyed after them.
    std::unique_ptr<windows_acceptor> acceptor_;
#endif
    std::vector<app_worker_slot> workers_;
    // The one lifecycle over every independently listening worker. It borrows
    // their core runtimes, so it is declared after them.
    worker_group group_;
    // Guarded by the application mutex. Worker handles are published only
    // once every worker is ready, so a posted job never meets a starting worker.
    bool workers_ready_{false};
    // Guarded by the application mutex. The TCP endpoint each listener slot
    // serves: slot i is listener i on every worker (and in the Windows
    // acceptor), nullopt once removed or while being added. Slots are never
    // reused, so a queued hand-over cannot reach another listener.
    std::vector<std::optional<asio::ip::tcp::endpoint>> listener_slots_;
    // HTTP/3 services are numbered alike, apart from the TCP listeners.
    std::size_t http3_slot_count_{0};
    // Where every TLS listener's ticket key epochs start, so a listener
    // added while the run serves rotates with the others.
    std::chrono::steady_clock::time_point tls_key_origin_{};
};

}  // namespace ruvia::detail
