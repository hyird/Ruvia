#include "ruvia/http/HttpCache.h"
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

    // NOTE: the outbound client's public API (ruvia/http/HttpClient.h) is installed
    // by ruvia::web (its runtime is web I/O policy), so it deliberately does NOT
    // appear in this http-standalone proof.
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(204);
    return response.status() == 204 ? 0 : 3;
}
