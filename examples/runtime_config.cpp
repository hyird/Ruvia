#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

namespace {

void assignIfPresent(std::pmr::string& target, std::optional<std::string_view> value) {
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

class GlobalHeaderMiddleware final : public ruvia::Middleware<GlobalHeaderMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Runtime-Example", "true");
    }
};

class RuntimeController final : public ruvia::Controller<RuntimeController> {
public:
    RUVIA_CONTROLLER_GROUP("/runtime", GlobalHeaderMiddleware)
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

    ruvia::MemoryPoolConfig memory;
    memory.requestInitialBufferBytes = 4096;
    const auto httpPort = app.env().get<std::uint16_t>("RUVIA_HTTP_PORT")
        .value_or(app.env().get<std::uint16_t>("RUVIA_PORT").value_or(8087));
    std::optional<ruvia::CompressionConfig> compression;
    if (app.env().get<bool>("RUVIA_GZIP").value_or(true)) {
        compression.emplace();
    }
    std::optional<ruvia::CorsConfig> cors;
    if (app.env().get<bool>("RUVIA_CORS").value_or(false)) {
        auto& config = cors.emplace();
        config.origin = ruvia::CorsOriginPolicy::any();
        config.requestHeaders =
            ruvia::CorsRequestHeadersPolicy::fixed("content-type, authorization");
        config.maxAge.emplace(std::chrono::seconds(600));
    }

    app
        .setListenAddress("0.0.0.0")
        .setThreadNum(app.env().get<std::uint32_t>("RUVIA_THREADS").value_or(2))
        .setKeepaliveTimeout(std::chrono::seconds(75))
        .setConnectionScanInterval(std::chrono::seconds(1))
        .setClientHeaderTimeout(std::chrono::seconds(60))
        .setClientBodyTimeout(std::chrono::seconds(60))
        .setSendTimeout(std::chrono::seconds(60))
        .setMaxConnectionsPerWorker(10000)
        .setKeepaliveRequests(1000)
        .setMaxBufferedBodyBytes(16 * 1024 * 1024)
        .setMaxStreamBodyBytes(std::nullopt)
        .setMaxWebSocketMessageBytes(16 * 1024 * 1024)
        .setMemoryPoolConfig(memory)
        .setCompression(std::move(compression))
        .setCors(std::move(cors));

    const auto cert = pathOrEmpty(app.env().get("RUVIA_TLS_CERT"));
    const auto key = pathOrEmpty(app.env().get("RUVIA_TLS_KEY"));
    if (!cert.empty() && !key.empty()) {
        std::pmr::string password;
        assignIfPresent(password, app.env().get("RUVIA_TLS_PASSWORD"));
        ruvia::TlsConfig tls(ruvia::TlsIdentity::fromFiles(
            cert,
            key,
            std::move(password)));
        const auto verifyFile = pathOrEmpty(app.env().get("RUVIA_TLS_VERIFY_FILE"));
        if (!verifyFile.empty()) {
            tls.setClientCertificatePolicy(
                ruvia::TlsClientCertificatePolicy::optional(verifyFile));
        }
        const auto httpsPort =
            app.env().get<std::uint16_t>("RUVIA_HTTPS_PORT").value_or(8443);
        const auto topology = app.env().get<bool>("RUVIA_AUTO_HTTPS").value_or(false)
            ? ruvia::ServerTopology::redirectHttpToHttps(
                  httpPort, httpsPort, std::move(tls))
            : ruvia::ServerTopology::httpAndHttps(
                  httpPort, httpsPort, std::move(tls));
        app.setServerTopology(std::move(topology));
    } else {
        app.setServerTopology(ruvia::ServerTopology::http(httpPort));
    }

    app.run();
}
