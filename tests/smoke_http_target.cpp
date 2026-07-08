#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/http/HttpResponse.h"

int main() {
    const auto parsed = ruvia::HttpParser().parse("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    if (parsed.status() != ruvia::HttpParseStatus::kComplete) {
        return 1;
    }

    const auto cache = ruvia::parseCacheControl("max-age=60");
    if (!cache.maxAge.has_value() || *cache.maxAge != 60) {
        return 2;
    }

    // The outbound client is http-owned (framework-free): its public surface must
    // be usable from a ruvia::http-only consumer such as this one (or ruvia::edge).
    ruvia::HttpClientConfig config;
    config.host = "example.test";
    config.port = 443;
    config.tls = true;
    return config.host.empty() || config.port == 0 || !config.tls ? 3 : 0;
}
