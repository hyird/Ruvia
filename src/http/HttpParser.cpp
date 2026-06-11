#include "ruvia/http/HttpParser.h"

#include "ruvia/http/HeaderUtils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>

namespace ruvia {
namespace {

struct RequestTargetView {
    std::string_view path;
    std::string_view query;
    std::string_view authority;
    std::uint16_t defaultPort{0};
};

// 256-entry character class tables (picohttpparser/llhttp style): one load
// replaces the multi-comparison chains, and lets scan loops validate while
// they advance instead of re-walking each token afterwards.
inline constexpr std::array<bool, 256> kTokenCharTable = [] {
    std::array<bool, 256> table{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        table[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        table[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        table[c] = true;
    }
    for (const unsigned char c : {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
        table[c] = true;
    }
    return table;
}();

// field-content bytes: HTAB, printable ASCII, and obs-text (0x80-0xFF).
// CR/LF/NUL/other controls and DEL are excluded.
inline constexpr std::array<bool, 256> kFieldValueCharTable = [] {
    std::array<bool, 256> table{};
    for (unsigned c = 0; c < 256; ++c) {
        table[c] = c == '\t' || (c >= 0x20 && c != 0x7F);
    }
    return table;
}();

[[nodiscard]] std::size_t findHeaderEnd(std::string_view buffer, std::size_t searchOffset) noexcept {
    const auto limit = std::min(buffer.size(), kMaxHttpHeaderBytes);
    if (limit < 4) {
        return std::string_view::npos;
    }

    // Hunt for the final '\n' of "\r\n\r\n" with memchr so the scan runs at
    // vectorized libc speed instead of byte-at-a-time comparisons.
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

enum class RequestHeaderKind {
    kOther,
    kAccept,
    kAcceptEncoding,
    kAccessControlRequestHeaders,
    kAccessControlRequestMethod,
    kAuthorization,
    kConnection,
    kContentLength,
    kContentType,
    kCookie,
    kExpect,
    kHost,
    kIfMatch,
    kIfModifiedSince,
    kIfNoneMatch,
    kIfRange,
    kIfUnmodifiedSince,
    kOrigin,
    kRange,
    kSecWebSocketKey,
    kSecWebSocketProtocol,
    kSecWebSocketVersion,
    kTransferEncoding,
    kUpgrade,
    kUserAgent
};

enum class ChunkSizeLineStatus {
    kOk,
    kInvalidSize,
    kOverflow,
    kInvalidExtension
};

// Slice and HeaderSlot carry no default member initializers on purpose: the
// 64-slot header table below stays uninitialized per request (slots are
// always written before they are read up to headerCount), so constructing a
// ParsedHeaderBlock does not re-zero ~1.5KB on every request.
struct Slice {
    std::uint32_t offset;
    std::uint32_t length;

    [[nodiscard]] std::string_view bind(std::string_view buffer) const noexcept {
        return buffer.substr(offset, length);
    }
};

struct HeaderSlot {
    Slice name;
    Slice value;
    RequestHeaderKind kind;
};

struct KnownHeaderIndexes {
    int connection{-1};
    int host{-1};
    int contentLength{-1};
    int transferEncoding{-1};
    int expect{-1};
    int contentType{-1};
    int cookie{-1};
    int origin{-1};
    int accessControlRequestMethod{-1};
    int accessControlRequestHeaders{-1};
    int authorization{-1};
    int acceptEncoding{-1};
    int accept{-1};
    int range{-1};
    int ifMatch{-1};
    int ifNoneMatch{-1};
    int ifModifiedSince{-1};
    int ifUnmodifiedSince{-1};
    int ifRange{-1};
    int upgrade{-1};
    int secWebSocketKey{-1};
    int secWebSocketVersion{-1};
    int secWebSocketProtocol{-1};
    int userAgent{-1};
};

struct ParsedHeaderBlock {
    Slice method;
    Slice target;
    Slice version;
    std::array<HeaderSlot, kMaxRequestHeaders> headers;
    std::size_t headerCount{0};
    KnownHeaderIndexes known;
    HttpRequestFlags flags;
    std::size_t contentLength{0};
    bool sawContentLength{false};
    bool sawChunked{false};
    bool sawTransferEncoding{false};
    bool transferGzip{false};
    bool transferDeflate{false};
    int acceptGzipQuality{-1};
    int acceptGzipWildcardQuality{-1};
    HttpTransferCodings transferCodings;
};

[[nodiscard]] RequestHeaderKind classifyRequestHeader(std::string_view name) noexcept {
    switch (name.size()) {
        case 4:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Host")) {
                return RequestHeaderKind::kHost;
            }
            break;
        case 5:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Range")) {
                return RequestHeaderKind::kRange;
            }
            break;
        case 6:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Accept")) {
                return RequestHeaderKind::kAccept;
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Cookie")) {
                return RequestHeaderKind::kCookie;
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Expect")) {
                return RequestHeaderKind::kExpect;
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Origin")) {
                return RequestHeaderKind::kOrigin;
            }
            break;
        case 8:
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-Match")) {
                return RequestHeaderKind::kIfMatch;
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-Range")) {
                return RequestHeaderKind::kIfRange;
            }
            break;
        case 10:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) {
                return RequestHeaderKind::kConnection;
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "User-Agent")) {
                return RequestHeaderKind::kUserAgent;
            }
            break;
        case 12:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Type")) {
                return RequestHeaderKind::kContentType;
            }
            break;
        case 13:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Authorization")) {
                return RequestHeaderKind::kAuthorization;
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-None-Match")) {
                return RequestHeaderKind::kIfNoneMatch;
            }
            break;
        case 14:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
                return RequestHeaderKind::kContentLength;
            }
            break;
        case 15:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Accept-Encoding")) {
                return RequestHeaderKind::kAcceptEncoding;
            }
            break;
        case 17:
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-Modified-Since")) {
                return RequestHeaderKind::kIfModifiedSince;
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Key")) {
                return RequestHeaderKind::kSecWebSocketKey;
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
                return RequestHeaderKind::kTransferEncoding;
            }
            break;
        case 19:
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-Unmodified-Since")) {
                return RequestHeaderKind::kIfUnmodifiedSince;
            }
            break;
        case 29:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Request-Method")) {
                return RequestHeaderKind::kAccessControlRequestMethod;
            }
            break;
        case 7:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
                return RequestHeaderKind::kUpgrade;
            }
            break;
        case 21:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Version")) {
                return RequestHeaderKind::kSecWebSocketVersion;
            }
            break;
        case 22:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Protocol")) {
                return RequestHeaderKind::kSecWebSocketProtocol;
            }
            break;
        case 30:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Request-Headers")) {
                return RequestHeaderKind::kAccessControlRequestHeaders;
            }
            break;
        default:
            break;
    }
    return RequestHeaderKind::kOther;
}

[[nodiscard]] bool isValidHeaderName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const auto ch : name) {
        if (!kTokenCharTable[static_cast<unsigned char>(ch)]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isHexDigit(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

[[nodiscard]] std::uint8_t hexValue(unsigned char c) noexcept {
    if (c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    return static_cast<std::uint8_t>(c - 'a' + 10);
}

[[nodiscard]] bool isValidChunkExtension(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }

    std::size_t cursor = 0;
    const auto skipBws = [&value, &cursor]() noexcept {
        while (cursor < value.size() && (value[cursor] == ' ' || value[cursor] == '\t')) {
            ++cursor;
        }
    };
    const auto parseToken = [&value, &cursor]() noexcept {
        const auto begin = cursor;
        while (cursor < value.size() && kTokenCharTable[static_cast<unsigned char>(value[cursor])]) {
            ++cursor;
        }
        return cursor != begin;
    };

    while (cursor < value.size()) {
        skipBws();
        if (cursor == value.size()) {
            return true;
        }
        if (value[cursor] != ';') {
            return false;
        }
        ++cursor;
        skipBws();
        if (!parseToken()) {
            return false;
        }
        skipBws();
        if (cursor == value.size() || value[cursor] == ';') {
            continue;
        }
        if (value[cursor] != '=') {
            return false;
        }
        ++cursor;
        skipBws();
        if (cursor == value.size()) {
            return false;
        }
        if (value[cursor] == '"') {
            ++cursor;
            bool closed = false;
            while (cursor < value.size()) {
                const auto c = static_cast<unsigned char>(value[cursor]);
                if (c == '"') {
                    ++cursor;
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    ++cursor;
                    if (cursor == value.size()) {
                        return false;
                    }
                    const auto escaped = static_cast<unsigned char>(value[cursor]);
                    if (escaped == 0x7F || escaped < 0x20) {
                        return false;
                    }
                    ++cursor;
                    continue;
                }
                if (c == 0x7F || (c < 0x20 && c != '\t')) {
                    return false;
                }
                ++cursor;
            }
            if (!closed) {
                return false;
            }
        } else if (!parseToken()) {
            return false;
        }
        skipBws();
        if (cursor < value.size() && value[cursor] != ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidHeaderValue(std::string_view value) noexcept {
    for (const auto ch : value) {
        if (!kFieldValueCharTable[static_cast<unsigned char>(ch)]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidRequestTargetBytes(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    for (const auto ch : target) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7F || c == '#') {
            return false;
        }
    }
    return true;
}

// uri reg-name chars: unreserved / sub-delims (pct-encoded handled by caller).
inline constexpr std::array<bool, 256> kRegNameCharTable = [] {
    std::array<bool, 256> table{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        table[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        table[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        table[c] = true;
    }
    for (const unsigned char c :
         {'-', '.', '_', '~', '!', '$', '&', '\'', '(', ')', '*', '+', ',', ';', '='}) {
        table[c] = true;
    }
    return table;
}();

[[nodiscard]] bool isRegNameChar(unsigned char c) noexcept {
    return kRegNameCharTable[c];
}

[[nodiscard]] bool parsePortValue(std::string_view value, std::uint16_t& port) noexcept {
    if (value.empty()) {
        return false;
    }

    unsigned int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end || parsed > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

[[nodiscard]] bool parsePort(std::string_view value) noexcept {
    std::uint16_t port = 0;
    return parsePortValue(value, port);
}

[[nodiscard]] bool isValidBracketedHost(std::string_view value) noexcept {
    const auto close = value.find(']');
    if (close == std::string_view::npos || close <= 1) {
        return false;
    }
    for (std::size_t i = 1; i < close; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == ':' || c == '.') {
            continue;
        }
        if (!isRegNameChar(c)) {
            return false;
        }
    }
    if (value.find('[', close + 1) != std::string_view::npos) {
        return false;
    }
    if (close + 1 == value.size()) {
        return true;
    }
    return value[close + 1] == ':' && parsePort(value.substr(close + 2));
}

[[nodiscard]] bool isValidHostHeader(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[') {
        return isValidBracketedHost(value);
    }

    // Single pass over the reg-name: '[' / ']' / a second ':' are not
    // reg-name chars and fall out of the table check; parsePort rejects an
    // empty port and any non-digit remainder (including extra colons).
    std::size_t i = 0;
    for (; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == ':') {
            break;
        }
        if (c == '%') {
            if (i + 2 >= value.size() ||
                !isHexDigit(static_cast<unsigned char>(value[i + 1])) ||
                !isHexDigit(static_cast<unsigned char>(value[i + 2]))) {
                return false;
            }
            i += 2;
            continue;
        }
        if (!kRegNameCharTable[c]) {
            return false;
        }
    }
    if (i == 0) {
        return false;
    }
    if (i == value.size()) {
        return true;
    }
    return parsePort(value.substr(i + 1));
}

struct AuthorityParts {
    std::string_view host;
    std::uint16_t port{0};
    bool hasPort{false};
};

[[nodiscard]] bool splitAuthority(std::string_view value, AuthorityParts& parts) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        parts.host = value.substr(0, close + 1);
        if (close + 1 == value.size()) {
            parts.port = 0;
            parts.hasPort = false;
            return true;
        }
        if (value[close + 1] != ':' || !parsePortValue(value.substr(close + 2), parts.port)) {
            return false;
        }
        parts.hasPort = true;
        return true;
    }

    const auto colon = value.find(':');
    if (colon == std::string_view::npos) {
        parts.host = value;
        parts.port = 0;
        parts.hasPort = false;
        return true;
    }
    parts.host = value.substr(0, colon);
    if (!parsePortValue(value.substr(colon + 1), parts.port)) {
        return false;
    }
    parts.hasPort = true;
    return true;
}

[[nodiscard]] bool authorityMatchesHost(
    std::string_view authority,
    std::string_view host,
    std::uint16_t defaultPort) noexcept {
    if (detail::httpAsciiEqualsIgnoreCase(authority, host)) {
        return true;
    }

    AuthorityParts authorityParts;
    AuthorityParts hostParts;
    if (!splitAuthority(authority, authorityParts) || !splitAuthority(host, hostParts)) {
        return false;
    }
    if (!detail::httpAsciiEqualsIgnoreCase(authorityParts.host, hostParts.host)) {
        return false;
    }

    const auto authorityPort = authorityParts.hasPort ? authorityParts.port : defaultPort;
    const auto hostPort = hostParts.hasPort ? hostParts.port : defaultPort;
    return authorityPort == hostPort;
}

[[nodiscard]] bool parseAbsoluteTarget(std::string_view target, RequestTargetView& output) noexcept {
    std::size_t authorityBegin = 0;
    if (target.size() >= 7 && detail::httpAsciiEqualsIgnoreCase(target.substr(0, 7), "http://")) {
        authorityBegin = 7;
        output.defaultPort = 80;
    } else if (target.size() >= 8 && detail::httpAsciiEqualsIgnoreCase(target.substr(0, 8), "https://")) {
        authorityBegin = 8;
        output.defaultPort = 443;
    } else {
        return false;
    }

    const auto rest = target.substr(authorityBegin);
    const auto separator = rest.find_first_of("/?");
    const auto authority = separator == std::string_view::npos ? rest : rest.substr(0, separator);
    if (!isValidHostHeader(authority)) {
        return false;
    }

    output.authority = authority;
    if (separator == std::string_view::npos) {
        output.path = "/";
        output.query = {};
        return true;
    }

    const auto pathBegin = authorityBegin + separator;
    if (target[pathBegin] == '?') {
        output.path = "/";
        output.query = target.substr(pathBegin + 1);
        return true;
    }

    const auto querySeparator = target.find('?', pathBegin);
    output.path = querySeparator == std::string_view::npos
        ? target.substr(pathBegin)
        : target.substr(pathBegin, querySeparator - pathBegin);
    output.query = querySeparator == std::string_view::npos
        ? std::string_view{}
        : target.substr(querySeparator + 1);
    return !output.path.empty() && output.path.front() == '/';
}

[[nodiscard]] bool parseRequestTarget(
    HttpMethod method,
    std::string_view target,
    RequestTargetView& output) noexcept {
    if (!isValidRequestTargetBytes(target)) {
        return false;
    }
    if (target == "*") {
        if (method != HttpMethod::kOptions) {
            return false;
        }
        output.path = "*";
        output.query = {};
        output.authority = {};
        output.defaultPort = 0;
        return true;
    }
    if (target.front() == '/') {
        const auto querySeparator = target.find('?');
        output.path = querySeparator == std::string_view::npos
            ? target
            : target.substr(0, querySeparator);
        output.query = querySeparator == std::string_view::npos
            ? std::string_view{}
            : target.substr(querySeparator + 1);
        output.authority = {};
        output.defaultPort = 0;
        return !output.path.empty();
    }
    return parseAbsoluteTarget(target, output);
}

[[nodiscard]] bool parseContentLength(std::string_view value, std::size_t& contentLength) noexcept {
    value = detail::httpTrimOws(value);
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

[[nodiscard]] TransferEncodingParse parseTransferEncoding(std::string_view value, ParsedHeaderBlock& block) noexcept {
    value = detail::httpTrimOws(value);
    for (;;) {
        const auto comma = value.find(',');
        auto token = detail::httpTrimOws(comma == std::string_view::npos ? value : value.substr(0, comma));
        if (const auto semicolon = token.find(';'); semicolon != std::string_view::npos) {
            if (!isValidChunkExtension(token.substr(semicolon))) {
                return TransferEncodingParse::kMalformed;
            }
            token = detail::httpTrimOws(token.substr(0, semicolon));
        }
        if (token.empty()) {
            return TransferEncodingParse::kMalformed;
        }

        if (block.sawChunked) {
            return TransferEncodingParse::kMalformed;
        }

        if (detail::httpAsciiEqualsIgnoreCase(token, "chunked")) {
            block.sawChunked = true;
            block.sawTransferEncoding = true;
            if (comma != std::string_view::npos) {
                return TransferEncodingParse::kMalformed;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "gzip") ||
                   detail::httpAsciiEqualsIgnoreCase(token, "x-gzip")) {
            if (block.transferCodings.count == kMaxTransferCodings) {
                return TransferEncodingParse::kUnsupported;
            }
            block.transferGzip = true;
            block.sawTransferEncoding = true;
            block.transferCodings.values[block.transferCodings.count++] = HttpTransferCoding::kGzip;
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "deflate")) {
            if (block.transferCodings.count == kMaxTransferCodings) {
                return TransferEncodingParse::kUnsupported;
            }
            block.transferDeflate = true;
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

[[nodiscard]] ChunkSizeLineStatus parseHttpChunkSizeLine(std::string_view value, std::size_t& size) noexcept {
    // RFC 9112: chunk-size starts at the first byte of the line. Leading OWS
    // before the size is a known request-smuggling vector and is rejected,
    // matching picohttpparser/llhttp strict parsing.
    std::size_t cursor = 0;
    std::size_t parsed = 0;
    constexpr auto maxBeforeShift = std::numeric_limits<std::size_t>::max() >> 4U;
    while (cursor < value.size() && isHexDigit(static_cast<unsigned char>(value[cursor]))) {
        if (parsed > maxBeforeShift) {
            return ChunkSizeLineStatus::kOverflow;
        }
        parsed = (parsed << 4U) | hexValue(static_cast<unsigned char>(value[cursor]));
        ++cursor;
    }
    if (cursor == 0) {
        return ChunkSizeLineStatus::kInvalidSize;
    }
    if (!isValidChunkExtension(value.substr(cursor))) {
        return ChunkSizeLineStatus::kInvalidExtension;
    }

    size = parsed;
    return ChunkSizeLineStatus::kOk;
}

[[nodiscard]] bool isForbiddenChunkTrailer(std::string_view name) noexcept {
    switch (classifyRequestHeader(name)) {
        case RequestHeaderKind::kHost:
        case RequestHeaderKind::kContentLength:
        case RequestHeaderKind::kTransferEncoding:
        case RequestHeaderKind::kConnection:
        case RequestHeaderKind::kContentType:
        case RequestHeaderKind::kCookie:
        case RequestHeaderKind::kExpect:
        case RequestHeaderKind::kIfMatch:
        case RequestHeaderKind::kIfModifiedSince:
        case RequestHeaderKind::kIfNoneMatch:
        case RequestHeaderKind::kIfRange:
        case RequestHeaderKind::kIfUnmodifiedSince:
        case RequestHeaderKind::kRange:
        case RequestHeaderKind::kUpgrade:
        case RequestHeaderKind::kAuthorization:
            return true;
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kUserAgent:
        case RequestHeaderKind::kOrigin:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kSecWebSocketVersion:
            break;
    }

    switch (name.size()) {
        case 2:
            return detail::httpAsciiEqualsIgnoreCase(name, "TE");
        case 7:
            return detail::httpAsciiEqualsIgnoreCase(name, "Trailer");
        case 10:
            return detail::httpAsciiEqualsIgnoreCase(name, "Keep-Alive") ||
                detail::httpAsciiEqualsIgnoreCase(name, "Set-Cookie");
        case 13:
            return detail::httpAsciiEqualsIgnoreCase(name, "Authorization") ||
                detail::httpAsciiEqualsIgnoreCase(name, "Cache-Control");
        case 14:
            return detail::httpAsciiEqualsIgnoreCase(name, "Max-Forwards");
        case 15:
            return detail::httpAsciiEqualsIgnoreCase(name, "Accept-Ranges");
        case 16:
            return detail::httpAsciiEqualsIgnoreCase(name, "Content-Range");
        case 17:
            return detail::httpAsciiEqualsIgnoreCase(name, "Content-Encoding");
        case 18:
            return detail::httpAsciiEqualsIgnoreCase(name, "Proxy-Authenticate");
        case 19:
            return detail::httpAsciiEqualsIgnoreCase(name, "Proxy-Authorization");
        default:
            return false;
    }
}

[[nodiscard]] Slice makeSlice(std::size_t offset, std::size_t length) noexcept {
    return Slice{
        .offset = static_cast<std::uint32_t>(offset),
        .length = static_cast<std::uint32_t>(length)};
}

[[nodiscard]] std::size_t trimRightOws(std::string_view buffer, std::size_t begin, std::size_t end) noexcept {
    while (end > begin && (buffer[end - 1] == ' ' || buffer[end - 1] == '\t')) {
        --end;
    }
    return end;
}

void recordKnownHeader(ParsedHeaderBlock& block, RequestHeaderKind kind, std::size_t index) noexcept {
    const auto idx = static_cast<int>(index);
    switch (kind) {
        case RequestHeaderKind::kConnection:
            if (block.known.connection < 0) {
                block.known.connection = idx;
            }
            break;
        case RequestHeaderKind::kHost:
            if (block.known.host < 0) {
                block.known.host = idx;
            }
            break;
        case RequestHeaderKind::kContentLength:
            if (block.known.contentLength < 0) {
                block.known.contentLength = idx;
            }
            break;
        case RequestHeaderKind::kTransferEncoding:
            if (block.known.transferEncoding < 0) {
                block.known.transferEncoding = idx;
            }
            break;
        case RequestHeaderKind::kExpect:
            if (block.known.expect < 0) {
                block.known.expect = idx;
            }
            break;
        case RequestHeaderKind::kContentType:
            if (block.known.contentType < 0) {
                block.known.contentType = idx;
            }
            break;
        case RequestHeaderKind::kCookie:
            if (block.known.cookie < 0) {
                block.known.cookie = idx;
            }
            break;
        case RequestHeaderKind::kOrigin:
            if (block.known.origin < 0) {
                block.known.origin = idx;
            }
            break;
        case RequestHeaderKind::kAccessControlRequestMethod:
            if (block.known.accessControlRequestMethod < 0) {
                block.known.accessControlRequestMethod = idx;
            }
            break;
        case RequestHeaderKind::kAccessControlRequestHeaders:
            if (block.known.accessControlRequestHeaders < 0) {
                block.known.accessControlRequestHeaders = idx;
            }
            break;
        case RequestHeaderKind::kAuthorization:
            if (block.known.authorization < 0) {
                block.known.authorization = idx;
            }
            break;
        case RequestHeaderKind::kAcceptEncoding:
            if (block.known.acceptEncoding < 0) {
                block.known.acceptEncoding = idx;
            }
            break;
        case RequestHeaderKind::kAccept:
            if (block.known.accept < 0) {
                block.known.accept = idx;
            }
            break;
        case RequestHeaderKind::kRange:
            if (block.known.range < 0) {
                block.known.range = idx;
            }
            break;
        case RequestHeaderKind::kIfMatch:
            if (block.known.ifMatch < 0) {
                block.known.ifMatch = idx;
            }
            break;
        case RequestHeaderKind::kIfNoneMatch:
            if (block.known.ifNoneMatch < 0) {
                block.known.ifNoneMatch = idx;
            }
            break;
        case RequestHeaderKind::kIfModifiedSince:
            if (block.known.ifModifiedSince < 0) {
                block.known.ifModifiedSince = idx;
            }
            break;
        case RequestHeaderKind::kIfUnmodifiedSince:
            if (block.known.ifUnmodifiedSince < 0) {
                block.known.ifUnmodifiedSince = idx;
            }
            break;
        case RequestHeaderKind::kIfRange:
            if (block.known.ifRange < 0) {
                block.known.ifRange = idx;
            }
            break;
        case RequestHeaderKind::kUpgrade:
            if (block.known.upgrade < 0) {
                block.known.upgrade = idx;
            }
            break;
        case RequestHeaderKind::kSecWebSocketKey:
            if (block.known.secWebSocketKey < 0) {
                block.known.secWebSocketKey = idx;
            }
            break;
        case RequestHeaderKind::kSecWebSocketVersion:
            if (block.known.secWebSocketVersion < 0) {
                block.known.secWebSocketVersion = idx;
            }
            break;
        case RequestHeaderKind::kSecWebSocketProtocol:
            if (block.known.secWebSocketProtocol < 0) {
                block.known.secWebSocketProtocol = idx;
            }
            break;
        case RequestHeaderKind::kUserAgent:
            if (block.known.userAgent < 0) {
                block.known.userAgent = idx;
            }
            break;
        case RequestHeaderKind::kOther:
            break;
    }
}

[[nodiscard]] HttpParseError parseHeaderBlock(std::string_view buffer, std::size_t headerBytes, ParsedHeaderBlock& block) noexcept {
    const auto headersEnd = headerBytes - 2;
    std::size_t cursor = 0;

    // Single-pass scans: each loop validates through the character class
    // tables while it advances, so tokens never get re-walked afterwards.
    const auto methodStart = cursor;
    while (cursor < headersEnd && kTokenCharTable[static_cast<unsigned char>(buffer[cursor])]) {
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

        // Field name must be a non-empty token followed by ':'. This also
        // rejects obs-fold continuations (leading SP/HTAB) and stray CRs.
        const auto nameStart = cursor;
        while (cursor < headersEnd && kTokenCharTable[static_cast<unsigned char>(buffer[cursor])]) {
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
        while (cursor < headersEnd && kFieldValueCharTable[static_cast<unsigned char>(buffer[cursor])]) {
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
                detail::httpUpdateConnectionFlags(
                    value,
                    block.flags.connectionClose,
                    block.flags.connectionKeepAlive,
                    block.flags.upgrade);
                break;
            case RequestHeaderKind::kExpect:
                if (!detail::httpUpdateExpectContinueFlag(value, block.flags.expectContinue)) {
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
                detail::httpUpdateAcceptedEncodingQuality(
                    value,
                    "gzip",
                    block.acceptGzipQuality,
                    block.acceptGzipWildcardQuality);
                block.flags.acceptsGzip = block.acceptGzipQuality >= 0
                    ? block.acceptGzipQuality > 0
                    : block.acceptGzipWildcardQuality > 0;
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
        block.headers[index] = HeaderSlot{
            .name = makeSlice(nameStart, nameEnd - nameStart),
            .value = makeSlice(valueStart, valueEnd - valueStart),
            .kind = kind};
        recordKnownHeader(block, kind, index);
        cursor += 2;
    }

    return HttpParseError::kNone;
}

}  // namespace

HttpChunkScanStatus validateHttpChunkTrailers(std::string_view trailers) noexcept {
    if (trailers.empty()) {
        return HttpChunkScanStatus::kComplete;
    }

    std::size_t cursor = 0;
    while (cursor < trailers.size()) {
        const auto lineEnd = trailers.find("\r\n", cursor);
        const auto line = lineEnd == std::string_view::npos
            ? trailers.substr(cursor)
            : trailers.substr(cursor, lineEnd - cursor);
        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            return HttpChunkScanStatus::kInvalidTrailer;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return HttpChunkScanStatus::kInvalidTrailer;
        }
        const auto name = line.substr(0, colon);
        const auto value = detail::httpTrimOws(line.substr(colon + 1));
        if (!isValidHeaderName(name) || !isValidHeaderValue(value) || isForbiddenChunkTrailer(name)) {
            return HttpChunkScanStatus::kInvalidTrailer;
        }
        if (lineEnd == std::string_view::npos) {
            return HttpChunkScanStatus::kComplete;
        }
        cursor = lineEnd + 2;
    }
    return HttpChunkScanStatus::kComplete;
}

bool parseHttpChunkSize(std::string_view value, std::size_t& size) noexcept {
    return parseHttpChunkSizeLine(value, size) == ChunkSizeLineStatus::kOk;
}

HttpChunkScanResult scanHttpChunkedBody(std::string_view body) noexcept {
    std::size_t cursor = 0;
    std::size_t decoded = 0;
    std::size_t encodedOverhead = 0;
    const auto addOverhead = [&encodedOverhead](std::size_t bytes) noexcept {
        if (bytes > kMaxHttpBodyBytes || encodedOverhead > kMaxHttpBodyBytes - bytes) {
            return false;
        }
        encodedOverhead += bytes;
        return true;
    };
    for (;;) {
        const auto lineEnd = body.find("\r\n", cursor);
        if (lineEnd == std::string_view::npos) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kIncomplete};
        }

        std::size_t chunkSize = 0;
        switch (parseHttpChunkSizeLine(body.substr(cursor, lineEnd - cursor), chunkSize)) {
            case ChunkSizeLineStatus::kOk:
                break;
            case ChunkSizeLineStatus::kInvalidSize:
                return HttpChunkScanResult{.status = HttpChunkScanStatus::kInvalidSize};
            case ChunkSizeLineStatus::kOverflow:
                return HttpChunkScanResult{.status = HttpChunkScanStatus::kSizeOverflow};
            case ChunkSizeLineStatus::kInvalidExtension:
                return HttpChunkScanResult{.status = HttpChunkScanStatus::kInvalidExtension};
        }
        if (!addOverhead(lineEnd - cursor + 2)) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
        }
        cursor = lineEnd + 2;

        if (chunkSize == 0) {
            if (body.substr(cursor, 2) == "\r\n") {
                if (!addOverhead(2)) {
                    return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
                }
                return HttpChunkScanResult{
                    .status = HttpChunkScanStatus::kComplete,
                    .consumedBytes = cursor + 2,
                    .decodedBytes = decoded};
            }
            const auto trailerEnd = body.find("\r\n\r\n", cursor);
            if (trailerEnd != std::string_view::npos) {
                if (const auto trailerStatus = validateHttpChunkTrailers(body.substr(cursor, trailerEnd - cursor));
                    trailerStatus != HttpChunkScanStatus::kComplete) {
                    return HttpChunkScanResult{.status = trailerStatus};
                }
                if (!addOverhead(trailerEnd - cursor + 4)) {
                    return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
                }
                return HttpChunkScanResult{
                    .status = HttpChunkScanStatus::kComplete,
                    .consumedBytes = trailerEnd + 4,
                    .decodedBytes = decoded};
            }
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kIncomplete};
        }

        if (chunkSize > kMaxHttpBodyBytes || decoded > kMaxHttpBodyBytes - chunkSize) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
        }
        if (body.size() < cursor + chunkSize + 2) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kIncomplete};
        }
        if (body.substr(cursor + chunkSize, 2) != "\r\n") {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kInvalidCrlf};
        }
        if (!addOverhead(2)) {
            return HttpChunkScanResult{.status = HttpChunkScanStatus::kTooLarge};
        }

        decoded += chunkSize;
        cursor += chunkSize + 2;
    }
}

void HttpParser::parseRequestHead(std::string_view buffer, std::size_t headerSearchOffset, HttpParseResult& result) noexcept {
    // Reset only the scalar fields and the reachable request state; the
    // result object is reused across read iterations and requests, so a
    // full value-initialization here would re-zero the 2KB header table.
    result.status = HttpParseStatus::kIncomplete;
    result.error = HttpParseError::kNone;
    result.headerBytes = 0;
    result.contentLength = 0;
    result.decodedBodyBytes = 0;
    result.consumedBytes = 0;
    result.chunked = false;
    result.transferGzip = false;
    result.transferDeflate = false;
    result.transferCodings = {};
    result.flags = {};
    result.bodyPlan = {};
    result.request.reset();

    const auto fail = [&result](HttpParseError error) noexcept {
        result.request.reset();
        result.status = HttpParseStatus::kError;
        result.error = error;
    };

    const auto headerBytes = findHeaderEnd(buffer, headerSearchOffset);
    if (headerBytes == std::string_view::npos) {
        if (buffer.size() >= kMaxHttpHeaderBytes) {
            return fail(HttpParseError::kHeaderTooLarge);
        }
        return;
    }

    if (headerBytes > kMaxHttpHeaderBytes) {
        return fail(HttpParseError::kHeaderTooLarge);
    }

    ParsedHeaderBlock block;
    if (const auto error = parseHeaderBlock(buffer, headerBytes, block); error != HttpParseError::kNone) {
        return fail(error);
    }

    result.headerBytes = headerBytes;
    result.consumedBytes = headerBytes;
    result.flags = block.flags;

    // parseHeaderBlock scans the method through the token table, so it is
    // already a valid non-empty token here.
    result.request.setMethod(parseMethod(block.method.bind(buffer)));
    if (result.request.method() == HttpMethod::kUnknown) {
        return fail(HttpParseError::kUnsupportedMethod);
    }

    const auto target = block.target.bind(buffer);
    result.request.setTarget(target);
    result.request.setHttpVersion(block.version.bind(buffer));

    if (result.request.httpVersion().size() != 8 ||
        result.request.httpVersion().substr(0, 5) != "HTTP/" ||
        result.request.httpVersion()[5] < '0' ||
        result.request.httpVersion()[5] > '9' ||
        result.request.httpVersion()[6] != '.') {
        return fail(HttpParseError::kInvalidRequestLine);
    }
    if (result.request.httpVersion()[7] < '0' || result.request.httpVersion()[7] > '9') {
        return fail(HttpParseError::kInvalidRequestLine);
    }
    if (result.request.httpVersion()[5] != '1' ||
        (result.request.httpVersion()[7] != '0' && result.request.httpVersion()[7] != '1')) {
        return fail(HttpParseError::kUnsupportedHttpVersion);
    }

    RequestTargetView targetView;
    if (!parseRequestTarget(result.request.method(), target, targetView)) {
        return fail(HttpParseError::kInvalidRequestTarget);
    }
    result.request.setPath(targetView.path);
    result.request.setQueryString(targetView.query);

    const auto knownValue = [&block, buffer](int index) noexcept -> std::string_view {
        return index < 0 ? std::string_view{} : block.headers[static_cast<std::size_t>(index)].value.bind(buffer);
    };
    for (std::size_t i = 0; i < block.headerCount; ++i) {
        (void)result.request.addHeader(HttpHeaderView{block.headers[i].name.bind(buffer), block.headers[i].value.bind(buffer)});
    }
    if (block.known.connection >= 0) {
        result.request.setConnectionHeader(knownValue(block.known.connection));
    }
    if (block.known.host >= 0) {
        result.request.setHostHeader(knownValue(block.known.host));
    }
    if (block.known.contentLength >= 0) {
        result.request.setContentLengthHeader(knownValue(block.known.contentLength));
    }
    if (block.known.transferEncoding >= 0) {
        result.request.setTransferEncodingHeader(knownValue(block.known.transferEncoding));
    }
    if (block.known.expect >= 0) {
        result.request.setExpectHeader(knownValue(block.known.expect));
    }
    if (block.known.contentType >= 0) {
        result.request.setContentTypeHeader(knownValue(block.known.contentType));
    }
    if (block.known.cookie >= 0) {
        result.request.setCookieHeader(knownValue(block.known.cookie));
    }
    if (block.known.origin >= 0) {
        result.request.setOriginHeader(knownValue(block.known.origin));
    }
    if (block.known.accessControlRequestMethod >= 0) {
        result.request.setAccessControlRequestMethodHeader(knownValue(block.known.accessControlRequestMethod));
    }
    if (block.known.accessControlRequestHeaders >= 0) {
        result.request.setAccessControlRequestHeadersHeader(knownValue(block.known.accessControlRequestHeaders));
    }
    if (block.known.authorization >= 0) {
        result.request.setAuthorizationHeader(knownValue(block.known.authorization));
    }
    if (block.known.acceptEncoding >= 0) {
        result.request.setAcceptEncodingHeader(knownValue(block.known.acceptEncoding));
    }
    if (block.known.accept >= 0) {
        result.request.setAcceptHeader(knownValue(block.known.accept));
    }
    if (block.known.range >= 0) {
        result.request.setRangeHeader(knownValue(block.known.range));
    }
    if (block.known.ifMatch >= 0) {
        result.request.setIfMatchHeader(knownValue(block.known.ifMatch));
    }
    if (block.known.ifNoneMatch >= 0) {
        result.request.setIfNoneMatchHeader(knownValue(block.known.ifNoneMatch));
    }
    if (block.known.ifModifiedSince >= 0) {
        result.request.setIfModifiedSinceHeader(knownValue(block.known.ifModifiedSince));
    }
    if (block.known.ifUnmodifiedSince >= 0) {
        result.request.setIfUnmodifiedSinceHeader(knownValue(block.known.ifUnmodifiedSince));
    }
    if (block.known.ifRange >= 0) {
        result.request.setIfRangeHeader(knownValue(block.known.ifRange));
    }
    if (block.known.upgrade >= 0) {
        result.request.setUpgradeHeader(knownValue(block.known.upgrade));
    }
    if (block.known.secWebSocketKey >= 0) {
        result.request.setSecWebSocketKeyHeader(knownValue(block.known.secWebSocketKey));
    }
    if (block.known.secWebSocketVersion >= 0) {
        result.request.setSecWebSocketVersionHeader(knownValue(block.known.secWebSocketVersion));
    }
    if (block.known.secWebSocketProtocol >= 0) {
        result.request.setSecWebSocketProtocolHeader(knownValue(block.known.secWebSocketProtocol));
    }
    if (block.known.userAgent >= 0) {
        result.request.setUserAgentHeader(knownValue(block.known.userAgent));
    }

    if (result.request.httpVersion() == "HTTP/1.1" && !block.flags.hasHost) {
        return fail(HttpParseError::kMissingHost);
    }
    if (!targetView.authority.empty() &&
        block.known.host >= 0 &&
        !authorityMatchesHost(targetView.authority, knownValue(block.known.host), targetView.defaultPort)) {
        return fail(HttpParseError::kInvalidHost);
    }

    if (block.sawTransferEncoding && block.sawContentLength) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    if (block.sawTransferEncoding && !block.sawChunked) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    // RFC 9112 §6.1: Transfer-Encoding in an HTTP/1.0 request must be treated
    // as faulty framing; the error path closes the connection after replying.
    if (block.sawTransferEncoding && result.request.httpVersion()[7] == '0') {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    result.chunked = block.sawChunked;
    result.transferGzip = block.transferGzip;
    result.transferDeflate = block.transferDeflate;
    result.transferCodings = block.transferCodings;
    result.contentLength = block.contentLength;
    result.flags.transferGzip = block.transferGzip;
    result.flags.transferDeflate = block.transferDeflate;
    result.bodyPlan = HttpBodyPlan{
        .kind = block.sawChunked ? HttpBodyKind::kChunked : (block.contentLength == 0 ? HttpBodyKind::kNone : HttpBodyKind::kContentLength),
        .contentLength = block.contentLength,
        .expectContinue = block.flags.expectContinue};
    result.status = HttpParseStatus::kComplete;
}

std::string_view httpParseErrorMessage(HttpParseError error) noexcept {
    switch (error) {
        case HttpParseError::kNone:
            return "no parse error";
        case HttpParseError::kHeaderTooLarge:
            return "request header is too large";
        case HttpParseError::kBodyTooLarge:
            return "request body is too large";
        case HttpParseError::kInvalidRequestLine:
            return "invalid request line";
        case HttpParseError::kUnsupportedHttpVersion:
            return "unsupported HTTP version";
        case HttpParseError::kInvalidRequestTarget:
            return "invalid request target";
        case HttpParseError::kUnsupportedMethod:
            return "unsupported request method";
        case HttpParseError::kInvalidHeader:
            return "invalid request header";
        case HttpParseError::kTooManyHeaders:
            return "too many request headers";
        case HttpParseError::kMissingHost:
            return "missing Host header";
        case HttpParseError::kInvalidHost:
            return "invalid Host header";
        case HttpParseError::kInvalidContentLength:
        case HttpParseError::kConflictingContentLength:
            return "invalid Content-Length header";
        case HttpParseError::kInvalidTransferEncoding:
            return "invalid Transfer-Encoding header";
        case HttpParseError::kUnsupportedTransferEncoding:
            return "unsupported transfer encoding";
        case HttpParseError::kExpectationFailed:
            return "unsupported Expect header";
        case HttpParseError::kInvalidChunkSize:
            return "invalid chunk size";
        case HttpParseError::kChunkSizeOverflow:
            return "chunk size is too large";
        case HttpParseError::kInvalidChunkExtension:
            return "invalid chunk extension";
        case HttpParseError::kInvalidChunkCrlf:
            return "invalid chunk delimiter";
        case HttpParseError::kInvalidTrailer:
            return "invalid chunk trailer";
    }

    return "invalid HTTP request";
}

std::uint16_t httpParseErrorStatus(HttpParseError error) noexcept {
    switch (error) {
        case HttpParseError::kHeaderTooLarge:
        case HttpParseError::kTooManyHeaders:
            return 431;
        case HttpParseError::kBodyTooLarge:
            return 413;
        case HttpParseError::kUnsupportedMethod:
        case HttpParseError::kUnsupportedTransferEncoding:
            return 501;
        case HttpParseError::kUnsupportedHttpVersion:
            return 505;
        case HttpParseError::kExpectationFailed:
            return 417;
        case HttpParseError::kNone:
        case HttpParseError::kInvalidRequestLine:
        case HttpParseError::kInvalidHeader:
        case HttpParseError::kInvalidRequestTarget:
        case HttpParseError::kMissingHost:
        case HttpParseError::kInvalidHost:
        case HttpParseError::kInvalidContentLength:
        case HttpParseError::kConflictingContentLength:
        case HttpParseError::kInvalidTransferEncoding:
        case HttpParseError::kInvalidChunkSize:
        case HttpParseError::kChunkSizeOverflow:
        case HttpParseError::kInvalidChunkExtension:
        case HttpParseError::kInvalidChunkCrlf:
        case HttpParseError::kInvalidTrailer:
            return 400;
    }

    return 400;
}

void HttpParser::parseHeaders(std::string_view buffer, HttpParseResult& result, std::size_t headerSearchOffset) const noexcept {
    parseRequestHead(buffer, headerSearchOffset, result);
}

void HttpParser::parseBody(std::string_view buffer, HttpParseResult& result) const noexcept {
    if (result.status != HttpParseStatus::kComplete) {
        return;
    }

    const auto fail = [&result](HttpParseError error) noexcept {
        result.request.reset();
        result.status = HttpParseStatus::kError;
        result.error = error;
    };
    const auto needMore = [&result](
        std::size_t headerBytes,
        std::size_t contentLength,
        std::size_t consumedBytes) noexcept {
        result.request.reset();
        result.status = HttpParseStatus::kIncomplete;
        result.headerBytes = headerBytes;
        result.contentLength = contentLength;
        result.consumedBytes = consumedBytes;
    };

    const auto headerBytes = result.headerBytes;
    const auto contentLength = result.contentLength;
    if (buffer.size() < headerBytes) {
        return needMore(0, 0, 0);
    }
    if (result.chunked) {
        const auto chunked = scanHttpChunkedBody(buffer.substr(headerBytes));
        switch (chunked.status) {
            case HttpChunkScanStatus::kComplete:
                result.decodedBodyBytes = chunked.decodedBytes;
                result.consumedBytes = headerBytes + chunked.consumedBytes;
                break;
            case HttpChunkScanStatus::kIncomplete:
                return needMore(headerBytes, 0, 0);
            case HttpChunkScanStatus::kInvalidSize:
                return fail(HttpParseError::kInvalidChunkSize);
            case HttpChunkScanStatus::kSizeOverflow:
                return fail(HttpParseError::kChunkSizeOverflow);
            case HttpChunkScanStatus::kInvalidExtension:
                return fail(HttpParseError::kInvalidChunkExtension);
            case HttpChunkScanStatus::kInvalidCrlf:
                return fail(HttpParseError::kInvalidChunkCrlf);
            case HttpChunkScanStatus::kInvalidTrailer:
                return fail(HttpParseError::kInvalidTrailer);
            case HttpChunkScanStatus::kTooLarge:
                return fail(HttpParseError::kBodyTooLarge);
        }
    } else {
        if (contentLength > kMaxHttpBodyBytes || contentLength > kMaxHttpRequestBytes - headerBytes) {
            return fail(HttpParseError::kBodyTooLarge);
        }
        result.decodedBodyBytes = contentLength;
        result.consumedBytes = headerBytes + contentLength;
    }
    if (result.consumedBytes > kMaxHttpRequestBytes) {
        return fail(HttpParseError::kBodyTooLarge);
    }
    if (buffer.size() < result.consumedBytes) {
        return needMore(headerBytes, contentLength, result.consumedBytes);
    }

    result.request.setBody(
        result.chunked ? std::string_view{} : buffer.substr(headerBytes, contentLength));
}

HttpParseResult HttpParser::parse(std::string_view buffer) const noexcept {
    HttpParseResult result;
    parseRequestHead(buffer, 0, result);
    parseBody(buffer, result);
    return result;
}

}  // namespace ruvia
