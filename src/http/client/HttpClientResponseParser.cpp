#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientResponseParser.h"

#include <charconv>
#include <stdexcept>

#include "../HeaderTokenUtils.h"
#include "../RequestBodyDecoding.h"
#include "../parser/HttpParserSyntax.h"
#include "HttpClientAccess.h"
#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::uint16_t parseStatusCode(std::string_view statusLine) {
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
    if (statusCode == 101) {
        throw std::runtime_error("http client: unsupported Switching Protocols response");
    }

    return static_cast<std::uint16_t>(statusCode);
}

[[nodiscard]] bool isSoleChunkedTransferCoding(std::string_view value) noexcept {
    bool sawItem = false;
    bool chunked = false;
    bool invalid = false;
    httpVisitCommaSeparatedQuotedItems(value, [&](std::string_view item) noexcept {
        if (item.empty()) {
            invalid = true;
            return false;
        }
        if (sawItem) {
            chunked = false;
            return false;
        }
        sawItem = true;
        if (const auto semicolon = item.find(';'); semicolon != std::string_view::npos) {
            if (!isValidHttpChunkExtension(item.substr(semicolon))) {
                invalid = true;
                return false;
            }
            item = httpTrimOws(item.substr(0, semicolon));
        }
        chunked = httpAsciiEqualsIgnoreCase(item, "chunked");
        return true;
    });
    return sawItem && chunked && !invalid;
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
    const auto statusCode = parseStatusCode(firstLine);

    FetchResponseAccess::setStatus(response, statusCode);
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
            if (parsed.hasTransferEncoding) {
                throw std::runtime_error("http client: repeated Transfer-Encoding header");
            }
            parsed.hasTransferEncoding = true;
            // Only a sole "chunked" coding is self-delimiting and decodable here; any
            // other coding (gzip, or "gzip, chunked", ...) is treated as unsupported.
            parsed.isChunked = isSoleChunkedTransferCoding(value);
        } else if (httpAsciiEqualsIgnoreCase(name, "Content-Encoding")) {
            if (parsed.hasContentEncoding) {
                // A second Content-Encoding header is a coding list we do not decode.
                parsed.contentCoding = HttpContentCoding::kNone;
            } else {
                parsed.hasContentEncoding = true;
                // requestContentCoding honors only a single known token; identity, a list,
                // or an unknown coding yields kNone (body delivered as received).
                parsed.contentCoding = requestContentCoding(value);
            }
        }
        if (FetchResponseAccess::headers(response).empty()) {
            FetchResponseAccess::headers(response).reserve(8);
        }
        FetchResponseAccess::headers(response).emplace_back(
            FetchResponseHeaderAccess::make(name, value, resource));

        if (lineEnd == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(lineEnd + 2);
    }

    // Transfer-Encoding only frames a body; a bodiless response (HEAD, 204, 304, 1xx) has
    // no framing to resolve, so leave those interoperable rather than rejecting them.
    if (parsed.responseMayHaveBody && parsed.hasTransferEncoding) {
        // RFC 7230 §3.3.3: Transfer-Encoding overrides Content-Length, and a message
        // carrying both is a framing ambiguity (a request-smuggling vector) — reject it.
        if (parsed.hasContentLength) {
            throw std::runtime_error(
                "http client: response has both Content-Length and Transfer-Encoding");
        }
        // An undecodable transfer coding leaves the body delimited only by connection close.
        if (!parsed.isChunked) {
            parsed.closeAfterResponse = true;
        }
    }

    return parsed;
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
