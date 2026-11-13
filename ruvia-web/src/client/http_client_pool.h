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
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tcp_transport.h"
#include "http/operation_interrupt.h"
#include "server/connection_lifecycle.h"

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
