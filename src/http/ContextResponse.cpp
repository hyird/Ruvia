#include "ruvia/http/Context.h"

#include "CookieSignature.h"
#include "CookieValidation.h"
#include "HttpRequestInternal.h"
#include "HttpResponseBodyAccess.h"
#include "HttpResponseHeaderAccess.h"
#include "HttpResponseHeaderState.h"
#include "ruvia/detail/NumberFormat.h"
#include "ResponseHeaderIndexCache.h"

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
                (void)response.removeHeader("Set-Cookie");
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
    static constexpr char kHex[] = "0123456789ABCDEF";
    output.push_back('%');
    output.push_back(kHex[ch >> 4]);
    output.push_back(kHex[ch & 0x0F]);
}

[[nodiscard]] std::pmr::string encodeRedirectLocation(
    std::string_view location,
    std::pmr::memory_resource* resource) {
    std::pmr::string encoded(resource);
    encoded.reserve(location.size());
    for (const auto raw : location) {
        const auto ch = static_cast<unsigned char>(raw);
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
    auto* const begin = responseHeaders_.begin();
    auto* const end = responseHeaders_.end();
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
    const auto* const begin = responseHeaders_.begin();
    const auto* const end = responseHeaders_.end();
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
        response_->setStatus(statusCode, {});
    }
}

Context& Context::removeResponseHeader(std::string_view name) {
    if (!isValidHttpHeaderName(name)) {
        throw std::invalid_argument("invalid HTTP header name");
    }

    const auto knownBit = detail::classifyResponseKnownHeader(name);
    auto* const begin = responseHeaders_.begin();
    auto* const end = responseHeaders_.end();
    auto* write = begin;
    bool removed = false;

    for (auto* read = begin; read != end; ++read) {
        const auto headerKnownBit = detail::responseHeaderKnownBit(*read);
        const bool matches = knownBit != 0
            ? headerKnownBit == knownBit
            : detail::httpAsciiEqualsIgnoreCase(read->name(), name);
        if (matches) {
            responseHeaders_.releaseHeader(*read);
            removed = true;
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }

    if (removed) {
        if (responseHeaders_.spilled_) {
            responseHeaders_.heap_.erase(
                responseHeaders_.heap_.begin() + static_cast<std::ptrdiff_t>(write - begin),
                responseHeaders_.heap_.end());
        } else {
            responseHeaders_.size_ = static_cast<std::size_t>(write - begin);
        }
        rebuildResponseHeaderIndexes();
    }
    if (response_ != nullptr) {
        responseStorage().removeHeader(name);
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
        const auto index = responseHeaders_.size();
        auto& header = responseHeaders_.add(name, value, knownBit);
        detail::setResponseHeaderAppend(header, true);
        recordResponseKnownHeaderIndex(knownBit, index);
        if (response_ != nullptr) {
            detail::appendResponseHeaderValidated(responseStorage(), name, value, knownBit);
        }
        return;
    }

    if (auto* const header = findResponseHeaderForUpdate(name, knownBit)) {
        responseHeaders_.assign(*header, name, value, knownBit);
        if (response_ != nullptr) {
            detail::setResponseHeaderValidated(responseStorage(), name, value, knownBit);
        }
        return;
    }

    const auto index = responseHeaders_.size();
    responseHeaders_.add(name, value, knownBit);
    recordResponseKnownHeaderIndex(knownBit, index);
    if (response_ != nullptr) {
        detail::setResponseHeaderValidated(responseStorage(), name, value, knownBit);
    }
}

void Context::header(std::string_view name, std::nullopt_t) {
    removeResponseHeader(name);
}

void Context::setCookie(std::string_view name, std::string_view value, const CookieOptions& options) {
    detail::validateCookie(name, value, options);
    const auto prefixText = detail::cookiePrefixText(options.prefix);
    const auto priorityText = detail::cookiePriorityToken(options.priority);
    std::array<char, 32> expiresBuffer{};
    std::string_view expiresText{};
    if (options.expires.has_value()) {
        const auto expiresTime = std::chrono::system_clock::to_time_t(*options.expires);
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &expiresTime);
#else
        gmtime_r(&expiresTime, &utc);
#endif
        const auto written = std::strftime(
            expiresBuffer.data(),
            expiresBuffer.size(),
            "%a, %d %b %Y %H:%M:%S GMT",
            &utc);
        if (written == 0) {
            throw std::invalid_argument("invalid cookie Expires");
        }
        expiresText = std::string_view(expiresBuffer.data(), written);
    }
    const bool hasMaxAge = options.maxAge >= 0;
    const auto maxAgeValue = hasMaxAge ? static_cast<std::uint64_t>(options.maxAge) : std::uint64_t{0};
    const auto maxAgeSize = hasMaxAge ? detail::unsignedDecimalSize(maxAgeValue) : std::size_t{0};
    std::size_t cookieSize = prefixText.size() + name.size() + 1 + value.size();
    if (!options.path.empty()) {
        cookieSize += std::string_view("; Path=").size() + options.path.size();
    }
    if (!options.domain.empty()) {
        cookieSize += std::string_view("; Domain=").size() + options.domain.size();
    }
    if (hasMaxAge) {
        cookieSize += std::string_view("; Max-Age=").size() + maxAgeSize;
    }
    if (!expiresText.empty()) {
        cookieSize += std::string_view("; Expires=").size() + expiresText.size();
    }
    if (options.httpOnly) {
        cookieSize += std::string_view("; HttpOnly").size();
    }
    if (options.secure) {
        cookieSize += std::string_view("; Secure").size();
    }
    if (!options.sameSite.empty()) {
        cookieSize += std::string_view("; SameSite=").size() + options.sameSite.size();
    }
    if (!priorityText.empty()) {
        cookieSize += std::string_view("; Priority=").size() + priorityText.size();
    }
    if (options.partitioned) {
        cookieSize += std::string_view("; Partitioned").size();
    }

    const auto index = responseHeaders_.size();
    auto& header = responseHeaders_.addUninitializedValue(
        "Set-Cookie",
        cookieSize,
        detail::kResponseHeaderSetCookie);
    recordResponseKnownHeaderIndex(detail::kResponseHeaderSetCookie, index);
    auto* cursor = detail::responseHeaderValueBegin(header);
    const auto append = [&cursor](std::string_view text) noexcept {
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

    append(prefixText);
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
    if (hasMaxAge) {
        append("; Max-Age=");
        appendUnsigned(maxAgeValue, maxAgeSize);
    }
    if (!expiresText.empty()) {
        append("; Expires=");
        append(expiresText);
    }
    if (options.httpOnly) {
        append("; HttpOnly");
    }
    if (options.secure) {
        append("; Secure");
    }
    if (!options.sameSite.empty()) {
        append("; SameSite=");
        append(options.sameSite);
    }
    if (!priorityText.empty()) {
        append("; Priority=");
        append(priorityText);
    }
    if (options.partitioned) {
        append("; Partitioned");
    }
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
    std::pmr::string signedValue(resource());
    signedValue.reserve(value.size() + 1 + detail::kCookieSignatureSize);
    if (!value.empty()) {
        signedValue.append(value.data(), value.size());
    }
    signedValue.push_back('.');
    char signature[detail::kCookieSignatureSize];
    detail::writeCookieSignature(signature, secret, value);
    signedValue.append(signature, sizeof(signature));
    setCookie(name, signedValue, options);
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
    if (responseStatusCode_ != 200 && response.statusCode() == 200) {
        response.setStatus(responseStatusCode_, {});
    }

    if (!hadResponseSlot) {
        const auto contextHeaderCount = responseHeaders_.size();
        if (contextHeaderCount > 0) {
            detail::reserveResponseHeaders(response, response.headers().size() + contextHeaderCount);
        }
        for (const auto& header : responseHeaders_) {
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

    if (responseStatusCode_ != 200 && response.statusCode() == 200) {
        response.setStatus(responseStatusCode_, {});
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

HttpResponse Context::body(
    std::string_view body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
    return this->body(
        body,
        statusCode,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()));
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

HttpResponse Context::body(
    std::nullptr_t,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
    return this->body(
        nullptr,
        statusCode,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()));
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
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::body(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::body(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
    return this->body(
        body,
        statusCode,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()));
}

HttpResponse Context::body(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    response.setBodyOwned(std::move(body));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::body(
    std::span<const std::byte> body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    response.setBodyView(byteBodyView(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::body(
    std::span<const std::byte> body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    response.setBodyView(byteBodyView(body));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::body(
    std::span<const std::byte> body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
    return this->body(
        body,
        statusCode,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()));
}

HttpResponse Context::body(std::span<const std::byte> body, ResponseInit init) const {
    HttpResponse response(resource());
    response.setBodyView(byteBodyView(body));
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

HttpResponse Context::newResponse(
    std::string_view body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
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

HttpResponse Context::newResponse(
    std::nullptr_t,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
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

HttpResponse Context::newResponse(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
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

HttpResponse Context::newResponse(
    std::span<const std::byte> body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
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

HttpResponse Context::text(
    std::string_view body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
    return text(
        body,
        statusCode,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()));
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
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::text(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::text(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
    return text(
        body,
        statusCode,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()));
}

HttpResponse Context::text(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.setBodyOwned(std::move(body));
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
    response.setBodyOwned(std::move(body));
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

HttpResponse Context::html(
    std::string_view body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
    return html(
        body,
        statusCode,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()));
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
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::html(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, {}, headers);
    return response;
}

HttpResponse Context::html(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::initializer_list<HttpHeaderView> headers) const {
    return html(
        body,
        statusCode,
        std::span<const HttpHeaderView>(headers.begin(), headers.size()));
}

HttpResponse Context::html(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

void Context::setRenderer(Renderer renderer) noexcept {
    renderer_ = renderer;
}

Context::Layout Context::setLayout(Layout layout) noexcept {
    layout_ = layout;
    return layout_;
}

Context::Layout Context::getLayout() const noexcept {
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
        response.setHeader("Location", encodedLocation);
    } else {
        response.setHeader("Location", location);
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
        HttpErrorInfo{
            .statusCode = statusCode,
            .statusText = statusText,
            .code = code,
            .message = message});
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::jsonError(
    std::uint16_t statusCode,
    std::string_view code,
    std::string_view message,
    std::string_view statusText) const {
    return error(statusCode, code, message, statusText);
}

Task<HttpResponse> Context::notFound() {
    if (notFoundHandler_ != nullptr) {
        co_return co_await notFoundHandler_(*this);
    }

    auto response = co_await makeErrorResponse(
        *this,
        HttpErrorInfo{.statusCode = 404, .message = "route not found"},
        false,
        nullptr);
    applyResponseState(response, 404, {});
    co_return response;
}

HttpResponse Context::streamingHead(std::string_view contentType) const {
    HttpResponse response(resource());
    if (!contentType.empty()) {
        response.setHeader("Content-Type", contentType);
    }
    applyResponseState(response, 0, {});
    return response;
}

Context& Context::setStableResponseHeader(std::string_view name, std::string_view value) {
    const auto knownBit = detail::classifyResponseKnownHeader(name);
    if (auto* const header = findResponseHeaderForUpdate(name, knownBit)) {
        responseHeaders_.assignStableView(*header, name, value, knownBit);
        return *this;
    }

    const auto index = responseHeaders_.size();
    responseHeaders_.addStableView(name, value, knownBit);
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
            response.setStatus(finalStatusCode, statusText);
        }
    } else if (finalStatusCode != 200) {
        response.setStatus(finalStatusCode, {});
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
        response.setHeader(header.name, header.value);
    }
}

}  // namespace ruvia
