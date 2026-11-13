#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ssl/context.hpp>
#include <openssl/types.h>

#include "ruvia/core/async.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_content_encoder.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/error.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/server_connection.h"
#include "ruvia/web/server_observation.h"
#include "ruvia/web/tls_config.h"

#include "http/tls_protocol_policy.h"
#include "integration/operation_interrupt.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>
#endif
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_signal.h"

namespace ruvia::detail {

#if defined(_WIN32)
// INVALID_SOCKET.
inline constexpr native_socket invalid_native_socket = ~native_socket{0};
#else
inline constexpr native_socket invalid_native_socket = -1;
#endif

// Closes a socket the server owns; an invalid handle is ignored.
void close_native_socket(native_socket socket) noexcept;

// listen_config::inherited_sockets_ adoption: checks that `socket` is a
// listening TCP socket bound to exactly `endpoint` (std::invalid_argument
// with `message` otherwise), then marks it close-on-exec (non-inheritable on
// Windows).
void adopt_inherited_tcp_socket(native_socket socket, const asio::ip::tcp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only);
// The same for a UDP socket bound to exactly `endpoint`.
void adopt_inherited_udp_socket(native_socket socket, const asio::ip::udp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only);

#if !defined(_WIN32)
// A dup() of `socket`, without FD_CLOEXEC, that the caller owns.
[[nodiscard]] native_socket duplicate_native_socket(native_socket socket);
#endif

// The adopted sockets of one listener: its TCP service's and, for a TLS
// listener, its HTTP/3 service's, in worker order on POSIX and one each on
// Windows. Shared by the listener's definition and its clones until the run
// that prepares the listener claims them; whatever is still unclaimed closes
// with the last owner.
class inherited_listener_sockets final {
public:
    inherited_listener_sockets(std::vector<native_socket> tcp, std::vector<native_socket> udp) noexcept
        : tcp_(std::move(tcp)),
          udp_(std::move(udp)) {}
    ~inherited_listener_sockets();

    inherited_listener_sockets(const inherited_listener_sockets&) = delete;
    inherited_listener_sockets& operator=(const inherited_listener_sockets&) = delete;
    inherited_listener_sockets(inherited_listener_sockets&&) = delete;
    inherited_listener_sockets& operator=(inherited_listener_sockets&&) = delete;

    [[nodiscard]] std::span<const native_socket> tcp() const& noexcept RUVIA_LIFETIMEBOUND {
        return tcp_;
    }
    std::span<const native_socket> tcp() const&& = delete;
    [[nodiscard]] std::span<const native_socket> udp() const& noexcept RUVIA_LIFETIMEBOUND {
        return udp_;
    }
    std::span<const native_socket> udp() const&& = delete;

    // Takes TCP socket `index` (invalid when none): on POSIX on the thread of
    // worker `index`, so distinct indices are claimed concurrently; on
    // Windows on the acceptor's.
    [[nodiscard]] native_socket claim_tcp(std::size_t index) noexcept {
        return index < tcp_.size() ? std::exchange(tcp_[index], invalid_native_socket) : invalid_native_socket;
    }
    // Takes every UDP socket at once, for the one opener of the HTTP/3 service.
    [[nodiscard]] std::vector<native_socket> claim_udp() noexcept {
        std::vector<native_socket> claimed(udp_.get_allocator());
        claimed.swap(udp_);
        return claimed;
    }

private:
    std::vector<native_socket> tcp_;
    std::vector<native_socket> udp_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// RFC 9325 §3.4: ticket encryption keys change regularly and are destroyed at
// the end of their validity. A key encrypts tickets for one rotation interval,
// still decrypts them for the next, and is then destroyed. The interval equals
// the server session lifetime (also the TLS 1.3 ticket lifetime and TLS 1.2
// lifetime hint), so every ticket stays decryptable for its whole lifetime and
// no key outlives two intervals.
inline constexpr std::chrono::seconds tls_session_lifetime{std::chrono::hours{2}};

// Product limit: after a failed copy_to, a worker retries this soon instead of
// at the next rotation, so a transient random-generator failure (or a key
// another worker has since generated) costs seconds of ticketless handshakes,
// not an epoch.
inline constexpr std::chrono::seconds tls_ticket_key_retry_delay{5};

// One ticket key: 16-byte key name, HMAC-SHA256 key and AES-256-CBC key, the
// protection OpenSSL applies to its own tickets. Cleansed on destruction.
struct tls_session_ticket_key final {
    tls_session_ticket_key() noexcept = default;
    tls_session_ticket_key(const tls_session_ticket_key&) noexcept = default;
    tls_session_ticket_key& operator=(const tls_session_ticket_key&) noexcept = default;
    ~tls_session_ticket_key();

    std::array<unsigned char, 16> name_{};
    std::array<unsigned char, 32> hmac_key_{};
    std::array<unsigned char, 32> aes_key_{};
};

class tls_session_ticket_key_set;

// One TLS listener's authoritative ticket keys, shared by every worker so a
// ticket issued by one worker resumes on any worker of the same listener.
// Epochs are counted from a steady-clock origin; the first worker to reach a
// new epoch generates its key under the mutex. Handshakes never touch this
// object: each worker copies the keys into its own set once per epoch.
class tls_session_ticket_key_ring final {
public:
    // Generates the key of the epoch starting at `origin`; throws when the
    // random generator fails.
    explicit tls_session_ticket_key_ring(std::chrono::steady_clock::time_point origin);

    tls_session_ticket_key_ring(const tls_session_ticket_key_ring&) = delete;
    tls_session_ticket_key_ring& operator=(const tls_session_ticket_key_ring&) = delete;

    // Any thread. Advances to the epoch containing `now`, destroying keys
    // whose validity ended, and copies the keys still valid into `target`.
    // Returns false when the new epoch's key could not be generated: `target`
    // then issues no tickets and only accepts the previous key, and a later
    // call retries the generation.
    [[nodiscard]] bool copy_to(std::chrono::steady_clock::time_point now, tls_session_ticket_key_set& target);

    // When the epoch containing `now` ends.
    [[nodiscard]] std::chrono::steady_clock::time_point next_rotation(
        std::chrono::steady_clock::time_point now) const noexcept;

private:
    [[nodiscard]] std::uint64_t epoch_at(std::chrono::steady_clock::time_point now) const noexcept;

    const std::chrono::steady_clock::time_point origin_;
    std::mutex mutex_;
    // Guarded by mutex_.
    std::uint64_t epoch_{0};
    std::optional<tls_session_ticket_key> current_;
    std::optional<tls_session_ticket_key> previous_;
};

// One worker's copy of a listener's ticket keys, installed as the ticket key
// callback of every TLS context of that listener. Only the owning worker
// thread reads it (inside handshakes) and refreshes it (between handshakes),
// so the handshake path takes no lock and touches no shared state.
class tls_session_ticket_key_set final {
public:
    tls_session_ticket_key_set();
    ~tls_session_ticket_key_set();

    tls_session_ticket_key_set(const tls_session_ticket_key_set&) = delete;
    tls_session_ticket_key_set& operator=(const tls_session_ticket_key_set&) = delete;

    // Startup only, before `context` serves a connection: issues and accepts
    // tickets with this set's keys and sets the matching session lifetime.
    void install(SSL_CTX& context);

private:
    friend class tls_session_ticket_key_ring;

    static int protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv, EVP_CIPHER_CTX* cipher,
        EVP_MAC_CTX* mac, int encrypt) noexcept;

    EVP_CIPHER* aes_256_cbc_;
    std::optional<tls_session_ticket_key> current_;
    std::optional<tls_session_ticket_key> previous_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Fixed identities are immutable; a live library publishes its own snapshots.
struct http_server_tls_identities final {
    // Certificate and staple form one immutable selection.
    struct identity final {
        identity(tls_certificate certificate, std::span<const std::byte> ocsp_response)
            : certificate_(std::move(certificate)),
              ocsp_response_(ocsp_response.begin(), ocsp_response.end()) {}
        tls_certificate certificate_;
        std::vector<std::byte> ocsp_response_;
    };
    struct named_identity final {
        named_identity(std::string_view name, identity value)
            : name_(name),
              value_(std::move(value)) {}
        std::string name_;
        identity value_;
    };
    struct addressed_identity final {
        asio::ip::address address_;
        identity value_;
    };

    http_server_tls_identities(identity primary)
        : primary_(std::move(primary)) {}
    http_server_tls_identities() = default;

    [[nodiscard]] const identity& select(
        std::string_view server_name, const asio::ip::address* local_address) const noexcept;

    std::optional<identity> primary_;
    std::vector<named_identity> exact_names_;
    std::vector<named_identity> wildcard_names_;
    std::vector<addressed_identity> local_addresses_;
    std::shared_ptr<tls_certificate_store> certificate_store_;
    std::shared_ptr<const tls_certificate_selector> select_certificate_;
    tls_certificate_id default_certificate_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// A server-chosen QUIC connection ID carries its owning worker's index in two
// bytes, so packets can be steered to that worker.
inline constexpr std::size_t http3_max_worker_count = 65536;

struct http_server_listener_definition final {
    // Every TLS member is loaded when the listener is configured: no identity
    // file name, in-memory PEM or private-key password survives in a
    // definition or its clones.
    struct tls_client_certificate_policy final {
        tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
        tls_trust_store client_ca_;
        // The configured bundle's name, only so replace_tls_identities() can
        // recognize the unchanged policy it requires.
        tls_file_name ca_file_;
    };

    struct redirect_http_to_https final {
        std::uint16_t https_port_;
    };

    // A mixed listener still owns exactly one TLS configuration and one TCP
    // socket. The worker chooses the stream type before parsing any HTTP.
    enum class tls_tcp_mode : std::uint8_t {
        tls_only,
        detect_plain_or_tls,
    };

    struct tls final {
        explicit tls(std::shared_ptr<const http_server_tls_identities> identities) noexcept
            : identities_(std::move(identities)) {}

        // Never null. A worker's copy is the set its handshakes select from
        // now; only that worker replaces it, between handshakes.
        std::shared_ptr<const http_server_tls_identities> identities_;
        std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
        std::optional<tls_client_certificate_policy> client_certificates_;
        // Created once by server configuration validation and shared by every
        // worker, which copies its keys into its own TLS contexts' key set.
        std::shared_ptr<tls_session_ticket_key_ring> session_ticket_keys_;
        tls_tcp_mode tcp_mode_{tls_tcp_mode::tls_only};
        // Applies only after the accepted stream was identified as plaintext.
        std::optional<redirect_http_to_https> plaintext_redirect_;
    };

    struct plain_http final {};

    // The HTTP/3 service of a TLS listener: a QUIC endpoint on the same
    // address and UDP port_, serving the listener's TLS identities.
    struct http3_service final {
        std::uint16_t port_;
        std::chrono::seconds alt_svc_max_age_;
        bool early_data_{false};
        bool retry_{false};
        // Created once by server configuration validation and shared by every
        // worker: keys the connection IDs that steer packets between workers
        // and derives stateless reset tokens (RFC 9000 §10.3.2).
        std::shared_ptr<const std::array<std::uint8_t, 32>> secret_;
    };

    // listener_dispatch::connection: application::on_connection serves every
    // connection. A TLS listener offers alpn_ (tls_config::alpn_ or its
    // default) in server preference, encoded as an RFC 7301 §3.1
    // ProtocolNameList without its length prefix; empty for a cleartext one.
    struct connection_dispatch final {
        connection_dispatch(std::string_view alpn)
            : alpn_(alpn) {}

        std::string alpn_;
    };

    using transport_type = std::variant<plain_http, tls, redirect_http_to_https>;

    http_server_listener_definition(asio::ip::tcp::endpoint configured_endpoint,
        transport_type configured_transport = plain_http{},
        std::optional<http3_service> configured_http3 = std::nullopt)
        : endpoint_(std::move(configured_endpoint)),
          transport_(std::move(configured_transport)),
          http3_(std::move(configured_http3)) {}

    [[nodiscard]] http_server_listener_definition clone() const;

    asio::ip::tcp::endpoint endpoint_;
    std::optional<bool> ipv6_only_;
    bool proxy_protocol_{false};
    // listen_config::inherited_sockets_ of this listener, shared by its
    // clones; null when it binds. The run that prepares the listener claims
    // them; the stored definition drops them once a run took them.
    std::shared_ptr<inherited_listener_sockets> inherited_sockets_;
    transport_type transport_;
    // Only a TLS listener carries one.
    std::optional<http3_service> http3_;
    // Absent for listener_dispatch::routes.
    std::optional<connection_dispatch> connection_;
    // listener_dispatch::request_stream: application::on_request_stream serves
    // every request, on this listener and on its HTTP/3 service.
    bool request_stream_{false};
};

// Only validated configurations carry a key ring; an unvalidated definition
// must never reach a serving TLS context.
[[nodiscard]] inline tls_session_ticket_key_ring& tls_session_ticket_keys(
    const http_server_listener_definition::tls& tls) {
    if (tls.session_ticket_keys_ == nullptr) {
        throw std::logic_error("TLS listener session ticket keys were not generated");
    }
    return *tls.session_ticket_keys_;
}

class http_server_session_config final {
public:
    explicit http_server_session_config(const http_server_listener_definition& definition);

    http_server_session_config(const http_server_session_config&) = delete;
    http_server_session_config& operator=(const http_server_session_config&) = delete;
    http_server_session_config(http_server_session_config&&) = delete;
    http_server_session_config& operator=(http_server_session_config&&) = delete;

    [[nodiscard]] const http_server_listener_definition::tls* tls() const& noexcept {
        return std::get_if<http_server_listener_definition::tls>(&transport_);
    }
    const http_server_listener_definition::tls* tls() const&& = delete;

    [[nodiscard]] bool accepts_plaintext() const noexcept {
        const auto* configured = tls();
        return configured != nullptr &&
               configured->tcp_mode_ == http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls;
    }

    [[nodiscard]] const http_server_listener_definition::redirect_http_to_https* redirect()
        const& noexcept {
        if (const auto* configured = tls(); configured != nullptr) {
            return configured->plaintext_redirect_ ? &*configured->plaintext_redirect_ : nullptr;
        }
        return std::get_if<http_server_listener_definition::redirect_http_to_https>(&transport_);
    }
    const http_server_listener_definition::redirect_http_to_https* redirect() const&& = delete;

    // Worker thread, between handshakes: every ClientHello this listener (TLS
    // over TCP and its HTTP/3 service) processes later selects from
    // `identities`. Handshakes keep only the identity they selected, so the
    // previous set may be released here.
    void adopt_tls_identities(std::shared_ptr<const http_server_tls_identities> identities) noexcept;

    http_server_listener_definition::transport_type transport_;
    std::optional<bool> ipv6_only_;
    bool proxy_protocol_{false};
    std::optional<http_server_listener_definition::http3_service> http3_;
    // Stable for the session's lifetime: the TLS contexts' ALPN selection
    // borrows connection_->alpn_.
    std::optional<http_server_listener_definition::connection_dispatch> connection_;
    bool request_stream_{false};
    // The Alt-Svc field value advertising http3_ (RFC 7838 §3), sent on every
    // response of this TLS listener; empty without an HTTP/3 service.
    std::string alternative_service_;
    // This worker's copy of the listener's ticket keys, refreshed on the
    // worker thread once per key epoch. The TLS contexts below point at it, so
    // it is declared before them and outlives them.
    std::optional<tls_session_ticket_key_set> ticket_keys_;
    // One context per family for the worker's whole run, carrying no
    // identity: replacing identities keeps its session cache and ticket keys,
    // and every handshake installs the identity it selects on its own SSL.
    // TLS over TCP:
    std::optional<asio::ssl::context> tls_context_;
    // TLS 1.3 with ALPN h3 for the HTTP/3 service:
    std::optional<asio::ssl::context> quic_tls_context_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The existing listener transaction stays alive across the public prepare/commit
// boundary, retaining its serialization lease and rollback state.
class prepared_listener_state final {
public:
    explicit prepared_listener_state(worker_ref caller)
        : caller_(std::move(caller)),
          ready_(caller_),
          finished_(caller_),
          ready_receipt_(ready_.make_receipt()),
          finished_receipt_(finished_.make_receipt()) {}
    ~prepared_listener_state() {
        if (decision_ == false) {
            (void)finished_.failure();
        }
    }

    void decide(bool activate) noexcept {
        std::coroutine_handle<> continuation;
        {
            std::lock_guard lock(mutex_);
            if (decision_) {
                return;
            }
            decision_ = activate;
            continuation = std::exchange(continuation_, {});
        }
        if (continuation) {
            caller_.defer_resume(continuation);
        }
    }
    struct decision_wait final {
        prepared_listener_state& state_;
        bool await_ready() const noexcept {
            return false;
        }
        bool await_suspend(std::coroutine_handle<> continuation) noexcept {
            std::lock_guard lock(state_.mutex_);
            if (state_.decision_) {
                return false;
            }
            state_.continuation_ = continuation;
            return true;
        }
        void await_resume() const noexcept {}
        void operator&() const = delete;
    };
    task<void> checkpoint(stop_token stopping) {
        auto registration = stopping.register_callback([this] { decide(false); });
        if (stopping.stop_requested()) {
            throw std::logic_error("prepared listener host is stopping");
        }
        ready_receipt_->complete();
        ready_receipt_.reset();
        co_await decision_wait{*this};
        std::lock_guard lock(mutex_);
        if (!*decision_) {
            throw std::logic_error("prepared listener was cancelled or its host stopped");
        }
    }
    void complete(std::exception_ptr failure) noexcept {
        if (ready_receipt_) {
            ready_receipt_->complete(failure);
            ready_receipt_.reset();
        }
        finished_receipt_->complete(std::move(failure));
        finished_receipt_.reset();
    }
    worker_ref caller_;
    runtime_control_completion ready_;
    runtime_control_completion finished_;

private:
    std::optional<runtime_control_completion::receipt> ready_receipt_;
    std::optional<runtime_control_completion::receipt> finished_receipt_;
    std::mutex mutex_;
    std::optional<bool> decision_;
    std::coroutine_handle<> continuation_;
};

struct prepared_listener_access final {
    static task<void> add(task<prepared_listener> preparation) {
        auto prepared = co_await std::move(preparation);
        co_await std::move(prepared).activate();
    }
    static std::shared_ptr<prepared_listener_state> make(worker_ref caller) {
        return std::make_shared<prepared_listener_state>(std::move(caller));
    }
    static task<prepared_listener> launch(std::shared_ptr<prepared_listener_state> state, task<void> operation) {
        if (!state->caller_.is_current() || !state->caller_.accepting()) {
            throw std::logic_error("listener preparation must run on its live caller worker");
        }
        async_start(std::move(operation), asio::bind_executor(state->caller_.executor(), [state](std::exception_ptr failure) { state->complete(std::move(failure)); }));
        co_await state->ready_.wait();
        if (auto failure = state->ready_.failure()) {
            co_await state->finished_.wait();
            (void)state->finished_.failure();
            std::rethrow_exception(failure);
        }
        co_return prepared_listener(std::move(state));
    }
    static task<void> finish(prepared_listener owner, bool activate) {
        const auto state = owner.state_;
        if (!state || !state->caller_.is_current()) {
            throw std::logic_error("prepared listener control must run on its preparation worker");
        }
        state->decide(activate);
        co_await state->finished_.wait();
        const auto failure = state->finished_.failure();
        owner.state_.reset();
        if (activate && failure) {
            std::rethrow_exception(failure);
        }
    }
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// 在构造冷任务前接管整批 socket，并拥有全部借用地址及配置副本。
// load() 将全部 socket 一次性交给唯一 loader；任何失败只关闭每个句柄一次。
class owned_listen_configs final {
public:
    explicit owned_listen_configs(listen_config config);
    explicit owned_listen_configs(std::span<const listen_config> configs);
    owned_listen_configs(owned_listen_configs&& other) noexcept
        : configs_(std::move(other.configs_)),
          addresses_(std::move(other.addresses_)),
          inherited_owned_(std::exchange(other.inherited_owned_, false)) {}
    owned_listen_configs(const owned_listen_configs&) = delete;
    owned_listen_configs& operator=(const owned_listen_configs&) = delete;
    owned_listen_configs& operator=(owned_listen_configs&&) = delete;
    ~owned_listen_configs();

    [[nodiscard]] std::vector<http_server_listener_definition> load();
    [[nodiscard]] bool needs_tls_preparation() const noexcept;
    std::vector<listen_config> configs_{};

private:
    void own_addresses();
    std::vector<std::string> addresses_{};
    bool inherited_owned_{true};
};

}  // namespace ruvia::detail

namespace ruvia {

class static_root;

namespace detail {

// One immutable document-root snapshot. The application's refresher builds it
// once and every worker shares it; reference counting happens only when a
// snapshot is published, adopted, or retired, never per request.
using document_root_snapshot = std::shared_ptr<const static_root>;

// One worker's adopted snapshot plus that worker's request leases on it. Only
// the owning worker touches the counter, so leasing stays a plain increment.
// The worker keeps a superseded generation until its leases drain, which keeps
// the snapshot alive for requests that still borrow it.
class document_root_generation final {
public:
    explicit document_root_generation(document_root_snapshot root) noexcept
        : root_(std::move(root)) {}

    ~document_root_generation() {
        if (leases_ != 0) {
            // A binding still points into this generation. Destroying it would
            // leave the request with a dangling root and counter.
            std::terminate();
        }
    }

    document_root_generation(const document_root_generation&) = delete;
    document_root_generation& operator=(const document_root_generation&) = delete;

    [[nodiscard]] const static_root* root() const noexcept {
        return root_.get();
    }

    [[nodiscard]] bool leased() const noexcept {
        return leases_ != 0;
    }

private:
    friend class document_root_binding;

    document_root_snapshot root_;
    std::size_t leases_{0};
};

// A request-time view of the configured document root. The object is
// deliberately not default-constructible or aggregate-initializable: callers
// choose between no root and a worker's current generation. It is move-only
// because a configured binding is also the request's lease on that generation;
// copying the view would make that ownership boundary ambiguous and could
// reintroduce a dangling static_root after a refresh.
class document_root_binding final {
public:
    [[nodiscard]] static document_root_binding none() noexcept {
        return document_root_binding(nullptr);
    }

    [[nodiscard]] static document_root_binding configured(document_root_generation& generation) noexcept {
        ++generation.leases_;
        return document_root_binding(&generation);
    }

    ~document_root_binding() {
        reset();
    }

    document_root_binding(const document_root_binding&) = delete;
    document_root_binding& operator=(const document_root_binding&) = delete;

    document_root_binding(document_root_binding&& other) noexcept
        : generation_(std::exchange(other.generation_, nullptr)) {}

    document_root_binding& operator=(document_root_binding&& other) noexcept {
        if (this != &other) {
            reset();
            generation_ = std::exchange(other.generation_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] const static_root* root() const noexcept {
        return generation_ == nullptr ? nullptr : generation_->root();
    }

private:
    explicit document_root_binding(document_root_generation* generation) noexcept
        : generation_(generation) {}

    void reset() noexcept {
        auto* const generation = std::exchange(generation_, nullptr);
        if (generation != nullptr) {
            if (generation->leases_ == 0) {
                std::terminate();
            }
            --generation->leases_;
        }
    }

    document_root_generation* generation_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

using stream_encoder_lease = http_stream_encoder_lease;

class worker_codecs final {
public:
    worker_codecs() noexcept = default;
    ~worker_codecs() = default;
    worker_codecs(const worker_codecs&) = delete;
    worker_codecs& operator=(const worker_codecs&) = delete;

    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding) {
        return content_codecs_.encoder(coding);
    }

    [[nodiscard]] stream_encoder_lease acquire_stream_encoder(http_content_coding coding) {
        return content_codecs_.acquire_stream_encoder(coding);
    }

    [[nodiscard]] websocket_deflate_codecs& websocket_deflate() noexcept {
        return websocket_deflate_;
    }

private:
    http_worker_codecs content_codecs_;
    websocket_deflate_codecs websocket_deflate_;
};
}  // namespace ruvia::detail

namespace ruvia::detail {

// Validates every TLS setting of one listener before any credential is read,
// then parses each identity (primary, SNI and local-address identities) from
// its PEM files or in-memory PEM, and the client CA bundle, exactly once.
// Invalid settings throw std::invalid_argument; unreadable, mismatched or
// invalid credentials throw asio::system_error. File names reach OpenSSL as
// UTF-8, which its file BIOs decode (on Windows into _wfopen), so no path is
// narrowed through the ANSI code page. Passwords and PEM text are only
// borrowed while their identity is parsed and are never copied; an empty
// password uses the noninteractive default and terminal prompting is disabled.
[[nodiscard]] http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config);

// The identities of `config` alone, validated then parsed as by
// load_http_server_tls(); for application::replace_tls_identities().
[[nodiscard]] std::shared_ptr<const http_server_tls_identities> load_http_server_tls_identities(
    const tls_config& config);

// Throws std::invalid_argument unless `config` keeps the client certificate
// policy (the same CA path and requirement) and the outgoing record limit
// `current` was loaded with: replacing identities never changes them.
void ensure_same_http_server_tls_policy(const http_server_listener_definition::tls& current, const tls_config& config);

// Installs client-certificate verification into a worker's TLS context.
// Never touches the file system.
void configure_http_server_client_certificates(SSL_CTX& context_value,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>& client_certificates);

}  // namespace ruvia::detail

namespace ruvia {
class env;
}

namespace ruvia::detail {

class document_root_publisher;
class inbound_buffer_budget;
class worker_codecs;

struct access_log_sink final {
    access_log_callback_ref callback_;

    void invoke(const access_log_record& record) const noexcept {
        callback_(record);
    }
};

// The one place access-log records are built: from a live request, or from
// fields a pipelined response kept after its request storage was reused.
struct access_log_record_access final {
    using fields_type = access_log_record::fields_type;

    [[nodiscard]] static constexpr access_log_record make(const fields_type& fields) noexcept {
        return access_log_record(fields);
    }

    // Microseconds from `start` to now, clamped at zero.
    [[nodiscard]] static std::uint64_t duration_since(std::chrono::steady_clock::time_point start) noexcept {
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        return micros < 0 ? 0 : static_cast<std::uint64_t>(micros);
    }
};

struct connection_failure_record_access final {
    [[nodiscard]] static connection_failure_record make(
        ip_address remote_address, std::exception_ptr exception) noexcept {
        return connection_failure_record(remote_address, std::move(exception));
    }
};

// The one place server observation records are built.
struct server_observation_access final {
    using tls_client_hello_fields = tls_client_hello_record::fields_type;

    [[nodiscard]] static tls_client_hello_record tls_client_hello(
        const tls_client_hello_fields& fields) noexcept {
        return tls_client_hello_record(fields);
    }

    [[nodiscard]] static constexpr http2_input_record http2_input(
        std::uint64_t connection_id, std::span<const std::byte> bytes) noexcept {
        return http2_input_record(connection_id, bytes);
    }
#if defined(RUVIA_ENABLE_HTTP2)
    [[nodiscard]] static constexpr http2_frame_record http2_frame(
        std::uint64_t connection_id, const http2_frame_metadata& metadata) noexcept {
        return http2_frame_record(connection_id, metadata);
    }
    [[nodiscard]] static constexpr http2_headers_record http2_headers(
        std::uint64_t connection_id, const http2_header_metadata& metadata) noexcept {
        return http2_headers_record(connection_id, metadata);
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    [[nodiscard]] static constexpr http3_frame_record http3_frame(
        std::uint64_t connection_id, const http3_frame_metadata& metadata) noexcept {
        return http3_frame_record(connection_id, metadata);
    }
    [[nodiscard]] static constexpr http3_field_record http3_field(
        std::uint64_t connection_id, const http3_field_metadata& metadata) noexcept {
        return http3_field_record(connection_id, metadata);
    }
#endif
};

// The connection level's failure outlet. An unset callback still reports: the
// failure goes to the shared last-resort reporter rather than being dropped
// with the connection that produced it.
//
// A connection that ends because the peer reset or closed it, or because this
// server cancelled its I/O (stop, drain, or a timeout that closes the socket),
// is a normal end of connection rather than a failure: browsers close SSE
// tabs with a reset, and drain aborts pending reads. Such endings are neither
// reported nor counted, so the sink stays reserved for real faults. Only an
// inbound_transport_error -- thrown by I/O on the connection's own transport,
// possibly propagated through a handler -- qualifies; any other exception is
// reported whatever error code it carries.
struct connection_failure_sink final {
    connection_failure_callback_ref callback_;
    // Owned by the web_worker_runtime this sink was configured for; null before one
    // claims it. Counting here rather than at each reporting site keeps the
    // count and the callback from drifting apart as new sites are added.
    std::atomic<std::size_t>* counter_{nullptr};

    void invoke(ip_address remote_address, std::exception_ptr exception) const noexcept;
    void invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept;
};

struct worker_failure_sink final {
    using invoke_type = void (*)(void*, const std::exception_ptr&) noexcept;

    void* target_{nullptr};
    invoke_type invoke_{nullptr};

    void notify(const std::exception_ptr& failure) const noexcept {
        if (invoke_ != nullptr) {
            invoke_(target_, failure);
        }
    }
};

// Fully normalized, worker-owned runtime options. Public callers configure application
// with server_config.h values; only the Web runtime constructs this state.
struct http_server_options final {
    // The application's document root: one app-scoped publisher shared by
    // every worker, plus this worker's current generation. Validated
    // configuration carries only the publisher; each worker's
    // static_root_runtime publishes its own generation before serving.
    class document_root_state final {
    public:
        document_root_state() noexcept = default;

        [[nodiscard]] static document_root_state refreshing(document_root_publisher& publisher) noexcept {
            return document_root_state(&publisher);
        }

        [[nodiscard]] document_root_publisher* publisher() const noexcept {
            return publisher_;
        }

        // Points new request bindings at the worker's adopted generation. Only
        // the owning worker calls this; earlier bindings keep their own lease.
        void publish(document_root_generation& generation) noexcept {
            if (publisher_ == nullptr) {
                std::terminate();
            }
            current_ = &generation;
        }

        [[nodiscard]] document_root_binding binding() const noexcept {
            return current_ == nullptr ? document_root_binding::none()
                                       : document_root_binding::configured(*current_);
        }

    private:
        explicit document_root_state(document_root_publisher* publisher) noexcept
            : publisher_(publisher) {}

        document_root_publisher* publisher_{nullptr};
        document_root_generation* current_{nullptr};
    };

    // The validated application server configuration, one copy per worker.
    server_config server_{};
    // Borrowed from the worker runtime, whose shutdown joins every connection;
    // each connection charges its inbound_buffer_account against it.
    inbound_buffer_budget* inbound_budget_{nullptr};
    // Presence enables the policy; absence bypasses it without retaining an
    // inactive configuration state.
    std::optional<compression_config> compression_{};
    // Borrowed from the worker runtime, which always sets it and outlives every
    // request and connection. Inline response compression and server WebSocket
    // connections (shared no-context-takeover deflate) borrow it; worker-thread
    // synchronous use only.
    worker_codecs* codecs_{nullptr};
    std::optional<http_cors_policy> cors_{};
    document_root_state document_root_{};
    // Peers whose forwarding headers may be believed. Empty by default, so an
    // unconfigured server treats every direct peer as the client.
    http_client_ip_extractor trusted_proxies_{};
    access_log_sink access_log_{};
    const env* env_{nullptr};
    worker_failure_sink failure_{};
    connection_failure_sink connection_failure_{};
    // Empty observers keep the connection paths free of observation work.
    tls_client_hello_callback_ref tls_client_hello_{};
    http2_input_callback_ref http2_input_{};
#if defined(RUVIA_ENABLE_HTTP2)
    http2_frame_callback_ref http2_frame_{};
    http2_headers_callback_ref http2_headers_{};
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    http3_frame_callback_ref http3_frame_{};
    http3_field_callback_ref http3_field_{};
#endif
    connection_admission_callback_ref connection_admission_{};
    connection_open_callback_ref connection_open_{};
    connection_close_callback_ref connection_close_{};
    // Serves every listener_dispatch::connection listener; run() requires it
    // when one exists.
    connection_callback_ref connection_{};
    // Serves every listener_dispatch::request_stream listener; run() requires
    // it when one exists.
    request_stream_callback_ref request_stream_{};
    std::optional<rate_limit_rule> default_rate_limit_per_worker_{};
    std::size_t rate_limit_capacity_per_worker_{default_rate_limit_capacity_per_worker};

    // Validates the whole aggregate, including its compression and CORS
    // policies, before publishing any of it.
    void set_server(const server_config& config);
    void set_rate_limit(const rate_limit_config& config);
    void clear_rate_limit() noexcept {
        default_rate_limit_per_worker_.reset();
        rate_limit_capacity_per_worker_ = default_rate_limit_capacity_per_worker;
    }
    void set_trusted_proxies(const trusted_proxy_config& config);
    void clear_trusted_proxies() noexcept {
        trusted_proxies_ = http_client_ip_extractor();
    }
};

}  // namespace ruvia::detail

namespace ruvia {
struct listen_config;
}

namespace ruvia::detail {

// Owns every inherited socket and cleanses every TLS secret, including when
// configuration validation or allocation fails.
[[nodiscard]] std::vector<http_server_listener_definition> load_server_listeners(
    std::span<const listen_config> configs);

// Rejects collisions in their separate TCP and HTTP/3 UDP endpoint namespaces.
void ensure_new_listener_endpoints(std::span<const http_server_listener_definition> existing,
    std::span<const http_server_listener_definition> added);

void validate_server_limits(const server_config& config);

// `listener` prepared for a run with `options`: a clone sharing its loaded
// credentials and inherited sockets, with a ticket key ring whose epochs
// start at `key_origin`, the run's origin for every TLS listener, and with
// HTTP/3 its connection ID secret. Throws std::invalid_argument for a
// listener the run cannot serve, including a connection or request_stream
// listener whose application callback `options` lacks.
[[nodiscard]] http_server_listener_definition validate_run_listener(const http_server_listener_definition& listener,
    const http_server_options& options, std::chrono::steady_clock::time_point key_origin);

class validated_http_server_configuration final {
public:
    validated_http_server_configuration(const validated_http_server_configuration&) = delete;
    validated_http_server_configuration& operator=(const validated_http_server_configuration&) = delete;
    validated_http_server_configuration(validated_http_server_configuration&&) = default;
    validated_http_server_configuration& operator=(validated_http_server_configuration&&) = default;

    [[nodiscard]] std::span<const http_server_listener_definition> listeners() const& noexcept RUVIA_LIFETIMEBOUND {
        return listeners_;
    }
    std::span<const http_server_listener_definition> listeners() const&& = delete;

    [[nodiscard]] const http_server_options& options() const& noexcept RUVIA_LIFETIMEBOUND {
        return options_;
    }
    const http_server_options& options() const&& = delete;

    // Where the ticket key epochs of every TLS listener of the run start,
    // including listeners added while it runs.
    [[nodiscard]] std::chrono::steady_clock::time_point tls_key_origin() const noexcept {
        return tls_key_origin_;
    }

private:
    validated_http_server_configuration(
        std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    friend validated_http_server_configuration validate_http_server_configuration(
        std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    std::vector<http_server_listener_definition> listeners_;
    http_server_options options_;
    std::chrono::steady_clock::time_point tls_key_origin_;
};

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options);

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP3)
namespace ruvia {
class http3_quic_connection;
struct http3_connection_options;
}  // namespace ruvia
#endif

namespace ruvia::detail {

// Builds one worker's TLS contexts of a listener at startup, from its loaded
// definition: the TCP context (TLS 1.1..1.3, h2 only with TLS >= 1.2,
// http/1.1, http/1.0, or connection dispatch's custom ALPN) and, with an
// HTTP/3 service, the QUIC context (TLS 1.3 only, ALPN h3 only, opt-in early
// data). Neither carries an identity: each
// handshake selects one from the identity set the session holds when its
// server_name is known (exact SNI, wildcard SNI, the local address bound by
// bind_tls_local_address(), else the primary) and installs it on its own SSL.
// Both bind resumption to the offered server_name and share the worker's copy
// of the listener's ticket keys. Replacing identities never rebuilds them, so
// their session caches and ticket keys survive it. observe_client_hello
// installs the message callback that delivers each connection's ClientHello
// to its observe_tls_client_hello() observation; without it no observation
// branch is installed. A listener without TLS keeps no context.
void configure_http_server_tls_contexts(http_server_session_config& session, bool observe_client_hello);

using prepared_http_server_session =
    std::unique_ptr<http_server_session_config>;

// Builds a stable-address candidate off the owner loop. Its contexts borrow
// only its own fields; move the owning pointer, never the session itself.
[[nodiscard]] prepared_http_server_session prepare_http_server_session_config(
    const http_server_listener_definition& definition, bool observe_client_hello);

// Attaches the local address a connection arrived at, which identity
// selection matches against local-address identities. The caller keeps it
// alive and unchanged for the SSL's whole lifetime; a connection without one
// never matches them.
void bind_tls_local_address(SSL& tls, const asio::ip::address& local_address);

// A selector needs the same stable endpoint/connection binding as an observer,
// even when no observer is registered. Does not install message observation.
[[nodiscard]] bool tls_certificate_selection_enabled(const http_server_session_config& session) noexcept;

// One connection's on_tls_client_hello delivery: every field of the record
// except the message, which the ClientHello supplies.
struct tls_client_hello_observation final {
    tls_client_hello_callback_ref callback_;
    server_observation_access::tls_client_hello_fields fields_;
};

// Attaches `observation` to a connection's SSL created from a context that
// configure_http_server_tls_contexts() built with observe_client_hello. The
// caller keeps it alive and unchanged for the SSL's whole lifetime.
void observe_tls_client_hello(SSL& tls, const tls_client_hello_observation& observation);

#if defined(RUVIA_ENABLE_HTTP3)
// Called after binding the QUIC connection, before receiving its Initial.
// `version` is the QUIC version of the client's first Initial. The caller
// owns resumption_context for the entire SSL lifetime.
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection,
    std::uint32_t version, const http3_connection_options& options, bool early_data,
    std::array<unsigned char, 32>& resumption_context);
#endif
}  // namespace ruvia::detail

#if defined(_WIN32)

namespace ruvia::detail {

class connection_count;
#if defined(RUVIA_ENABLE_HTTP3)
class http3_worker_directory;
#endif

// An accepted TCP SOCKET that no I/O completion port owns yet: the acceptor
// creates it for AcceptEx and hands it to a business worker, whose io_context
// binds it on adoption. Closes it unless released.
class windows_accepted_socket final {
public:
    explicit windows_accepted_socket(SOCKET handle) noexcept
        : handle_(handle) {}
    windows_accepted_socket(windows_accepted_socket&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_SOCKET)) {}
    windows_accepted_socket(const windows_accepted_socket&) = delete;
    windows_accepted_socket& operator=(const windows_accepted_socket&) = delete;
    windows_accepted_socket& operator=(windows_accepted_socket&&) = delete;
    ~windows_accepted_socket();

    [[nodiscard]] SOCKET get() const noexcept {
        return handle_;
    }
    // The caller now owns the handle.
    SOCKET release() noexcept {
        return std::exchange(handle_, INVALID_SOCKET);
    }

private:
    SOCKET handle_;
};

// The application's dedicated Windows acceptor (AGENTS.md §5): a lightweight
// worker with its own core runtime and thread that owns every TCP listener,
// and every HTTP/3 UDP socket, exclusively (SO_EXCLUSIVEADDRUSE), so the
// business workers stay alike.
//
// TCP: AcceptEx accepts a raw SOCKET that no completion port owns and hands
// it to the business worker holding the fewest connections (ties rotate),
// through that worker's bounded queue: admission is reserved first, and a
// socket the queue refuses is closed and counted as refused. The worker binds
// the socket to its own io_context and serves it like any connection.
//
// HTTP/3: the acceptor receives every datagram and hands it, through the
// HTTP/3 worker directory, to the worker its destination connection ID names,
// or for a long header naming none (a new connection's Initial) to the worker
// http3_initial_worker picks. Workers send through transmit(): the datagram
// is queued, bounded per worker, and sent from the listener's socket.
//
// Lifecycle (driven by the application run):
//   start()          binds (or adopts) every listener; the two-phase start's
//                    first phase
//   serve()          after every business worker serves: accepting begins
//   stop_admission() pending accepts and receives are cancelled; the UDP
//                    sockets stay open for the business workers' last
//                    datagrams (CONNECTION_CLOSE)
//   request_stop()   after the business workers joined: every socket closes
//   join()
// Listener slots match the business workers' and are never reused within a
// run; application::add_listener() and remove_listener() change them on the
// acceptor thread through post_control().
class windows_acceptor final {
public:
    // Business worker thread: adopts one accepted socket.
    using adopt_type = void (*)(void* target, std::size_t listener_index, windows_accepted_socket accepted);

    // One business worker as a hand-over target.
    struct business_worker final {
        worker_ref worker_;
        const connection_count* load_{nullptr};
        void* target_{nullptr};
        adopt_type adopt_{nullptr};
    };

    windows_acceptor(std::span<const http_server_listener_definition> listeners, std::size_t worker_count,
#if defined(RUVIA_ENABLE_HTTP3)
        http3_worker_directory* directory,
#endif
        worker_failure_sink failure);
    ~windows_acceptor();

    windows_acceptor(const windows_acceptor&) = delete;
    windows_acceptor& operator=(const windows_acceptor&) = delete;

    // Construction thread, once per business worker in worker order.
    void bind_worker(std::size_t index, business_worker target);
    // Lifecycle thread. False when a stop came first; rethrows the
    // acceptor's failure (a listener that cannot be bound).
    [[nodiscard]] bool start();
    [[nodiscard]] bool serve();
    [[nodiscard]] task<void> start(worker_ref caller);
    [[nodiscard]] task<void> serve(worker_ref caller);
    // Any thread.
    void stop_admission() noexcept;
    void request_stop() noexcept;
    // Lifecycle thread, after request_stop().
    void join();
    // Waits for the native thread HANDLE asynchronously; join() afterwards only
    // reclaims a thread whose exit has already been observed.
    [[nodiscard]] task<void> join(worker_ref caller, asio::io_context& caller_io);
    [[nodiscard]] std::exception_ptr failure() noexcept;
    // Any thread: sockets the chosen worker's queue refused, and accept
    // failures other than peer disconnects.
    [[nodiscard]] std::size_t refused() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t accept_failures() const noexcept {
        return accept_failures_.load(std::memory_order_relaxed);
    }
    // Any thread: runs `control` on the acceptor thread; false once stopping.
    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept {
        return runtime_.post_control(std::move(control));
    }
    // Acceptor thread, the controls of application::add_listener(): binds or
    // adopts `definition`'s TCP socket in slot `index` and its HTTP/3 UDP
    // socket in slot `http3_index`, accepting nothing yet, or throws with
    // both slots left empty.
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory
#endif
    );
    // Starts accepting for (and receiving datagrams of) a prepared listener.
    void open_listener(std::size_t index) noexcept;
    // Closes the sockets of a listener prepared but never opened.
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept;
    // application::remove_listener(): stops accepting for listener `index`
    // and closes its TCP socket, holding `receipt` until its accepts ended.
    // Its UDP socket keeps receiving for the established QUIC connections
    // until every business worker released it.
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept;
#if defined(RUVIA_ENABLE_HTTP3)
    // Business worker `worker_index`'s thread: queues one datagram for the
    // socket of HTTP/3 listener `http3_listener_index`. False when it was
    // dropped (the worker's bound is full or the acceptor stopped).
    [[nodiscard]] bool transmit(std::size_t worker_index, std::size_t http3_listener_index,
        const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    // Any thread: one business worker's endpoint of a removed HTTP/3
    // listener served its last connection. The last worker releases this
    // listener's claim; the socket closes once no listener still needs it.
    void release_http3_listener(std::size_t http3_listener_index) noexcept;
#endif

private:
    struct tcp_listener;
    struct worker_slot;
#if defined(RUVIA_ENABLE_HTTP3)
    struct udp_transport;
    struct udp_listener;
#endif

    void complete_phase(bool startup, std::exception_ptr failure = {}) noexcept;
    task<void> run();
    void open_listeners();
    void open_tcp_listener(tcp_listener& listener);
#if defined(RUVIA_ENABLE_HTTP3)
    void open_udp_listener(udp_listener& listener);
#endif
    void prepare_listener_tasks(tcp_listener& listener);
    void start_listener(tcp_listener& listener) noexcept;
    void serve_on_context();
    void close_admission() noexcept;
    task<void> run_accepts(tcp_listener& listener);
    void report_accept_error(const std::error_code& error);
    void dispatch(std::size_t listener_index, windows_accepted_socket accepted);
    [[nodiscard]] std::size_t select_worker() noexcept;
#if defined(RUVIA_ENABLE_HTTP3)
    task<void> run_receives(udp_listener& listener);
    void steer(const udp_listener& listener, std::span<const std::byte> bytes,
        const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local,
        std::uint32_t interface_index) noexcept;
    void send_datagram(std::size_t http3_listener_index, const asio::ip::udp::endpoint& remote,
        std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local,
        std::uint32_t interface_index) noexcept;
#endif

    // Declared first: every socket below belongs to its io_context.
    worker_runtime runtime_;
    asio::io_context& io_;
    worker_failure_sink failure_sink_;
    worker_signal finalize_signal_;
    task_scope tasks_;
    std::vector<std::unique_ptr<worker_slot>> workers_;
    // Indexed by listener slot, the HTTP/3 ones by HTTP/3 slot; empty for a
    // listener whose addition was rolled back. A removed listener keeps its
    // closed socket object until the acceptor ends: its accepts reference it.
    std::vector<std::unique_ptr<tcp_listener>> tcp_listeners_;
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_directory* directory_;
    std::vector<std::unique_ptr<udp_listener>> udp_listeners_;
#endif
    // Owner thread.
    std::size_t next_worker_{0};
    bool admitting_{false};
    bool stopping_{false};
    std::atomic<std::size_t> refused_{0};
    std::atomic<std::size_t> accept_failures_{0};
    std::mutex phase_mutex_;
    std::optional<runtime_control_completion::receipt> startup_receipt_;
    std::optional<runtime_control_completion::receipt> serve_receipt_;
};

}  // namespace ruvia::detail
#endif

namespace ruvia::detail {

// Runtime capability is deliberately separate from Accept-Encoding
// negotiation. A precompressed file sidecar can satisfy a selected coding
// without this process owning an incremental encoder.
enum class http_response_coding_availability : std::uint8_t {
    identity_only,
    identity_and_compression,
};

// Static-file representations remain owned by the static response path.
enum class http_response_compression_source : std::uint8_t {
    buffered,
    stream,
};

enum class http_response_compression_decision : std::uint8_t {
    fixed_representation,
    negotiated_identity,
    encode,
};

// Compression policy and encoder execution are separate outcomes. A response
// may intentionally remain identity (body too small while identity is
// acceptable, no-transform, integrity digest fields, an incompressible media
// type, or an unsupported runtime capability), or the encoder may actually
// fail. Those cases have different meanings when the client has forbidden
// identity and must not collapse back to a bool.
enum class http_response_compression_status : std::uint8_t {
    compressed,
    not_applicable,
    unavailable,
    failed,
};

// The 406 every coding-policy path answers when the client accepts none of the
// codings a response can be sent with. Options rather than http_error_info so
// a throw site can build an http_error from the same value.
[[nodiscard]] constexpr http_error_info_options http_not_acceptable_coding_error() noexcept {
    return {.status_ = http_status::not_acceptable,
        .code_ = "not_acceptable",
        .message_ = "no acceptable response content coding"};
}

// Return true when the response cannot satisfy the client's coding policy:
// the negotiation failed, a framework-selected coding fell back to forbidden
// identity, or a handler supplied a pre-encoded representation the client
// excluded. Bodyless statuses are representation-free and therefore never
// need a content-coding fallback.
[[nodiscard]] inline bool http_response_coding_fallback_forbidden(const http_response_coding_selection& selection,
    http_known_method request_method, const http_response& response, const http_request* request = nullptr) {
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return false;
    }
    // An empty 416 carries no content and so no coding to negotiate; its
    // Content-Range describes the selected representation (RFC 9110 15.5.17).
    if (response.status() == http_status::range_not_satisfiable && response.body_bytes().empty()) {
        return false;
    }
    switch (evaluate_http_response_content_coding(selection, response, request)) {
        case http_response_content_coding_status::accepted:
            return false;
        case http_response_content_coding_status::not_acceptable:
        case http_response_content_coding_status::invalid:
            return true;
        case http_response_content_coding_status::absent:
            break;
    }
    if (!selection.acceptable()) {
        return true;
    }
    if (selection.coding() == http_content_coding::identity || selection.identity_accepted()) {
        return false;
    }
    // This guard targets only the framework's silent identity fallback.
    return true;
}

// Outcome of the synchronous compression phase for an in-memory body. An
// eligible body above sync_bytes_ on a worker with a blocking pool is left as
// identity and flagged for offload; every other outcome is final here.
struct http_buffered_compression_step final {
    http_response_compression_status status_;
    bool offload_;
};

// Applies the buffered compression policy without suspending: eligibility,
// size bounds, and encoding up to sync_bytes_. sync_bytes_ is the inline
// ceiling: a larger body is flagged for offload when `offload_available`, and
// otherwise stays identity (not_applicable). Only an offload result requires
// compress_response_offloaded(), so identity and ineligible responses never
// create a coroutine frame. The inline encoder is the worker's reusable one:
// its state stays in worker and only the encoded body comes from the
// response's resource.
[[nodiscard]] http_buffered_compression_step apply_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, const compression_config& options, worker_codecs& codecs, bool offload_available);

// Encodes the body flagged by apply_response_compression() on the worker's
// bounded blocking pool with a codec of its own, since worker codecs never leave the
// worker. Rejection by the pool is an intentional identity fallback;
// encoder/commit failures remain typed failures so a client that forbids
// identity receives a terminal error.
[[nodiscard]] task<http_response_compression_status> compress_response_offloaded(
    http_response_coding_selection selection, http_response& response, const worker_ref& worker_value,
    const stop_token& stop_token_value = {});

// The single decimal Content-Length a handler declared, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> declared_content_length(const http_response& response) noexcept;

// The size rule for a body of `declared` bytes, shared by buffered bodies,
// HEAD metadata and streams whose head declares a length: below min_bytes_
// (or empty) stays identity unless identity is refused, and above max_bytes_
// always stays identity.
[[nodiscard]] bool declared_length_permits_encoding(const http_response_coding_selection& selection,
    const compression_config& options, std::uint64_t declared) noexcept;

// Response compression policy. Fixed representations do not vary by coding,
// except an application-encoded body, whose 200-or-406 outcome does; eligible
// representations are annotated before deciding between identity and an
// encoder. Encoder availability never changes Accept-Encoding parsing.
[[nodiscard]] http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability);

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_response_coding_policy_disabled final {};

// The response lifecycle has a state that is different from HTTP negotiation
// failure: after a generated terminal error, coding application is deliberately
// disabled. That state stays distinct from the negotiation result, so "406
// negotiation" and "do not transform this error again" are never conflated.
class http_response_coding_policy final {
public:
    [[nodiscard]] static http_response_coding_policy disabled() noexcept {
        return http_response_coding_policy(http_response_coding_policy_disabled{});
    }

    // A failed negotiation stays alive in the selection until a buffered
    // handler has produced its response status: a 204/304 has no response
    // content, so rejecting it before dispatch would turn a representation-free
    // response into a false 406. The selection's identity coding is then only
    // a placeholder that never becomes a silent identity fallback for a
    // response that actually has a body.
    [[nodiscard]] static http_response_coding_policy selected(
        http_response_coding_selection selection) noexcept {
        return http_response_coding_policy(selection);
    }

    [[nodiscard]] const http_response_coding_selection* selection() const& noexcept {
        return std::get_if<http_response_coding_selection>(&value_);
    }
    const http_response_coding_selection* selection() const&& = delete;

private:
    explicit http_response_coding_policy(http_response_coding_policy_disabled disabled) noexcept
        : value_(disabled) {}

    explicit http_response_coding_policy(http_response_coding_selection selection) noexcept
        : value_(selection) {}

    using value_type = std::variant<http_response_coding_selection, http_response_coding_policy_disabled>;
    value_type value_;
};

// A selected non-identity coding is only a promise until the response policy
// actually installs Content-Encoding. In particular, no-transform,
// incompressible media, or an existing file body can leave the response as
// identity. If the client explicitly excluded identity, that policy fallback
// is a 406 outcome; an encoder failure is surfaced separately as a 500.
[[nodiscard]] inline bool http_response_needs_not_acceptable(const http_response_coding_policy& policy,
    const http_request& request, const http_response& response) noexcept {
    const auto* selection = policy.selection();
    if (selection == nullptr) {
        return false;
    }
    return http_response_coding_fallback_forbidden(*selection, request.known_method(), response, &request);
}

[[nodiscard]] inline std::optional<http_error_info> http_buffered_response_preparation_error(
    const http_response_coding_policy& policy, const http_request& request,
    const http_response& response, http_response_compression_status compression) noexcept {
    const auto* selection = policy.selection();
    if (selection != nullptr && selection->coding() != http_content_coding::identity &&
        !selection->identity_accepted() &&
        plan_http_response_body(request.known_method(), response.status()).status_allows_body()) {
        if (compression == http_response_compression_status::failed) {
            return http_error_info({.status_ = http_status::internal_server_error,
                .code_ = "response_compression_failed",
                .message_ = "response compression failed"});
        }
        if (compression == http_response_compression_status::unavailable) {
            return http_error_info({.status_ = http_status::service_unavailable,
                .code_ = "blocking_pool_unavailable",
                .message_ = "blocking pool cannot accept the operation"});
        }
    }
    if (http_response_needs_not_acceptable(policy, request, response)) {
        return http_error_info(http_not_acceptable_coding_error());
    }
    return std::nullopt;
}

enum class buffered_response_recovery_action { ready,
    handle_error,
    prepare_terminal };

struct buffered_response_recovery_step final {
    buffered_response_recovery_action action_{buffered_response_recovery_action::ready};
    std::optional<http_error_info> error_;
};

// A secondary failure preserves the error handler's representation and
// disables coding instead of invoking it again. An error response (4xx/5xx)
// goes straight to that terminal path: it is not a representation of the
// target resource (RFC 9110 15.5.7), so the server may disregard
// Accept-Encoding for it (RFC 9110 12.4.1). Replacing it with a 406 would
// disguise the original failure and run the error handler a second time.
class buffered_response_recovery final {
public:
    [[nodiscard]] bool recovered() const noexcept {
        return stage_ != stage::application;
    }

    [[nodiscard]] buffered_response_recovery_step advance(http_response_coding_policy& policy,
        const http_request& request, const http_response& response,
        http_response_compression_status compression) noexcept {
        if (stage_ == stage::terminal) {
            return {};
        }
        auto error = http_buffered_response_preparation_error(policy, request, response, compression);
        if (!error.has_value()) {
            return {};
        }
        if (stage_ == stage::application && !response.status().is_error()) {
            stage_ = stage::negotiated_error;
            return {buffered_response_recovery_action::handle_error, std::move(error)};
        }
        policy = http_response_coding_policy::disabled();
        stage_ = stage::terminal;
        return {buffered_response_recovery_action::prepare_terminal, std::nullopt};
    }

private:
    enum class stage { application,
        negotiated_error,
        terminal };
    stage stage_{stage::application};
};

struct prepared_application_response final {
    http_buffered_response_write_plan write_plan_;
    bool recovered_;
};

}  // namespace ruvia::detail
