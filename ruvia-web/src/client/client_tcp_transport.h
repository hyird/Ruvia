#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/types.h>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/tls_config.h"

#include "http/tls_protocol_policy.h"
#include "server/connection_lifecycle.h"

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
