#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// A minimal benchmark server designed for wrk / wrk2 pressure testing.
// Disable compression so wrk measures pure framework overhead, not zlib.
//
// Usage:
//   ./ruvia_bench            # listen on 0.0.0.0:8088
//   RUVIA_PORT=9000 ./ruvia_bench
//
//   # in another terminal:
//   wrk -t4 -c256 -d30s http://127.0.0.1:8088/plaintext
//   wrk -t4 -c256 -d30s http://127.0.0.1:8088/json

namespace {

constexpr std::string_view kPlainBody = "Hello, World!";
constexpr std::string_view kJsonBody = "{\"message\":\"Hello, World!\"}";

// Set from RUVIA_FILE at startup; the /file route serves it through the file
// response path (sendfile / kTLS sendfile zero-copy when applicable).
std::string gFilePath;

std::string readEnvironment(std::string_view name) {
#ifdef _WIN32
    const auto key = std::string(name);
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, key.c_str()) != 0 || value == nullptr) {
        return {};
    }

    std::unique_ptr<char, decltype(&std::free)> guard(value, std::free);
    return std::string(value, size > 0 ? size - 1 : 0);
#else
    const auto key = std::string(name);
    const auto* value = std::getenv(key.c_str());
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

}  // namespace

class BenchController final : public ruvia::Controller<BenchController> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/plaintext", plaintext);
    RUVIA_GET("/json", json);
    RUVIA_GET("/echo", echo);
    RUVIA_GET("/file", file);
    RUVIA_ROUTES_END

private:
    // Borrows a static literal; zero copy on the response body path.
    ruvia::Task<ruvia::HttpResponse> plaintext(ruvia::Context& c) {
        ruvia::HttpResponse resp(c.resource());
        resp.setHeader("Content-Type", "text/plain; charset=utf-8");
        resp.setBodyView(kPlainBody);
        co_return resp;
    }

    ruvia::Task<ruvia::HttpResponse> json(ruvia::Context& c) {
        ruvia::HttpResponse resp(c.resource());
        resp.setHeader("Content-Type", "application/json; charset=utf-8");
        resp.setBodyView(kJsonBody);
        co_return resp;
    }

    // Serves a file from disk; exercises the zero-copy file path
    // (sendfile on plain TCP, kTLS sendfile on TLS when the kernel supports it).
    ruvia::Task<ruvia::HttpResponse> file(ruvia::Context& c) {
        co_return c.file(gFilePath, "application/octet-stream");
    }

    // Reads the request body and echoes it back.
    ruvia::Task<ruvia::HttpResponse> echo(ruvia::Context& c) {
        const auto body = co_await c.body();
        ruvia::HttpResponse resp(c.resource());
        resp.setHeader("Content-Type", "text/plain; charset=utf-8");
        resp.setBodyCopy(body);
        co_return resp;
    }
};

int main() {
    const auto portEnv = readEnvironment("RUVIA_PORT");
    const std::uint16_t port = portEnv.empty() ? 8088 : static_cast<std::uint16_t>(std::stoul(portEnv));
    gFilePath = readEnvironment("RUVIA_FILE");

    // Enable TLS when a cert/key pair is supplied, so the same binary can serve
    // either plaintext or HTTPS (for kTLS / sendfile-over-TLS benchmarking).
    const auto tlsCert = readEnvironment("RUVIA_TLS_CERT");
    const auto tlsKey = readEnvironment("RUVIA_TLS_KEY");
    const bool tlsEnabled = !tlsCert.empty() && !tlsKey.empty();

    auto& a = ruvia::app();
    a.setListenAddress("0.0.0.0")
     .setCompression({.enabled = false})
     .setIdleTimeout(std::chrono::seconds(60))
     .setHeaderTimeout(std::chrono::seconds(15))
     .setWriteTimeout(std::chrono::seconds(30))
     .setMaxConnectionsPerWorker(100000)
     .setMaxRequestsPerConnection(0);

    if (tlsEnabled) {
        a.setHttpsListenPort(port)
         .useTls(ruvia::TlsConfig{
             .certificateChainFile = tlsCert,
             .privateKeyFile = tlsKey});
    } else {
        a.setHttpListenPort(port);
    }

    a.run();
}
