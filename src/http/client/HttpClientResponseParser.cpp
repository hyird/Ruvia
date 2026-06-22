#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientResponseParser.h"

#include <charconv>
#include <stdexcept>

#include "../HeaderTokenUtils.h"
#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] int parseStatusCode(std::string_view statusLine) {
    const auto sp1 = statusLine.find(' ');
    if (sp1 == std::string_view::npos) {
        throw std::runtime_error("http client: invalid response status line");
    }

    const auto version = statusLine.substr(0, sp1);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        throw std::runtime_error("http client: unsupported response HTTP version");
    }

    if (statusLine.size() < sp1 + 4 || (statusLine.size() > sp1 + 4 && statusLine[sp1 + 4] != ' ')) {
        throw std::runtime_error("http client: invalid response status code");
    }

    int statusCode = 0;
    const auto codeStr = statusLine.substr(sp1 + 1, 3);
    const auto [ptr, ec] = std::from_chars(
        codeStr.data(),
        codeStr.data() + codeStr.size(),
        statusCode);
    if (ec != std::errc{} || ptr != codeStr.data() + codeStr.size() || statusCode < 100 || statusCode > 999) {
        throw std::runtime_error("http client: invalid response status code");
    }

    return statusCode;
}

}  // namespace

HttpClientResponseHead parseHttpClientResponseHead(
    std::string_view method,
    std::string_view headerSection,
    FetchResponse& response,
    std::pmr::memory_resource* resource) {
    const auto crlfPos = headerSection.find("\r\n");
    const auto firstLine = crlfPos == std::string_view::npos
        ? headerSection
        : headerSection.substr(0, crlfPos);
    const int statusCode = parseStatusCode(firstLine);

    response.statusCode = statusCode;
    HttpClientResponseHead parsed{
        .bodyOffset = headerSection.size() + 4,
        .responseMayHaveBody =
            !httpAsciiEqualsIgnoreCase(method, "HEAD") &&
            statusCode >= 200 &&
            statusCode != 204 &&
            statusCode != 205 &&
            statusCode != 304};

    auto remaining = crlfPos == std::string_view::npos
        ? std::string_view{}
        : headerSection.substr(crlfPos + 2);
    std::size_t headerCount = 0;
    while (!remaining.empty()) {
        const auto lineEnd = remaining.find("\r\n");
        const auto line = lineEnd == std::string_view::npos
            ? remaining
            : remaining.substr(0, lineEnd);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            throw std::runtime_error("http client: invalid response header");
        }

        auto name = line.substr(0, colon);
        auto value = httpTrimOws(line.substr(colon + 1));
        if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value)) {
            throw std::runtime_error("http client: invalid response header");
        }
        if (headerCount == kMaxRequestHeaders) {
            throw std::runtime_error("http client: too many response headers");
        }
        ++headerCount;
        if (httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
            std::size_t contentLength = 0;
            const auto [ptr, ec] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                contentLength);
            if (ec != std::errc{} || ptr != value.data() + value.size()) {
                throw std::runtime_error("http client: invalid Content-Length header");
            }
            if (parsed.hasContentLength && parsed.contentLength != contentLength) {
                throw std::runtime_error("http client: conflicting Content-Length header");
            }
            parsed.contentLength = contentLength;
            parsed.hasContentLength = true;
        } else if (httpAsciiEqualsIgnoreCase(name, "Connection")) {
            parsed.closeAfterResponse = parsed.closeAfterResponse || httpHasToken(value, "close");
        } else if (httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
            parsed.hasTransferEncoding = true;
            parsed.closeAfterResponse = true;
        }
        if (response.headers.empty()) {
            response.headers.reserve(8);
        }
        response.headers.emplace_back(name, value, resource);

        if (lineEnd == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(lineEnd + 2);
    }

    return parsed;
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
