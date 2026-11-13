#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_signal.h"
#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/http2_connection.h"
#endif
#include <array>
#include <exception>
#include <span>
#include <string_view>

#include <asio/buffer.hpp>
#include <asio/ip/address.hpp>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/web/http_client.h"

#include "client/client_tcp_transport.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_memory.h"

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
