#include "server/request_stream_dispatch.h"

#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/streaming.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_session.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_session.h"
#endif

#include "context/context_access.h"
#include "context/context_services.h"
#include "integration/operation_lane_lease.h"
#include "router/compiled_route_plan.h"
#include "router/route_table.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

struct request_stream_access final {
    [[nodiscard]] static request_stream make(context& context_value, response_stream_writer& response,
        const request_stream_tunnel_accept* accept_tunnel,
        const request_stream_websocket_accept* accept_websocket = nullptr) noexcept {
        return request_stream(context_value, response, accept_tunnel, accept_websocket);
    }

    [[nodiscard]] static task<void> close(request_stream& stream) {
        return stream.operation_scope_.close_and_join();
    }
};

namespace {

// Runs the connection's callback; returns its failure, if any.
task<std::exception_ptr> run_request_stream_callback(
    context& context_value, response_stream_writer& response, const request_stream_tunnel_accept* accept_tunnel,
    const request_stream_websocket_accept* accept_websocket = nullptr) {
    const auto callback = context_access::services(context_value).request_stream();
    auto stream = request_stream_access::make(context_value, response, accept_tunnel, accept_websocket);
    std::exception_ptr failure;
    try {
        co_await callback(stream);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await request_stream_access::close(stream);
    co_return failure;
}

// After the callback: a committed response or tunnel stands, and a failure
// past it propagates to the transport, which aborts the stream and reports it.
// Otherwise nothing reached the peer: an explicit connection abort is terminal;
// without it the request gets the framework's answer to the failure, and a
// callback that selected nothing is a fault. Unexpected failures reach the sink.
// No application error handler takes part.
void complete_request_stream(context& context_value, std::exception_ptr failure) {
    if (context_access::services(context_value).connection_aborted() ||
        context_access::services(context_value).request_reset()) {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return;
    }
    if (context_access::output_committed(context_value)) {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return;
    }
    if (failure == nullptr) {
        if (context_access::has_response(context_value)) {
            return;
        }
        failure = std::make_exception_ptr(std::logic_error("request stream callback completed without a response"));
    }
    auto answer = make_default_exception_response(failure);
    if (answer.unexpected_) {
        context_access::services(context_value)
            .connection_failure()
            .invoke(context_access::services(context_value).remote_address(), failure);
    }
    context_access::set_response(context_value, std::move(answer.response_));
}

// The response route of a request stream connection: the callback writes
// through the transport's response stream, bound to the context.
task<void> serve_request_stream(void*, context& context_value) {
    auto failure = co_await run_request_stream_callback(context_value, context_value.stream(), nullptr);
    complete_request_stream(context_value, std::move(failure));
}

[[nodiscard]] const route_entry& request_stream_response_route() noexcept {
    // Process-invariant and allocation-free: no route path, method token or
    // protocol to own.
    static const route_entry route({.method_ = http_known_method::unknown,
        .path_ = {},
        .endpoint_ = route_endpoint::response_stream(&serve_request_stream, request_body_mode::stream)});
    return route;
}

[[nodiscard]] const route_entry& request_stream_tunnel_route() noexcept {
    // The tunnel upgrade runs serve_request_stream_tunnel() for this route
    // instead of its handler.
    static const route_entry route({.method_ = http_known_method::unknown,
        .path_ = {},
        .endpoint_ = route_endpoint::tunnel(&serve_request_stream, {}, {})});
    return route;
}

}  // namespace

route_resolution resolve_request_stream_route(bool tunnel) noexcept {
    return route_resolution::resolved(tunnel ? request_stream_tunnel_route() : request_stream_response_route(), nullptr);
}

bool is_request_stream_tunnel_route(const route_entry& route) noexcept {
    return &route == &request_stream_tunnel_route();
}

bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept {
    return request.known_method() == http_known_method::connect || (!content && http1_request_offers_upgrade(request));
}

task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept,
    const request_stream_websocket_accept* accept_websocket) {
    auto failure = co_await run_request_stream_callback(context_value, context_value.stream(), &accept, accept_websocket);
    complete_request_stream(context_value, std::move(failure));
}

}  // namespace ruvia::detail

namespace ruvia {

const http_request& request_stream::request() const& noexcept {
    return detail::context_access::request(context_);
}

const conn_info& request_stream::conn() const& noexcept {
    return context_.conn();
}

request_body_state request_stream::body_state() const noexcept {
    return detail::context_access::services(context_).body_state();
}

void request_stream::reset() {
    detail::context_access::services(context_).reset_request({});
}

void request_stream::reset(http2_error_code error) {
    if (request().protocol_version() != http_protocol_version::http2) {
        throw std::invalid_argument("HTTP/2 reset code requires HTTP/2");
    }
    detail::context_access::services(context_).reset_request(error);
}

void request_stream::reset(http3_error_code error) {
    if (request().protocol_version() != http_protocol_version::http3) {
        throw std::invalid_argument("HTTP/3 reset code requires HTTP/3");
    }
    detail::context_access::services(context_).reset_request(error);
}

body_reader& request_stream::body() & {
    if (accept_tunnel_ != nullptr) {
        throw std::logic_error("a tunnel request carries no content: its bytes belong to accept_tunnel()");
    }
    return context_.req().body_reader();
}

std::span<const http_header> request_stream::trailers() const noexcept {
    return context_.req().trailers();
}

std::optional<std::string_view> request_stream::trailer(std::string_view name) const noexcept {
    return context_.req().trailer(name);
}

task<void> request_stream::inform(const http_interim_response_head& response, operation_options operation) & {
    return context_.inform(response, std::move(operation));
}

bool request_stream::early_data() const noexcept {
    return context_.early_data();
}

bool request_stream::push(http_push_request_view request) {
    return context_.push(request);
}

void request_stream::advertise_origins(std::span<const std::string_view> origins) {
    context_.advertise_origins(origins);
}

void request_stream::advertise_alternative_service(std::string_view value) {
    context_.advertise_alternative_service(value);
}

http_client_ref request_stream::client() const {
    return context_.client();
}

http_client_ref request_stream::client(std::string_view alias) const {
    return context_.client(alias);
}

#ifdef RUVIA_ENABLE_DATABASE
db_session request_stream::db() const {
    return context_.db();
}

db_session request_stream::db(std::string_view alias) const {
    return context_.db(alias);
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_session request_stream::redis() const {
    return context_.redis();
}

redis_session request_stream::redis(std::string_view alias) const {
    return context_.redis(alias);
}
#endif

context_response_editor request_stream::draft() & noexcept {
    return context_.draft();
}

task<http_response> request_stream::file(file_response_options options) {
    return make_scoped_operation(operation_scope_, context_.file(options));
}

task<http_response> request_stream::static_file(const static_root& root, static_file_response_options options) {
    return make_scoped_operation(operation_scope_, context_.static_file(root, options));
}

void request_stream::respond(http_response response) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    require_unselected_response();
    if (tunnel_requested() && request().known_method() == http_known_method::connect &&
        response.status().is_successful()) {
        throw std::logic_error("a 2xx response to CONNECT establishes the tunnel; use accept_tunnel()");
    }
    if (response.status() == http_status::switching_protocols) {
        throw std::logic_error("a 101 response requires accept_tunnel() or accept_websocket()");
    }
    context_.respond(std::move(response));
}

response_stream_writer& request_stream::response() & noexcept {
    return response_;
}

bool request_stream::tunnel_requested() const noexcept {
    return accept_tunnel_ != nullptr;
}

task<http_tunnel*> request_stream::accept_tunnel(http_status_code status, http_tunnel_route_config config) & {
    return make_scoped_operation(operation_scope_, accept_tunnel_impl(status, config));
}

task<http_tunnel*> request_stream::accept_tunnel_impl(http_status_code status, http_tunnel_route_config config) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    if (accept_tunnel_ == nullptr) {
        throw std::logic_error("accept_tunnel() requires a CONNECT or HTTP/1.1 Upgrade request");
    }
    require_unselected_response();
    if (request().known_method() == http_known_method::connect) {
        if (!status.is_successful()) {
            throw std::invalid_argument("a CONNECT tunnel is accepted with a 2xx status");
        }
        context_.draft().status(status);
    } else if (status != http_status::switching_protocols) {
        throw std::invalid_argument("an HTTP/1.1 Upgrade is accepted with 101 (Switching Protocols)");
    }
    co_return co_await (*accept_tunnel_)(status, config);
}

task<websocket_accept_result> request_stream::accept_websocket(websocket_route_config config) & {
    return make_scoped_operation(operation_scope_, accept_websocket_impl(std::move(config)));
}

task<websocket_accept_result> request_stream::accept_websocket_impl(websocket_route_config config) {
    // Validated before any state check so an invalid configuration selects nothing.
    const auto endpoint = detail::route_endpoint::websocket(nullptr, std::move(config));
    co_return co_await accept_websocket_endpoint(*endpoint.websocket());
}

task<websocket_accept_result> request_stream::accept_websocket_endpoint(const detail::websocket_route_endpoint& endpoint) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    if (accept_websocket_ == nullptr) {
        throw std::logic_error("accept_websocket() requires an Upgrade or extended CONNECT request");
    }
    require_unselected_response();
    co_return co_await (*accept_websocket_)(endpoint);
}

void request_stream::require_unselected_response() const {
    if (detail::context_access::services(context_).request_reset() ||
        detail::context_access::services(context_).connection_aborted() ||
        response_.committed() || detail::context_access::output_committed(context_) ||
        detail::context_access::has_response(context_)) {
        throw std::logic_error("the request's response is already selected or committed");
    }
}

::ruvia::stop_token request_stream::stop_token() const noexcept {
    return context_.stop_token();
}

}  // namespace ruvia
