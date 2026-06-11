#include <chrono>
#include <string_view>

#include "ruvia/app/App.h"
#include "ruvia/auth/Jwt.h"
#include "ruvia/http/Controller.h"

namespace {

constexpr std::string_view kJwtSecret = "replace-this-development-secret";

ruvia::JwtSignOptions signOptions(ruvia::Context& c) {
    ruvia::JwtSignOptions options;
    options.secret.assign(kJwtSecret.data(), kJwtSecret.size());
    options.issuer.assign("ruvia-example");
    options.audience.assign("ruvia-api");
    options.expiresIn = std::chrono::minutes(30);
    options.claims.emplace_back(
        std::pmr::string("scope", c.resource()),
        std::pmr::string("example", c.resource()));
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
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, const ruvia::Next& next) {
        const auto token = ruvia::jwtBearerToken(c.header(ruvia::HttpRequest::KnownHeader::kAuthorization));
        if (!token) {
            co_return c.error(401, "missing_token", "missing bearer token");
        }

        try {
            const auto payload = ruvia::jwtVerify(*token, verifyOptions(), c.resource());
            c.setHeader("X-Jwt-Subject", payload.subject());
        } catch (...) {
            co_return c.error(401, "invalid_token", "invalid bearer token");
        }

        co_return co_await next(c);
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
        options.subject.assign(c.query("sub").toStringView().value_or("example-user"));
        auto jwt = ruvia::jwtSign(options, c.resource());
        co_return c.text(jwt);
    }

    ruvia::Task<ruvia::HttpResponse> me(ruvia::Context& c) {
        co_return c.text("authenticated\n");
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0", 8085)
        .setThreadNum(2)
        .run();
}
