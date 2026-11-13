#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_client_ip.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"
#include "ruvia/web/error.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/web_worker.h"

#include "http/static_root_index.h"
#include "http/tls_protocol_policy.h"
#include "integration/operation_interrupt.h"
#include "server/connection_lifecycle.h"

namespace ruvia {

class http_client;
class http_client_ref;
class stop_token;
class operation_scope;

#ifdef RUVIA_ENABLE_DATABASE
class db_session;
#endif

#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif
namespace detail {

class borrowed_clients;
class db_registry;
struct request_io_metrics;
class http_client_registry;
class redis_registry;

// A copyable view of the complete client registry set owned by one worker, or
// of the clients a router borrows for its dispatches. The only other state is
// detached: partial registry graphs are not representable. Detached views are
// only placeholders for contexts built without worker capabilities; an
// attached view never outlives its worker, which joins all posted work before
// destroying its registries, nor a router's dispatch.
class worker_client_registry_view final {
public:
    [[nodiscard]] static constexpr worker_client_registry_view detached() noexcept {
        return worker_client_registry_view(nullptr);
    }

    constexpr worker_client_registry_view(http_client_registry& http_clients
#ifdef RUVIA_ENABLE_DATABASE
        ,
        db_registry& databases
#endif
#ifdef RUVIA_ENABLE_REDIS
        ,
        redis_registry& redis
#endif
        ) noexcept
        : http_clients_(&http_clients)
#ifdef RUVIA_ENABLE_DATABASE
          ,
          databases_(&databases)
#endif
#ifdef RUVIA_ENABLE_REDIS
          ,
          redis_(&redis)
#endif
    {
    }

    explicit constexpr worker_client_registry_view(const borrowed_clients& borrowed) noexcept
        : borrowed_(&borrowed) {}

    [[nodiscard]] constexpr bool attached() const noexcept {
        return http_clients_ != nullptr || borrowed_ != nullptr;
    }

    // `io_metrics` is the request's counter block, or null outside a request.
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_session db(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics,
        const stop_token& stop_token) const;
    [[nodiscard]] db_session db(std::string_view alias, ::ruvia::operation_scope& operation_scope,
        request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_session redis(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics,
        const stop_token& stop_token) const;
    [[nodiscard]] redis_session redis(std::string_view alias, ::ruvia::operation_scope& operation_scope,
        request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

    [[nodiscard]] http_client_ref client(
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;
    [[nodiscard]] http_client_ref client(std::string_view alias,
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;

private:
    explicit constexpr worker_client_registry_view(std::nullptr_t) noexcept {}

    [[nodiscard]] const http_client& borrowed_http_clients(std::string_view alias) const;

    const borrowed_clients* borrowed_{nullptr};
    http_client_registry* http_clients_{nullptr};
#ifdef RUVIA_ENABLE_DATABASE
    db_registry* databases_{nullptr};
#endif
#ifdef RUVIA_ENABLE_REDIS
    redis_registry* redis_{nullptr};
#endif
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia {

class body_reader;
class response_stream_writer;
class websocket;
class http_tunnel;

namespace detail {

class request_body_loader;
class context_request_body_source;
class context_response_output;

// A context always has exactly one request-body source. The buffered alternative
// means the complete bytes are already exposed by http_request; the other two
// borrow non-null runtime facades for the duration of dispatch.
class context_buffered_request_body_source final {
private:
    friend class context_request_body_source;
    constexpr context_buffered_request_body_source() noexcept = default;
};

class context_lazy_request_body_source final {
public:
    [[nodiscard]] constexpr request_body_loader& loader() const noexcept {
        return *loader_;
    }

private:
    friend class context_request_body_source;

    explicit constexpr context_lazy_request_body_source(request_body_loader& loader) noexcept
        : loader_(&loader) {}

    request_body_loader* loader_;
};

class context_streaming_request_body_source final {
public:
    [[nodiscard]] constexpr body_reader& reader() const noexcept {
        return *reader_;
    }

private:
    friend class context_request_body_source;

    explicit constexpr context_streaming_request_body_source(body_reader& reader_value) noexcept
        : reader_(&reader_value) {}

    body_reader* reader_;
};

class context_request_body_source final {
public:
    constexpr context_request_body_source() noexcept
        : value_(context_buffered_request_body_source{}) {}

    [[nodiscard]] static constexpr context_request_body_source lazy(
        request_body_loader& loader) noexcept {
        return context_request_body_source(context_lazy_request_body_source(loader));
    }

    [[nodiscard]] static constexpr context_request_body_source streaming(body_reader& reader_value) noexcept {
        return context_request_body_source(context_streaming_request_body_source(reader_value));
    }

    [[nodiscard]] constexpr const context_lazy_request_body_source* lazy() const& noexcept {
        return std::get_if<context_lazy_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_lazy_request_body_source* lazy() const&& = delete;

    [[nodiscard]] constexpr const context_streaming_request_body_source* streaming() const& noexcept {
        return std::get_if<context_streaming_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_streaming_request_body_source* streaming() const&& = delete;

private:
    using value_type = std::variant<context_buffered_request_body_source, context_lazy_request_body_source,
        context_streaming_request_body_source>;

    template <typename source>
    explicit constexpr context_request_body_source(source source_value) noexcept
        : value_(source_value) {}

    value_type value_;
};

// A context likewise has exactly one response output. The buffered alternative
// uses the ordinary http_response return path; long-lived outputs borrow one
// non-null runtime facade and cannot coexist in the same context. Every
// alternative but buffered and an uncommitted response stream means the
// response head is on the wire and no HTTP response can follow.
class context_buffered_response_output final {
private:
    friend class context_response_output;
    constexpr context_buffered_response_output() noexcept = default;
};

class context_response_stream_output final {
public:
    [[nodiscard]] constexpr response_stream_writer& writer() const noexcept {
        return *writer_;
    }

private:
    friend class context_response_output;

    explicit constexpr context_response_stream_output(response_stream_writer& writer) noexcept
        : writer_(&writer) {}

    response_stream_writer* writer_;
};

class context_websocket_output final {
public:
    [[nodiscard]] constexpr websocket& get_websocket() const noexcept {
        return *websocket_;
    }

private:
    friend class context_response_output;

    explicit constexpr context_websocket_output(websocket& websocket_value) noexcept
        : websocket_(&websocket_value) {}

    websocket* websocket_;
};

class context_tunnel_output final {
public:
    [[nodiscard]] constexpr http_tunnel& tunnel() const noexcept {
        return *tunnel_;
    }

private:
    friend class context_response_output;
    explicit constexpr context_tunnel_output(http_tunnel& tunnel) noexcept
        : tunnel_(&tunnel) {}
    http_tunnel* tunnel_;
};

// The upgrade or CONNECT handshake head was handed to the transport. Holds
// while no websocket/tunnel facade is bound -- before the session binds one and
// after it restores this output -- so a buffered error built by middleware can
// never be mistaken for a sendable response.
class context_upgraded_response_output final {
private:
    friend class context_response_output;
    constexpr context_upgraded_response_output() noexcept = default;
};

class context_response_output final {
public:
    constexpr context_response_output() noexcept
        : value_(context_buffered_response_output{}) {}

    [[nodiscard]] static constexpr context_response_output response_stream(
        response_stream_writer& writer) noexcept {
        return context_response_output(context_response_stream_output(writer));
    }

    [[nodiscard]] static constexpr context_response_output websocket_value(websocket& websocket_value) noexcept {
        return context_response_output(context_websocket_output(websocket_value));
    }

    [[nodiscard]] static constexpr context_response_output tunnel(http_tunnel& tunnel) noexcept {
        return context_response_output(context_tunnel_output(tunnel));
    }

    [[nodiscard]] static constexpr context_response_output upgraded() noexcept {
        return context_response_output(context_upgraded_response_output{});
    }

    // Whether the response head already reached the transport.
    [[nodiscard]] bool committed() const noexcept;

    [[nodiscard]] constexpr const context_tunnel_output* tunnel() const& noexcept {
        return std::get_if<context_tunnel_output>(&value_);
    }
    const context_tunnel_output* tunnel() const&& = delete;
    [[nodiscard]] constexpr const context_buffered_response_output* buffered() const& noexcept {
        return std::get_if<context_buffered_response_output>(&value_);
    }
    [[nodiscard]] constexpr const context_buffered_response_output* buffered() const&& = delete;

    [[nodiscard]] constexpr const context_response_stream_output* response_stream() const& noexcept {
        return std::get_if<context_response_stream_output>(&value_);
    }
    [[nodiscard]] constexpr const context_response_stream_output* response_stream() const&& = delete;

    [[nodiscard]] constexpr const context_websocket_output* get_websocket() const& noexcept {
        return std::get_if<context_websocket_output>(&value_);
    }
    [[nodiscard]] constexpr const context_websocket_output* get_websocket() const&& = delete;

private:
    using value_type = std::variant<context_buffered_response_output, context_response_stream_output,
        context_websocket_output, context_tunnel_output, context_upgraded_response_output>;

    template <typename output_type>
    explicit constexpr context_response_output(output_type output) noexcept
        : value_(output) {}

    value_type value_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia {

class http_request;
class http_request_trailers;
class env;
}  // namespace ruvia

namespace ruvia::detail {

class rate_limiter;
class http_interim_response_output;
class inbound_buffer_account;
class route_table;
struct connection_failure_sink;
class tls_served_certificate;
class worker_state_registry;
class borrowed_clients;

// Borrows the request's connection.
class http_push_output final {
public:
    using submit_type = bool (*)(void*, http_push_request_view);
    http_push_output(void* target, submit_type submit) noexcept
        : target_(target),
          submit_(submit) {}
    // Header-list bounds are enforced by the HTTP/2 submit path.
    [[nodiscard]] bool push(http_push_request_view request) {
        return submit_(target_, request);
    }

private:
    void* target_;
    submit_type submit_;
};

// Request-scoped typed access to the owning connection's advertisement lane.
class http_connection_advertisement_output final {
public:
    using origins_type = void (*)(void*, std::span<const std::string_view>);
    using alternative_service_type = void (*)(void*, std::string_view);
    http_connection_advertisement_output(
        void* target, origins_type origins, alternative_service_type service) noexcept
        : target_(target),
          origins_(origins),
          service_(service) {}
    void advertise_origins(std::span<const std::string_view> origins);
    void advertise_alternative_service(std::string_view value);

private:
    void* target_;
    origins_type origins_;
    alternative_service_type service_;
};

struct context_worker_services final {
    std::reference_wrapper<const worker_ref> worker_;
    worker_client_registry_view clients_{worker_client_registry_view::detached()};
    rate_limiter* rate_limiter_{};
    const ruvia::env* env_{};
    std::size_t max_decoded_body_bytes_{default_max_buffered_body_bytes};
    const worker_state_registry* states_{};
    bool precompressed_static_files_{};
    // Bounds each blocking-pool wait of a file response's open, as the body
    // reads the transport waits on are bounded: the server write_timeout_.
    std::optional<timeout_duration> file_io_timeout_{};
    // Where a file response's real faults (no blocking pool, a stalled open)
    // are reported; the worker's connection failure outlet, never null.
    const connection_failure_sink* connection_failure_{};
    const http_client_ip_extractor* trusted_proxies_{};
};

struct context_connection_services final {
    conn_info info_;
    inbound_buffer_account* inbound_buffers_{};
    const route_table* routes_{};
    rate_limiter* route_rate_limiter_{};
    // Whether the peer is a configured trusted proxy. Decided once per
    // connection: the peer address never changes, so its IP parse and block
    // match are not repeated per request.
    bool peer_trusted_{false};
    // The certificate a TLS connection presented; null for cleartext.
    tls_served_certificate* served_certificate_{};
    // The Alt-Svc field value (RFC 7838 §3) this connection's listener
    // advertises on every response; empty when it advertises none.
    std::string_view alternative_service_{};
    // Set for a listener_dispatch::request_stream connection: every request
    // goes to this callback instead of the routes.
    request_stream_callback_ref request_stream_{};
    connection_lifecycle* lifecycle_{};
    // A router's borrowed clients and worker states, bound for its dispatches
    // like routes_; null keeps the worker's own.
    const borrowed_clients* router_clients_{};
    const worker_state_registry* router_states_{};
};

// The code request_stream::reset() chose; monostate selects the protocol's
// cancellation code (HTTP/2 CANCEL, H3_REQUEST_CANCELLED).
using request_reset_code = std::variant<std::monostate, http2_error_code, http3_error_code>;

struct request_stream_control final {
    callback_ref<void(request_reset_code)> reset_;
    bool reset_requested_{false};
};

// Borrowed protocol channels outlive every context for this request.
struct context_request_channels final {
    http_interim_response_output* interim_output_{};
    http_connection_advertisement_output* connection_advertisements_{};
    http_push_output* push_output_{};
    const std::optional<http_priority>* priority_update_{};
};

struct context_request_services final {
    std::reference_wrapper<const stop_token> stop_token_;
    context_request_body_source body_source_{};
    const http_request_trailers* trailers_{};
    context_request_channels channels_{};
    context_response_output response_output_{};
    std::size_t dispatch_depth_{};
    // The matched route's declared body_limit; 0 when it declares none.
    std::size_t route_body_limit_{};
    request_io_metrics* io_metrics_{nullptr};
    // Admission's Accept-Encoding selection for this request; empty for a
    // context created outside admission.
    std::optional<http_response_coding_selection> response_coding_{};
    // The arrival instant the request's owner pinned for rate-limit decisions
    // (test_app); empty when those decisions read the steady clock.
    std::optional<std::chrono::steady_clock::time_point> pinned_arrival_{};
    bool early_data_{false};
    request_body_state body_state_{};
    request_stream_control* stream_control_{};
};

class context_services final {
public:
    context_services() = delete;

    context_services(const context_worker_services& worker_services, const stop_token& stop_token_value)
        : worker_services_(&worker_services),
          request_services_{.stop_token_ = stop_token_value} {
        if (!worker_services_->worker_.get().valid()) {
            throw std::invalid_argument("context services require a valid worker");
        }
    }
    context_services(context_worker_services&&, const stop_token&) = delete;
    context_services(const context_worker_services&, stop_token&&) = delete;

    [[nodiscard]] constexpr worker_client_registry_view client_registries() const noexcept {
        return connection_services_.router_clients_ != nullptr
                   ? worker_client_registry_view(*connection_services_.router_clients_)
                   : worker_services_->clients_;
    }

    // Either argument may be null to keep the worker's own capability.
    void bind_router_capabilities(const borrowed_clients* clients, const worker_state_registry* states) noexcept {
        connection_services_.router_clients_ = clients;
        connection_services_.router_states_ = states;
    }

    [[nodiscard]] detail::rate_limiter* rate_limiter() const noexcept {
        return connection_services_.route_rate_limiter_ != nullptr
                   ? connection_services_.route_rate_limiter_
                   : worker_services_->rate_limiter_;
    }

    [[nodiscard]] const ruvia::env* env() const noexcept {
        return worker_services_->env_;
    }

    [[nodiscard]] inbound_buffer_account* inbound_buffers() const noexcept {
        return connection_services_.inbound_buffers_;
    }

    [[nodiscard]] context_services with_inbound_buffers(inbound_buffer_account& account) const noexcept {
        auto services = *this;
        services.connection_services_.inbound_buffers_ = &account;
        return services;
    }
    context_services with_inbound_buffers(inbound_buffer_account&&) const = delete;
    [[nodiscard]] constexpr std::size_t max_decoded_body_bytes() const noexcept {
        return worker_services_->max_decoded_body_bytes_;
    }

    // Ceiling for this request's decoded Content-Encoding representation. A
    // route's body_limit bounds what the route reads, not only the wire
    // bytes: a small compressed body must not inflate past it up to the
    // worker-wide bound.
    [[nodiscard]] constexpr std::size_t max_decoded_request_body_bytes() const noexcept {
        const auto route_limit = request_services_.route_body_limit_;
        return route_limit != 0 && route_limit < worker_services_->max_decoded_body_bytes_
                   ? route_limit
                   : worker_services_->max_decoded_body_bytes_;
    }

    void bind_route_body_limit(std::size_t limit) noexcept {
        request_services_.route_body_limit_ = limit;
    }

    [[nodiscard]] request_io_metrics* io_metrics() const noexcept {
        return request_services_.io_metrics_;
    }

    void bind_io_metrics(request_io_metrics* metrics) noexcept {
        request_services_.io_metrics_ = metrics;
    }

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_services_->worker_.get();
    }

    [[nodiscard]] const stop_token& get_stop_token() const noexcept {
        return request_services_.stop_token_.get();
    }

    // A transport that can observe the client leaving binds a request stop
    // that also fires then, on a request-local copy; `token` must observe the previous one and
    // outlive every context dispatched with these services.
    void bind_stop_token(const stop_token& token) noexcept {
        request_services_.stop_token_ = token;
    }
    void bind_stop_token(stop_token&&) = delete;

    void bind_early_data(bool value) noexcept {
        request_services_.early_data_ = value;
    }
    [[nodiscard]] bool early_data() const noexcept {
        return request_services_.early_data_;
    }

    void bind_body_state(request_body_state state) noexcept {
        request_services_.body_state_ = state;
    }
    [[nodiscard]] request_body_state body_state() const noexcept {
        return request_services_.body_state_;
    }
    void bind_stream_control(request_stream_control& control) noexcept {
        request_services_.stream_control_ = &control;
    }
    [[nodiscard]] bool request_reset() const noexcept {
        const auto* control = request_services_.stream_control_;
        return control != nullptr && control->reset_requested_;
    }
    void reset_request(request_reset_code error) const {
        if (auto* control = request_services_.stream_control_) {
            if (!control->reset_requested_) {
                control->reset_requested_ = true;
                control->reset_(error);
            }
        } else {
            abort_connection();
        }
    }

    [[nodiscard]] const route_table* routes() const noexcept {
        return connection_services_.routes_;
    }

    // Every dispatch path hands the context the table it may re-enter, which
    // also selects the not-found handler for the request path when the
    // handler asks for one. The route table is server-owned and outlives every
    // dispatched request.
    void bind_routes(const route_table& routes_value) noexcept {
        connection_services_.routes_ = &routes_value;
    }
    void bind_routes(route_table&&) = delete;
    void bind_route_rate_limiter(detail::rate_limiter& limiter) noexcept {
        connection_services_.route_rate_limiter_ = &limiter;
    }

    [[nodiscard]] constexpr const context_request_body_source& request_body_source() const noexcept {
        return request_services_.body_source_;
    }

    // Bind on a request-local copy; the worker and connection views stay shared.
    void bind_streaming_request_body(body_reader& value) noexcept {
        streaming_access::bind_worker(value, worker());
        request_services_.body_source_ = context_request_body_source::streaming(value);
    }
    void bind_lazy_request_body(request_body_loader& value) noexcept {
        request_services_.body_source_ = context_request_body_source::lazy(value);
    }

    [[nodiscard]] const http_request_trailers* request_trailers() const noexcept {
        return request_services_.trailers_;
    }

    // Bound by whichever layer owns the request body, alongside its source.
    void bind_request_trailers(const http_request_trailers& trailers) noexcept {
        request_services_.trailers_ = &trailers;
    }
    void bind_request_trailers(http_request_trailers&&) = delete;

    [[nodiscard]] http_interim_response_output* interim_output() const noexcept {
        return request_services_.channels_.interim_output_;
    }

    [[nodiscard]] http_connection_advertisement_output* connection_advertisements() const noexcept {
        return request_services_.channels_.connection_advertisements_;
    }

    [[nodiscard]] http_push_output* push_output() const noexcept {
        return request_services_.channels_.push_output_;
    }

    [[nodiscard]] const std::optional<http_priority>* request_priority_update() const noexcept {
        return request_services_.channels_.priority_update_;
    }

    void bind_request_channels(const context_request_channels& channels) noexcept {
        request_services_.channels_ = channels;
    }

    void bind_interim_output(http_interim_response_output& output) noexcept {
        request_services_.channels_.interim_output_ = &output;
    }

    [[nodiscard]] constexpr context_response_output& response_output() noexcept {
        return request_services_.response_output_;
    }

    [[nodiscard]] constexpr const context_response_output& response_output() const noexcept {
        return request_services_.response_output_;
    }

    void bind_response_stream(response_stream_writer& value) noexcept {
        streaming_access::bind_worker(value, worker());
        request_services_.response_output_ = context_response_output::response_stream(value);
    }

    [[nodiscard]] std::size_t dispatch_depth() const noexcept {
        return request_services_.dispatch_depth_;
    }

    // Subrequests inherit the parent connection identity, one dispatch level deeper.
    [[nodiscard]] context_services for_subrequest(const stop_token& stop_token_value) const {
        context_services services(*worker_services_, stop_token_value);
        services.connection_services_.info_ = connection_services_.info_;
        services.connection_services_.inbound_buffers_ = connection_services_.inbound_buffers_;
        services.connection_services_.routes_ = connection_services_.routes_;
        services.connection_services_.route_rate_limiter_ = connection_services_.route_rate_limiter_;
        services.connection_services_.lifecycle_ = connection_services_.lifecycle_;
        services.connection_services_.router_clients_ = connection_services_.router_clients_;
        services.connection_services_.router_states_ = connection_services_.router_states_;
        services.request_services_.dispatch_depth_ = request_services_.dispatch_depth_ + 1;
        services.request_services_.io_metrics_ = request_services_.io_metrics_;
        services.request_services_.early_data_ = request_services_.early_data_;
        services.request_services_.pinned_arrival_ = request_services_.pinned_arrival_;
        return services;
    }
    context_services for_subrequest(stop_token&&) const = delete;

    // Server-owned static-file policy. When response compression is enabled,
    // static_file() may negotiate an indexed precompressed variant. It never
    // performs request-time file compression.
    [[nodiscard]] constexpr bool precompressed_static_files() const noexcept {
        return worker_services_->precompressed_static_files_;
    }

    [[nodiscard]] constexpr std::optional<timeout_duration> file_io_timeout() const noexcept {
        return worker_services_->file_io_timeout_;
    }

    [[nodiscard]] const connection_failure_sink& connection_failure() const noexcept {
        return *worker_services_->connection_failure_;
    }

    // The request's Accept-Encoding selection as admission folded it, or
    // null for a context created outside admission. A failed negotiation is a
    // selection whose acceptable() is false.
    [[nodiscard]] const http_response_coding_selection* response_coding() const noexcept {
        return request_services_.response_coding_ ? &*request_services_.response_coding_ : nullptr;
    }

    void bind_response_coding(const http_response_coding_selection& selection) noexcept {
        request_services_.response_coding_ = selection;
    }

    [[nodiscard]] const std::optional<std::chrono::steady_clock::time_point>& pinned_arrival() const noexcept {
        return request_services_.pinned_arrival_;
    }

    void pin_arrival(std::chrono::steady_clock::time_point arrival) noexcept {
        request_services_.pinned_arrival_ = arrival;
    }

    [[nodiscard]] const worker_state_registry* worker_states() const noexcept {
        return connection_services_.router_states_ != nullptr ? connection_services_.router_states_
                                                              : worker_services_->states_;
    }

    [[nodiscard]] constexpr const conn_info& get_conn_info() const noexcept {
        return connection_services_.info_;
    }

    // The connection metadata one request sees. Identical to get_conn_info() unless
    // the peer is a configured trusted proxy, in which case its forwarding
    // headers name the client. Resolved per request rather than per connection:
    // each keep-alive request may carry different forwarding headers. Admission
    // resolves it once and binds the result with bind_conn_info, so every
    // context created for the request reads get_conn_info().
    [[nodiscard]] conn_info resolve_conn_info(const http_request& request) const noexcept;

    void bind_conn_info(const conn_info& resolved) noexcept {
        connection_services_.info_ = resolved;
    }

    // The RFC 9110 §7.4 facts of the transport connection, never of a
    // forwarded hop: whether its peer is a trusted gateway, and the
    // certificate it presented (null for cleartext and for subrequests).
    [[nodiscard]] constexpr bool peer_trusted() const noexcept {
        return connection_services_.peer_trusted_;
    }

    [[nodiscard]] constexpr tls_served_certificate* served_certificate() const noexcept {
        return connection_services_.served_certificate_;
    }
    [[nodiscard]] connection_lifecycle* connection_lifecycle_value() const noexcept {
        return connection_services_.lifecycle_;
    }
    void note_connection_request() const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->note_request();
        }
    }
    void set_connection_close_reason(connection_close_reason reason) const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->set_reason(reason);
        }
    }
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept {
        const auto* lifecycle = connection_services_.lifecycle_;
        return lifecycle == nullptr ? std::shared_ptr<void>{} : lifecycle->state();
    }
    void abort_connection() const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->abort();
        }
    }
    [[nodiscard]] bool connection_aborted() const noexcept {
        const auto* lifecycle = connection_services_.lifecycle_;
        return lifecycle != nullptr && lifecycle->aborted();
    }
    [[nodiscard]] std::string_view remote_address() const noexcept {
        return connection_services_.info_.remote_text_;
    }

    [[nodiscard]] context_services with_connection_lifecycle(connection_lifecycle& lifecycle) const noexcept {
        auto services = *this;
        services.connection_services_.lifecycle_ = &lifecycle;
        services.connection_services_.info_.local_ = lifecycle.local_endpoint();
        services.connection_services_.info_.remote_ = lifecycle.remote_endpoint();
        if (!services.connection_services_.info_.via_trusted_proxy_) {
            services.connection_services_.info_.client_ = lifecycle.remote_endpoint();
        }
        return services;
    }

    // Views borrow connection-owned storage and remain valid for every context
    // created while that connection is dispatched. `connection_id` is the
    // worker's conn_info::connection_id() for the connection, zero without one.
    [[nodiscard]] context_services with_plain_transport(
        std::uint64_t connection_id, std::string_view remote_address, std::uint16_t remote_port) const noexcept {
        auto services = *this;
        services.connection_services_.info_ = conn_info::plain(connection_id, remote_address);
        services.connection_services_.info_.set_remote_port(remote_port);
        services.connection_services_.peer_trusted_ =
            trusts_peer(services.connection_services_.info_.remote_.address());
        services.connection_services_.served_certificate_ = nullptr;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_plain_transport(
        std::uint64_t, std::basic_string<char, traits_type, allocator_type>&&, std::uint16_t) const = delete;

    // `alpn` and `server_name` borrow the connection's SSL, which outlives it.
    [[nodiscard]] context_services with_tls_transport(std::uint64_t connection_id, std::string_view remote_address,
        std::optional<std::string_view> client_certificate_subject, std::string_view alpn, std::string_view server_name,
        std::uint16_t remote_port, tls_served_certificate& served_certificate) const noexcept {
        auto services = *this;
        services.connection_services_.info_ =
            conn_info::tls(connection_id, remote_address, client_certificate_subject, alpn, server_name);
        services.connection_services_.info_.set_remote_port(remote_port);
        services.connection_services_.peer_trusted_ =
            trusts_peer(services.connection_services_.info_.remote_.address());
        services.connection_services_.served_certificate_ = &served_certificate;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_tls_transport(std::uint64_t, std::basic_string<char, traits_type, allocator_type>&&,
        std::optional<std::string_view>, std::string_view, std::string_view, std::uint16_t,
        tls_served_certificate&) const = delete;

    template <typename traits_type, typename allocator_type>
    context_services with_tls_transport(std::uint64_t, std::string_view, std::basic_string<char, traits_type, allocator_type>&&,
        std::string_view, std::string_view, std::uint16_t, tls_served_certificate&) const = delete;

    [[nodiscard]] constexpr std::string_view alternative_service() const noexcept {
        return connection_services_.alternative_service_;
    }

    // Empty unless the connection's listener is a listener_dispatch::request_stream one.
    [[nodiscard]] constexpr request_stream_callback_ref request_stream() const noexcept {
        return connection_services_.request_stream_;
    }

    [[nodiscard]] context_services with_request_stream(request_stream_callback_ref callback) const noexcept {
        auto services = *this;
        services.connection_services_.request_stream_ = callback;
        return services;
    }

    // `value` borrows listener-owned storage that outlives the connection.
    [[nodiscard]] context_services with_alternative_service(std::string_view value) const noexcept {
        auto services = *this;
        services.connection_services_.alternative_service_ = value;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_alternative_service(std::basic_string<char, traits_type, allocator_type>&&) const = delete;

private:
    [[nodiscard]] bool trusts_peer(const ip_address& remote_address) const noexcept {
        return worker_services_->trusted_proxies_ != nullptr &&
               worker_services_->trusted_proxies_->is_trusted_proxy(remote_address);
    }

    const context_worker_services* worker_services_;
    context_connection_services connection_services_{.info_ = conn_info::plain(0, {})};
    context_request_services request_services_;
};

// Adds the listener's Alt-Svc advertisement (RFC 7838 §3) to a final
// response. An application that set its own Alt-Svc field keeps it: the
// advertisement is a default, never a second, conflicting field. `value`
// borrows listener storage that outlives every response of the connection.
inline void apply_alternative_service(http_response& response, std::string_view value) {
    if (!value.empty() && !response.header("Alt-Svc").has_value()) {
        response.header_stable_view("Alt-Svc", value);
    }
}

}  // namespace ruvia::detail

namespace ruvia {
class session_middleware;
}

namespace ruvia::detail {

struct session_untouched final {};

struct session_unrecognized final {
    std::string id_;
};

struct session_loaded final {
    std::string id_;
    std::string data_;
};

struct session_persist_new final {
    std::string data_;
};

struct session_rotate final {
    std::string old_id_;
    std::string data_;
};

struct session_clear final {
    std::optional<std::string> old_id_;
};

class context_session_state final {
public:
    context_session_state() noexcept = default;

    void bind(const session_middleware* owner);
    [[nodiscard]] bool available() const noexcept {
        return available_;
    }
    [[nodiscard]] const session_middleware* owner() const noexcept {
        return owner_;
    }
    // The first commit freezes all mutation before any asynchronous storage I/O.
    // A failed commit is terminal: its external writes cannot safely be replayed.
    [[nodiscard]] bool begin_commit();
    void finish_commit() noexcept;
    void fail_commit(std::exception_ptr exception) noexcept;
    void observe_presented_id(std::string_view id);
    void load_recognized(std::string_view data);
    void set(std::string_view data);
    void clear();
    void regenerate();

    [[nodiscard]] std::string_view data() const& noexcept;
    [[nodiscard]] std::string_view data() const&& = delete;

    [[nodiscard]] const session_untouched* untouched() const& noexcept {
        return std::get_if<session_untouched>(&value_);
    }
    [[nodiscard]] const session_untouched* untouched() const&& = delete;

    [[nodiscard]] const session_unrecognized* unrecognized() const& noexcept {
        return std::get_if<session_unrecognized>(&value_);
    }
    [[nodiscard]] const session_unrecognized* unrecognized() const&& = delete;

    [[nodiscard]] const session_loaded* loaded() const& noexcept {
        return std::get_if<session_loaded>(&value_);
    }
    [[nodiscard]] const session_loaded* loaded() const&& = delete;

    [[nodiscard]] const session_persist_new* persist_new() const& noexcept {
        return std::get_if<session_persist_new>(&value_);
    }
    [[nodiscard]] const session_persist_new* persist_new() const&& = delete;

    [[nodiscard]] const session_rotate* rotate() const& noexcept {
        return std::get_if<session_rotate>(&value_);
    }
    [[nodiscard]] const session_rotate* rotate() const&& = delete;

    [[nodiscard]] const session_clear* cleared() const& noexcept {
        return std::get_if<session_clear>(&value_);
    }
    [[nodiscard]] const session_clear* cleared() const&& = delete;

private:
    enum class phase_type : unsigned char { active,
        committing,
        committed,
        failed };
    void require_mutable() const;

    bool available_{false};
    const session_middleware* owner_{nullptr};
    phase_type phase_{phase_type::active};
    std::exception_ptr failure_;
    std::variant<session_untouched, session_unrecognized, session_loaded, session_persist_new,
        session_rotate, session_clear>
        value_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Privileged access to a context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct session_access final {
    static void bind(context& context_value, const session_middleware* owner) {
        context_value.session_state().bind(owner);
    }

    [[nodiscard]] static bool available(const context& context_value) noexcept {
        return context_value.session_state().available();
    }

    // No callback chain: the bound session_middleware owns this capability's
    // single response-commit stage for both buffered and long-lived routes.
    [[nodiscard]] static task<void> commit(context& context);

    static void observe_presented_id(context& context_value, std::string_view id) {
        context_value.session_state().observe_presented_id(id);
    }

    static void load(context& context_value, std::string_view data) {
        context_value.session_state().load_recognized(data);
    }

    [[nodiscard]] static const context_session_state& state(const context& context_value) noexcept {
        return context_value.session_state();
    }
};

inline constexpr std::string_view session_rotation_script =
    "if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end "
    "if not redis.call('SET', KEYS[2], ARGV[1], 'EX', ARGV[2], 'NX') then return 0 end "
    "redis.call('DEL', KEYS[1]) return 1";

[[nodiscard]] inline bool is_valid_session_id(std::string_view id) noexcept {
    if (id.size() != secure_token_size) {
        return false;
    }
    for (const char ch : id) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

inline void set_session_cookie(http_response_headers& headers, std::string_view cookie_name,
    std::string_view value, const cookie_options& options) {
    headers.set_cookie(set_cookie_plan(cookie_name, value, options));
}

inline void append_session_cookie_header(
    http_response_headers& headers, std::string_view cookie_name, std::string_view id, bool secure) {
    set_session_cookie(headers, cookie_name, id,
        {
            .same_site_ = cookie_same_site::lax,
            .http_only_ = cookie_attribute_policy::emit,
            .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        });
}

inline void append_expired_session_cookie_header(
    http_response_headers& headers, std::string_view cookie_name, bool secure) {
    set_session_cookie(headers, cookie_name, "",
        {
            .same_site_ = cookie_same_site::lax,
            .expires_ = cookie_deletion_expires,
            .http_only_ = cookie_attribute_policy::emit,
            .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        });
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class request_body_loader final {
public:
    // `operation` bounds the wait for body bytes; an interrupted call keeps what
    // arrived so far and a later call resumes it.
    using read_all_type = task<std::string_view> (*)(void*, operation_options);
    using discard_type = task<void> (*)(void*, operation_options);

    constexpr request_body_loader(void* target, read_all_type read_all, discard_type discard) noexcept
        : target_(target),
          read_all_(read_all),
          discard_(discard) {}

    request_body_loader(const request_body_loader&) = delete;
    request_body_loader& operator=(const request_body_loader&) = delete;

    [[nodiscard]] task<std::string_view> read_all(operation_options operation) {
        switch (state_) {
            case state_type::available:
                state_ = state_type::consuming;
                break;
            case state_type::buffered:
                co_return buffered_body_;
            case state_type::consuming:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::discarded:
                throw std::logic_error("request body was discarded");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        const operation_steps steps(operation);
        try {
            buffered_body_ = co_await read_all_(target_, std::move(operation));
        } catch (...) {
            settle_failure(steps);
            throw;
        }
        state_ = state_type::buffered;
        co_return buffered_body_;
    }

    task<void> discard(operation_options operation) {
        switch (state_) {
            case state_type::available:
                state_ = state_type::consuming;
                break;
            case state_type::buffered:
            case state_type::discarded:
                co_return;
            case state_type::consuming:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        const operation_steps steps(operation);
        try {
            co_await discard_(target_, std::move(operation));
        } catch (...) {
            settle_failure(steps);
            throw;
        }
        state_ = state_type::discarded;
    }

private:
    enum class state_type : std::uint8_t {
        available,
        consuming,
        buffered,
        discarded,
        failed,
    };

    // Called while handling a consumption failure: an interruption by the
    // operation's own bound returns the body to `available`, so a later call
    // resumes it; any other failure is final.
    void settle_failure(const operation_steps& steps) noexcept {
        state_ = state_type::failed;
        try {
            throw;
        } catch (const std::system_error& error) {
            if (steps.interrupted(error)) {
                state_ = state_type::available;
            }
        } catch (...) {
        }
    }

    void* target_;
    read_all_type read_all_;
    discard_type discard_;
    std::string_view buffered_body_;
    state_type state_{state_type::available};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

template <typename reader_type>
[[nodiscard]] task<std::optional<std::span<const std::byte>>> body_reader_read_thunk(void* target, operation_options operation) {
    return static_cast<reader_type*>(target)->read(std::move(operation));
}

template <typename reader_type>
[[nodiscard]] body_reader make_body_reader_facade(reader_type& reader_value) noexcept {
    return streaming_access::make_body_reader(&reader_value, &body_reader_read_thunk<reader_type>);
}

template <typename reader_type>
class body_reader_binding final {
public:
    template <typename... args_type>
    explicit body_reader_binding(args_type&&... args)
        : reader_(std::forward<args_type>(args)...),
          facade_(make_body_reader_facade(reader_)) {}

    body_reader_binding(const body_reader_binding&) = delete;
    body_reader_binding& operator=(const body_reader_binding&) = delete;
    body_reader_binding(body_reader_binding&&) = delete;
    body_reader_binding& operator=(body_reader_binding&&) = delete;

    [[nodiscard]] reader_type& reader() noexcept {
        return reader_;
    }
    [[nodiscard]] body_reader& facade() noexcept {
        return facade_;
    }

private:
    reader_type reader_;
    body_reader facade_;
};

template <typename loader_type>
[[nodiscard]] task<std::string_view> request_body_loader_read_all_thunk(void* target, operation_options operation) {
    return static_cast<loader_type*>(target)->read_all(std::move(operation));
}

template <typename loader_type>
task<void> request_body_loader_discard_thunk(void* target, operation_options operation) {
    return static_cast<loader_type*>(target)->discard(std::move(operation));
}

template <typename loader_type>
[[nodiscard]] request_body_loader make_request_body_loader_facade(loader_type& loader) noexcept {
    return request_body_loader(
        &loader, &request_body_loader_read_all_thunk<loader_type>, &request_body_loader_discard_thunk<loader_type>);
}

template <typename loader_type>
class request_body_loader_binding final {
public:
    template <typename... args_type>
    explicit request_body_loader_binding(args_type&&... args)
        : loader_(std::forward<args_type>(args)...),
          facade_(make_request_body_loader_facade(loader_)) {}

    request_body_loader_binding(const request_body_loader_binding&) = delete;
    request_body_loader_binding& operator=(const request_body_loader_binding&) = delete;
    request_body_loader_binding(request_body_loader_binding&&) = delete;
    request_body_loader_binding& operator=(request_body_loader_binding&&) = delete;

    [[nodiscard]] loader_type& loader() noexcept {
        return loader_;
    }
    [[nodiscard]] const loader_type& loader() const noexcept {
        return loader_;
    }
    [[nodiscard]] request_body_loader& facade() noexcept {
        return facade_;
    }

private:
    loader_type loader_;
    request_body_loader facade_;
};

}  // namespace ruvia::detail

namespace ruvia {
class context;
}

namespace ruvia::detail {

// One request's typed interim output lane. The protocol driver outlives this
// object; operations own their fields until completion.
class http_interim_response_output final {
public:
    using write_type = task<void> (*)(void*, const http_interim_response_head&);
    http_interim_response_output(void* target, write_type write) noexcept
        : target_(target),
          write_(write) {}

    // An interrupted inform() resets the request of `context_value`.
    [[nodiscard]] task<void> inform(
        const http_interim_response_head& response, operation_options operation, context& context_value);
    // Answers a 100-continue expectation before the body is read, on the same
    // lane as inform() so the two heads never interleave. A final response
    // that has started already answered the expectation, and RFC 9110 §15.2
    // forbids any 1xx after it, so then nothing is written.
    task<void> send_continue();
    void commit_final() {
        if (output_active_) {
            throw std::logic_error("interim response output is still active");
        }
        final_committed_ = true;
    }

private:
    [[nodiscard]] task<void> write_owned(
        http_status_code status, std::vector<http_header> fields, operation_options operation, context& context_value);
    void* target_;
    write_type write_;
    bool final_committed_{};
    bool output_active_{};
    operation_interrupt_source interrupts_;
    ::ruvia::operation_scope scope_;
};
}  // namespace ruvia::detail

namespace ruvia::detail {

// A lazily built index for queries(name). Values stay in request order within
// each name; groups are sorted only for lookup and are never exposed publicly.
class request_query_values final {
public:
    explicit request_query_values(const request_name_value_list& fields) {
        if (fields.empty()) {
            return;
        }
        static constexpr std::size_t inline_order_capacity = 32;
        std::array<std::size_t, inline_order_capacity> inline_order;
        std::vector<std::size_t> heap_order;
        std::span<std::size_t> order;
        if (fields.size() <= inline_order_capacity) {
            order = std::span<std::size_t>(inline_order.data(), fields.size());
        } else {
            heap_order.resize(fields.size());
            order = heap_order;
        }
        for (std::size_t i = 0; i < fields.size(); ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, [&fields](std::size_t left, std::size_t right) noexcept {
            const auto left_name = fields[left].name();
            const auto right_name = fields[right].name();
            return left_name == right_name ? left < right : left_name < right_name;
        });
        std::size_t group_count = 1;
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (fields[order[i]].name() != fields[order[i - 1]].name()) {
                ++group_count;
            }
        }
        groups_.reserve(group_count);
        values_.reserve(fields.size());
        for (const auto index : order) {
            const auto name = fields[index].name();
            if (groups_.empty() || groups_.back().name_ != name) {
                groups_.push_back({name, values_.size(), 0});
            }
            values_.push_back(fields[index].value());
            ++groups_.back().count_;
        }
    }

    [[nodiscard]] std::span<const std::string_view> values(
        std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND {
        const auto group = std::ranges::lower_bound(groups_, name, {}, &group_type::name_);
        if (group == groups_.end() || group->name_ != name) {
            return {};
        }
        return std::span<const std::string_view>(values_).subspan(group->begin_, group->count_);
    }
    std::span<const std::string_view> values(std::string_view) const&& = delete;

private:
    struct group_type final {
        std::string_view name_;
        std::size_t begin_;
        std::size_t count_;
    };
    std::vector<group_type> groups_;
    std::vector<std::string_view> values_;
};

// Field views borrow the request query string or decoded_bytes_, so neither
// moves once published.
class request_query_cache final {
public:
    request_query_cache() noexcept
        : fields_(request_name_value_list_access::make()) {}

    [[nodiscard]] std::string& decoded_bytes() noexcept {
        return decoded_bytes_;
    }
    [[nodiscard]] request_name_value_list& mutable_fields() noexcept {
        return fields_;
    }

    [[nodiscard]] const request_name_value_list& fields() const& noexcept {
        return fields_;
    }
    const request_name_value_list& fields() const&& = delete;

    [[nodiscard]] const request_query_values& values() & {
        if (!values_) {
            values_.emplace(fields_);
        }
        return *values_;
    }
    const request_query_values& values() && = delete;

private:
    std::string decoded_bytes_;
    request_name_value_list fields_;
    std::optional<request_query_values> values_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Draft and selected response have disjoint ownership. Building a response
// consumes the draft; selecting or replacing a response never merges metadata.
class context_response_state final {
public:
    context_response_state() noexcept = default;

    [[nodiscard]] bool has_draft() const noexcept {
        return draft_.has_value();
    }
    [[nodiscard]] http_status_code draft_status() const noexcept {
        return draft_ ? draft_->status() : http_status::ok;
    }
    void reset_draft() noexcept {
        draft_.reset();
    }
    [[nodiscard]] http_response take_draft_or_new() noexcept {
        if (!draft_) {
            return http_response{};
        }
        auto response = std::move(*draft_);
        draft_.reset();
        return response;
    }
    [[nodiscard]] http_response& draft() {
        if (!draft_) {
            draft_.emplace();
        }
        return *draft_;
    }
    [[nodiscard]] http_response* final() noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] const http_response* final() const noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] http_response& policy() {
        if (!policy_) {
            policy_.emplace();
        }
        return *policy_;
    }
    [[nodiscard]] http_response& defaults() {
        if (!defaults_) {
            defaults_.emplace();
        }
        return *defaults_;
    }
    void policy_status(http_status_code status) {
        policy_status_ = status;
    }
    void apply_policy(http_response& response) const {
        if (defaults_ && !defaults_->headers().empty()) {
            response.transfer_headers_from(*defaults_, http_response_header_transfer::merge);
        }
        if (policy_ && !policy_->headers().empty()) {
            response.transfer_headers_from(*policy_, http_response_header_transfer::apply);
        }
        if (policy_status_) {
            response.status(*policy_status_);
        }
    }

    void finalize(http_response&& response) {
        final_.emplace(std::move(response));
    }

    [[nodiscard]] http_response&& release_final() & {
        assert(final_);
        apply_policy(*final_);
        return std::move(*final_);
    }
    http_response&& release_final() && = delete;

private:
    std::optional<http_response> draft_;
    std::optional<http_response> policy_;
    std::optional<http_response> defaults_;
    std::optional<http_status_code> policy_status_;
    std::optional<http_response> final_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

struct aligned_storage_deleter final {
    std::size_t alignment_{alignof(std::max_align_t)};
    void operator()(std::byte* storage) const noexcept {
        ::operator delete[](storage, std::align_val_t{alignment_});
    }
};

// Owns every request-local context value that is not a borrowed pointer: lazy
// caches, response/session machines, typed bindings, and middleware next() hop
// storage. Synchronous construction borrows the input view, then retains one
// services value here before returning; no reference to the input survives.
class context_request_storage final {
public:
    explicit context_request_storage(const context_services& service_view) noexcept
        : services_(service_view),
          decoded_body_reservation_(services_.inbound_buffers()) {}

    context_request_storage(const context_request_storage&) = delete;
    context_request_storage& operator=(const context_request_storage&) = delete;

    [[nodiscard]] std::byte* allocate_raw(std::size_t size, std::size_t alignment) {
        auto* storage = static_cast<std::byte*>(::operator new[](size, std::align_val_t{alignment}));
        try {
            owned_blocks_.emplace_back(storage, aligned_storage_deleter{alignment});
        } catch (...) {
            ::operator delete[](storage, std::align_val_t{alignment});
            throw;
        }
        return storage;
    }

    context_services services_;
    context_response_state response_state_;
    context_session_state session_state_;
    request_bindings request_bindings_;

    std::optional<std::string> decoded_body_;
    inbound_buffer_reservation decoded_body_reservation_;
    std::optional<request_name_value_list> headers_;
    std::optional<request_query_cache> query_;
    std::optional<request_name_value_list> cookies_;
    std::string decoded_route_params_;
    // Views borrow route storage, the request path, or decoded_route_params_.
    std::optional<request_name_value_list> route_params_;
    std::vector<std::unique_ptr<std::byte[], aligned_storage_deleter>> owned_blocks_;
    bool route_params_invalid_{false};
    bool route_params_unencoded_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class context_websocket_binding;
class context_tunnel_binding;

struct context_access final {
    // An interrupted output operation cannot withdraw what it wrote, so it
    // resets the request: the HTTP/2 or HTTP/3 stream, or the HTTP/1 connection.
    static void reset_interrupted_request(void* raw_context) noexcept {
        auto& request_services = static_cast<context*>(raw_context)->services();
        try {
            request_services.reset_request({});
        } catch (...) {
            request_services.abort_connection();
        }
    }

    // A request-stream router borrows the already-bound transport context.
    // Restore every borrowed route view before its resolution frame retires;
    // retain the output state, since a stream may have committed in that scope.
    class route_binding final {
    public:
        route_binding(context& value, std::string_view path, std::span<const std::string_view> names,
            std::span<const std::string_view> values, std::uintptr_t scope)
            : context_(value),
              services_(value.services()),
              path_(value.route_path_),
              names_(value.param_names_),
              values_(value.param_values_),
              count_(value.param_count_),
              scope_(value.route_rate_limit_scope_),
              worker_states_(value.capabilities_.worker_states()) {
            value.route_path_ = path;
            value.param_names_ = names.data();
            value.param_values_ = values.data();
            value.param_count_ = values.size();
            value.route_rate_limit_scope_ = scope;
        }
        route_binding(const route_binding&) = delete;
        route_binding& operator=(const route_binding&) = delete;
        ~route_binding() {
            const auto output = context_.services().response_output();
            context_.services() = services_;
            context_.services().response_output() = output;
            context_.route_path_ = path_;
            context_.param_names_ = names_;
            context_.param_values_ = values_;
            context_.param_count_ = count_;
            context_.route_rate_limit_scope_ = scope_;
            context_.capabilities_.bind_worker_states(worker_states_);
        }

        // Points the context's worker-state lookups at the registry its
        // services now name; the destructor restores the previous one.
        void bind_worker_states() noexcept {
            context_.capabilities_.bind_worker_states(context_.services().worker_states());
        }

    private:
        context& context_;
        context_services services_;
        std::string_view path_;
        const std::string_view* names_;
        const std::string_view* values_;
        std::size_t count_;
        std::uintptr_t scope_;
        const worker_state_registry* worker_states_;
    };

    [[nodiscard]] static context make(
        const http_request& request, const context_services& services) {
        return context(request, services);
    }

    [[nodiscard]] static context make(const http_request& request,
        std::string_view route_path, const std::string_view* param_names,
        const std::string_view* param_values, std::size_t param_count,
        std::uintptr_t route_rate_limit_scope, const context_services& services) {
        return context(request, route_path, param_names, param_values, param_count,
            route_rate_limit_scope, services);
    }

    [[nodiscard]] static std::byte* allocate_request_storage(
        context& context_value, std::size_t size, std::size_t alignment) {
        return context_value.request_storage().allocate_raw(size, alignment);
    }

    // The response for an indexed entry already resolved below `root`. The
    // representation is selected and the content type copied before the task
    // is returned, so `relative` and `content_type_value` are read only by
    // this call; `root` and `base_entry` (a view into it) must outlive the
    // task, which opens a file-backed entry on the blocking pool.
    [[nodiscard]] static task<http_response> static_file_resolved(
        context& context_value, const static_root& root, std::string_view relative,
        std::string_view content_type_value, static_root_entry_view base_entry,
        static_file_selection_mode mode);

    [[nodiscard]] static http_response make_static_json_response(
        context& context_value, std::string_view json_body) {
        auto response = context_value.response_state().take_draft_or_new();
        if (!response.header("Content-Type")) {
            response.content_type(http_content_type::application_json);
        }
        response.static_body(json_body);
        return response;
    }

    [[nodiscard]] static http_response make_owned_json_response(
        context& context_value, std::string& json_body) {
        return context_value.json_serialized(json_body);
    }

    [[nodiscard]] static http_interim_response_output* interim_output(context& context_value) noexcept {
        return context_value.services().interim_output();
    }

    [[nodiscard]] static const http_request& request(const context& context_value) noexcept {
        return context_value.request_;
    }

    [[nodiscard]] static const context_request_body_source& request_body_source(
        const context& context_value) noexcept {
        return context_value.request_body_source();
    }

    [[nodiscard]] static context_services& services(context& context_value) noexcept {
        return context_value.services();
    }

    [[nodiscard]] static const context_services& services(const context& context_value) noexcept {
        return context_value.services();
    }

    [[nodiscard]] static std::optional<std::string_view> decoded_body(
        const context& context_value) noexcept {
        if (!context_value.body_decoded_) {
            return std::nullopt;
        }
        return std::string_view(*context_value.request_storage_->decoded_body_);
    }

    [[nodiscard]] static bool body_identity(const context& context_value) noexcept {
        return context_value.body_identity_;
    }

    static void mark_body_identity(const context& context_value) noexcept {
        context_value.body_identity_ = true;
    }

    [[nodiscard]] static std::string_view store_decoded_body(
        const context& context_value, std::string&& decoded_content) {
        auto& storage = context_value.request_storage();
        storage.decoded_body_reservation_.resize_to(inbound_string_heap_bytes(decoded_content));
        auto& decoded = context_value.decoded_body();
        decoded = std::move(decoded_content);
        storage.decoded_body_reservation_.resize_to(inbound_string_heap_bytes(decoded));
        context_value.body_decoded_ = true;
        return std::string_view(decoded);
    }

    [[nodiscard]] static detail::rate_limiter* rate_limiter(context& context_value) noexcept {
        return context_value.services().rate_limiter();
    }

    [[nodiscard]] static const std::optional<std::chrono::steady_clock::time_point>& rate_limit_arrival(
        context& context_value) noexcept {
        return context_value.services().pinned_arrival();
    }

    [[nodiscard]] static std::uintptr_t route_rate_limit_scope(const context& context_value) noexcept {
        return context_value.route_rate_limit_scope_;
    }

    static void set_response(context& context_value, http_response&& response) {
        context_value.store_response(std::move(response));
    }

    [[nodiscard]] static const http_response_headers& handshake_headers(context& context_value) {
        auto& state = context_value.response_state();
        state.apply_policy(state.draft());
        return state.draft().headers();
    }
    [[nodiscard]] static http_response& response_policy(context& context_value) {
        return context_value.response_state().policy();
    }
    static void discard_draft(context& context_value) {
        context_value.response_state().reset_draft();
    }
    static void apply_response_policy(context& context_value, http_response& response) {
        context_value.response_state().apply_policy(response);
    }
    static void security_header(context& context_value, std::string_view name, std::string_view value, bool replace) {
        auto& state = context_value.response_state();
        auto& response = replace ? state.policy() : state.defaults();
        response.header_stable_view(name, value);
    }

    // The status the handler set; a file response sends it for the full body.
    [[nodiscard]] static http_status_code response_status(const context& context_value) noexcept {
        return context_value.response_state().draft_status();
    }

    static void apply_response_state(context& context_value, http_response& response,
        std::optional<http_status_code> status_code) {
        context_value.apply_response_state(response, status_code);
    }

    // Called by a driver right before it hands the upgrade/CONNECT handshake
    // head to the transport: from here on no HTTP response can follow, and no
    // 1xx head either -- the stream now carries the upgraded protocol. Throws,
    // before anything is written, while an interim head is still being written.
    static void commit_upgrade(context& context_value) {
        if (auto* interim = context_value.services().interim_output()) {
            interim->commit_final();
        }
        context_value.response_output() = context_response_output::upgraded();
    }

    [[nodiscard]] static bool output_committed(const context& context_value) noexcept {
        return context_value.response_output().committed();
    }

    static void set_error(context& context_value, std::exception_ptr exception) noexcept {
        context_value.store_error(std::move(exception));
    }

    [[nodiscard]] static bool has_response(const context& context_value) noexcept {
        return context_value.has_response();
    }

    // The final response, for the dispatcher to return; see
    // context_response_state::release_final.
    [[nodiscard]] static http_response&& release_response(context& context_value) {
        return context_value.response_state().release_final();
    }

    [[nodiscard]] static http_response streaming_head(context& context_value) {
        return context_value.streaming_head();
    }

    // Commits the bound session, then builds the head a response stream sends before its first byte.
    [[nodiscard]] static task<http_response> commit_streaming_head(context& context_value);

private:
    friend class context_websocket_binding;
    friend class context_tunnel_binding;
    [[nodiscard]] static context_response_output bind_tunnel(context& context_value, http_tunnel& tunnel) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::tunnel(tunnel);
        return previous;
    }

    [[nodiscard]] static context_response_output bind_websocket(
        context& context_value, websocket& websocket_value) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::websocket_value(websocket_value);
        return previous;
    }

    static void restore_response_output(context& context_value, context_response_output output) noexcept {
        context_value.response_output() = output;
    }
};

class context_tunnel_binding final {
public:
    context_tunnel_binding(context& context_value, http_tunnel& tunnel) noexcept
        : context_(context_value),
          previous_(context_access::bind_tunnel(context_value, tunnel)) {}
    context_tunnel_binding(const context_tunnel_binding&) = delete;
    context_tunnel_binding& operator=(const context_tunnel_binding&) = delete;
    ~context_tunnel_binding() {
        context_access::restore_response_output(context_, previous_);
    }

private:
    context& context_;
    context_response_output previous_;
};

// The facade borrowed by context is valid only while the established session
// owns its connection. Restoring the previous output capability on every exit
// prevents onion middleware post-processing from observing a dangling facade.
class context_websocket_binding final {
public:
    context_websocket_binding(context& context_value, websocket& websocket_value) noexcept
        : context_(&context_value),
          previous_(context_access::bind_websocket(context_value, websocket_value)) {}

    context_websocket_binding(const context_websocket_binding&) = delete;
    context_websocket_binding& operator=(const context_websocket_binding&) = delete;
    context_websocket_binding(context_websocket_binding&&) = delete;
    context_websocket_binding& operator=(context_websocket_binding&&) = delete;

    ~context_websocket_binding() {
        context_access::restore_response_output(*context_, previous_);
    }

private:
    context* context_;
    context_response_output previous_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] http_response make_default_error_response(http_error_info error);

// The default 404 answer for a request no route matched.
[[nodiscard]] http_response make_route_not_found_response();

[[nodiscard]] task<http_response> invoke_error_handler(
    context& context_value, http_error_info error, http_error_handler_ref handler);

}  // namespace ruvia::detail

namespace ruvia::detail {

// Web-runtime signal for an otherwise valid request whose complete
// Content-Encoding stack cannot be decoded. router dispatch owns the 415 JSON
// mapping and the RFC 9110 Accept-Encoding response advertisement, which names
// what the failed read accepts: a streaming read decodes nothing.
class unsupported_request_content_coding final : public std::exception {
public:
    enum class read_type { decoded,
        streaming };

    explicit unsupported_request_content_coding(read_type read) noexcept
        : read_(read) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "request Content-Encoding is not supported";
    }

    // An Accept-Encoding field value with static storage.
    [[nodiscard]] std::string_view accepted_codings() const noexcept {
        return read_ == read_type::decoded ? http_supported_request_content_codings() : "identity";
    }

private:
    read_type read_;
};

}  // namespace ruvia::detail
