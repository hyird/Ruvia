#include "ruvia/web/Context.h"

#include "ruvia/web/detail/CookieSignature.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/detail/CookieValidation.h"
#include "ruvia/http/detail/SetCookiePlan.h"
#include "ruvia/http/detail/Hex.h"
#include "ruvia/web/detail/http/HttpErrorResponse.h"

#include <chrono>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ruvia {

namespace {

[[nodiscard]] std::string_view byteBodyView(std::span<const std::byte> body) noexcept {
    return body.empty()
        ? std::string_view{}
        : std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
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

[[nodiscard]] std::size_t responseHeaderValueCount(
    const HttpResponse& response,
    std::string_view name,
    std::string_view value) noexcept {
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), name) &&
            header.value() == value) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::size_t headerOccurrenceThrough(
    const HttpResponse& source,
    const HttpResponseHeader& target) noexcept {
    std::size_t count = 0;
    for (const auto& candidate : source.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(candidate.name(), target.name()) &&
            candidate.value() == target.value()) {
            ++count;
        }
        if (&candidate == &target) {
            break;
        }
    }
    return count;
}

// A Context-built response already contains the active state. A raw response
// does not. Merge by occurrence count so both paths converge without treating
// repeated equal append fields as a set.
void mergeActiveResponseHeaders(HttpResponse& response, const HttpResponse& active) {
    const auto activeHeaderCount = active.headers().size();
    if (activeHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + activeHeaderCount);
    }
    for (const auto& header : active.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie || detail::responseHeaderAppend(header)) {
            if (responseHeaderValueCount(response, name, value) <
                headerOccurrenceThrough(active, header)) {
                detail::appendResponseHeaderValidated(response, name, value, knownBit);
            }
        } else if (!responseHasHeaderName(response, name)) {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
}

void assignActiveResponseHeaders(HttpResponse& response, const HttpResponse& active) {
    const auto activeHeaderCount = active.headers().size();
    if (activeHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + activeHeaderCount);
    }

    bool replacedSetCookie = false;
    for (const auto& header : active.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        if (knownBit == detail::kResponseHeaderContentType) {
            continue;
        }
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie) {
            if (!replacedSetCookie) {
                response.header("Set-Cookie", std::nullopt);
                replacedSetCookie = true;
            }
            detail::appendResponseHeaderValidated(response, name, value, knownBit);
        } else if (detail::responseHeaderAppend(header)) {
            if (responseHeaderValueCount(response, name, value) <
                headerOccurrenceThrough(active, header)) {
                detail::appendResponseHeaderValidated(response, name, value, knownBit);
            }
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

void Context::status(std::uint16_t statusCode) {
    responseState_.activeResponse().status(statusCode);
}

Context& Context::removeResponseHeader(std::string_view name) {
    responseState_.activeResponse().header(name, std::nullopt);
    return *this;
}

void Context::header(std::string_view name, std::string_view value, HeaderOptions options) {
    responseState_.activeResponse().header(
        name,
        value,
        HttpResponse::HeaderOptions{.append = options.append});
}

void Context::header(std::string_view name, std::nullopt_t) {
    removeResponseHeader(name);
}

namespace {

// The name the client sends back in Cookie is the wire name: an enum prefix
// becomes part of the name at serialization. Request-side lookups and the MAC
// of a signed cookie must both use it; the bare name never reaches the client.
[[nodiscard]] std::string_view cookieWireName(
    std::pmr::string& storage,
    std::string_view name,
    const ruvia::CookieOptions& options) {
    if (!options.prefix) {
        return name;
    }
    const auto prefix = ruvia::detail::cookiePrefixText(*options.prefix);
    storage.reserve(prefix.size() + name.size());
    storage.append(prefix.data(), prefix.size());
    storage.append(name.data(), name.size());
    return storage;
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
    const detail::SetCookiePlan plan(name, value, options);
    auto& header = detail::appendResponseHeaderUninitializedValue(
        responseState_.activeResponse(),
        "Set-Cookie",
        plan.size(),
        detail::kResponseHeaderSetCookie);
    plan.write(detail::responseHeaderValueBegin(header));
}

void Context::setSignedCookie(
    std::string_view name,
    std::string_view value,
    std::string_view secret,
    const CookieOptions& options) {
    std::pmr::string wireName(resource());
    setCookie(
        name,
        composeSignedCookieValue(
            resource(),
            cookieWireName(wireName, name, options),
            value,
            secret),
        options);
}

void Context::deleteCookie(std::string_view name, CookieOptions options) {
    options.maxAge = std::chrono::seconds(0);
    setCookie(name, "", options);
}

void Context::storeResponse(HttpResponse&& response) {
    if (&response == &responseState_.activeResponse()) {
        responseState_.finalizeActive();
        return;
    }
    mergeActiveResponseHeaders(response, responseState_.activeResponse());
    responseState_.finalize(std::move(response));
}

void Context::storeAssignedResponse(HttpResponse&& response) {
    if (&response == &responseState_.activeResponse()) {
        responseState_.finalizeActive();
        return;
    }
    assignActiveResponseHeaders(response, responseState_.activeResponse());
    responseState_.finalize(std::move(response));
}

HttpResponse Context::body(std::string_view body) const {
    HttpResponse response(resource());
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::nullptr_t) const {
    HttpResponse response(resource());
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::pmr::string&& body) const {
    HttpResponse response(resource());
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::body(std::span<const std::byte> body) const {
    HttpResponse response(resource());
    response.body(byteBodyView(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::bodyStaticView(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::text(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::text(std::pmr::string&& body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::textStaticView(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::jsonSerialized(std::pmr::string& body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "application/json");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::html(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.body(body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::html(std::pmr::string&& body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::htmlStaticView(std::string_view body) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, std::nullopt);
    return response;
}

HttpResponse Context::redirect(
    std::string_view location,
    std::uint16_t statusCode) const {
    HttpResponse response(resource());
    applyResponseState(response, statusCode);
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
    auto response = detail::makeDefaultErrorResponse(
        resource(),
        HttpErrorInfo(statusCode, code, message, statusText));
    applyResponseState(response, statusCode);
    return response;
}

Task<HttpResponse> Context::notFound() {
    if (notFoundHandler_ != nullptr) {
        co_return co_await notFoundHandler_(*this);
    }

    auto response = detail::makeDefaultErrorResponse(
        resource(),
        HttpErrorInfo(404, {}, "route not found"));
    applyResponseState(response, 404);
    co_return response;
}

HttpResponse Context::streamingHead(std::string_view contentType) const {
    HttpResponse response(resource());
    if (!contentType.empty()) {
        response.header("Content-Type", contentType);
    }
    applyResponseState(response, std::nullopt);
    return response;
}

Context& Context::setStableResponseHeader(std::string_view name, std::string_view value) {
    detail::setResponseHeaderStableView(responseState_.activeResponse(), name, value);
    return *this;
}

void Context::applyResponseState(
    HttpResponse& response,
    std::optional<std::uint16_t> statusCode) const {
    const auto& activeResponse = responseState_.activeResponse();
    const auto finalStatusCode = statusCode.value_or(activeResponse.status());
    response.status(finalStatusCode);
    const auto contextHeaderCount = activeResponse.headers().size();
    if (contextHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + contextHeaderCount);
    }
    for (const auto& header : activeResponse.headers()) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        const auto name = header.name();
        const auto value = header.value();
        if (knownBit == detail::kResponseHeaderSetCookie || detail::responseHeaderAppend(header)) {
            detail::appendResponseHeaderValidated(response, name, value, knownBit);
        } else {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
}

}  // namespace ruvia
