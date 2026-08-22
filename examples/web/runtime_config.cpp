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

    ruvia::MemoryPoolConfig memory;
    memory.requestInitialBufferBytes = 4096;
    const auto httpPort = app.env().get<std::uint16_t>("RUVIA_HTTP_PORT").value_or(app.env().get<std::uint16_t>("RUVIA_PORT").value_or(8087));
    std::optional<ruvia::CompressionConfig> compression;
    if (app.env().get<bool>("RUVIA_GZIP").value_or(false)) {
        compression.emplace();
    }
    std::optional<ruvia::CorsConfig> cors;
    if (app.env().get<bool>("RUVIA_CORS").value_or(false)) {
        auto& config = cors.emplace();
        config.origin = ruvia::CorsOriginPolicy::any();
        config.requestHeaders = ruvia::CorsRequestHeadersPolicy::fixed({"content-type", "authorization"});
        config.maxAge.emplace(std::chrono::seconds(600));
    }

    app.setWorkerCount(app.env().get<std::uint32_t>("RUVIA_WORKERS").value_or(2)).setIdleTimeout(std::chrono::seconds(75)).setConnectionScanInterval(std::chrono::seconds(1)).setRequestHeaderTimeout(std::chrono::seconds(60)).setRequestBodyTimeout(std::chrono::seconds(60)).setWriteTimeout(std::chrono::seconds(60)).setMaxConnectionsPerWorker(10000).setMaxRequestsPerConnection(1000).setBodyLimit(16 * 1024 * 1024).setStreamBodyLimit(std::nullopt).setMaxWebSocketMessageBytes(16 * 1024 * 1024).setMemoryPoolConfig(memory).setCompression(std::move(compression)).setCors(std::move(cors));

    const auto cert = pathOrEmpty(app.env().get("RUVIA_TLS_CERT"));
    const auto key = pathOrEmpty(app.env().get("RUVIA_TLS_KEY"));
    if (!cert.empty() && !key.empty()) {
        std::string password;
        assignIfPresent(password, app.env().get("RUVIA_TLS_PASSWORD"));
        ruvia::TlsConfig tls(ruvia::TlsIdentity::fromFiles({
            .certificateChainFile = cert,
            .privateKeyFile = key,
            .privateKeyPassword = password,
        }));
        const auto verifyFile = pathOrEmpty(app.env().get("RUVIA_TLS_VERIFY_FILE"));
        if (!verifyFile.empty()) {
            tls.setClientCertificatePolicy(ruvia::TlsClientCertificatePolicy::optional(verifyFile));
        }
        const auto httpsPort = app.env().get<std::uint16_t>("RUVIA_HTTPS_PORT").value_or(8443);
        std::vector<ruvia::ListenerConfig> listeners;
        listeners.reserve(2);
        if (app.env().get<bool>("RUVIA_AUTO_HTTPS").value_or(false)) {
            listeners.push_back(ruvia::ListenerConfig::redirectHttpToHttps(ruvia::ListenerId{1}, {
                .address = "0.0.0.0",
                .port = httpPort,
                .target = ruvia::ListenerId{2},
            }));
        } else {
            listeners.push_back(ruvia::ListenerConfig::http(ruvia::ListenerId{1}, {
                .address = "0.0.0.0",
                .port = httpPort,
            }));
        }
        listeners.push_back(ruvia::ListenerConfig::https(ruvia::ListenerId{2}, {
            .address = "0.0.0.0",
            .port = httpsPort,
            .tls = std::move(tls),
        }));
        app.setListeners(std::move(listeners));
    } else {
        app.setListeners({ruvia::ListenerConfig::http(ruvia::ListenerId{1}, {
            .address = "0.0.0.0",
            .port = httpPort,
        })});
    }

    app.setProcessSignalHandlers(ruvia::ProcessSignalHandlerPolicy::kInstall).run();
}
