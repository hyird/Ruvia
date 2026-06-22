#include "HttpHeaderBlockParser.h"

#include "HttpRequestTarget.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <system_error>

namespace ruvia::detail {
namespace {

[[nodiscard]] bool parseContentLength(std::string_view value, std::size_t& contentLength) noexcept {
    value = httpTrimOws(value);
    if (value.empty()) {
        return false;
    }

    std::size_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }

    contentLength = parsed;
    return true;
}

enum class TransferEncodingParse {
    kOk,
    kMalformed,
    kUnsupported
};

[[nodiscard]] TransferEncodingParse parseTransferEncoding(
    std::string_view value,
    ParsedRequestHeaderBlock& block) noexcept {
    value = httpTrimOws(value);
    for (;;) {
        const auto comma = value.find(',');
        auto token = httpTrimOws(comma == std::string_view::npos ? value : value.substr(0, comma));
        if (const auto semicolon = token.find(';'); semicolon != std::string_view::npos) {
            if (!isValidHttpChunkExtension(token.substr(semicolon))) {
                return TransferEncodingParse::kMalformed;
            }
            token = httpTrimOws(token.substr(0, semicolon));
        }
        if (token.empty()) {
            return TransferEncodingParse::kMalformed;
        }

        if (block.sawChunked) {
            return TransferEncodingParse::kMalformed;
        }

        if (httpAsciiEqualsIgnoreCase(token, "chunked")) {
            block.sawChunked = true;
            block.sawTransferEncoding = true;
            if (comma != std::string_view::npos) {
                return TransferEncodingParse::kMalformed;
            }
        } else if (httpAsciiEqualsIgnoreCase(token, "gzip") ||
                   httpAsciiEqualsIgnoreCase(token, "x-gzip")) {
            if (block.transferCodings.count == kMaxTransferCodings) {
                return TransferEncodingParse::kUnsupported;
            }
            block.sawTransferEncoding = true;
            block.transferCodings.values[block.transferCodings.count++] = HttpTransferCoding::kGzip;
        } else if (httpAsciiEqualsIgnoreCase(token, "deflate")) {
            if (block.transferCodings.count == kMaxTransferCodings) {
                return TransferEncodingParse::kUnsupported;
            }
            block.sawTransferEncoding = true;
            block.transferCodings.values[block.transferCodings.count++] = HttpTransferCoding::kDeflate;
        } else {
            return TransferEncodingParse::kUnsupported;
        }
        if (comma == std::string_view::npos) {
            return TransferEncodingParse::kOk;
        }
        value.remove_prefix(comma + 1);
    }
}

[[nodiscard]] HttpHeaderSlice makeSlice(std::size_t offset, std::size_t length) noexcept {
    return HttpHeaderSlice{
        .offset = static_cast<std::uint32_t>(offset),
        .length = static_cast<std::uint32_t>(length)};
}

[[nodiscard]] std::size_t trimRightOws(std::string_view buffer, std::size_t begin, std::size_t end) noexcept {
    while (end > begin && (buffer[end - 1] == ' ' || buffer[end - 1] == '\t')) {
        --end;
    }
    return end;
}

}  // namespace

std::size_t findHttpHeaderEnd(std::string_view buffer, std::size_t searchOffset) noexcept {
    const auto limit = std::min(buffer.size(), kMaxHttpHeaderBytes);
    if (limit < 4) {
        return std::string_view::npos;
    }

    auto cursor = searchOffset >= limit ? limit : std::max<std::size_t>(3, searchOffset + 3);
    while (cursor < limit) {
        const auto* hit = static_cast<const char*>(
            std::memchr(buffer.data() + cursor, '\n', limit - cursor));
        if (hit == nullptr) {
            return std::string_view::npos;
        }
        const auto i = static_cast<std::size_t>(hit - buffer.data());
        if (buffer[i - 1] == '\r' && buffer[i - 2] == '\n' && buffer[i - 3] == '\r') {
            return i + 1;
        }
        cursor = i + 1;
    }

    return std::string_view::npos;
}

HttpParseError parseHttpHeaderBlock(
    std::string_view buffer,
    std::size_t headerBytes,
    ParsedRequestHeaderBlock& block) noexcept {
    const auto headersEnd = headerBytes - 2;
    std::size_t cursor = 0;

    const auto methodStart = cursor;
    while (cursor < headersEnd && isHttpTokenChar(static_cast<unsigned char>(buffer[cursor]))) {
        ++cursor;
    }
    if (cursor == methodStart || cursor >= headersEnd || buffer[cursor] != ' ') {
        return HttpParseError::kInvalidRequestLine;
    }
    block.method = makeSlice(methodStart, cursor - methodStart);
    ++cursor;

    const auto targetStart = cursor;
    while (cursor < headersEnd && buffer[cursor] != ' ') {
        if (buffer[cursor] == '\r' || buffer[cursor] == '\n') {
            return HttpParseError::kInvalidRequestLine;
        }
        ++cursor;
    }
    if (cursor == targetStart || cursor >= headersEnd) {
        return HttpParseError::kInvalidRequestLine;
    }
    block.target = makeSlice(targetStart, cursor - targetStart);
    ++cursor;

    const auto versionStart = cursor;
    while (cursor < headersEnd && buffer[cursor] != '\r') {
        if (buffer[cursor] == '\n' || buffer[cursor] == ' ') {
            return HttpParseError::kInvalidRequestLine;
        }
        ++cursor;
    }
    if (cursor == versionStart || cursor + 1 >= headersEnd || buffer[cursor + 1] != '\n') {
        return HttpParseError::kInvalidRequestLine;
    }
    block.version = makeSlice(versionStart, cursor - versionStart);
    cursor += 2;

    while (cursor < headersEnd) {
        if (block.headerCount == kMaxRequestHeaders) {
            return HttpParseError::kTooManyHeaders;
        }

        const auto nameStart = cursor;
        while (cursor < headersEnd && isHttpTokenChar(static_cast<unsigned char>(buffer[cursor]))) {
            ++cursor;
        }
        if (cursor == nameStart || cursor >= headersEnd || buffer[cursor] != ':') {
            return HttpParseError::kInvalidHeader;
        }
        const auto nameEnd = cursor;
        ++cursor;

        while (cursor < headersEnd && (buffer[cursor] == ' ' || buffer[cursor] == '\t')) {
            ++cursor;
        }
        const auto valueStart = cursor;
        while (cursor < headersEnd && isHttpFieldValueChar(static_cast<unsigned char>(buffer[cursor]))) {
            ++cursor;
        }
        if (cursor + 1 >= headersEnd || buffer[cursor] != '\r' || buffer[cursor + 1] != '\n') {
            return HttpParseError::kInvalidHeader;
        }
        const auto valueEnd = trimRightOws(buffer, valueStart, cursor);

        const auto name = buffer.substr(nameStart, nameEnd - nameStart);
        const auto value = buffer.substr(valueStart, valueEnd - valueStart);

        const auto kind = classifyRequestHeader(name);
        switch (kind) {
            case RequestHeaderKind::kHost:
                if (block.flags.hasHost || !isValidHostHeader(value)) {
                    return HttpParseError::kInvalidHost;
                }
                block.flags.hasHost = true;
                break;
            case RequestHeaderKind::kContentLength: {
                std::size_t parsedContentLength = 0;
                if (!parseContentLength(value, parsedContentLength)) {
                    return HttpParseError::kInvalidContentLength;
                }
                if (block.sawContentLength && parsedContentLength != block.contentLength) {
                    return HttpParseError::kConflictingContentLength;
                }
                block.sawContentLength = true;
                block.contentLength = parsedContentLength;
                break;
            }
            case RequestHeaderKind::kTransferEncoding: {
                switch (parseTransferEncoding(value, block)) {
                    case TransferEncodingParse::kOk:
                        break;
                    case TransferEncodingParse::kMalformed:
                        return HttpParseError::kInvalidTransferEncoding;
                    case TransferEncodingParse::kUnsupported:
                        return HttpParseError::kUnsupportedTransferEncoding;
                }
                break;
            }
            case RequestHeaderKind::kConnection:
                httpUpdateConnectionFlags(
                    value,
                    block.flags.connectionClose,
                    block.flags.connectionKeepAlive,
                    block.flags.upgrade);
                break;
            case RequestHeaderKind::kExpect:
                if (!httpUpdateExpectContinueFlag(value, block.flags.expectContinue)) {
                    return HttpParseError::kExpectationFailed;
                }
                break;
            case RequestHeaderKind::kSecWebSocketKey:
                if (block.flags.secWebSocketKeyCount < 2) {
                    ++block.flags.secWebSocketKeyCount;
                }
                break;
            case RequestHeaderKind::kSecWebSocketVersion:
                if (block.flags.secWebSocketVersionCount < 2) {
                    ++block.flags.secWebSocketVersionCount;
                }
                break;
            case RequestHeaderKind::kSecWebSocketProtocol:
                if (block.flags.secWebSocketProtocolCount < 2) {
                    ++block.flags.secWebSocketProtocolCount;
                }
                break;
            case RequestHeaderKind::kAcceptEncoding:
                block.gzipEncoding.update(value, "gzip");
                break;
            case RequestHeaderKind::kOther:
            case RequestHeaderKind::kAccept:
            case RequestHeaderKind::kAccessControlRequestHeaders:
            case RequestHeaderKind::kAccessControlRequestMethod:
            case RequestHeaderKind::kAuthorization:
            case RequestHeaderKind::kContentType:
            case RequestHeaderKind::kCookie:
            case RequestHeaderKind::kIfMatch:
            case RequestHeaderKind::kIfModifiedSince:
            case RequestHeaderKind::kIfNoneMatch:
            case RequestHeaderKind::kIfRange:
            case RequestHeaderKind::kIfUnmodifiedSince:
            case RequestHeaderKind::kOrigin:
            case RequestHeaderKind::kRange:
            case RequestHeaderKind::kUpgrade:
            case RequestHeaderKind::kUserAgent:
                break;
        }

        const auto index = block.headerCount++;
        block.headers[index] = ParsedRequestHeaderSlot{
            .name = makeSlice(nameStart, nameEnd - nameStart),
            .value = makeSlice(valueStart, valueEnd - valueStart),
            .kind = kind};
        block.known.record(kind, index);
        cursor += 2;
    }

    return HttpParseError::kNone;
}

}  // namespace ruvia::detail
