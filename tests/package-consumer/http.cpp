#include <ruvia/http/HttpClient.h>
#include <ruvia/http/HttpParser.h>
#include <ruvia/http/HttpResponse.h>
#include <ruvia/http/detail/AsciiCase.h>

int main() {
    const auto parsed = ruvia::HttpParser().parse("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    return parsed.status() == ruvia::HttpParseStatus::kComplete ? 0 : 1;
}
