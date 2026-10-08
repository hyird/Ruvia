// Runtime configuration: dotenv, app-wide middleware via App::use, memory
// pool, timeouts, limits, compression and optional TLS. Typed Env::get<T>()
// returns nullopt only when a variable is absent; malformed values fail fast.
// Run ruvia_example_runtime_config and GET http://127.0.0.1:8087/runtime.
// RUVIA_HTTP_PORT/PORT and RUVIA_WORKERS override the listener and worker count.
// RUVIA_GZIP=true enables negotiated gzip/Brotli/zstd; RUVIA_CORS=true enables
// the CORS policy below. A .env file is optional and preserves existing values.
// For HTTPS, set RUVIA_TLS_CERT/KEY and optionally RUVIA_HTTPS_PORT (8443).
// RUVIA_TLS_VERIFY_FILE supplies a client CA; RUVIA_REQUIRE_CLIENT_CERT=true
// requires mTLS. SNI uses RUVIA_SNI_HOST/CERT/KEY. RUVIA_AUTO_HTTPS=true redirects
// plain HTTP. Never replace certificate verification with an insecure policy.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

#include "environment.h"

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

// Registered app-wide below (App::use): one instance per worker runs before the
// controller and route middlewares of EVERY matched route, in use() order.
// Requests that match no route (404/405) never enter a middleware chain.
class GlobalHeaderMiddleware final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Runtime-Example", "true");
    }
};

class scoped_header_middleware final : public ruvia::Middleware {
public:
    explicit scoped_header_middleware(std::string value)
        : value_(std::move(value)) {}

    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Scoped-Example", value_);
    }

private:
    std::string value_;
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
    const example::environment env(&app.env());
    app.use<GlobalHeaderMiddleware>();
    // Prefix selection is compiled into the route plan. Constructor arguments
    // are owned by the registration and materialized separately on each worker.
    app.useAt<scoped_header_middleware>({.prefix = "/runtime"}, "configured");

    const auto httpPort = app.env()
                              .get<std::uint16_t>("RUVIA_HTTP_PORT")
                              .value_or(env.get<std::uint16_t>("RUVIA_PORT").value_or(8087));
    app.server({
        .worker_count = env.get<std::uint32_t>("RUVIA_WORKERS").value_or(2),
        .process_signal_handlers = ruvia::process_signal_handler_policy::install,
        .idle_timeout = std::chrono::seconds(75),
        .connection_scan_interval = std::chrono::seconds(1),
        .request_header_timeout = std::chrono::seconds(60),
        .request_body_timeout = std::chrono::seconds(60),
        .write_timeout = std::chrono::seconds(60),
        .max_connections_per_worker = 10000,
        .max_requests_per_connection = 1000,
        .max_buffered_body_bytes = 16 * 1024 * 1024,
        .max_web_socket_message_bytes = 16 * 1024 * 1024,
        .memory_pool =
            {
                .requestInitialBufferBytes = 4096,
            },
    });
    if (env.get<bool>("RUVIA_GZIP").value_or(false)) {
        app.compression({});
    }
    if (env.get<bool>("RUVIA_CORS").value_or(false)) {
        app.cors({
            .requestHeaders =
                {
                    .mode = ruvia::CorsRequestHeadersMode::kFixed,
                    .names = {"content-type", "authorization"},
                },
            .maxAge = std::chrono::seconds(600),
        });
    }

    const auto cert = pathOrEmpty(env.get("RUVIA_TLS_CERT"));
    const auto key = pathOrEmpty(env.get("RUVIA_TLS_KEY"));
    if (!cert.empty() && !key.empty()) {
        std::string password;
        assignIfPresent(password, env.get("RUVIA_TLS_PASSWORD"));
        ruvia::ListenConfig listener{
            .address = "0.0.0.0",
            .http = httpPort,
            .https = env.get<std::uint16_t>("RUVIA_HTTPS_PORT").value_or(8443),
            .tls =
                {
                    .certificateChainFile = cert,
                    .privateKeyFile = key,
                    .privateKeyPassword = password,
                },
            .autoHttpsRedirect = env.get<bool>("RUVIA_AUTO_HTTPS").value_or(false),
        };
        const auto verifyFile = pathOrEmpty(env.get("RUVIA_TLS_VERIFY_FILE"));
        if (!verifyFile.empty()) {
            listener.tls.clientCertificates.verifyFile = verifyFile;
            listener.tls.clientCertificates.requirement =
                env.get<bool>("RUVIA_REQUIRE_CLIENT_CERT").value_or(false)
                    ? ruvia::TlsClientCertificateRequirement::kRequired
                    : ruvia::TlsClientCertificateRequirement::kOptional;
        }
        if (const auto host = env.get("RUVIA_SNI_HOST")) {
            // Each SNI entry has its own certificate/key. The default listener
            // certificate remains the fallback for other server names.
            listener.tls.sni.push_back({
                .host = std::string(*host),
                .certificateChainFile = pathOrEmpty(env.get("RUVIA_SNI_CERT")),
                .privateKeyFile = pathOrEmpty(env.get("RUVIA_SNI_KEY")),
            });
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
