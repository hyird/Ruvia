// JWT auth: signing, verification, bearer-token middleware and protected
// routes. Built only with RUVIA_ENABLE_JWT=ON.
// Run ruvia_example_auth_jwt on port 8085. POST /auth/token to issue a demo
// token; send it as Authorization: Bearer <token> to GET /auth/me.
// The demo issuer grants a fixed identity; replace it with real authentication.

#include <chrono>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/auth/jwt.h"
#include "ruvia/web/controller.h"

namespace {

// Demonstration only. Production must load an independently generated random
// key from protected configuration, never reuse this public example key.
constexpr std::string_view jwt_secret =
    "development-only-not-for-production-0123456789abcdef0123456789abcdef";

ruvia::jwt_sign_options sign_options(ruvia::context& c) {
    ruvia::jwt_sign_options options;
    options.secret_ = jwt_secret;
    options.issuer_.assign("ruvia-example");
    options.audience_.assign("ruvia-api");
    options.expires_in_ = std::chrono::minutes(30);
    options.claims_.emplace_back(ruvia::jwt_claim_options{.name_ = "scope", .value_ = "example"});
    options.resource_ = c.arena();
    return options;
}

ruvia::jwt_verify_options verify_options(std::string_view token, std::pmr::memory_resource* resource) {
    ruvia::jwt_verify_options options;
    options.token_ = token;
    options.secret_ = jwt_secret;
    options.issuer_.assign("ruvia-example");
    options.audience_.assign("ruvia-api");
    options.leeway_ = std::chrono::seconds(30);
    options.resource_ = resource;
    return options;
}

}  // namespace

// What an authenticated caller is, for the routes behind this middleware.
struct authenticated_user final {
    std::string_view subject_;
};

class jwt_auth_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        const auto token = ruvia::jwt_bearer_token(c.req().header("Authorization").value_or(""));
        if (!token) {
            c.respond(c.error({.status_ = ruvia::http_status::unauthorized,
                .code_ = "missing_token",
                .message_ = "missing bearer token"}));
            co_return;
        }

        // Only verification is guarded: next() must stay outside the catch, or a
        // downstream handler's exception would be reported as an invalid token
        // instead of reaching on_error.
        std::optional<ruvia::jwt_payload> payload;
        try {
            payload.emplace(ruvia::jwt_verify(verify_options(*token, c.arena())));
        } catch (...) {
            c.respond(c.error({.status_ = ruvia::http_status::unauthorized,
                .code_ = "invalid_token",
                .message_ = "invalid bearer token"}));
            co_return;
        }

        // The payload and the value built from it stay owned by this coroutine
        // frame, which outlives the next() below -- that is what makes binding
        // by address safe and allocation-free.
        const authenticated_user user_value{.subject_ = payload->subject()};
        const auto binding = c.bind_request_state(user_value);
        co_await next_value();
    }
};

class auth_controller final : public ruvia::controller<auth_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/auth")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/token", token);
    RUVIA_GET("/me", me, jwt_auth_middleware);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> token(ruvia::context& c) {
        auto options = sign_options(c);
        options.subject_.assign(c.req().query("sub").value_or("example-user"));
        auto jwt = ruvia::jwt_sign(options);
        co_return c.text(std::move(jwt));
    }

    // The middleware published the verified identity as request state; the
    // handler reads it back by type, with no out-of-band channel.
    ruvia::task<ruvia::http_response> me(ruvia::context& c) {
        const auto& user_value = c.request_state<authenticated_user>();
        std::pmr::string reply(c.arena());
        reply.append("authenticated as ");
        reply.append(user_value.subject_);
        reply.push_back('\n');
        co_return c.text(std::move(reply));
    }
};

int main() {
    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8085})
        .server({.worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .run();
}
