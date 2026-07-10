#include <ruvia/http/HttpClient.h>
#include <ruvia/http/HttpParser.h>
#include <ruvia/http/HttpProtocolError.h>
#include <ruvia/http/HttpResponse.h>
#include <ruvia/http/MultipartParser.h>
#include <ruvia/http/detail/AsciiCase.h>
#include <ruvia/http/detail/HttpParserInternal.h>
#include <ruvia/http/detail/http1/Http1ServerSemantics.h>
#include <ruvia/http/detail/server/HttpResponseWritePlan.h>

int main() {
    const ruvia::HttpProtocolError error(400, "bad request");
    if (error.status() != 400) {
        return 2;
    }
    const auto multipart = ruvia::parseMultipartBody("--x--\r\n", "x");
    if (!multipart.empty()) {
        return 3;
    }
    const auto parsed = ruvia::HttpParser().parse("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    if (parsed.status() != ruvia::HttpParseStatus::kComplete) {
        return 1;
    }

    ruvia::detail::HttpServerParser serverParser;
    const auto streamPlan = ruvia::detail::http1PlanResponseStream(
        serverParser.parse("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n"),
        /*closeForServerPolicy=*/false);
    if (streamPlan.framing() != ruvia::detail::ResponseStreamFraming::kHttp1Chunked ||
        !streamPlan.requestCanPersist() || streamPlan.connectionWillClose()) {
        return 4;
    }

    ruvia::HttpResponse response;
    response.setBodyCopy("body");
    const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(
        ruvia::HttpMethod::kHead, response);
    return writePlan.bodySuppressed() && !writePlan.sendBody() &&
            writePlan.contentLength() == 4
        ? 0
        : 5;
}
