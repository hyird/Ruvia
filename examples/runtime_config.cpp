#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

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
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& c, const ruvia::Next& next) {
        auto response = co_await next(c);
        response.setHeader("X-Runtime-Example", "true");
        co_return response;
    }
};

class RuntimeController final : public ruvia::Controller<RuntimeController> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/runtime", runtime);
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

    app
        .setListenAddress("0.0.0.0", app.env().get<std::uint16_t>("RUVIA_PORT").value_or(8087))
        .setThreadNum(app.env().get<std::uint32_t>("RUVIA_THREADS").value_or(2))
        .setIdleTimeout(std::chrono::seconds(60))
        .setConnectionScanInterval(std::chrono::seconds(1))
        .setHeaderTimeout(std::chrono::seconds(15))
        .setBodyTimeout(std::chrono::seconds(30))
        .setWriteTimeout(std::chrono::seconds(30))
        .setMaxConnectionsPerWorker(10000)
        .setMaxRequestsPerConnection(1000)
        .setMaxBufferedBodyBytes(16 * 1024 * 1024)
        .setMaxStreamBodyBytes(0)
        .setMaxWebSocketMessageBytes(16 * 1024 * 1024)
        .setMemoryPoolConfig(memory)
        .setCompression(ruvia::CompressionConfig{
            .enabled = app.env().get<bool>("RUVIA_GZIP").value_or(true),
            .minBytes = 1024,
        })
        .setCors(ruvia::CorsConfig{
            .enabled = app.env().get<bool>("RUVIA_CORS").value_or(false),
            .allowOrigin = "*",
            .allowHeaders = "content-type, authorization",
            .maxAge = std::chrono::seconds(600),
        })
        .use<GlobalHeaderMiddleware>();

    const auto cert = pathOrEmpty(app.env().get("RUVIA_TLS_CERT"));
    const auto key = pathOrEmpty(app.env().get("RUVIA_TLS_KEY"));
    if (!cert.empty() && !key.empty()) {
        ruvia::TlsConfig tls;
        tls.certificateChainFile = cert;
        tls.privateKeyFile = key;
        assignIfPresent(tls.privateKeyPassword, app.env().get("RUVIA_TLS_PASSWORD"));
        tls.verifyFile = pathOrEmpty(app.env().get("RUVIA_TLS_VERIFY_FILE"));
        app.useTls(std::move(tls));
    }

    app.run();
}
