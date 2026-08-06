// JWT auth: signing, verification, bearer-token middleware and protected
// routes. Built only with RUVIA_ENABLE_JWT=ON.

#include <chrono>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/auth/Jwt.h"
#include "ruvia/web/Controller.h"

namespace {

constexpr std::string_view kJwtSecret = "replace-this-development-secret";

ruvia::JwtSignOptions signOptions(ruvia::Context& c) {
    ruvia::JwtSignOptions options;
    options.secret.assign(kJwtSecret.data(), kJwtSecret.size());
    options.issuer.assign("ruvia-example");
    options.audience.assign("ruvia-api");
    options.expiresIn = std::chrono::minutes(30);
    options.claims.emplace_back("scope", "example");
    return options;
}

ruvia::JwtVerifyOptions verifyOptions() {
    ruvia::JwtVerifyOptions options;
    options.secret.assign(kJwtSecret.data(), kJwtSecret.size());
    options.issuer.assign("ruvia-example");
    options.audience.assign("ruvia-api");
    options.leeway = std::chrono::seconds(30);
    return options;
}

}  // namespace

class JwtAuthMiddleware final : public ruvia::Middleware<JwtAuthMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        const auto token = ruvia::jwtBearerToken(c.req().header("Authorization").value_or(""));
        if (!token) {
            c.respond(c.error(ruvia::http_status::kUnauthorized, "missing_token", "missing bearer token"));
            co_return;
        }

        try {
            const auto payload = ruvia::jwtVerify(*token, verifyOptions(), c.resource());
            c.header("X-Jwt-Subject", payload.subject());
        } catch (...) {
            c.respond(c.error(ruvia::http_status::kUnauthorized, "invalid_token", "invalid bearer token"));
            co_return;
        }

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
        auto jwt = ruvia::jwtSign(options, c.resource());
        co_return c.text(std::move(jwt));
    }

    ruvia::Task<ruvia::HttpResponse> me(ruvia::Context& c) {
        co_return c.text("authenticated\n");
    }
};

int main() {
    ruvia::app().setListenAddress("0.0.0.0").setListeners({ruvia::ListenerConfig::http(8085)}).setWorkersPerListener(2).setSignalShutdown(true).run();
}
