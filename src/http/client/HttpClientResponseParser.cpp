#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientResponseParser.h"

#include <charconv>
#include <stdexcept>

#include "ruvia/http/HeaderUtils.h"

namespace ruvia::detail {

HttpClientResponseHead parseHttpClientResponseHead(
    std::string_view method,
    std::string_view headerSection,
    FetchResponse& response,
    std::pmr::memory_resource* resource) {
    const auto crlfPos = headerSection.find("\r\n");
    const auto firstLine = crlfPos == std::string_view::npos
        ? headerSection
        : headerSection.substr(0, crlfPos);
    const auto sp1 = firstLine.find(' ');

    int statusCode = 200;
    if (sp1 != std::string_view::npos) {
        const auto sp2 = firstLine.find(' ', sp1 + 1);
        const auto codeStr = firstLine.substr(
            sp1 + 1,
            sp2 == std::string_view::npos ? std::string_view::npos : sp2 - sp1 - 1);
        std::from_chars(codeStr.data(), codeStr.data() + codeStr.size(), statusCode);
    }

    response.statusCode = statusCode;
    HttpClientResponseHead parsed{
        .bodyOffset = headerSection.size() + 4,
        .responseMayHaveBody =
            !httpAsciiEqualsIgnoreCase(method, "HEAD") &&
            (statusCode < 100 || statusCode >= 200) &&
            statusCode != 204 &&
            statusCode != 205 &&
            statusCode != 304};

    auto remaining = crlfPos == std::string_view::npos
        ? std::string_view{}
        : headerSection.substr(crlfPos + 2);
    while (!remaining.empty()) {
        const auto lineEnd = remaining.find("\r\n");
        const auto line = lineEnd == std::string_view::npos
            ? remaining
            : remaining.substr(0, lineEnd);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            auto name = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            if (httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
                const auto [ptr, ec] = std::from_chars(
                    value.data(),
                    value.data() + value.size(),
                    parsed.contentLength);
                if (ec != std::errc{} || ptr != value.data() + value.size()) {
                    throw std::runtime_error("http client: invalid Content-Length header");
                }
                parsed.hasContentLength = true;
            } else if (httpAsciiEqualsIgnoreCase(name, "Connection")) {
                parsed.closeAfterResponse = parsed.closeAfterResponse || httpHasToken(value, "close");
            } else if (httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
                parsed.closeAfterResponse = true;
            }
            if (response.headers.empty()) {
                response.headers.reserve(8);
            }
            response.headers.emplace_back(name, value, resource);
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(lineEnd + 2);
    }

    return parsed;
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
