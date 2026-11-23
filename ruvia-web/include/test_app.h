#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "context.h"
#include "controller.h"
#include "event_loop.h"
#include "http_error.h"
#include "http_server.h"
#include "ip_address.h"
#include "middleware.h"
#include "model.h"
#include "outbound_bytes.h"
#include "rate_limiting.h"
#include "worker_ref.h"
#include "worker_services.h"

namespace ruvia {

class http_client;
#ifdef RUVIA_ENABLE_DATABASE
class db_client;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_client;
#endif
// In-memory testing facade for applications built on Ruvia -- the actix
// test_request / Hono app.request() analog. test_app serves exactly the
// controllers named through controllers<...>() (none by default),
// compiles them with the same route plan application::run() uses,
// finalizes the production route table, and dispatches TestRequests through
// the real buffered request chain of a server with default options: request
// admission (body ceilings, rate limits), routing,
// controller/route/global middleware, validators, prefix and app-wide
// not_found/on_error fallbacks, url_for, worker state, and the response tail
// (CORS, content-coding and error recovery) all behave as they do in a
// running server. The private worker is brought up and retired by the same
// worker capability chain a server worker uses.
// No socket is opened; dispatch() drives that worker to completion
// synchronously and returns an independently owned response.
//
// Scope: buffered-response routes (including 404/405/501/OPTIONS fallbacks).
// Streaming/SSE/websocket routes and handlers that await worker-bound
// services (db(), redis(), run_blocking()) need a running server; drive those
// through a real loopback server instead.

class test_app;

// Builder for one in-memory request. Owns every string handed to it, so a
// test_request may be built from temporaries and reused across dispatches.
class test_request final {
public:
    test_request(const test_request&) = default;
    test_request(test_request&&) = default;
    test_request& operator=(const test_request&) = default;
    test_request& operator=(test_request&&) = default;

    [[nodiscard]] static test_request method(std::string_view method_token, std::string_view target) {
        return test_request(method_token, target);
    }

    [[nodiscard]] static test_request get(std::string_view target) {
        return test_request("GET", target);
    }

    [[nodiscard]] static test_request post(std::string_view target) {
        return test_request("POST", target);
    }

    [[nodiscard]] static test_request put(std::string_view target) {
        return test_request("PUT", target);
    }

    [[nodiscard]] static test_request patch(std::string_view target) {
        return test_request("PATCH", target);
    }

    [[nodiscard]] static test_request delete_(std::string_view target) {
        return test_request("DELETE", target);
    }

    [[nodiscard]] static test_request head(std::string_view target) {
        return test_request("HEAD", target);
    }

    [[nodiscard]] static test_request options(std::string_view target) {
        return test_request("OPTIONS", target);
    }

    test_request& header(std::string_view name, std::string_view value) & {
        headers_.emplace_back(name, value);
        return *this;
    }
    test_request&& header(std::string_view name, std::string_view value) && {
        return std::move(header(name, value));
    }

    test_request& body(outbound_bytes bytes_value) & {
        body_ = std::move(bytes_value).take_string();
        return *this;
    }
    test_request&& body(outbound_bytes bytes_value) && {
        return std::move(body(std::move(bytes_value)));
    }

    test_request& body(outbound_bytes bytes_value, std::string_view content_type_value) & {
        body_ = std::move(bytes_value).take_string();
        return header("Content-Type", content_type_value);
    }
    test_request&& body(outbound_bytes bytes_value, std::string_view content_type_value) && {
        return std::move(body(std::move(bytes_value), content_type_value));
    }

    test_request& json(outbound_bytes json_text) & {
        return body(std::move(json_text), "application/json");
    }
    test_request&& json(outbound_bytes json_text) && {
        return std::move(json(std::move(json_text)));
    }

    template <typename t_type>
        requires(!std::convertible_to<const t_type&, outbound_bytes>) && requires(const t_type& v) {
            { to_json(v) } -> std::same_as<std::string>;
        }
    test_request& json(const t_type& value) & {
        body_ = to_json(value);
        return header("Content-Type", "application/json");
    }
    template <typename t_type>
        requires(!std::convertible_to<const t_type&, outbound_bytes>) && requires(const t_type& v) {
            { to_json(v) } -> std::same_as<std::string>;
        }
    test_request&& json(const t_type& value) && {
        return std::move(json(value));
    }

    test_request& form(outbound_bytes url_encoded) & {
        return body(std::move(url_encoded), "application/x-www-form-urlencoded");
    }
    test_request&& form(outbound_bytes url_encoded) && {
        return std::move(form(std::move(url_encoded)));
    }

    // Appends one pair to the request's single Cookie header, building it the
    // way a browser would ("a=1; b=2").
    test_request& cookie(std::string_view name, std::string_view value) & {
        if (!cookies_.empty()) {
            cookies_.append("; ");
        }
        cookies_.append(name);
        cookies_.push_back('=');
        cookies_.append(value);
        return *this;
    }
    test_request&& cookie(std::string_view name, std::string_view value) && {
        return std::move(cookie(name, value));
    }

    test_request& remote(std::string_view address, std::uint16_t port = 0) & {
        remote_address_.assign(address);
        remote_port_ = port;
        return *this;
    }
    test_request&& remote(std::string_view address, std::uint16_t port = 0) && {
        return std::move(remote(address, port));
    }

    test_request& remote(std::string_view address, port_number port) & {
        return remote(address, port.value());
    }
    test_request&& remote(std::string_view address, port_number port) && {
        return std::move(remote(address, port.value()));
    }

    // Pins the steady-clock instant the request arrives at for the app-wide
    // and route rate limits, so a test places requests at exact window
    // offsets instead of sleeping. Unpinned requests read the steady clock,
    // as in a running server. Instants must not run backwards for one client.
    test_request& arrival(std::chrono::steady_clock::time_point instant) & {
        arrival_ = instant;
        return *this;
    }
    test_request&& arrival(std::chrono::steady_clock::time_point instant) && {
        return std::move(arrival(instant));
    }

private:
    friend class test_app;

    test_request(std::string_view method_token, std::string_view target) :
        method_(method_token),
        target_(target) {}

    std::string method_;
    std::string target_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string cookies_;
    std::string body_;
    std::string remote_address_{"127.0.0.1"};
    std::uint16_t remote_port_{0};
    std::optional<std::chrono::steady_clock::time_point> arrival_{};
};

// One in-memory application instance. Construction is cheap; controllers are
// instantiated and the route table finalized on the first dispatch(), so the
// configuration calls below may run in any order before that. Not thread-safe:
// drive one test_app from one thread, like the single-threaded worker it
// stands in for. Middleware, worker state, fallback handlers and the rate
// limit are the dispatch_configuration members application shares, with the
// same registration rules; here they are accepted until the first dispatch().
class test_app final : public dispatch_configuration<test_app> {
public:
    test_app();
    ~test_app();

    test_app(const test_app&) = delete;
    test_app& operator=(const test_app&) = delete;
    test_app(test_app&&) = delete;
    test_app& operator=(test_app&&) = delete;

    // Names the controllers this app serves; each call appends. A controller
    // named twice fails the first request with "duplicate route registration".
    template <typename... controller_types>
        requires(sizeof...(controller_types) > 0)
    test_app& controllers() & RUVIA_LIFETIMEBOUND {
        const std::array<detail::controller_registrar, sizeof...(controller_types)> registrars{detail::controller_registrar_of<controller_types>()...};
        return add_controllers(registrars);
    }
    template <typename... controller_types>
        requires(sizeof...(controller_types) > 0)
    test_app& controllers() && = delete;

    test_app& server(server_config config) & RUVIA_LIFETIMEBOUND;
    test_app& server(server_config) && = delete;
    test_app& trusted_proxies(trusted_proxy_config config) & RUVIA_LIFETIMEBOUND;
    test_app& trusted_proxies(trusted_proxy_config) && = delete;
    test_app& trusted_proxies(std::nullptr_t) & RUVIA_LIFETIMEBOUND;
    test_app& trusted_proxies(std::nullptr_t) && = delete;

    // The private worker event loop and handle, so tests can construct and
    // connect standalone db_client / redis_client / http_client instances on
    // the exact worker that executes test_app requests and lend them below.
    [[nodiscard]] event_loop loop() const& noexcept;
    event_loop loop() const&& = delete;
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

#ifdef RUVIA_ENABLE_REDIS
    test_app& bind_redis(std::string_view alias, const redis_client& client) & RUVIA_LIFETIMEBOUND;
    test_app& bind_redis(std::string_view, const redis_client&) && = delete;
    test_app& bind_redis(std::string_view, const redis_client&&) = delete;
#endif
#ifdef RUVIA_ENABLE_DATABASE
    test_app& bind_db(std::string_view alias, const db_client& client) & RUVIA_LIFETIMEBOUND;
    test_app& bind_db(std::string_view, const db_client&) && = delete;
    test_app& bind_db(std::string_view, const db_client&&) = delete;
#endif
    test_app& bind_http_client(std::string_view alias, const http_client& client) & RUVIA_LIFETIMEBOUND;
    test_app& bind_http_client(std::string_view, const http_client&) && = delete;
    test_app& bind_http_client(std::string_view, const http_client&&) = delete;

    template <typename state_type>
    test_app& bind_worker_state(state_type& state) & RUVIA_LIFETIMEBOUND {
        return add_worker_state(detail::worker_state_definition::borrow(state));
    }
    template <typename state_type>
    test_app& bind_worker_state(state_type&) && = delete;

    // Dispatches one request through the production route table and returns
    // an independently owned response. Request-level failures become the same error
    // responses a server would send. Handlers execute on one real Ruvia worker,
    // including per-worker state semantics.
    [[nodiscard]] dispatch_response dispatch(const test_request& request) &;
    dispatch_response dispatch(const test_request&) && = delete;

private:
    friend class dispatch_configuration<test_app>;

    test_app& add_controllers(std::span<const detail::controller_registrar> registrars) &;
    void edit_configuration(function_ref<void(detail::app_configuration&)> edit);

    struct impl_type;
    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia
