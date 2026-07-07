// Hono-style reverse proxy.
//
// Every incoming request is forwarded to a registered upstream HTTP client and the upstream
// response is streamed straight back to the downstream client. The handler is a normal controller
// method that simply returns c.proxy(...) -- the framework streams the response body (HTTP/1.1
// chunked or HTTP/2 DATA), so nothing is buffered.
//
// HTTP client support is part of ruvia-http.

#include <chrono>
#include <utility>

#include "ruvia/app/App.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/Controller.h"

class ReverseProxyController final : public ruvia::Controller<ReverseProxyController> {
public:
    RUVIA_ROUTES_BEGIN
    // app.all('*') in Hono: proxy every method + path to the upstream.
    RUVIA_ALL("/*", proxyAll);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> proxyAll(ruvia::Context& c) {
        // Forward method + origin-form target (path + query) + headers + body to "upstream";
        // return the upstream response, which the framework streams back.
        co_return co_await c.proxy("upstream", c.req().raw().target());
    }
};

int main() {
    ruvia::HttpClientConfig upstream;
    upstream.host = "example.com";  // connect target: a DNS name or IP
    upstream.port = 443;
    upstream.tls = true;
    upstream.http2 = false;  // set true to speak HTTP/2 to the upstream

    // Reverse-proxy TLS knobs (all optional; used only when tls == true):
    //   upstream.hostHeader = "api.internal";              // Host header sent to the upstream vhost
    //   upstream.tlsOptions.sniHost = "api.internal";      // SNI + verified certificate name
    //   upstream.tlsOptions.caFile = "/etc/ssl/internal-ca.pem";          // trust a private CA
    //   upstream.tlsOptions.insecureSkipVerify = true;                    // self-signed (DANGER)
    //   upstream.tlsOptions.certificateChainFile = "/etc/ssl/client.pem"; // mutual TLS
    //   upstream.tlsOptions.privateKeyFile = "/etc/ssl/client.key";

    ruvia::app()
        .setListenAddress("0.0.0.0")
        .setHttpListenPort(8080)
        .setThreadNum(2)
        // Multi-domain HTTPS termination on the front (bind a certificate per host via SNI):
        //   .useTls(defaultTls)
        //   .addTlsCertificate("a.example.com", tlsForA)
        //   .addTlsCertificate("b.example.com", tlsForB)
        .useHttpClient("upstream", std::move(upstream))
        .run();
}
