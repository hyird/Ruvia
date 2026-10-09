#pragma once

// In-memory testing facade for applications built on Ruvia -- the actix
// test_request / Hono app.request() analog. test_app collects every controller
// linked into the binary (the same CRTP registration path application::run() uses),
// finalizes the production route table, and dispatches TestRequests through
// the real buffered dispatch pipeline: routing, controller/route/global
// middleware, validators, prefix and app-wide not_found/on_error fallbacks,
// url_for, route body limits, route rate limits, and worker state all behave as
// they do in a running server.
// No socket is opened; request() drives a private worker to completion
// synchronously and copies the response out.
//
// Scope: buffered-response routes (including 404/405/501/OPTIONS fallbacks).
// Streaming/SSE/websocket routes and handlers that await worker-bound
// services (db(), redis(), run_blocking()) need a running server; drive those
// through a real loopback server instead. Route deadlines are enforced by the
// private worker's timer.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/http_status.h"
#include "ruvia/web/detail/app/app_configuration.h"
#include "ruvia/web/detail/integration/worker_state.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/error_handlers.h"

namespace ruvia {

class test_app;

// Builder for one in-memory request. Owns every string handed to it, so a
// test_request may be built from temporaries and reused across dispatches.
class test_request final {
public:
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

    [[nodiscard]] static test_request del(std::string_view target) {
        return test_request("DELETE", target);
    }

    [[nodiscard]] static test_request head(std::string_view target) {
        return test_request("HEAD", target);
    }

    [[nodiscard]] static test_request options(std::string_view target) {
        return test_request("OPTIONS", target);
    }

    test_request& header(std::string_view name, std::string_view value) {
        headers_.emplace_back(std::string(name), std::string(value));
        return *this;
    }

    test_request& body(std::string_view bytes_value) {
        body_.assign(bytes_value);
        return *this;
    }

    test_request& body(std::string_view bytes_value, std::string_view content_type_value) {
        body_.assign(bytes_value);
        return header("Content-Type", content_type_value);
    }

    test_request& json(std::string_view json_text) {
        return body(json_text, "application/json");
    }

    test_request& form(std::string_view url_encoded) {
        return body(url_encoded, "application/x-www-form-urlencoded");
    }

    // Appends one pair to the request's single Cookie header, building it the
    // way a browser would ("a=1; b=2").
    test_request& cookie(std::string_view name, std::string_view value) {
        if (!cookies_.empty()) {
            cookies_.append("; ");
        }
        cookies_.append(name);
        cookies_.push_back('=');
        cookies_.append(value);
        return *this;
    }

private:
    friend class test_app;

    test_request(std::string_view method_token, std::string_view target)
        : method_(method_token),
          target_(target) {}

    std::string method_;
    std::string target_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string cookies_;
    std::string body_;
};

// One dispatched response, copied out of the request arena: safe to hold and
// inspect after further requests.
class test_response final {
public:
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view body() const& noexcept {
        return body_;
    }
    std::string_view body() const&& = delete;

    // First header with this name (ASCII case-insensitive). Repeatable fields
    // such as Set-Cookie keep every occurrence in headers().
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    std::optional<std::string_view> header(std::string_view) const&& = delete;

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& headers()
        const& noexcept {
        return headers_;
    }
    const std::vector<std::pair<std::string, std::string>>& headers() const&& = delete;

private:
    friend class test_app;

    explicit test_response(http_status_code status) noexcept
        : status_(status) {}

    http_status_code status_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};

// One in-memory application instance. Construction is cheap; controllers are
// instantiated and the route table finalized on the first request(), so the
// configuration calls below may run in any order before that. Not thread-safe:
// drive one test_app from one thread, like the single-threaded worker it
// stands in for.
class test_app final {
public:
    test_app();
    ~test_app();

    test_app(const test_app&) = delete;
    test_app& operator=(const test_app&) = delete;
    test_app(test_app&&) = delete;
    test_app& operator=(test_app&&) = delete;

    // Uses the same composed dispatch configuration as application, with test_app's
    // first-request lifecycle guard instead of the process lifecycle.
    template <typename middleware_type, typename... args_types>
    test_app& use(args_types&&... args) {
        return use_middleware(
            detail::make_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }

    template <typename middleware_type, typename... args_types>
    test_app& use_at(const middleware_scope_options& options, args_types&&... args) {
        return use_middleware(
            detail::make_scoped_app_middleware<middleware_type>(options, std::forward<args_types>(args)...));
    }

    template <typename state_type, typename factory_type>
    test_app& use_worker_state(factory_type&& factory) {
        return use_worker_state_definition(
            detail::worker_state_definition::make<state_type>(std::forward<factory_type>(factory)));
    }

    template <typename state_type>
    test_app& use_worker_state() {
        return use_worker_state_definition(detail::make_default_worker_state<state_type>());
    }

    test_app& on_error(http_error_handler_type handler);
    test_app& on_not_found(http_not_found_handler_type handler);
    // Prefixes use the same segment and trailing-slash normalization as application;
    // duplicate normalized registrations throw std::invalid_argument rather
    // than allowing production and in-memory tests to choose different
    // handlers by call order.
    test_app& on_error(scoped_error_handler_options options);
    test_app& on_not_found(scoped_not_found_handler_options options);

    // Dispatches one request through the production route table and returns
    // the copied-out response. Request-level failures become the same error
    // responses a server would send. Handlers execute on one real Ruvia worker,
    // including route Deadline cancellation and per-worker state semantics.
    [[nodiscard]] test_response request(const test_request& request);

private:
    test_app& use_middleware(detail::controller_middleware_descriptor descriptor);
    test_app& use_worker_state_definition(detail::worker_state_definition definition);

    struct impl_type;
    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia
