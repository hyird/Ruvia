// Runtime configuration: dotenv, app-wide middleware via App::use, memory
// pool, timeouts, limits, compression and optional TLS. Typed Env::get<T>()
// returns nullopt only when a variable is absent; malformed values fail fast.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

namespace {

void assignIfPresent(std::string& target, std::optional<std::string_view> value) {
    if (value) {
        target.assign(value->data(), value->size());
    }
}

std::filesystem::path pathOrEmpty(std::optional<std::string_view> value) {
    if (!value) {
        return {};
    }
    return std::filesystem::path(std::string_view(*value));
}

}  // namespace

// Registered app-wide below (App::use): one shared instance runs before the
// controller and route middlewares of EVERY matched route, in use() order.
// Requests that match no route (404/405) never enter a middleware chain.
class GlobalHeaderMiddleware final : public ruvia::Middleware<GlobalHeaderMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Runtime-Example", "true");
    }
};

class RuntimeController final : public ruvia::Controller<RuntimeController> {
public:
    RUVIA_CONTROLLER_GROUP("/runtime")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", runtime);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> runtime(ruvia::Context& c) {
        co_return c.text("runtime configured\n");
    }
};

int main() {
    auto& app = ruvia::app();
    app.loadDotenv();
    app.use<GlobalHeaderMiddleware>();

    const auto httpPort = app.env()
                              .get<std::uint16_t>("RUVIA_HTTP_PORT")
                              .value_or(app.env().get<std::uint16_t>("RUVIA_PORT").value_or(8087));
    app.server({
        .workerCount = app.env().get<std::uint32_t>("RUVIA_WORKERS").value_or(2),
        .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall,
        .idleTimeout = std::chrono::seconds(75),
        .connectionScanInterval = std::chrono::seconds(1),
        .requestHeaderTimeout = std::chrono::seconds(60),
        .requestBodyTimeout = std::chrono::seconds(60),
        .writeTimeout = std::chrono::seconds(60),
        .maxConnectionsPerWorker = 10000,
        .maxRequestsPerConnection = 1000,
        .maxBufferedBodyBytes = 16 * 1024 * 1024,
        .maxWebSocketMessageBytes = 16 * 1024 * 1024,
        .memoryPool =
            {
                .requestInitialBufferBytes = 4096,
            },
    });
    if (app.env().get<bool>("RUVIA_GZIP").value_or(false)) {
        app.compression({});
    }
    if (app.env().get<bool>("RUVIA_CORS").value_or(false)) {
        app.cors({
            .requestHeaders =
                {
                    .mode = ruvia::CorsRequestHeadersMode::kFixed,
                    .names = {"content-type", "authorization"},
                },
            .maxAge = std::chrono::seconds(600),
        });
    }

    const auto cert = pathOrEmpty(app.env().get("RUVIA_TLS_CERT"));
    const auto key = pathOrEmpty(app.env().get("RUVIA_TLS_KEY"));
    if (!cert.empty() && !key.empty()) {
        std::string password;
        assignIfPresent(password, app.env().get("RUVIA_TLS_PASSWORD"));
        ruvia::ListenConfig listener{
            .address = "0.0.0.0",
            .http = httpPort,
            .https = app.env().get<std::uint16_t>("RUVIA_HTTPS_PORT").value_or(8443),
            .tls =
                {
                    .certificateChainFile = cert,
                    .privateKeyFile = key,
                    .privateKeyPassword = password,
                },
            .autoHttpsRedirect = app.env().get<bool>("RUVIA_AUTO_HTTPS").value_or(false),
        };
        const auto verifyFile = pathOrEmpty(app.env().get("RUVIA_TLS_VERIFY_FILE"));
        if (!verifyFile.empty()) {
            listener.tls.clientCertificates.verifyFile = verifyFile;
        }
        app.listen(std::move(listener));
    } else {
        app.listen({
            .address = "0.0.0.0",
            .http = httpPort,
        });
    }

    app.run();
}
