#pragma once

// In-memory testing facade for applications built on Ruvia -- the actix
// TestRequest / Hono app.request() analog. TestApp collects every controller
// linked into the binary (the same CRTP registration path App::run() uses),
// finalizes the production route table, and dispatches TestRequests through
// the real buffered dispatch pipeline: routing, controller/route/global
// middleware, validators, prefix and app-wide notFound/onError fallbacks,
// urlFor, route body limits, route rate limits, and worker state all behave as
// they do in a running server.
// No socket is opened and no worker thread is started; request() runs the
// handler coroutine to completion synchronously and copies the response out.
//
// Scope: buffered-response routes (including 404/405/501/OPTIONS fallbacks).
// Streaming/SSE/WebSocket routes and handlers that await worker-bound
// services (timers, db(), redis(), runBlocking()) need a running server; drive
// those through a real loopback server instead. A route Deadline is rejected
// with std::logic_error before dispatch because this facade has no worker timer
// and must not silently turn a bounded production route into an unbounded test.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/HttpStatus.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/app/AppConfiguration.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"

namespace ruvia {

class TestApp;

// Builder for one in-memory request. Owns every string handed to it, so a
// TestRequest may be built from temporaries and reused across dispatches.
class TestRequest final {
public:
    [[nodiscard]] static TestRequest method(std::string_view methodToken, std::string_view target) {
        return TestRequest(methodToken, target);
    }

    [[nodiscard]] static TestRequest get(std::string_view target) {
        return TestRequest("GET", target);
    }

    [[nodiscard]] static TestRequest post(std::string_view target) {
        return TestRequest("POST", target);
    }

    [[nodiscard]] static TestRequest put(std::string_view target) {
        return TestRequest("PUT", target);
    }

    [[nodiscard]] static TestRequest patch(std::string_view target) {
        return TestRequest("PATCH", target);
    }

    [[nodiscard]] static TestRequest del(std::string_view target) {
        return TestRequest("DELETE", target);
    }

    [[nodiscard]] static TestRequest head(std::string_view target) {
        return TestRequest("HEAD", target);
    }

    [[nodiscard]] static TestRequest options(std::string_view target) {
        return TestRequest("OPTIONS", target);
    }

    TestRequest& header(std::string_view name, std::string_view value) {
        headers_.emplace_back(std::string(name), std::string(value));
        return *this;
    }

    TestRequest& body(std::string_view bytes) {
        body_.assign(bytes);
        return *this;
    }

    TestRequest& body(std::string_view bytes, std::string_view contentType) {
        body_.assign(bytes);
        return header("Content-Type", contentType);
    }

    TestRequest& json(std::string_view jsonText) {
        return body(jsonText, "application/json");
    }

    TestRequest& form(std::string_view urlEncoded) {
        return body(urlEncoded, "application/x-www-form-urlencoded");
    }

    // Appends one pair to the request's single Cookie header, building it the
    // way a browser would ("a=1; b=2").
    TestRequest& cookie(std::string_view name, std::string_view value) {
        if (!cookies_.empty()) {
            cookies_.append("; ");
        }
        cookies_.append(name);
        cookies_.push_back('=');
        cookies_.append(value);
        return *this;
    }

private:
    friend class TestApp;

    TestRequest(std::string_view methodToken, std::string_view target)
        : method_(methodToken),
          target_(target) {}

    std::string method_;
    std::string target_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string cookies_;
    std::string body_;
};

// One dispatched response, copied out of the request arena: safe to hold and
// inspect after further requests.
class TestResponse final {
public:
    [[nodiscard]] HttpStatusCode status() const noexcept {
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

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& headers() const& noexcept {
        return headers_;
    }
    const std::vector<std::pair<std::string, std::string>>& headers() const&& = delete;

private:
    friend class TestApp;

    explicit TestResponse(HttpStatusCode status) noexcept
        : status_(status) {}

    HttpStatusCode status_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};

// One in-memory application instance. Construction is cheap; controllers are
// instantiated and the route table finalized on the first request(), so the
// configuration calls below may run in any order before that. Not thread-safe:
// drive one TestApp from one thread, like the single-threaded worker it
// stands in for.
class TestApp final : public detail::AppConfiguration<TestApp> {
public:
    TestApp();
    ~TestApp();

    TestApp(const TestApp&) = delete;
    TestApp& operator=(const TestApp&) = delete;
    TestApp(TestApp&&) = delete;
    TestApp& operator=(TestApp&&) = delete;

    // The App configuration knobs that change dispatch behavior, with the
    // same semantics as their App counterparts; use<>() and useWorkerState<>()
    // come from the shared configuration base.
    TestApp& onError(HttpErrorHandler handler);
    TestApp& onNotFound(HttpNotFoundHandler handler);
    // Prefixes use the same segment and trailing-slash normalization as App;
    // duplicate normalized registrations throw std::invalid_argument rather
    // than allowing production and in-memory tests to choose different
    // handlers by call order.
    TestApp& onError(ScopedErrorHandlerOptions options);
    TestApp& onNotFound(ScopedNotFoundHandlerOptions options);

    // Dispatches one request through the production route table and returns
    // the copied-out response. Request-level failures become the same error
    // responses a server would send. Throws std::logic_error when the selected
    // route declares a Deadline, which cannot be simulated without a worker.
    [[nodiscard]] TestResponse request(const TestRequest& request);

private:
    friend class detail::AppConfiguration<TestApp>;

    TestApp& useMiddleware(detail::ControllerMiddlewareDescriptor descriptor);
    TestApp& useWorkerStateDefinition(detail::WorkerStateDefinition definition);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
