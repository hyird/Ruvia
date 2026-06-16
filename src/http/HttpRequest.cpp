#include "ruvia/http/HttpTypes.h"

#include "ruvia/http/HeaderUtils.h"

#include <charconv>
#include <system_error>

namespace ruvia {
namespace {

template <typename T>
std::optional<T> parseInteger(std::optional<std::string_view> input) noexcept {
    if (!input || input->empty()) {
        return std::nullopt;
    }

    T value{};
    const auto* begin = input->data();
    const auto* end = begin + input->size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }

    return value;
}

}  // namespace

std::optional<std::pmr::string> RequestValue::toString() const {
    if (!value_) {
        return std::nullopt;
    }

    if (decodeMode_ != DecodeMode::kNone) {
        const auto mode = decodeMode_ == DecodeMode::kForm
            ? detail::UrlDecodeMode::kForm
            : detail::UrlDecodeMode::kPercent;
        if (detail::hasUrlEncoding(*value_, mode)) {
            return detail::decodeUrlComponentToString(*value_, resource(), mode);
        }
    }

    return std::pmr::string(value_->data(), value_->size(), resource());
}

std::optional<bool> RequestValue::toBool() const noexcept {
    if (!value_) {
        return std::nullopt;
    }

    if (*value_ == "1" || detail::httpAsciiEqualsIgnoreCase(*value_, "true")) {
        return true;
    }
    if (*value_ == "0" || detail::httpAsciiEqualsIgnoreCase(*value_, "false")) {
        return false;
    }
    return std::nullopt;
}

std::optional<int> RequestValue::toInt() const noexcept {
    return parseInteger<int>(value_);
}

std::optional<unsigned int> RequestValue::toUInt() const noexcept {
    return parseInteger<unsigned int>(value_);
}

std::optional<std::int32_t> RequestValue::toInt32() const noexcept {
    return parseInteger<std::int32_t>(value_);
}

std::optional<std::uint32_t> RequestValue::toUInt32() const noexcept {
    return parseInteger<std::uint32_t>(value_);
}

std::optional<std::int64_t> RequestValue::toInt64() const noexcept {
    return parseInteger<std::int64_t>(value_);
}

std::optional<std::uint64_t> RequestValue::toUInt64() const noexcept {
    return parseInteger<std::uint64_t>(value_);
}

std::optional<std::pmr::string> HttpRequest::decodedPath() const {
    if (!detail::hasUrlEncoding(path_, detail::UrlDecodeMode::kPercent)) {
        return std::pmr::string(path_.data(), path_.size(), resource());
    }
    return detail::decodeUrlComponentToString(path_, resource(), detail::UrlDecodeMode::kPercent);
}

std::string_view HttpRequest::header(KnownHeader name) const noexcept {
    const auto hasKnown = [this](KnownRequestHeaderBit bit) noexcept {
        return (knownHeaderBits_ & static_cast<std::uint32_t>(bit)) != 0;
    };
    switch (name) {
        case KnownHeader::kAccept:
            return hasKnown(kKnownHeaderAccept) ? acceptHeader_ : std::string_view{};
        case KnownHeader::kAcceptEncoding:
            return hasKnown(kKnownHeaderAcceptEncoding) ? acceptEncodingHeader_ : std::string_view{};
        case KnownHeader::kAccessControlRequestHeaders:
            return hasKnown(kKnownHeaderAccessControlRequestHeaders) ? accessControlRequestHeadersHeader_ : std::string_view{};
        case KnownHeader::kAccessControlRequestMethod:
            return hasKnown(kKnownHeaderAccessControlRequestMethod) ? accessControlRequestMethodHeader_ : std::string_view{};
        case KnownHeader::kAuthorization:
            return hasKnown(kKnownHeaderAuthorization) ? authorizationHeader_ : std::string_view{};
        case KnownHeader::kConnection:
            return hasKnown(kKnownHeaderConnection) ? connectionHeader_ : std::string_view{};
        case KnownHeader::kContentLength:
            return hasKnown(kKnownHeaderContentLength) ? contentLengthHeader_ : std::string_view{};
        case KnownHeader::kContentType:
            return hasKnown(kKnownHeaderContentType) ? contentTypeHeader_ : std::string_view{};
        case KnownHeader::kCookie:
            return hasKnown(kKnownHeaderCookie) ? cookieHeader_ : std::string_view{};
        case KnownHeader::kExpect:
            return hasKnown(kKnownHeaderExpect) ? expectHeader_ : std::string_view{};
        case KnownHeader::kHost:
            return hasKnown(kKnownHeaderHost) ? hostHeader_ : std::string_view{};
        case KnownHeader::kIfMatch:
            return hasKnown(kKnownHeaderIfMatch) ? ifMatchHeader_ : std::string_view{};
        case KnownHeader::kIfModifiedSince:
            return hasKnown(kKnownHeaderIfModifiedSince) ? ifModifiedSinceHeader_ : std::string_view{};
        case KnownHeader::kIfNoneMatch:
            return hasKnown(kKnownHeaderIfNoneMatch) ? ifNoneMatchHeader_ : std::string_view{};
        case KnownHeader::kIfRange:
            return hasKnown(kKnownHeaderIfRange) ? ifRangeHeader_ : std::string_view{};
        case KnownHeader::kIfUnmodifiedSince:
            return hasKnown(kKnownHeaderIfUnmodifiedSince) ? ifUnmodifiedSinceHeader_ : std::string_view{};
        case KnownHeader::kOrigin:
            return hasKnown(kKnownHeaderOrigin) ? originHeader_ : std::string_view{};
        case KnownHeader::kRange:
            return hasKnown(kKnownHeaderRange) ? rangeHeader_ : std::string_view{};
        case KnownHeader::kSecWebSocketKey:
            return hasKnown(kKnownHeaderSecWebSocketKey) ? secWebSocketKeyHeader_ : std::string_view{};
        case KnownHeader::kSecWebSocketProtocol:
            return hasKnown(kKnownHeaderSecWebSocketProtocol) ? secWebSocketProtocolHeader_ : std::string_view{};
        case KnownHeader::kSecWebSocketVersion:
            return hasKnown(kKnownHeaderSecWebSocketVersion) ? secWebSocketVersionHeader_ : std::string_view{};
        case KnownHeader::kTransferEncoding:
            return hasKnown(kKnownHeaderTransferEncoding) ? transferEncodingHeader_ : std::string_view{};
        case KnownHeader::kUpgrade:
            return hasKnown(kKnownHeaderUpgrade) ? upgradeHeader_ : std::string_view{};
        case KnownHeader::kUserAgent:
            return hasKnown(kKnownHeaderUserAgent) ? userAgentHeader_ : std::string_view{};
    }
    return {};
}

std::string_view HttpRequest::header(std::string_view name) const noexcept {
    const auto hasKnown = [this](KnownRequestHeaderBit bit) noexcept {
        return (knownHeaderBits_ & static_cast<std::uint32_t>(bit)) != 0;
    };
    switch (name.size()) {
        case 4:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Host")) {
                return hasKnown(kKnownHeaderHost) ? hostHeader_ : std::string_view{};
            }
            break;
        case 6:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Accept")) {
                return hasKnown(kKnownHeaderAccept) ? acceptHeader_ : std::string_view{};
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Expect")) {
                return hasKnown(kKnownHeaderExpect) ? expectHeader_ : std::string_view{};
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Cookie")) {
                return hasKnown(kKnownHeaderCookie) ? cookieHeader_ : std::string_view{};
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Origin")) {
                return hasKnown(kKnownHeaderOrigin) ? originHeader_ : std::string_view{};
            }
            break;
        case 7:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
                return hasKnown(kKnownHeaderUpgrade) ? upgradeHeader_ : std::string_view{};
            }
            break;
        case 5:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Range")) {
                return hasKnown(kKnownHeaderRange) ? rangeHeader_ : std::string_view{};
            }
            break;
        case 8:
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-Match")) {
                return hasKnown(kKnownHeaderIfMatch) ? ifMatchHeader_ : std::string_view{};
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-Range")) {
                return hasKnown(kKnownHeaderIfRange) ? ifRangeHeader_ : std::string_view{};
            }
            break;
        case 10:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) {
                return hasKnown(kKnownHeaderConnection) ? connectionHeader_ : std::string_view{};
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "User-Agent")) {
                return hasKnown(kKnownHeaderUserAgent) ? userAgentHeader_ : std::string_view{};
            }
            break;
        case 12:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Type")) {
                return hasKnown(kKnownHeaderContentType) ? contentTypeHeader_ : std::string_view{};
            }
            break;
        case 13:
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-None-Match")) {
                return hasKnown(kKnownHeaderIfNoneMatch) ? ifNoneMatchHeader_ : std::string_view{};
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Authorization")) {
                return hasKnown(kKnownHeaderAuthorization) ? authorizationHeader_ : std::string_view{};
            }
            break;
        case 14:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
                return hasKnown(kKnownHeaderContentLength) ? contentLengthHeader_ : std::string_view{};
            }
            break;
        case 15:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Accept-Encoding")) {
                return hasKnown(kKnownHeaderAcceptEncoding) ? acceptEncodingHeader_ : std::string_view{};
            }
            break;
        case 17:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
                return hasKnown(kKnownHeaderTransferEncoding) ? transferEncodingHeader_ : std::string_view{};
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-Modified-Since")) {
                return hasKnown(kKnownHeaderIfModifiedSince) ? ifModifiedSinceHeader_ : std::string_view{};
            }
            if (detail::httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Key")) {
                return hasKnown(kKnownHeaderSecWebSocketKey) ? secWebSocketKeyHeader_ : std::string_view{};
            }
            break;
        case 19:
            if (detail::httpAsciiEqualsIgnoreCase(name, "If-Unmodified-Since")) {
                return hasKnown(kKnownHeaderIfUnmodifiedSince) ? ifUnmodifiedSinceHeader_ : std::string_view{};
            }
            break;
        case 21:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Version")) {
                return hasKnown(kKnownHeaderSecWebSocketVersion) ? secWebSocketVersionHeader_ : std::string_view{};
            }
            break;
        case 22:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Protocol")) {
                return hasKnown(kKnownHeaderSecWebSocketProtocol) ? secWebSocketProtocolHeader_ : std::string_view{};
            }
            break;
        case 29:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Request-Method")) {
                return hasKnown(kKnownHeaderAccessControlRequestMethod) ? accessControlRequestMethodHeader_ : std::string_view{};
            }
            break;
        case 30:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Request-Headers")) {
                return hasKnown(kKnownHeaderAccessControlRequestHeaders) ? accessControlRequestHeadersHeader_ : std::string_view{};
            }
            break;
        default:
            break;
    }

    for (std::size_t i = 0; i < headerCount_; ++i) {
        if (detail::httpAsciiEqualsIgnoreCase(headers_[i].name, name)) {
            return headers_[i].value;
        }
    }

    return {};
}

QueryValue HttpRequest::query(std::string_view name) const noexcept {
    auto input = queryString_;
    while (!input.empty()) {
        const auto ampersand = input.find('&');
        const auto pair = ampersand == std::string_view::npos ? input : input.substr(0, ampersand);
        const auto equals = pair.find('=');
        const auto key = equals == std::string_view::npos ? pair : pair.substr(0, equals);
        if (detail::urlComponentEquals(key, name, detail::UrlDecodeMode::kForm)) {
            if (equals == std::string_view::npos) {
                return QueryValue(std::string_view{}, resource(), RequestValue::DecodeMode::kForm);
            }
            return QueryValue(pair.substr(equals + 1), resource(), RequestValue::DecodeMode::kForm);
        }

        if (ampersand == std::string_view::npos) {
            break;
        }
        input.remove_prefix(ampersand + 1);
    }

    return QueryValue(std::nullopt, resource(), RequestValue::DecodeMode::kForm);
}

std::optional<std::string_view> HttpRequest::cookie(std::string_view name) const noexcept {
    auto input = header(KnownHeader::kCookie);
    while (!input.empty()) {
        const auto semicolon = input.find(';');
        const auto part = detail::httpTrimOws(semicolon == std::string_view::npos ? input : input.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos) {
            const auto key = detail::httpTrimOws(part.substr(0, equals));
            const auto value = detail::httpTrimOws(part.substr(equals + 1));
            if (key == name) {
                return value;
            }
        }

        if (semicolon == std::string_view::npos) {
            break;
        }
        input.remove_prefix(semicolon + 1);
    }

    return std::nullopt;
}

std::pmr::memory_resource* HttpRequest::resource() const noexcept {
    return resource_ == nullptr ? std::pmr::get_default_resource() : resource_;
}

}  // namespace ruvia
