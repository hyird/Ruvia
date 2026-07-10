#include "ruvia/http/Context.h"

#include "ruvia/web/detail/CookieSignature.h"
#include "ruvia/http/detail/CookieValidation.h"
#include "ruvia/http/detail/HttpImfFixdate.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/HttpResponseHeadersAccess.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/detail/HttpNumberFormat.h"
#include "ruvia/http/detail/ResponseHeaderIndexCache.h"
#include "ruvia/http/detail/Hex.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia {

namespace {

[[nodiscard]] std::string_view byteBodyView(std::span<const std::byte> body) noexcept {
    return body.empty()
        ? std::string_view{}
        : std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
}

[[nodiscard]] bool responseHasHeaderValue(
    const HttpResponse& response,
    std::string_view name,
    std::string_view value) noexcept {
    for (const auto& header : response.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), name) &&
            header.value() == value) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool responseHasHeaderName(
    const HttpResponse& response,
    std::string_view name) noexcept {
    for (const auto& header : response.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), name)) {
            return true;
        }
    }
    return false;
}

void mergeResponseSlotHeaders(HttpResponse& response, const HttpResponse& slot) {
    const auto slotHeaderCount = slot.headers().size();
    if (slotHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + slotHeaderCount);
    }
    for (const auto& header : slot.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie || detail::responseHeaderAppend(header)) {
            if (!responseHasHeaderValue(response, name, value)) {
                detail::appendResponseHeaderValidated(response, name, value, knownBit);
            }
        } else if (!responseHasHeaderName(response, name)) {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
}

void assignResponseSlotHeaders(HttpResponse& response, const HttpResponse& slot) {
    const auto slotHeaderCount = slot.headers().size();
    if (slotHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + slotHeaderCount);
    }

    bool removedAssignedSetCookie = false;
    for (const auto& header : slot.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        if (knownBit == detail::kResponseHeaderContentType) {
            continue;
        }
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie) {
            if (!removedAssignedSetCookie) {
                response.header("Set-Cookie", std::nullopt);
                removedAssignedSetCookie = true;
            }
            detail::appendResponseHeaderValidated(response, name, value, knownBit);
        } else if (detail::responseHeaderAppend(header)) {
            detail::appendResponseHeaderValidated(response, name, value, knownBit);
        } else {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
}

[[nodiscard]] bool redirectLocationNeedsEncoding(std::string_view location) noexcept {
    for (const auto ch : location) {
        if (static_cast<unsigned char>(ch) >= 0x80) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool encodeUriKeepsByte(unsigned char ch) noexcept {
    if ((ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9')) {
        return true;
    }

    switch (ch) {
        case ';':
        case ',':
        case '/':
        case '?':
        case ':':
        case '@':
        case '&':
        case '=':
        case '+':
        case '$':
        case '-':
        case '_':
        case '.':
        case '!':
        case '~':
        case '*':
        case '\'':
        case '(':
        case ')':
        case '#':
            return true;
        default:
            return false;
    }
}

void appendPercentEncodedByte(std::pmr::string& output, unsigned char ch) {
    output.push_back('%');
    output.push_back(detail::upperHexDigit(ch >> 4));
    output.push_back(detail::upperHexDigit(ch & 0x0F));
}

[[nodiscard]] std::pmr::string encodeRedirectLocation(
    std::string_view location,
    std::pmr::memory_resource* resource) {
    std::pmr::string encoded(resource);
    encoded.reserve(location.size());
    for (std::size_t i = 0; i < location.size(); ++i) {
        const auto ch = static_cast<unsigned char>(location[i]);
        // Pass an already well-formed percent-escape (%HH) through verbatim. This
        // pass only runs when the location carries a non-ASCII byte, but it then
        // rewrites the whole string -- so without this, a location that is already
        // percent-encoded elsewhere (e.g. "%20") would have its '%' re-encoded to
        // "%25", double-encoding it to "%2520" and corrupting the redirect target.
        // RFC 3986 2.4 forbids encoding the same string more than once; the caller
        // means a valid target, not a literal percent. A lone or malformed '%' is
        // not a valid escape and is percent-encoded like any other octet below.
        if (ch == '%' && i + 2 < location.size() &&
            detail::decodeHexNibble(location[i + 1]) >= 0 &&
            detail::decodeHexNibble(location[i + 2]) >= 0) {
            encoded.push_back('%');
            encoded.push_back(location[i + 1]);
            encoded.push_back(location[i + 2]);
            i += 2;
            continue;
        }
        if (encodeUriKeepsByte(ch)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        appendPercentEncodedByte(encoded, ch);
    }
    return encoded;
}

}  // namespace

HttpResponseHeader* Context::findResponseHeaderForUpdate(
    std::string_view name,
    std::uint32_t knownBit) noexcept {
    auto* const begin = detail::HttpResponseHeadersAccess::begin(responseHeaders_);
    auto* const end = detail::HttpResponseHeadersAccess::end(responseHeaders_);
    auto* const header = detail::findResponseHeaderIndexed(
        begin,
        end,
        responseHeaderIndexes_,
        detail::responseKnownHeaderSlot(knownBit),
        name,
        knownBit);
    return header == end ? nullptr : header;
}

void Context::recordResponseKnownHeaderIndex(
    std::uint32_t knownBit,
    std::size_t index) noexcept {
    detail::recordResponseHeaderIndex(
        responseHeaderIndexes_,
        detail::responseKnownHeaderSlot(knownBit),
        index);
}

void Context::rebuildResponseHeaderIndexes() noexcept {
    responseHeaderIndexes_.fill(detail::kMissingResponseHeaderIndexSlot);
    const auto& headers = responseHeaders_;
    const auto* const begin = headers.begin();
    const auto* const end = headers.end();
    for (auto* cursor = begin; cursor != end; ++cursor) {
        const auto knownBit = detail::responseHeaderKnownBit(*cursor);
        if (knownBit == 0) {
            continue;
        }
        recordResponseKnownHeaderIndex(knownBit, static_cast<std::size_t>(cursor - begin));
    }
}

void Context::status(std::uint16_t statusCode) {
    if (statusCode < 100 || statusCode > 999) {
        throw std::invalid_argument("invalid HTTP status code");
    }
    responseStatusCode_ = statusCode;
    if (response_ != nullptr) {
        response_->status(statusCode, {});
    }
}

Context& Context::removeResponseHeader(std::string_view name) {
    if (!isValidHttpHeaderName(name)) {
        throw std::invalid_argument("invalid HTTP header name");
    }

    const auto knownBit = detail::classifyResponseKnownHeader(name);
    auto* const begin = detail::HttpResponseHeadersAccess::begin(responseHeaders_);
    auto* const end = detail::HttpResponseHeadersAccess::end(responseHeaders_);
    auto* write = begin;
    bool removed = false;

    for (auto* read = begin; read != end; ++read) {
        const auto headerKnownBit = detail::responseHeaderKnownBit(*read);
        const bool matches = knownBit != 0
            ? headerKnownBit == knownBit
            : detail::httpAsciiEqualsIgnoreCase(read->name(), name);
        if (matches) {
            detail::HttpResponseHeadersAccess::release(responseHeaders_, *read);
            removed = true;
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }

    if (removed) {
        detail::HttpResponseHeadersAccess::truncate(responseHeaders_, begin, write);
        rebuildResponseHeaderIndexes();
    }
    if (response_ != nullptr) {
        responseStorage().header(name, std::nullopt);
    }
    return *this;
}

void Context::header(std::string_view name, std::string_view value, HeaderOptions options) {
    if (!isValidHttpHeaderName(name)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto knownBit = detail::classifyResponseKnownHeader(name);
    if (options.append) {
        if (detail::responseHeaderAppendForbidden(knownBit)) {
            throw std::invalid_argument("HTTP response header cannot be appended");
        }
        const auto index = responseHeaders_.size();
        auto& header = detail::HttpResponseHeadersAccess::add(responseHeaders_, name, value, knownBit);
        detail::setResponseHeaderAppend(header, true);
        recordResponseKnownHeaderIndex(knownBit, index);
        if (response_ != nullptr) {
            detail::appendResponseHeaderValidated(responseStorage(), name, value, knownBit);
        }
        return;
    }

    if (auto* const header = findResponseHeaderForUpdate(name, knownBit)) {
        detail::HttpResponseHeadersAccess::assign(responseHeaders_, *header, name, value, knownBit);
        if (response_ != nullptr) {
            detail::setResponseHeaderValidated(responseStorage(), name, value, knownBit);
        }
        return;
    }

    const auto index = responseHeaders_.size();
    (void)detail::HttpResponseHeadersAccess::add(responseHeaders_, name, value, knownBit);
    recordResponseKnownHeaderIndex(knownBit, index);
    if (response_ != nullptr) {
        detail::setResponseHeaderValidated(responseStorage(), name, value, knownBit);
    }
}

void Context::header(std::string_view name, std::nullopt_t) {
    removeResponseHeader(name);
}

namespace {

struct SetCookieSerialization final {
    std::array<char, 32> expiresBuffer{};
    std::size_t expiresSize{0};
    std::string_view prefixText{};
    std::string_view priorityText{};
    std::string_view sameSiteText{};
    std::uint64_t maxAgeValue{0};
    std::size_t maxAgeSize{0};
    bool hasMaxAge{false};
    std::size_t size{0};

    [[nodiscard]] std::string_view expiresText() const noexcept {
        return std::string_view(expiresBuffer.data(), expiresSize);
    }
};

[[nodiscard]] SetCookieSerialization prepareSetCookie(
    std::string_view name,
    std::string_view value,
    const CookieOptions& options) {
    detail::validateCookie(name, value, options);
    SetCookieSerialization serialization;
    serialization.prefixText = detail::cookiePrefixText(options.prefix);
    serialization.priorityText = detail::cookiePriorityToken(options.priority);
    serialization.sameSiteText = detail::cookieSameSiteToken(options.sameSite);
    if (options.expires.has_value()) {
        const auto expiresTime = std::chrono::system_clock::to_time_t(*options.expires);
        const auto utc = detail::httpUtcTm(expiresTime);
        // Locale-independent IMF-fixdate (RFC 7231 7.1.1.1). std::strftime's
        // %a/%b are locale-dependent and could emit a malformed HTTP date under
        // a non-C locale; the shared formatter uses fixed English day/month
        // tables, matching the Date-header and file-response paths.
        serialization.expiresSize =
            detail::httpWriteImfFixdate(serialization.expiresBuffer.data(), utc);
    }
    serialization.hasMaxAge = options.maxAge >= 0;
    serialization.maxAgeValue =
        serialization.hasMaxAge ? static_cast<std::uint64_t>(options.maxAge) : std::uint64_t{0};
    serialization.maxAgeSize =
        serialization.hasMaxAge ? detail::httpUnsignedDecimalSize(serialization.maxAgeValue) : std::size_t{0};

    std::size_t cookieSize = serialization.prefixText.size() + name.size() + 1 + value.size();
    if (!options.path.empty()) {
        cookieSize += std::string_view("; Path=").size() + options.path.size();
    }
    if (!options.domain.empty()) {
        cookieSize += std::string_view("; Domain=").size() + options.domain.size();
    }
    if (serialization.hasMaxAge) {
        cookieSize += std::string_view("; Max-Age=").size() + serialization.maxAgeSize;
    }
    if (serialization.expiresSize != 0) {
        cookieSize += std::string_view("; Expires=").size() + serialization.expiresSize;
    }
    if (options.httpOnly) {
        cookieSize += std::string_view("; HttpOnly").size();
    }
    if (options.secure) {
        cookieSize += std::string_view("; Secure").size();
    }
    if (!serialization.sameSiteText.empty()) {
        cookieSize += std::string_view("; SameSite=").size() + serialization.sameSiteText.size();
    }
    if (!serialization.priorityText.empty()) {
        cookieSize += std::string_view("; Priority=").size() + serialization.priorityText.size();
    }
    if (options.partitioned) {
        cookieSize += std::string_view("; Partitioned").size();
    }
    serialization.size = cookieSize;
    return serialization;
}

void writeSetCookie(
    char* cursor,
    std::string_view name,
    std::string_view value,
    const CookieOptions& options,
    const SetCookieSerialization& serialization) {
    const auto append = [&cursor](std::string_view text) noexcept {
        if (text.empty()) {
            return;
        }
        std::memcpy(cursor, text.data(), text.size());
        cursor += text.size();
    };
    const auto appendUnsigned = [&cursor](std::uint64_t number, std::size_t size) {
        auto* const end = cursor + size;
        const auto [ptr, ec] = std::to_chars(cursor, end, number);
        if (ec != std::errc{} || ptr != end) {
            throw std::logic_error("failed to format cookie Max-Age");
        }
        cursor = ptr;
    };

    append(serialization.prefixText);
    append(name);
    *cursor++ = '=';
    append(value);
    if (!options.path.empty()) {
        append("; Path=");
        append(options.path);
    }
    if (!options.domain.empty()) {
        append("; Domain=");
        append(options.domain);
    }
    if (serialization.hasMaxAge) {
        append("; Max-Age=");
        appendUnsigned(serialization.maxAgeValue, serialization.maxAgeSize);
    }
    if (serialization.expiresSize != 0) {
        append("; Expires=");
        append(serialization.expiresText());
    }
    if (options.httpOnly) {
        append("; HttpOnly");
    }
    if (options.secure) {
        append("; Secure");
    }
    if (!serialization.sameSiteText.empty()) {
        append("; SameSite=");
        append(serialization.sameSiteText);
    }
    if (!serialization.priorityText.empty()) {
        append("; Priority=");
        append(serialization.priorityText);
    }
    if (options.partitioned) {
        append("; Partitioned");
    }
}

[[nodiscard]] std::pmr::string composeSignedCookieValue(
    std::pmr::memory_resource* resource,
    std::string_view name,
    std::string_view value,
    std::string_view secret) {
    std::pmr::string signedValue(resource);
    signedValue.reserve(value.size() + 1 + detail::kCookieSignatureSize);
    if (!value.empty()) {
        signedValue.append(value.data(), value.size());
    }
    signedValue.push_back('.');
    char signature[detail::kCookieSignatureSize];
    detail::writeCookieSignature(signature, secret, name, value);
    signedValue.append(signature, sizeof(signature));
    return signedValue;
}

}  // namespace

void Context::setCookie(std::string_view name, std::string_view value, const CookieOptions& options) {
    const auto serialization = prepareSetCookie(name, value, options);
    const auto index = responseHeaders_.size();
    auto& header = detail::HttpResponseHeadersAccess::addUninitializedValue(
        responseHeaders_,
        "Set-Cookie",
        serialization.size,
        detail::kResponseHeaderSetCookie);
    recordResponseKnownHeaderIndex(detail::kResponseHeaderSetCookie, index);
    writeSetCookie(detail::responseHeaderValueBegin(header), name, value, options, serialization);
    if (response_ != nullptr) {
        detail::appendResponseHeaderValidated(
            responseStorage(),
            "Set-Cookie",
            header.value(),
            detail::kResponseHeaderSetCookie);
    }
}

void Context::setSignedCookie(
    std::string_view name,
    std::string_view value,
    std::string_view secret,
    const CookieOptions& options) {
    setCookie(name, composeSignedCookieValue(resource(), name, value, secret), options);
}

std::pmr::string Context::generateCookie(
    std::string_view name,
    std::string_view value,
    const CookieOptions& options) const {
    const auto serialization = prepareSetCookie(name, value, options);
    std::pmr::string cookie(resource());
    cookie.resize_and_overwrite(serialization.size, [&](char* out, std::size_t size) {
        writeSetCookie(out, name, value, options, serialization);
        return size;
    });
    return cookie;
}

std::pmr::string Context::generateSignedCookie(
    std::string_view name,
    std::string_view value,
    std::string_view secret,
    const CookieOptions& options) const {
    return generateCookie(name, composeSignedCookieValue(resource(), name, value, secret), options);
}

std::optional<std::string_view> Context::deleteCookie(std::string_view name, CookieOptions options) {
    auto deleted = req().cookie(name);
    options.maxAge = 0;
    setCookie(name, "", options);
    return deleted;
}

void Context::storeResponse(HttpResponse&& response) {
    const bool hadResponseSlot = response_ != nullptr;
    if (hadResponseSlot && response_ != &response) {
        mergeResponseSlotHeaders(response, *response_);
    }

    // The context status is a default: a status already carried by the
    // response (an explicit helper argument or setStatus call) wins.
    if (responseStatusCode_ != 200 && response.status() == 200) {
        response.status(responseStatusCode_, {});
    }

    if (!hadResponseSlot) {
        const auto contextHeaderCount = responseHeaders_.size();
        if (contextHeaderCount > 0) {
            detail::reserveResponseHeaders(response, response.headers().size() + contextHeaderCount);
        }
        const auto& headers = responseHeaders_;
        for (const auto& header : headers) {
            const auto knownBit = detail::responseHeaderKnownBit(header);
            const auto name = header.name();
            const auto value = header.value();
            if (knownBit == detail::kResponseHeaderSetCookie || detail::responseHeaderAppend(header)) {
                if (!responseHasHeaderValue(response, name, value)) {
                    detail::appendResponseHeaderValidated(response, name, value, knownBit);
                }
            } else if (!responseHasHeaderName(response, name)) {
                detail::setResponseHeaderValidated(response, name, value, knownBit);
            }
        }
    }

    responseStorage() = std::move(response);
    responseFinalized_ = true;
}

void Context::storeAssignedResponse(HttpResponse&& response) {
    const bool hadResponseSlot = response_ != nullptr;
    if (hadResponseSlot && response_ != &response) {
        assignResponseSlotHeaders(response, *response_);
    }

    if (responseStatusCode_ != 200 && response.status() == 200) {
        response.status(responseStatusCode_, {});
    }

    if (response_ == nullptr) {
        response_ = &memory_.emplace<HttpResponse>(std::move(response));
    } else {
        *response_ = std::move(response);
    }
    responseFinalized_ = true;
}

HttpResponse Context::body(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    response.setBodyView(body);
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::body(
    std::string_view body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    response.setBodyView(body);
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::body(std::string_view body, ResponseInit init) const {
    HttpResponse response(resource());
    response.setBodyView(body);
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::body(
    std::nullptr_t,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::body(
    std::nullptr_t,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::body(std::nullptr_t, ResponseInit init) const {
    HttpResponse response(resource());
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::body(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::body(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::body(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::body(
    std::span<const std::byte> body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    response.setBodyCopy(byteBodyView(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::body(
    std::span<const std::byte> body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    response.setBodyCopy(byteBodyView(body));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::body(std::span<const std::byte> body, ResponseInit init) const {
    HttpResponse response(resource());
    response.setBodyCopy(byteBodyView(body));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::body(
    HttpBodyStream stream,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseStreamBody(response, std::move(stream));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::body(
    HttpBodyStream stream,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseStreamBody(response, std::move(stream));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::body(HttpBodyStream stream, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseStreamBody(response, std::move(stream));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::newResponse(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    return this->body(body, statusCode, statusText);
}

HttpResponse Context::newResponse(
    std::string_view body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    return this->body(body, statusCode, headers);
}

HttpResponse Context::newResponse(std::string_view body, ResponseInit init) const {
    return this->body(body, init);
}

HttpResponse Context::newResponse(
    std::nullptr_t,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    return this->body(nullptr, statusCode, statusText);
}

HttpResponse Context::newResponse(
    std::nullptr_t,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    return this->body(nullptr, statusCode, headers);
}

HttpResponse Context::newResponse(std::nullptr_t, ResponseInit init) const {
    return this->body(nullptr, init);
}

HttpResponse Context::newResponse(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    return this->body(body, statusCode, statusText);
}

HttpResponse Context::newResponse(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    return this->body(body, statusCode, headers);
}

HttpResponse Context::newResponse(std::pmr::string& body, ResponseInit init) const {
    return this->body(body, init);
}

HttpResponse Context::newResponse(
    std::span<const std::byte> body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    return this->body(body, statusCode, statusText);
}

HttpResponse Context::newResponse(
    std::span<const std::byte> body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    return this->body(body, statusCode, headers);
}

HttpResponse Context::newResponse(std::span<const std::byte> body, ResponseInit init) const {
    return this->body(body, init);
}

HttpResponse Context::text(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::text(
    std::string_view body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::text(std::string_view body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::text(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::text(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::text(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::textStaticView(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::jsonSerialized(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "application/json");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::html(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::html(
    std::string_view body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::html(std::string_view body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::html(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::html(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::html(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

void Context::renderer(Renderer renderer) noexcept {
    renderer_ = renderer;
}

Context::Layout Context::layout(Layout layout) noexcept {
    layout_ = layout;
    return layout_;
}

Context::Layout Context::layout() const noexcept {
    return layout_;
}

Task<HttpResponse> Context::render(std::string_view body) {
    return render(body, RenderOptions{});
}

Task<HttpResponse> Context::render(std::string_view body, std::string_view head) {
    return render(body, RenderOptions{.head = head});
}

Task<HttpResponse> Context::render(std::string_view body, RenderOptions options) {
    if (renderer_ == nullptr) {
        co_return html(body);
    }
    co_return co_await renderer_(*this, body, options);
}

HttpResponse Context::redirect(
    std::string_view location,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    applyResponseState(response, statusCode, statusText);
    if (redirectLocationNeedsEncoding(location)) {
        auto encodedLocation = encodeRedirectLocation(location, resource());
        response.header("Location", encodedLocation);
    } else {
        response.header("Location", location);
    }
    return response;
}

HttpResponse Context::error(
    std::uint16_t statusCode,
    std::string_view code,
    std::string_view message,
    std::string_view statusText) const {
    auto response = makeErrorResponse(
        resource(),
        HttpErrorInfo(statusCode, code, message, statusText));
    applyResponseState(response, statusCode, statusText);
    return response;
}

Task<HttpResponse> Context::notFound() {
    if (notFoundHandler_ != nullptr) {
        co_return co_await notFoundHandler_(*this);
    }

    auto response = co_await makeErrorResponse(
        *this,
        HttpErrorInfo(404, {}, "route not found"),
        false,
        nullptr);
    applyResponseState(response, 404, {});
    co_return response;
}

HttpResponse Context::streamingHead(std::string_view contentType) const {
    HttpResponse response(resource());
    if (!contentType.empty()) {
        response.header("Content-Type", contentType);
    }
    applyResponseState(response, 0, {});
    return response;
}

Context& Context::setStableResponseHeader(std::string_view name, std::string_view value) {
    const auto knownBit = detail::classifyResponseKnownHeader(name);
    if (auto* const header = findResponseHeaderForUpdate(name, knownBit)) {
        detail::HttpResponseHeadersAccess::assignStableView(
            responseHeaders_, *header, name, value, knownBit);
        return *this;
    }

    const auto index = responseHeaders_.size();
    (void)detail::HttpResponseHeadersAccess::addStableView(responseHeaders_, name, value, knownBit);
    recordResponseKnownHeaderIndex(knownBit, index);
    return *this;
}

void Context::applyResponseState(
    HttpResponse& response,
    std::uint16_t statusCode,
    std::string_view statusText,
    std::span<const HttpHeaderView> headers) const {
    const auto finalStatusCode = statusCode == 0 ? responseStatusCode_ : statusCode;
    if (!statusText.empty()) {
        if (finalStatusCode != 200 || statusText != "OK") {
            response.status(finalStatusCode, statusText);
        }
    } else if (finalStatusCode != 200) {
        response.status(finalStatusCode, {});
    }
    if (response_ != nullptr && response_ != &response) {
        mergeResponseSlotHeaders(response, *response_);
    }
    const auto contextHeaderCount = responseHeaders_.size();
    if (contextHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + contextHeaderCount);
    }
    for (const auto& header : responseHeaders_) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie || detail::responseHeaderAppend(header)) {
            // Appended headers live in both the context list and a materialized
            // response slot; the slot merge above may have carried them already.
            if (!responseHasHeaderValue(response, name, value)) {
                detail::appendResponseHeaderValidated(response, name, value, knownBit);
            }
        } else {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
    applyExplicitResponseHeaders(response, headers);
}

void Context::applyExplicitResponseHeaders(
    HttpResponse& response,
    std::span<const HttpHeaderView> headers) const {
    if (headers.empty()) {
        return;
    }
    detail::reserveResponseHeaders(response, response.headers().size() + headers.size());
    for (const auto& header : headers) {
        const auto knownBit = detail::classifyResponseKnownHeader(header.name());
        if (knownBit == detail::kResponseHeaderSetCookie) {
            response.header(header.name(), header.value(), HttpResponse::HeaderOptions{.append = true});
        } else {
            response.header(header.name(), header.value());
        }
    }
}

}  // namespace ruvia
