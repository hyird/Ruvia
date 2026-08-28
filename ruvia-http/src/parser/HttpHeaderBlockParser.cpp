#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/http/detail/field/HttpTeFields.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

#include <algorithm>
#include <cstring>

namespace ruvia::detail {
namespace {

[[nodiscard]] HttpHeaderSlice makeSlice(std::size_t offset, std::size_t length) noexcept {
    return HttpHeaderSlice{.offset = static_cast<std::uint32_t>(offset), .length = static_cast<std::uint32_t>(length)};
}

[[nodiscard]] std::size_t trimRightOws(std::string_view buffer, std::size_t begin, std::size_t end) noexcept {
    while (end > begin && (buffer[end - 1] == ' ' || buffer[end - 1] == '\t')) {
        --end;
    }
    return end;
}

// The request line: method SP request-target SP HTTP-version CRLF (RFC 9112
// section 3.1). On success `cursor` sits on the first header field, and
// `ignoreUpgrade` reports an HTTP/1.0 version, which changes how the field
// section treats Upgrade.
[[nodiscard]] std::optional<HttpParseError> parseRequestLine(std::string_view buffer, std::size_t headersEnd, std::size_t& cursor, ParsedRequestHeaderBlock& block, bool& ignoreUpgrade) noexcept {
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
    ignoreUpgrade = buffer.substr(versionStart, cursor - versionStart) == "HTTP/1.0";
    cursor += 2;

    return std::nullopt;
}

// What one known request header means: which values are validated at the
// protocol boundary, which are parsed into a typed field on the block, and
// which may appear only once. Finding the bytes is the loop's job below; every
// rule about what they mean is here.
[[nodiscard]] std::optional<HttpParseError> applyRequestHeader(RequestHeaderKind kind, std::string_view value, bool ignoreUpgrade, ParsedRequestHeaderBlock& block) noexcept {
    switch (kind) {
        case RequestHeaderKind::kHost:
            if (block.hostHeaderIndex >= 0 || !isValidHostHeader(value)) {
                return HttpParseError::kInvalidHost;
            }
            break;
        case RequestHeaderKind::kContentLength: {
            switch (block.contentLength.parseField(value)) {
                case HttpContentLengthParseStatus::kOk:
                    break;
                case HttpContentLengthParseStatus::kInvalid:
                    return HttpParseError::kInvalidContentLength;
                case HttpContentLengthParseStatus::kConflicting:
                    return HttpParseError::kConflictingContentLength;
            }
            break;
        }
        case RequestHeaderKind::kTransferEncoding: {
            switch (block.transferEncoding.parseField(value)) {
                case HttpTransferEncodingParseStatus::kOk:
                    break;
                case HttpTransferEncodingParseStatus::kMalformed:
                    return HttpParseError::kInvalidTransferEncoding;
                case HttpTransferEncodingParseStatus::kUnsupported:
                    return HttpParseError::kUnsupportedTransferEncoding;
            }
            break;
        }
        case RequestHeaderKind::kConnection: {
            if (block.connectionOptions.parseField(value, HttpFieldListRole::kRecipient, [](std::string_view option) noexcept { return !httpConnectionOptionConflictsWithManagedField(option); }) != HttpFieldListParseStatus::kOk) {
                return HttpParseError::kInvalidConnection;
            }
            break;
        }
        case RequestHeaderKind::kExpect: {
            if (!isValidReceivedHttpExpectFieldValue(value)) {
                return HttpParseError::kInvalidHeader;
            }
            block.expectations.parseField(value);
            break;
        }
        case RequestHeaderKind::kUpgrade:
            // RFC 9110 section 7.8 requires a server to ignore Upgrade in
            // an HTTP/1.0 request. The bytes still have to be a valid
            // generic field value, but Upgrade-specific grammar must not
            // turn an otherwise valid HTTP/1.0 request into a 400.
            if (!ignoreUpgrade && block.upgradeProtocols.parseField(value, HttpFieldListRole::kRecipient, [](const HttpUpgradeProtocol&) noexcept { return true; }) != HttpFieldListParseStatus::kOk) {
                return HttpParseError::kInvalidUpgrade;
            }
            break;
        case RequestHeaderKind::kAcceptEncoding:
            block.responseCodingQualities.update(value);
            break;
        case RequestHeaderKind::kAccessControlRequestMethod:
            if (!isValidHttpCorsRequestMethod(value)) {
                return HttpParseError::kInvalidHeader;
            }
            if (const auto bit = singletonRequestHeaderBit(kind); (block.seenHeaderBits & bit) != 0) {
                return HttpParseError::kInvalidHeader;
            } else {
                block.seenHeaderBits |= bit;
            }
            break;
        case RequestHeaderKind::kOrigin:
            if (!isValidHttpOriginFieldValue(value)) {
                return HttpParseError::kInvalidHeader;
            }
            if (const auto bit = singletonRequestHeaderBit(kind); (block.seenHeaderBits & bit) != 0) {
                return HttpParseError::kInvalidHeader;
            } else {
                block.seenHeaderBits |= bit;
            }
            break;
        case RequestHeaderKind::kContentType: {
            // Content-Type is a typed field, not an arbitrary singleton.
            // Validate its media-type grammar at the protocol boundary so
            // recipients and both request writers accept the same values.
            if (!isValidHttpContentTypeFieldValue(value)) {
                return HttpParseError::kInvalidHeader;
            }
            const auto bit = singletonRequestHeaderBit(kind);
            if ((block.seenHeaderBits & bit) != 0) {
                return HttpParseError::kInvalidHeader;
            }
            block.seenHeaderBits |= bit;
            break;
        }
        case RequestHeaderKind::kContentEncoding:
            if (!isValidHttpContentEncodingFieldValue(value, HttpFieldListRole::kRecipient)) {
                return HttpParseError::kInvalidHeader;
            }
            break;
        case RequestHeaderKind::kAuthorization:
        case RequestHeaderKind::kIfMatch:
        case RequestHeaderKind::kIfModifiedSince:
        case RequestHeaderKind::kIfNoneMatch:
        case RequestHeaderKind::kIfRange:
        case RequestHeaderKind::kIfUnmodifiedSince:
        case RequestHeaderKind::kRange:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketVersion:
        case RequestHeaderKind::kUserAgent:
            if (const auto bit = singletonRequestHeaderBit(kind); bit != 0) {
                if ((block.seenHeaderBits & bit) != 0) {
                    return HttpParseError::kInvalidHeader;
                }
                block.seenHeaderBits |= bit;
            }
            break;
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kCookie:
        case RequestHeaderKind::kSecWebSocketProtocol:
            break;
        case RequestHeaderKind::kAccessControlRequestHeaders:
            if (!isValidHttpCorsRequestHeaderNames(value)) {
                return HttpParseError::kInvalidHeader;
            }
            break;
    }

    return std::nullopt;
}

// The header section: one field line at a time, name and value delimited by
// ':' and CRLF with optional whitespace trimmed off the value.
[[nodiscard]] std::optional<HttpParseError> parseHeaderFields(std::string_view buffer, std::size_t headersEnd, std::size_t cursor, bool ignoreUpgrade, ParsedRequestHeaderBlock& block) noexcept {
    while (cursor < headersEnd) {
        if (block.headerCount == kMaxHttpHeaderFields) {
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

        if (httpAsciiEqualsIgnoreCase(name, "Trailer")) {
            if (!isValidHttpRequestTrailerFieldValue(value, HttpFieldListRole::kRecipient)) {
                return HttpParseError::kInvalidHeader;
            }
            if (!httpFindHeaderToken(value, [](std::string_view) noexcept { return true; }).empty()) {
                block.nonEmptyTrailerHeaderPresent = true;
            }
        }
        if (httpAsciiEqualsIgnoreCase(name, "TE")) {
            if (!isValidReceivedHttpTeFieldValue(value)) {
                return HttpParseError::kInvalidHeader;
            }
            block.teHeaderPresent = true;
        }

        const auto kind = classifyRequestHeader(name);
        if (const auto error = applyRequestHeader(kind, value, ignoreUpgrade, block)) {
            return error;
        }

        const auto index = block.headerCount++;
        block.headers[index] = ParsedRequestHeaderSlot{.name = makeSlice(nameStart, nameEnd - nameStart), .value = makeSlice(valueStart, valueEnd - valueStart), .kind = kind};
        if (kind == RequestHeaderKind::kHost) {
            block.hostHeaderIndex = static_cast<KnownRequestHeaderIndex>(index);
        }
        cursor += 2;
    }

    return std::nullopt;
}

}  // namespace

std::size_t findHttpHeaderEnd(std::string_view buffer, std::size_t searchOffset) noexcept {
    const auto limit = std::min(buffer.size(), kMaxHttpHeaderBytes);
    if (limit < 4) {
        return std::string_view::npos;
    }

    auto cursor = searchOffset >= limit ? limit : std::max<std::size_t>(3, searchOffset + 3);
    while (cursor < limit) {
        const auto* hit = static_cast<const char*>(std::memchr(buffer.data() + cursor, '\n', limit - cursor));
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

std::optional<HttpParseError> parseHttpHeaderBlock(std::string_view buffer, std::size_t headerBytes, ParsedRequestHeaderBlock& block) noexcept {
    const auto headersEnd = headerBytes - 2;
    std::size_t cursor = 0;
    bool ignoreUpgrade = false;
    if (const auto error = parseRequestLine(buffer, headersEnd, cursor, block, ignoreUpgrade)) {
        return error;
    }
    return parseHeaderFields(buffer, headersEnd, cursor, ignoreUpgrade, block);
}

}  // namespace ruvia::detail
