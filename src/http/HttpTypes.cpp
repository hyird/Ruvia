#include "ruvia/http/HttpTypes.h"

#include "ruvia/http/HeaderUtils.h"

#include <bit>
#include <charconv>
#include <system_error>

namespace ruvia {

namespace {

bool isTchar(unsigned char value) noexcept {
    if (value >= '0' && value <= '9') {
        return true;
    }
    if (value >= 'A' && value <= 'Z') {
        return true;
    }
    if (value >= 'a' && value <= 'z') {
        return true;
    }
    switch (value) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

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

const std::filesystem::path& detail::fileTokenPath(const FileToken& token) noexcept {
    return token.borrowedPath_ == nullptr ? token.ownedPath_ : *token.borrowedPath_;
}

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

FileToken::FileToken(const FileToken& other)
    : borrowedPath_(other.borrowedPath_) {
    if (other.borrowedPath_ == nullptr) {
        ownedPath_ = other.ownedPath_;
    }
}

FileToken& FileToken::operator=(const FileToken& other) {
    if (this == &other) {
        return *this;
    }

    borrowedPath_ = other.borrowedPath_;
    if (other.borrowedPath_ == nullptr) {
        ownedPath_ = other.ownedPath_;
    } else {
        ownedPath_.clear();
    }
    return *this;
}

bool isValidHttpHeaderName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const auto value : name) {
        if (!isTchar(static_cast<unsigned char>(value))) {
            return false;
        }
    }
    return true;
}

bool isValidHttpHeaderValue(std::string_view value) noexcept {
    for (const auto c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

bool isValidHttpStatusText(std::string_view value) noexcept {
    return isValidHttpHeaderValue(value);
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

MultipartPart::MultipartPart(std::pmr::memory_resource* resource)
    : name(resource), filename(resource), contentType(resource) {}

HttpResponseHeaders::HttpResponseHeaders(std::pmr::memory_resource* resource)
    : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      heap_(resource_) {}

HttpResponseHeaders::~HttpResponseHeaders() {
    clear();
}

HttpResponseHeaders::HttpResponseHeaders(HttpResponseHeaders&& other) noexcept
    : resource_(other.resource_),
      heap_(resource_) {
    moveFrom(std::move(other));
}

HttpResponseHeaders& HttpResponseHeaders::operator=(HttpResponseHeaders&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    clear();
    resource_ = other.resource_;
    heap_ = std::pmr::vector<HttpResponseHeader>(resource_);
    spilled_ = false;
    moveFrom(std::move(other));
    return *this;
}

void HttpResponseHeaders::reserve(std::size_t count) {
    if (count <= kInlineCapacity) {
        return;
    }

    spill();
    heap_.reserve(count);
}

HttpResponseHeader HttpResponseHeaders::makeHeader(
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    const auto total = name.size() + value.size();
    char* bytes = nullptr;
    if (total > 0) {
        bytes = static_cast<char*>(resource_->allocate(total, 1));
        std::memcpy(bytes, name.data(), name.size());
        std::memcpy(bytes + name.size(), value.data(), value.size());
    }
    return HttpResponseHeader{
        .bytes = bytes,
        .nameSize = static_cast<std::uint32_t>(name.size()),
        .valueSize = static_cast<std::uint32_t>(value.size()),
        .knownBit = knownBit};
}

void HttpResponseHeaders::releaseHeader(HttpResponseHeader& header) noexcept {
    if (header.bytes != nullptr) {
        resource_->deallocate(
            const_cast<char*>(header.bytes),
            static_cast<std::size_t>(header.nameSize) + header.valueSize,
            1);
        header.bytes = nullptr;
        header.nameSize = 0;
        header.valueSize = 0;
    }
}

HttpResponseHeader& HttpResponseHeaders::add(
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    if (!spilled_ && size_ == kInlineCapacity) {
        spill();
    }
    const auto header = makeHeader(name, value, knownBit);
    if (!spilled_) {
        auto* target = inlineData() + size_;
        *target = header;
        ++size_;
        return *target;
    }
    heap_.push_back(header);
    return heap_.back();
}

void HttpResponseHeaders::assign(
    HttpResponseHeader& header,
    std::string_view name,
    std::string_view value,
    std::uint32_t knownBit) {
    // Allocate the replacement first so a bad_alloc leaves the slot intact.
    const auto replacement = makeHeader(name, value, knownBit);
    releaseHeader(header);
    header = replacement;
}

HttpResponseHeader* HttpResponseHeaders::inlineData() noexcept {
    return reinterpret_cast<HttpResponseHeader*>(inline_.data());
}

const HttpResponseHeader* HttpResponseHeaders::inlineData() const noexcept {
    return reinterpret_cast<const HttpResponseHeader*>(inline_.data());
}

HttpResponseHeader* HttpResponseHeaders::data() noexcept {
    return spilled_ ? heap_.data() : inlineData();
}

const HttpResponseHeader* HttpResponseHeaders::data() const noexcept {
    return spilled_ ? heap_.data() : inlineData();
}

void HttpResponseHeaders::clear() noexcept {
    auto* items = data();
    const auto count = size();
    for (std::size_t i = 0; i < count; ++i) {
        releaseHeader(items[i]);
    }
    if (spilled_) {
        heap_.clear();
    }
    size_ = 0;
}

void HttpResponseHeaders::spill() {
    if (spilled_) {
        return;
    }

    heap_.reserve(std::max<std::size_t>(kInlineCapacity * 2, size_ + 1));
    auto* items = inlineData();
    for (std::size_t i = 0; i < size_; ++i) {
        heap_.push_back(items[i]);
    }
    size_ = heap_.size();
    spilled_ = true;
}

void HttpResponseHeaders::moveFrom(HttpResponseHeaders&& other) noexcept {
    if (other.spilled_) {
        spilled_ = true;
        heap_ = std::move(other.heap_);
        size_ = heap_.size();
        other.spilled_ = false;
        other.size_ = 0;
        return;
    }

    // Headers are trivially relocatable: a single memcpy transfers ownership
    // of every name/value allocation to this container.
    if (other.size_ > 0) {
        std::memcpy(inline_.data(), other.inline_.data(), other.size_ * sizeof(InlineStorage));
    }
    size_ = other.size_;
    other.size_ = 0;
}

HttpResponse::HttpResponse(std::pmr::memory_resource* resource)
    : statusText_("OK", resource),
      headers_(resource),
      body_(resource) {}

std::pmr::memory_resource* HttpResponse::resource() const noexcept {
    return body_.get_allocator().resource();
}

std::uint16_t HttpResponse::statusCode() const noexcept {
    return statusCode_;
}

std::string_view HttpResponse::statusText() const noexcept {
    return statusText_;
}

const HttpResponseHeaders& HttpResponse::headers() const noexcept {
    return headers_;
}

HttpResponseBodyKind HttpResponse::bodyKind() const noexcept {
    return bodyKind_;
}

std::string_view HttpResponse::bodyBytes() const noexcept {
    if (bodyKind_ == HttpResponseBodyKind::kOwned) {
        return std::string_view(body_.data(), body_.size());
    }
    if (bodyKind_ == HttpResponseBodyKind::kBorrowed ||
        bodyKind_ == HttpResponseBodyKind::kStaticBorrowed) {
        return bodyView_;
    }
    return {};
}

std::size_t HttpResponse::bodySize() const noexcept {
    if (bodyKind_ == HttpResponseBodyKind::kFile && fileBody_) {
        return static_cast<std::size_t>(fileBody_->length);
    }
    return bodyBytes().size();
}

bool HttpResponse::bodyEmpty() const noexcept {
    return bodySize() == 0;
}

bool HttpResponse::hasFileBody() const noexcept {
    return bodyKind_ == HttpResponseBodyKind::kFile && fileBody_.has_value();
}

const FileBody& HttpResponse::fileBody() const {
    if (!fileBody_) {
        throw std::logic_error("response does not contain a file body");
    }
    return *fileBody_;
}

void HttpResponse::setStatus(std::uint16_t statusCode, std::string_view statusText) {
    if (statusCode < 100 || statusCode > 999) {
        throw std::invalid_argument("invalid HTTP status code");
    }
    if (!isValidHttpStatusText(statusText)) {
        throw std::invalid_argument("invalid HTTP status text");
    }
    statusCode_ = statusCode;
    statusText_.assign(statusText.data(), statusText.size());
}

std::uint32_t HttpResponse::classifyKnownHeader(std::string_view name) noexcept {
    switch (name.size()) {
        case 4:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Vary")) return kKnownHeaderVary;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Date")) return kKnownHeaderDate;
            if (detail::httpAsciiEqualsIgnoreCase(name, "ETag")) return kKnownHeaderEtag;
            return 0;
        case 5:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Allow")) return kKnownHeaderAllow;
            return 0;
        case 6:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Server")) return kKnownHeaderServer;
            return 0;
        case 8:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Location")) return kKnownHeaderLocation;
            return 0;
        case 10:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) return kKnownHeaderConnection;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Set-Cookie")) return kKnownHeaderSetCookie;
            return 0;
        case 12:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Type")) return kKnownHeaderContentType;
            return 0;
        case 13:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Cache-Control")) return kKnownHeaderCacheControl;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Accept-Ranges")) return kKnownHeaderAcceptRanges;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Range")) return kKnownHeaderContentRange;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Last-Modified")) return kKnownHeaderLastModified;
            return 0;
        case 14:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Length")) return kKnownHeaderContentLength;
            return 0;
        case 16:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Encoding")) return kKnownHeaderContentEncoding;
            return 0;
        case 17:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) return kKnownHeaderTransferEncoding;
            return 0;
        case 27:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Origin")) return kKnownHeaderAccessControlAllowOrigin;
            return 0;
        case 28:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Methods")) return kKnownHeaderAccessControlAllowMethods;
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Headers")) return kKnownHeaderAccessControlAllowHeaders;
            return 0;
        case 22:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Max-Age")) return kKnownHeaderAccessControlMaxAge;
            return 0;
        case 29:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Expose-Headers")) return kKnownHeaderAccessControlExposeHeaders;
            return 0;
        case 32:
            if (detail::httpAsciiEqualsIgnoreCase(name, "Access-Control-Allow-Credentials")) return kKnownHeaderAccessControlAllowCredentials;
            return 0;
        default:
            return 0;
    }
}

std::size_t HttpResponse::knownHeaderSlot(std::uint32_t bit) noexcept {
    constexpr std::uint32_t knownMask = (1U << kKnownHeaderCount) - 1U;
    if (bit == 0 || (bit & ~knownMask) != 0 || (bit & (bit - 1U)) != 0) {
        return kKnownHeaderCount;
    }
    return static_cast<std::size_t>(std::countr_zero(bit));
}

std::string_view HttpResponse::header(KnownHeaderBit bit) const noexcept {
    const auto slot = knownHeaderSlot(bit);
    if (slot < knownHeaderIndexes_.size()) {
        const auto index = knownHeaderIndexes_[slot];
        if (index >= 0) {
            return headers_.begin()[index].value();
        }
    }
    return {};
}

std::string_view HttpResponse::header(std::string_view name) const noexcept {
    if (const auto bit = classifyKnownHeader(name); bit != 0) {
        return header(static_cast<KnownHeaderBit>(bit));
    }

    for (const auto& header : headers_) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), name)) {
            return header.value();
        }
    }
    return {};
}

void HttpResponse::setHeader(std::string_view key, std::string_view value) {
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto knownBit = classifyKnownHeader(key);
    const auto knownSlot = knownHeaderSlot(knownBit);
    if (knownSlot < knownHeaderIndexes_.size()) {
        const auto index = knownHeaderIndexes_[knownSlot];
        if (index >= 0) {
            headers_.assign(headers_.begin()[index], key, value, knownBit);
            return;
        }
        const auto nextIndex = headers_.size();
        headers_.add(key, value, knownBit);
        knownHeaderBits_ |= knownBit;
        knownHeaderIndexes_[knownSlot] = static_cast<std::int32_t>(nextIndex);
        return;
    }

    for (auto& header : headers_) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), key)) {
            headers_.assign(header, key, value, knownBit);
            return;
        }
    }

    appendHeader(key, value);
}

void HttpResponse::appendHeader(std::string_view key, std::string_view value) {
    if (!isValidHttpHeaderName(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto knownBit = classifyKnownHeader(key);
    const auto index = headers_.size();
    headers_.add(key, value, knownBit);
    if (knownBit != 0) {
        knownHeaderBits_ |= knownBit;
        const auto slot = knownHeaderSlot(knownBit);
        if (slot < knownHeaderIndexes_.size() && knownHeaderIndexes_[slot] < 0) {
            knownHeaderIndexes_[slot] = static_cast<std::int32_t>(index);
        }
    }
}

void HttpResponse::reserveHeaders(std::size_t count) {
    headers_.reserve(count);
}

void HttpResponse::setBodyCopy(std::string_view value) {
    fileBody_.reset();
    bodyView_ = {};
    bodyKind_ = value.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kOwned;
    body_.assign(value.data(), value.size());
}

void HttpResponse::setBodyView(std::string_view value) noexcept {
    fileBody_.reset();
    body_.clear();
    bodyView_ = value;
    bodyKind_ = value.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kBorrowed;
}

void HttpResponse::setBodyStaticView(std::string_view value) noexcept {
    fileBody_.reset();
    body_.clear();
    bodyView_ = value;
    bodyKind_ = value.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kStaticBorrowed;
}

void HttpResponse::setBody(std::pmr::string&& value) {
    fileBody_.reset();
    bodyView_ = {};
    bodyKind_ = value.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kOwned;
    body_ = std::move(value);
}

void HttpResponse::materializeBody() {
    if (bodyKind_ != HttpResponseBodyKind::kBorrowed) {
        return;
    }

    body_.assign(bodyView_.data(), bodyView_.size());
    bodyView_ = {};
    bodyKind_ = body_.empty() ? HttpResponseBodyKind::kEmpty : HttpResponseBodyKind::kOwned;
}

void HttpResponse::setFileBody(FileToken file, std::uint64_t size) {
    setFileBody(std::move(file), size, 0, size);
}

void HttpResponse::setFileBody(FileToken file, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    if (file.empty()) {
        throw std::invalid_argument("file response path must not be empty");
    }
    if (offset > size || length > size - offset) {
        throw std::invalid_argument("file response byte range is outside the file");
    }

    body_.clear();
    bodyView_ = {};
    bodyKind_ = HttpResponseBodyKind::kFile;
    fileBody_ = FileBody{std::move(file), size, offset, length};
}

HttpMethod parseMethod(std::string_view method) {
    switch (method.size()) {
        case 3:
            if (method == "GET") {
                return HttpMethod::kGet;
            }
            if (method == "PUT") {
                return HttpMethod::kPut;
            }
            break;
        case 4:
            if (method == "POST") {
                return HttpMethod::kPost;
            }
            if (method == "HEAD") {
                return HttpMethod::kHead;
            }
            break;
        case 5:
            if (method == "PATCH") {
                return HttpMethod::kPatch;
            }
            break;
        case 6:
            if (method == "DELETE") {
                return HttpMethod::kDelete;
            }
            break;
        case 7:
            if (method == "OPTIONS") {
                return HttpMethod::kOptions;
            }
            break;
        default:
            break;
    }
    return HttpMethod::kUnknown;
}

std::string_view methodName(HttpMethod method) {
    switch (method) {
        case HttpMethod::kGet:
            return "GET";
        case HttpMethod::kPost:
            return "POST";
        case HttpMethod::kPut:
            return "PUT";
        case HttpMethod::kDelete:
            return "DELETE";
        case HttpMethod::kPatch:
            return "PATCH";
        case HttpMethod::kHead:
            return "HEAD";
        case HttpMethod::kOptions:
            return "OPTIONS";
        case HttpMethod::kUnknown:
        default:
            return "UNKNOWN";
    }
}

}  // namespace ruvia
