#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "app.h"
#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "crypto.h"
#include "event_loop.h"
#include "http_server.h"
#include "runtime_control.h"
#include "stop_token.h"
#include "task.h"
#include "tls.h"
#include "worker_ref.h"

// Listener configuration validated once at startup: inherited sockets, TLS
// session ticket keys, listener definitions and their prepared per-worker
// sessions and TLS contexts, plus the Windows acceptor worker that hands
// accepted sockets to business workers.
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
    inherited_listener_sockets(std::vector<native_socket> tcp, std::vector<native_socket> udp) noexcept :
        tcp_(std::move(tcp)),
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
    [[nodiscard]] std::chrono::steady_clock::time_point next_rotation(std::chrono::steady_clock::time_point now) const noexcept;

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

    static int protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv, EVP_CIPHER_CTX* cipher, EVP_MAC_CTX* mac, int encrypt) noexcept;

    EVP_CIPHER* aes_256_cbc_;
    std::optional<tls_session_ticket_key> current_;
    std::optional<tls_session_ticket_key> previous_;
};

// Fixed identities are immutable; a live library publishes its own snapshots.
struct http_server_tls_identities final {
    // Certificate and staple form one immutable selection.
    struct identity final {
        identity(tls_certificate certificate, std::span<const std::byte> ocsp_response) :
            certificate_(std::move(certificate)),
            ocsp_response_(ocsp_response.begin(), ocsp_response.end()) {}
        tls_certificate certificate_;
        std::vector<std::byte> ocsp_response_;
    };
    struct named_identity final {
        named_identity(std::string_view name, identity value) :
            name_(name),
            value_(std::move(value)) {}
        std::string name_;
        identity value_;
    };
    struct addressed_identity final {
        asio::ip::address address_;
        identity value_;
    };

    http_server_tls_identities(identity primary) :
        primary_(std::move(primary)) {}
    http_server_tls_identities() = default;

    [[nodiscard]] const identity& select(std::string_view server_name, const asio::ip::address* local_address) const noexcept;

    std::optional<identity> primary_;
    std::vector<named_identity> exact_names_;
    std::vector<named_identity> wildcard_names_;
    std::vector<addressed_identity> local_addresses_;
    std::shared_ptr<tls_certificate_store> certificate_store_;
    std::shared_ptr<const tls_certificate_selector> select_certificate_;
    tls_certificate_id default_certificate_{};
};

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
        explicit tls(std::shared_ptr<const http_server_tls_identities> identities) noexcept :
            identities_(std::move(identities)) {}

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
        connection_dispatch(std::string_view alpn) :
            alpn_(alpn) {}

        std::string alpn_;
    };

    using transport_type = std::variant<plain_http, tls, redirect_http_to_https>;

    http_server_listener_definition(asio::ip::tcp::endpoint configured_endpoint, transport_type configured_transport = plain_http{}, std::optional<http3_service> configured_http3 = std::nullopt) :
        endpoint_(std::move(configured_endpoint)),
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
[[nodiscard]] inline tls_session_ticket_key_ring& tls_session_ticket_keys(const http_server_listener_definition::tls& tls) {
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
        return configured != nullptr && configured->tcp_mode_ == http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls;
    }

    [[nodiscard]] const http_server_listener_definition::redirect_http_to_https* redirect() const& noexcept {
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

// The existing listener transaction stays alive across the public prepare/commit
// boundary, retaining its serialization lease and rollback state.
class prepared_listener_state final {
public:
    explicit prepared_listener_state(worker_ref caller) :
        caller_(std::move(caller)),
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
        auto registration = stopping.register_callback([this] {
            decide(false);
        });
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
        async_start(std::move(operation), asio::bind_executor(state->caller_.executor(), [state](std::exception_ptr failure) {
            state->complete(std::move(failure));
        }));
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

// 在构造冷任务前接管整批 socket，并拥有全部借用地址及配置副本。
// load() 将全部 socket 一次性交给唯一 loader；任何失败只关闭每个句柄一次。
class owned_listen_configs final {
public:
    explicit owned_listen_configs(listen_config config);
    explicit owned_listen_configs(std::span<const listen_config> configs);
    owned_listen_configs(owned_listen_configs&& other) noexcept :
        configs_(std::move(other.configs_)),
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

// Validates every TLS setting of one listener before any credential is read,
// then parses each identity (primary, SNI and local-address identities) from
// its PEM files or in-memory PEM, and the client CA bundle, exactly once.
// Invalid settings throw std::invalid_argument; unreadable, mismatched or
// invalid credentials throw asio::system_error. File names reach OpenSSL as
// UTF-8, which its file BIOs decode (on Windows into _wfopen), so no path is
// narrowed through the ANSI code page. Passwords and PEM text are only
// borrowed while their identity is parsed and are never copied; an empty
// password uses the noninteractive default and terminal prompting is disabled.
[[nodiscard]] http_server_listener_definition::tls load_http_server_tls(const tls_config& config);

// The identities of `config` alone, validated then parsed as by
// load_http_server_tls(); for application::replace_tls_identities().
[[nodiscard]] std::shared_ptr<const http_server_tls_identities> load_http_server_tls_identities(const tls_config& config);

// Throws std::invalid_argument unless `config` keeps the client certificate
// policy (the same CA path and requirement) and the outgoing record limit
// `current` was loaded with: replacing identities never changes them.
void ensure_same_http_server_tls_policy(const http_server_listener_definition::tls& current, const tls_config& config);

// Installs client-certificate verification into a worker's TLS context.
// Never touches the file system.
void configure_http_server_client_certificates(SSL_CTX& context_value, const std::optional<http_server_listener_definition::tls_client_certificate_policy>& client_certificates);

// Owns every inherited socket and cleanses every TLS secret, including when
// configuration validation or allocation fails.
[[nodiscard]] std::vector<http_server_listener_definition> load_server_listeners(std::span<const listen_config> configs);

// Rejects collisions in their separate TCP and HTTP/3 UDP endpoint namespaces.
void ensure_new_listener_endpoints(std::span<const http_server_listener_definition> existing, std::span<const http_server_listener_definition> added);

void validate_server_limits(const server_config& config);

// `listener` prepared for a run with `options`: a clone sharing its loaded
// credentials and inherited sockets, with a ticket key ring whose epochs
// start at `key_origin`, the run's origin for every TLS listener, and with
// HTTP/3 its connection ID secret. Throws std::invalid_argument for a
// listener the run cannot serve, including a connection or request_stream
// listener whose application callback `options` lacks.
[[nodiscard]] http_server_listener_definition validate_run_listener(const http_server_listener_definition& listener, const http_server_options& options, std::chrono::steady_clock::time_point key_origin);

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
    validated_http_server_configuration(std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    friend validated_http_server_configuration validate_http_server_configuration(std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    std::vector<http_server_listener_definition> listeners_;
    http_server_options options_;
    std::chrono::steady_clock::time_point tls_key_origin_;
};

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(std::span<const http_server_listener_definition> listeners, http_server_options&& options);

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP3)
namespace ruvia {
class http3_quic_connection;
struct http3_connection_options;
}  // namespace ruvia
#endif

namespace ruvia::detail {

// Builds one worker's TLS contexts of a listener at startup, from its loaded
// definition: the TCP context (TLS 1.2..1.3, ALPN h2,
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

using prepared_http_server_session = std::unique_ptr<http_server_session_config>;

// Builds a stable-address candidate off the owner loop. Its contexts borrow
// only its own fields; move the owning pointer, never the session itself.
[[nodiscard]] prepared_http_server_session prepare_http_server_session_config(const http_server_listener_definition& definition, bool observe_client_hello);

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
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection, std::uint32_t version, const http3_connection_options& options, bool early_data, std::array<unsigned char, 32>& resumption_context);
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
    explicit windows_accepted_socket(SOCKET handle) noexcept :
        handle_(handle) {}
    windows_accepted_socket(windows_accepted_socket&& other) noexcept :
        handle_(std::exchange(other.handle_, INVALID_SOCKET)) {}
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
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index, const http_server_listener_definition& definition
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
    [[nodiscard]] bool transmit(std::size_t worker_index, std::size_t http3_listener_index, const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
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
    void steer(const udp_listener& listener, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    void send_datagram(std::size_t http3_listener_index, const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
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
