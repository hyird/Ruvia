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
    // be usable from a ruvia::http-only consumer.
    ruvia::HttpOrigin origin;
    origin.host = "example.test";
    origin.port = 443;
    origin.tls = true;
    return origin.host.empty() || origin.port == 0 || !origin.tls ? 3 : 0;
}
