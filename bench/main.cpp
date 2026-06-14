#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

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

static constexpr std::string_view kPlainBody = "Hello, World!";
static constexpr std::string_view kJsonBody  = "{\"message\":\"Hello, World!\"}";

}  // namespace

class BenchController final : public ruvia::Controller<BenchController> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/plaintext", plaintext);
    RUVIA_GET("/json",      json);
    RUVIA_GET("/echo",      echo);
    RUVIA_ROUTES_END

private:
    // Borrows a static literal — zero copy on the response body path.
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

    // Reads the request body and echoes it back (tests body reading throughput).
    ruvia::Task<ruvia::HttpResponse> echo(ruvia::Context& c) {
        const auto body = co_await c.body();
        ruvia::HttpResponse resp(c.resource());
        resp.setHeader("Content-Type", "text/plain; charset=utf-8");
        resp.setBodyCopy(body);
        co_return resp;
    }
};

int main() {
    const auto* portEnv = std::getenv("RUVIA_PORT");
    const std::uint16_t port = portEnv ? static_cast<std::uint16_t>(std::stoul(portEnv)) : 8088;

    auto& a = ruvia::app();
    a.setListenAddress("0.0.0.0", port)
     .setCompression({.enabled = false})   // raw perf without zlib
     .setIdleTimeout(std::chrono::seconds(60))
     .setHeaderTimeout(std::chrono::seconds(15))
     .setWriteTimeout(std::chrono::seconds(30))
     .setMaxConnectionsPerWorker(100000)
     .setMaxRequestsPerConnection(0);      // unlimited keep-alive pipelining

    a.run();
}
