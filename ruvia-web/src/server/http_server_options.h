#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>
#include <openssl/types.h>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_content_encoder.h"
#include "ruvia/http/websocket_deflate_codecs.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/server_connection.h"
#include "ruvia/web/server_observation.h"

#include "server/http_server_listener.h"

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
