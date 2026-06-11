#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/UrlEncoding.h"

namespace ruvia {

class Context;
class FileToken;
class HttpParser;
class StaticRoot;

enum class HttpMethod {
    kGet,
    kPost,
    kPut,
    kDelete,
    kPatch,
    kHead,
    kOptions,
    kUnknown
};

inline constexpr HttpMethod Get = HttpMethod::kGet;
inline constexpr HttpMethod Post = HttpMethod::kPost;
inline constexpr HttpMethod Put = HttpMethod::kPut;
inline constexpr HttpMethod Delete = HttpMethod::kDelete;
inline constexpr HttpMethod Patch = HttpMethod::kPatch;
inline constexpr HttpMethod Head = HttpMethod::kHead;
inline constexpr HttpMethod Options = HttpMethod::kOptions;

enum class RequestBodyMode {
    kBuffered,
    kStream
};

enum class ResponseBodyMode {
    kBuffered,
    kStream,
    kSse,
    kWebSocket
};

inline constexpr std::size_t kMaxRequestHeaders = 64;

struct HttpHeaderView {
    std::string_view name;
    std::string_view value;
};

struct MultipartPart {
    std::pmr::string name;
    std::pmr::string filename;
    std::pmr::string contentType;
    std::string_view body;

    explicit MultipartPart(std::pmr::memory_resource* resource = std::pmr::get_default_resource());
};

class RequestValue final {
public:
    enum class DecodeMode : std::uint8_t {
        kNone,
        kPercent,
        kForm
    };

    RequestValue() = default;
    RequestValue(
        std::optional<std::string_view> value,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
        DecodeMode decodeMode = DecodeMode::kNone) noexcept
        : value_(value),
          resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
          decodeMode_(decodeMode) {}

    [[nodiscard]] bool exists() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] std::optional<std::string_view> toStringView() const noexcept {
        return value_;
    }

    [[nodiscard]] std::optional<std::pmr::string> toString() const;
    [[nodiscard]] std::optional<bool> toBool() const noexcept;
    [[nodiscard]] std::optional<int> toInt() const noexcept;
    [[nodiscard]] std::optional<unsigned int> toUInt() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> toInt32() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> toUInt32() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> toInt64() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> toUInt64() const noexcept;

private:
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_ == nullptr ? std::pmr::get_default_resource() : resource_;
    }

    std::optional<std::string_view> value_;
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
    DecodeMode decodeMode_{DecodeMode::kNone};
};

using QueryValue = RequestValue;
using ParamValue = RequestValue;

namespace detail {

class HttpServer;
const std::filesystem::path& fileTokenPath(const ruvia::FileToken& token) noexcept;

}  // namespace detail

class FileToken final {
public:
    FileToken() = default;
    FileToken(const FileToken& other);
    FileToken& operator=(const FileToken& other);
    FileToken(FileToken&&) noexcept = default;
    FileToken& operator=(FileToken&&) noexcept = default;

    [[nodiscard]] bool empty() const noexcept {
        return detail::fileTokenPath(*this).empty();
    }

private:
    friend class Context;
    friend class StaticRoot;
    friend class HttpResponse;
    friend const std::filesystem::path& detail::fileTokenPath(const FileToken& token) noexcept;

    [[nodiscard]] static FileToken borrow(const FileToken& token) noexcept {
        FileToken borrowed;
        borrowed.borrowedPath_ = token.borrowedPath_ == nullptr ? &token.ownedPath_ : token.borrowedPath_;
        return borrowed;
    }

    explicit FileToken(std::filesystem::path path) : ownedPath_(std::move(path)) {}

    std::filesystem::path ownedPath_;
    const std::filesystem::path* borrowedPath_{nullptr};
};

class HttpRequest final {
public:
    enum class KnownHeader : std::uint8_t {
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
        kUserAgent,
    };

    [[nodiscard]] HttpMethod method() const noexcept {
        return method_;
    }

    [[nodiscard]] std::string_view target() const noexcept {
        return target_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::optional<std::pmr::string> decodedPath() const;

    [[nodiscard]] std::string_view queryString() const noexcept {
        return queryString_;
    }

    [[nodiscard]] std::string_view httpVersion() const noexcept {
        return httpVersion_;
    }

    [[nodiscard]] std::span<const HttpHeaderView> headers() const noexcept {
        return std::span<const HttpHeaderView>(headers_.data(), headerCount_);
    }

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view header(KnownHeader name) const noexcept;
    [[nodiscard]] QueryValue query(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

    [[nodiscard]] std::string_view remoteAddress() const noexcept {
        return remoteAddress_;
    }

private:
    friend class Context;
    friend class HttpParser;
    friend class detail::HttpServer;

    enum KnownRequestHeaderBit : std::uint32_t {
        kKnownHeaderAccept                      = 1U << 0,
        kKnownHeaderAcceptEncoding              = 1U << 1,
        kKnownHeaderAccessControlRequestHeaders = 1U << 2,
        kKnownHeaderAccessControlRequestMethod  = 1U << 3,
        kKnownHeaderConnection                  = 1U << 4,
        kKnownHeaderContentLength               = 1U << 5,
        kKnownHeaderContentType                 = 1U << 6,
        kKnownHeaderCookie                      = 1U << 7,
        kKnownHeaderExpect                      = 1U << 8,
        kKnownHeaderHost                        = 1U << 9,
        kKnownHeaderIfMatch                     = 1U << 10,
        kKnownHeaderIfModifiedSince             = 1U << 11,
        kKnownHeaderIfNoneMatch                 = 1U << 12,
        kKnownHeaderIfRange                     = 1U << 13,
        kKnownHeaderIfUnmodifiedSince           = 1U << 14,
        kKnownHeaderOrigin                      = 1U << 15,
        kKnownHeaderRange                       = 1U << 16,
        kKnownHeaderSecWebSocketKey             = 1U << 17,
        kKnownHeaderSecWebSocketProtocol        = 1U << 18,
        kKnownHeaderSecWebSocketVersion         = 1U << 19,
        kKnownHeaderTransferEncoding            = 1U << 20,
        kKnownHeaderUpgrade                     = 1U << 21,
        kKnownHeaderAuthorization               = 1U << 22,
        kKnownHeaderUserAgent                   = 1U << 23,
    };

    void setMethod(HttpMethod method) noexcept {
        method_ = method;
    }

    void setTarget(std::string_view target) noexcept {
        target_ = target;
    }

    void setPath(std::string_view path) noexcept {
        path_ = path;
    }

    void setQueryString(std::string_view queryString) noexcept {
        queryString_ = queryString;
    }

    void setHttpVersion(std::string_view httpVersion) noexcept {
        httpVersion_ = httpVersion;
    }

    bool addHeader(HttpHeaderView header) noexcept {
        if (headerCount_ == kMaxRequestHeaders) {
            return false;
        }
        headers_[headerCount_++] = header;
        return true;
    }

    void setBody(std::string_view body) noexcept {
        body_ = body;
    }

    void setConnectionHeader(std::string_view value) noexcept {
        connectionHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderConnection;
    }

    void setAuthorizationHeader(std::string_view value) noexcept {
        authorizationHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderAuthorization;
    }

    void setHostHeader(std::string_view value) noexcept {
        hostHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderHost;
    }

    void setContentLengthHeader(std::string_view value) noexcept {
        contentLengthHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderContentLength;
    }

    void setTransferEncodingHeader(std::string_view value) noexcept {
        transferEncodingHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderTransferEncoding;
    }

    void setExpectHeader(std::string_view value) noexcept {
        expectHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderExpect;
    }

    void setContentTypeHeader(std::string_view value) noexcept {
        contentTypeHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderContentType;
    }

    void setCookieHeader(std::string_view value) noexcept {
        cookieHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderCookie;
    }

    void setOriginHeader(std::string_view value) noexcept {
        originHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderOrigin;
    }

    void setAccessControlRequestMethodHeader(std::string_view value) noexcept {
        accessControlRequestMethodHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderAccessControlRequestMethod;
    }

    void setAccessControlRequestHeadersHeader(std::string_view value) noexcept {
        accessControlRequestHeadersHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderAccessControlRequestHeaders;
    }

    void setAcceptEncodingHeader(std::string_view value) noexcept {
        acceptEncodingHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderAcceptEncoding;
    }

    void setAcceptHeader(std::string_view value) noexcept {
        acceptHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderAccept;
    }

    void setRangeHeader(std::string_view value) noexcept {
        rangeHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderRange;
    }

    void setIfMatchHeader(std::string_view value) noexcept {
        ifMatchHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderIfMatch;
    }

    void setIfNoneMatchHeader(std::string_view value) noexcept {
        ifNoneMatchHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderIfNoneMatch;
    }

    void setIfModifiedSinceHeader(std::string_view value) noexcept {
        ifModifiedSinceHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderIfModifiedSince;
    }

    void setIfUnmodifiedSinceHeader(std::string_view value) noexcept {
        ifUnmodifiedSinceHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderIfUnmodifiedSince;
    }

    void setIfRangeHeader(std::string_view value) noexcept {
        ifRangeHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderIfRange;
    }

    void setUpgradeHeader(std::string_view value) noexcept {
        upgradeHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderUpgrade;
    }

    void setSecWebSocketKeyHeader(std::string_view value) noexcept {
        secWebSocketKeyHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderSecWebSocketKey;
    }

    void setSecWebSocketVersionHeader(std::string_view value) noexcept {
        secWebSocketVersionHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderSecWebSocketVersion;
    }

    void setSecWebSocketProtocolHeader(std::string_view value) noexcept {
        secWebSocketProtocolHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderSecWebSocketProtocol;
    }

    void setUserAgentHeader(std::string_view value) noexcept {
        userAgentHeader_ = value;
        knownHeaderBits_ |= kKnownHeaderUserAgent;
    }

    void setResource(std::pmr::memory_resource* resource) noexcept {
        resource_ = resource;
    }

    void setRemoteAddress(std::string_view remoteAddress) noexcept {
        remoteAddress_ = remoteAddress;
    }

    // Resets to the default-constructed state without touching headers_
    // beyond headerCount_; entries past the count are unreachable, so the
    // parser can reuse one request object per connection instead of
    // zero-initializing the 2KB header table on every parse attempt.
    void reset() noexcept {
        method_ = HttpMethod::kUnknown;
        target_ = {};
        path_ = {};
        queryString_ = {};
        httpVersion_ = "HTTP/1.1";
        headerCount_ = 0;
        knownHeaderBits_ = 0;
        body_ = {};
        connectionHeader_ = {};
        authorizationHeader_ = {};
        hostHeader_ = {};
        contentLengthHeader_ = {};
        transferEncodingHeader_ = {};
        expectHeader_ = {};
        contentTypeHeader_ = {};
        cookieHeader_ = {};
        originHeader_ = {};
        accessControlRequestMethodHeader_ = {};
        accessControlRequestHeadersHeader_ = {};
        acceptEncodingHeader_ = {};
        acceptHeader_ = {};
        rangeHeader_ = {};
        ifMatchHeader_ = {};
        ifNoneMatchHeader_ = {};
        ifModifiedSinceHeader_ = {};
        ifUnmodifiedSinceHeader_ = {};
        ifRangeHeader_ = {};
        upgradeHeader_ = {};
        secWebSocketKeyHeader_ = {};
        secWebSocketVersionHeader_ = {};
        secWebSocketProtocolHeader_ = {};
        userAgentHeader_ = {};
        remoteAddress_ = {};
        resource_ = nullptr;
    }

    HttpMethod method_{HttpMethod::kUnknown};
    std::string_view target_;
    std::string_view path_;
    std::string_view queryString_;
    std::string_view httpVersion_{"HTTP/1.1"};
    std::array<HttpHeaderView, kMaxRequestHeaders> headers_{};
    std::size_t headerCount_{0};
    std::uint32_t knownHeaderBits_{0};
    std::string_view body_;
    std::string_view connectionHeader_;
    std::string_view authorizationHeader_;
    std::string_view hostHeader_;
    std::string_view contentLengthHeader_;
    std::string_view transferEncodingHeader_;
    std::string_view expectHeader_;
    std::string_view contentTypeHeader_;
    std::string_view cookieHeader_;
    std::string_view originHeader_;
    std::string_view accessControlRequestMethodHeader_;
    std::string_view accessControlRequestHeadersHeader_;
    std::string_view acceptEncodingHeader_;
    std::string_view acceptHeader_;
    std::string_view rangeHeader_;
    std::string_view ifMatchHeader_;
    std::string_view ifNoneMatchHeader_;
    std::string_view ifModifiedSinceHeader_;
    std::string_view ifUnmodifiedSinceHeader_;
    std::string_view ifRangeHeader_;
    std::string_view upgradeHeader_;
    std::string_view secWebSocketKeyHeader_;
    std::string_view secWebSocketVersionHeader_;
    std::string_view secWebSocketProtocolHeader_;
    std::string_view userAgentHeader_;
    std::string_view remoteAddress_;
    std::pmr::memory_resource* resource_{nullptr};
};

struct HttpResponseHeader {
    // name bytes followed by value bytes in one allocation owned by the
    // enclosing HttpResponseHeaders' resource. Trivially relocatable so
    // HttpResponse moves through the middleware chain stay memcpy-cheap, and
    // name()/value() views remain stable across container moves.
    const char* bytes{nullptr};
    std::uint32_t nameSize{0};
    std::uint32_t valueSize{0};
    std::uint32_t knownBit{0};

    [[nodiscard]] std::string_view name() const noexcept {
        return {bytes, nameSize};
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return {bytes + nameSize, valueSize};
    }
};

static_assert(std::is_trivially_copyable_v<HttpResponseHeader>);

class HttpResponseHeaders final {
public:
    using value_type = HttpResponseHeader;
    using iterator = HttpResponseHeader*;
    using const_iterator = const HttpResponseHeader*;

    explicit HttpResponseHeaders(std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~HttpResponseHeaders();

    HttpResponseHeaders(const HttpResponseHeaders&) = delete;
    HttpResponseHeaders& operator=(const HttpResponseHeaders&) = delete;
    HttpResponseHeaders(HttpResponseHeaders&& other) noexcept;
    HttpResponseHeaders& operator=(HttpResponseHeaders&& other) noexcept;

    HttpResponseHeader& add(std::string_view name, std::string_view value, std::uint32_t knownBit = 0);
    void assign(HttpResponseHeader& header, std::string_view name, std::string_view value, std::uint32_t knownBit);

    void reserve(std::size_t count);

    [[nodiscard]] iterator begin() noexcept {
        return data();
    }

    [[nodiscard]] iterator end() noexcept {
        return data() + size();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return data() + size();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return begin();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return end();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return spilled_ ? heap_.size() : size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

private:
    static constexpr std::size_t kInlineCapacity = 8;
    struct InlineStorage {
        alignas(HttpResponseHeader) std::byte bytes[sizeof(HttpResponseHeader)];
    };

    [[nodiscard]] HttpResponseHeader* inlineData() noexcept;
    [[nodiscard]] const HttpResponseHeader* inlineData() const noexcept;
    [[nodiscard]] HttpResponseHeader* data() noexcept;
    [[nodiscard]] const HttpResponseHeader* data() const noexcept;
    [[nodiscard]] HttpResponseHeader makeHeader(std::string_view name, std::string_view value, std::uint32_t knownBit);
    void releaseHeader(HttpResponseHeader& header) noexcept;
    void clear() noexcept;
    void spill();
    void moveFrom(HttpResponseHeaders&& other) noexcept;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<HttpResponseHeader> heap_;
    std::array<InlineStorage, kInlineCapacity> inline_;
    std::size_t size_{0};
    bool spilled_{false};
};

struct FileBody {
    FileToken file;
    std::uint64_t size{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

enum class HttpResponseBodyKind : std::uint8_t {
    kEmpty,
    kBorrowed,
    kStaticBorrowed,
    kOwned,
    kFile
};

class HttpResponse final {
public:
    enum KnownHeaderBit : std::uint32_t {
        kKnownHeaderContentLength    = 1U << 0,
        kKnownHeaderContentEncoding  = 1U << 1,
        kKnownHeaderContentType      = 1U << 2,
        kKnownHeaderConnection       = 1U << 3,
        kKnownHeaderVary             = 1U << 4,
        kKnownHeaderDate             = 1U << 5,
        kKnownHeaderServer           = 1U << 6,
        kKnownHeaderCacheControl     = 1U << 7,
        kKnownHeaderTransferEncoding = 1U << 8,
        kKnownHeaderAllow            = 1U << 9,
        kKnownHeaderAccessControlAllowOrigin      = 1U << 10,
        kKnownHeaderAccessControlAllowCredentials = 1U << 11,
        kKnownHeaderAccessControlAllowMethods     = 1U << 12,
        kKnownHeaderAccessControlAllowHeaders     = 1U << 13,
        kKnownHeaderAccessControlMaxAge           = 1U << 14,
        kKnownHeaderAccessControlExposeHeaders    = 1U << 15,
        kKnownHeaderAcceptRanges                  = 1U << 16,
        kKnownHeaderContentRange                  = 1U << 17,
        kKnownHeaderEtag                          = 1U << 18,
        kKnownHeaderLastModified                  = 1U << 19,
        kKnownHeaderLocation                      = 1U << 20,
        kKnownHeaderSetCookie                     = 1U << 21,
    };
    static constexpr std::size_t kKnownHeaderCount = 22;

    explicit HttpResponse(std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] std::uint16_t statusCode() const noexcept;
    [[nodiscard]] std::string_view statusText() const noexcept;
    [[nodiscard]] const HttpResponseHeaders& headers() const noexcept;
    [[nodiscard]] HttpResponseBodyKind bodyKind() const noexcept;
    [[nodiscard]] std::string_view bodyBytes() const noexcept;
    [[nodiscard]] std::size_t bodySize() const noexcept;
    [[nodiscard]] bool bodyEmpty() const noexcept;
    [[nodiscard]] bool hasFileBody() const noexcept;
    [[nodiscard]] const FileBody& fileBody() const;
    [[nodiscard]] std::uint32_t knownHeaderBits() const noexcept { return knownHeaderBits_; }
    [[nodiscard]] bool hasKnownHeader(KnownHeaderBit bit) const noexcept {
        return (knownHeaderBits_ & static_cast<std::uint32_t>(bit)) != 0;
    }
    [[nodiscard]] std::string_view header(KnownHeaderBit bit) const noexcept;
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    void setStatus(std::uint16_t statusCode, std::string_view statusText);
    void setHeader(std::string_view key, std::string_view value);
    void appendHeader(std::string_view key, std::string_view value);
    void reserveHeaders(std::size_t count);
    void setBodyCopy(std::string_view value);
    void setBodyView(std::string_view value) noexcept;
    void setBody(std::pmr::string&& value);
    void materializeBody();

    [[nodiscard]] static std::uint32_t classifyKnownHeader(std::string_view name) noexcept;

private:
    friend class Context;

    void setBodyStaticView(std::string_view value) noexcept;
    void setFileBody(FileToken file, std::uint64_t size);
    void setFileBody(FileToken file, std::uint64_t size, std::uint64_t offset, std::uint64_t length);
    [[nodiscard]] static std::size_t knownHeaderSlot(std::uint32_t bit) noexcept;

    std::uint16_t statusCode_{200};
    std::uint32_t knownHeaderBits_{0};
    std::array<std::int32_t, kKnownHeaderCount> knownHeaderIndexes_{
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1};
    std::pmr::string statusText_;
    HttpResponseHeaders headers_;
    std::pmr::string body_;
    std::string_view bodyView_;
    HttpResponseBodyKind bodyKind_{HttpResponseBodyKind::kEmpty};
    std::optional<FileBody> fileBody_;
};

HttpMethod parseMethod(std::string_view method);
std::string_view methodName(HttpMethod method);
[[nodiscard]] bool isValidHttpHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpStatusText(std::string_view value) noexcept;

}  // namespace ruvia
