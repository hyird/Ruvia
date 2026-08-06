#include <array>

#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/Http1InterimResponseWriter.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpResponse.h"

int main() {
    const ruvia::HttpInterimResponseHead earlyHints(ruvia::http_status::kEarlyHints);
    if (earlyHints.status() != ruvia::http_status::kEarlyHints || !earlyHints.headers().empty()) {
        return 4;
    }
    std::array<char, 32> interimHeadBuffer{};
    const auto interimResult = ruvia::Http1InterimResponseWriter().prepare(ruvia::HttpInterimResponseHead(ruvia::http_status::kContinue), interimHeadBuffer);
    if (interimResult.prepared() == nullptr || interimResult.prepared()->head() != "HTTP/1.1 100 Continue\r\n\r\n") {
        return 5;
    }
    const auto result = ruvia::Http1RequestParser().parse("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    const auto* parsed = result.parsed();
    if (parsed == nullptr || parsed->request().path() != "/" || !parsed->wireBody().empty()) {
        return 1;
    }

    const auto cache = ruvia::parseCacheControl("max-age=60");
    if (!cache.maxAge.has_value() || *cache.maxAge != 60) {
        return 2;
    }

    // The outbound client is http-owned (framework-free): its public surface must
    // be usable from a ruvia::http-only consumer.
    const auto origin = ruvia::HttpOriginView::https("example.test");
    ruvia::HttpClientRequestView outboundRequest;
    std::array<char, 256> requestHead;
    const auto requestPlan = ruvia::Http1ClientRequestWriter().prepare(origin, outboundRequest, requestHead);
    const auto* preparedRequest = requestPlan.prepared();
    if (preparedRequest == nullptr) {
        return 3;
    }
    ruvia::Http1ClientResponseParser responseParser(*preparedRequest);
    const auto responseResult = responseParser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    const auto* responseHead = responseResult.parsed();
    return origin.host().empty() || origin.port() != 443 || origin.scheme() != ruvia::HttpScheme::kHttps || responseHead == nullptr || responseHead->head().status() != ruvia::http_status::kNoContent || responseHead->plan().withoutContent() == nullptr ? 3 : 0;
}
