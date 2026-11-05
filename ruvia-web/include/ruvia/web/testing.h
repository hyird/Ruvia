#pragma once

// In-memory testing facade for applications built on Ruvia -- the actix
// test_request / Hono app.request() analog. test_app collects every controller
// linked into the binary (the same macro registration path application::run() uses),
// finalizes the production route table, and dispatches TestRequests through
// the real buffered request chain of a server with default options: request
// admission (body ceilings, rate limits), routing,
// controller/route/global middleware, validators, prefix and app-wide
// not_found/on_error fallbacks, url_for, worker state, and the response tail
// (CORS, content-coding and error recovery) all behave as they do in a
// running server. The private worker is brought up and retired by the same
// worker capability chain a server worker uses.
// No socket is opened; request() drives that worker to completion
// synchronously and copies the response out.
//
// Scope: buffered-response routes (including 404/405/501/OPTIONS fallbacks).
// Streaming/SSE/websocket routes and handlers that await worker-bound
// services (db(), redis(), run_blocking()) need a running server; drive those
// through a real loopback server instead.

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/app/app_configuration.h"
#include "ruvia/web/detail/integration/worker_state.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/error.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/server_config.h"
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

    [[nodiscard]] static test_request delete_request(std::string_view target) {
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

    template <typename t_type>
        requires detail::is_model_json_value<t_type>
    test_request& json(const t_type& value) {
        const auto serialized = to_json(value);
        return json(std::string_view(serialized));
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

    test_request& remote(std::string_view address, std::uint16_t port = 0) {
        remote_address_.assign(address);
        remote_port_ = port;
        return *this;
    }

    // Pins the steady-clock instant the request arrives at for the app-wide
    // and route rate limits, so a test places requests at exact window
    // offsets instead of sleeping. Unpinned requests read the steady clock,
    // as in a running server. Instants must not run backwards for one client.
    test_request& arrival(std::chrono::steady_clock::time_point instant) {
        arrival_ = instant;
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
    std::string remote_address_{"127.0.0.1"};
    std::uint16_t remote_port_{0};
    std::optional<std::chrono::steady_clock::time_point> arrival_{};
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
    template <middleware middleware_type, typename... args_types>
    test_app& use(args_types&&... args) {
        return use_middleware(
            detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }

    template <middleware middleware_type, typename... args_types>
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

    test_app& server(server_config config);
    test_app& compression(compression_config config);
    test_app& compression(std::nullptr_t);
    test_app& cors(cors_config config);
    test_app& cors(std::nullptr_t);
    test_app& rate_limit(rate_limit_config config);
    test_app& rate_limit(std::nullptr_t);
    test_app& trusted_proxies(trusted_proxy_config config);
    test_app& trusted_proxies(std::nullptr_t);

    // Same registration rules as application: a second app-wide handler, a
    // "/" prefix (here and in use_at) and duplicate normalized prefixes throw
    // std::invalid_argument rather than allowing production and in-memory
    // tests to choose different handlers by call order.
    test_app& on_error(http_error_handler handler);
    test_app& on_not_found(http_not_found_handler handler);
    test_app& on_error(std::nullptr_t);
    test_app& on_not_found(std::nullptr_t);
    test_app& on_error(scoped_error_handler_options options);
    test_app& on_not_found(scoped_not_found_handler_options options);

    // Dispatches one request through the production route table and returns
    // the copied-out response. Request-level failures become the same error
    // responses a server would send. Handlers execute on one real Ruvia worker,
    // including per-worker state semantics.
    [[nodiscard]] dispatch_response request(const test_request& request);

private:
    test_app& use_middleware(detail::owned_middleware_descriptor descriptor);
    test_app& use_worker_state_definition(detail::worker_state_definition definition);

    struct impl_type;
    std::unique_ptr<impl_type, pmr_object_deleter<impl_type>> impl_;
};

}  // namespace ruvia
