// JWT auth: signing, verification, bearer-token middleware and protected
// routes. Built only with RUVIA_ENABLE_JWT=ON.

#include <chrono>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/auth/Jwt.h"
#include "ruvia/web/Controller.h"

namespace {

constexpr std::string_view kJwtSecret = "replace-this-development-secret";

ruvia::JwtSignOptions signOptions(ruvia::Context& c) {
    ruvia::JwtSignOptions options;
    options.secret = kJwtSecret;
    options.issuer.assign("ruvia-example");
    options.audience.assign("ruvia-api");
    options.expiresIn = std::chrono::minutes(30);
    options.claims.emplace_back(ruvia::JwtClaimOptions{.name = "scope", .value = "example"});
    options.resource = c.resource();
    return options;
}

ruvia::JwtVerifyOptions verifyOptions(std::string_view token, std::pmr::memory_resource* resource) {
    ruvia::JwtVerifyOptions options;
    options.token = token;
    options.secret = kJwtSecret;
    options.issuer.assign("ruvia-example");
    options.audience.assign("ruvia-api");
    options.leeway = std::chrono::seconds(30);
    options.resource = resource;
    return options;
}

}  // namespace

// What an authenticated caller is, for the routes behind this middleware.
struct AuthenticatedUser final {
    std::string_view subject;
};

class JwtAuthMiddleware final : public ruvia::Middleware<JwtAuthMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        const auto token = ruvia::jwtBearerToken(c.req().header("Authorization").value_or(""));
        if (!token) {
            c.respond(c.error({.status = ruvia::http_status::kUnauthorized,
                .code = "missing_token",
                .message = "missing bearer token"}));
            co_return;
        }

        // Only verification is guarded: next() must stay outside the catch, or a
        // downstream handler's exception would be reported as an invalid token
        // instead of reaching onError.
        std::optional<ruvia::JwtPayload> payload;
        try {
            payload.emplace(ruvia::jwtVerify(verifyOptions(*token, c.resource())));
        } catch (...) {
            c.respond(c.error({.status = ruvia::http_status::kUnauthorized,
                .code = "invalid_token",
                .message = "invalid bearer token"}));
            co_return;
        }

        // The payload and the value built from it stay owned by this coroutine
        // frame, which outlives the next() below -- that is what makes binding
        // by address safe and allocation-free.
        const AuthenticatedUser user{.subject = payload->subject()};
        const auto binding = c.bindRequestState(user);
        co_await next();
    }
};

class AuthController final : public ruvia::Controller<AuthController> {
public:
    RUVIA_CONTROLLER_GROUP("/auth")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/token", token);
    RUVIA_GET("/me", me, JwtAuthMiddleware);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> token(ruvia::Context& c) {
        auto options = signOptions(c);
        options.subject.assign(c.req().query("sub").value_or("example-user"));
        auto jwt = ruvia::jwtSign(options);
        co_return c.text(std::move(jwt));
    }

    // The middleware published the verified identity as request state; the
    // handler reads it back by type, with no out-of-band channel.
    ruvia::Task<ruvia::HttpResponse> me(ruvia::Context& c) {
        const auto& user = c.requestState<AuthenticatedUser>();
        std::pmr::string reply(c.resource());
        reply.append("authenticated as ");
        reply.append(user.subject);
        reply.push_back('\n');
        co_return c.text(std::move(reply));
    }
};

int main() {
    ruvia::app()
        .listen({.address = "0.0.0.0", .http = 8085})
        .server({.workerCount = 2,
            .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall})
        .run();
}
