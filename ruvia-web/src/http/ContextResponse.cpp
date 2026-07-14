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

void Context::status(std::uint16_t statusCode) {
    responseMetadata_.status(statusCode);
    if (response_ != nullptr) {
        response_->status(statusCode);
    }
}

Context& Context::removeResponseHeader(std::string_view name) {
    responseMetadata_.header(name, std::nullopt);
    if (response_ != nullptr) {
        responseStorage().header(name, std::nullopt);
    }
    return *this;
}

void Context::header(std::string_view name, std::string_view value, HeaderOptions options) {
    responseMetadata_.header(
        name,
        value,
        HttpResponse::HeaderOptions{.append = options.append});
    const auto knownBit = detail::classifyResponseKnownHeader(name);
    if (options.append) {
        if (response_ != nullptr) {
            detail::appendResponseHeaderValidated(responseStorage(), name, value, knownBit);
        }
        return;
    }
    if (response_ != nullptr) {
        detail::setResponseHeaderValidated(responseStorage(), name, value, knownBit);
    }
}

void Context::header(std::string_view name, std::nullopt_t) {
    removeResponseHeader(name);
}

namespace {

// The signature must bind the name the client sends back in Cookie, which is
// the wire name: an enum prefix becomes part of that name at serialization, so
// signing the bare name would make the prefixed cookie unverifiable on read.
[[nodiscard]] std::string_view signedCookieWireName(
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
        responseMetadata_,
        "Set-Cookie",
        plan.size(),
        detail::kResponseHeaderSetCookie);
    plan.write(detail::responseHeaderValueBegin(header));
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
    std::pmr::string wireName(resource());
    setCookie(
        name,
        composeSignedCookieValue(
            resource(),
            signedCookieWireName(wireName, name, options),
            value,
            secret),
        options);
}

std::pmr::string Context::generateCookie(
    std::string_view name,
    std::string_view value,
    const CookieOptions& options) const {
    const detail::SetCookiePlan plan(name, value, options);
    std::pmr::string cookie(resource());
    cookie.resize_and_overwrite(plan.size(), [&](char* out, std::size_t size) {
        plan.write(out);
        return size;
    });
    return cookie;
}

std::pmr::string Context::generateSignedCookie(
    std::string_view name,
    std::string_view value,
    std::string_view secret,
    const CookieOptions& options) const {
    std::pmr::string wireName(resource());
    return generateCookie(
        name,
        composeSignedCookieValue(
            resource(),
            signedCookieWireName(wireName, name, options),
            value,
            secret),
        options);
}

std::optional<std::string_view> Context::deleteCookie(std::string_view name, CookieOptions options) {
    auto deleted = req().cookie(name);
    options.maxAge = std::chrono::seconds(0);
    setCookie(name, "", options);
    return deleted;
}

void Context::storeResponse(HttpResponse&& response) {
    const bool hadResponseSlot = response_ != nullptr;
    if (hadResponseSlot && response_ != &response) {
        mergeResponseSlotHeaders(response, *response_);
    }

    if (!hadResponseSlot) {
        const auto contextHeaderCount = responseMetadata_.headers().size();
        if (contextHeaderCount > 0) {
            detail::reserveResponseHeaders(response, response.headers().size() + contextHeaderCount);
        }
        const auto& headers = responseMetadata_.headers();
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

    if (response_ == nullptr) {
        response_ = &memory_.emplace<HttpResponse>(std::move(response));
    } else {
        *response_ = std::move(response);
    }
    responseFinalized_ = true;
}

HttpResponse Context::body(
    std::string_view body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    response.setBodyView(body);
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::body(
    std::string_view body,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    response.setBodyView(body);
    applyResponseState(response, statusCode, headers);
    return response;
}

HttpResponse Context::body(std::string_view body, ResponseInit init) const {
    HttpResponse response(resource());
    response.setBodyView(body);
    applyResponseState(response, init.status, init.headers);
    return response;
}

HttpResponse Context::body(
    std::nullptr_t,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::body(
    std::nullptr_t,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    applyResponseState(response, statusCode, headers);
    return response;
}

HttpResponse Context::body(std::nullptr_t, ResponseInit init) const {
    HttpResponse response(resource());
    applyResponseState(response, init.status, init.headers);
    return response;
}

HttpResponse Context::body(
    std::pmr::string& body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::body(
    std::pmr::string& body,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, headers);
    return response;
}

HttpResponse Context::body(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, init.status, init.headers);
    return response;
}

HttpResponse Context::body(
    std::span<const std::byte> body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    response.setBodyCopy(byteBodyView(body));
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::body(
    std::span<const std::byte> body,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    response.setBodyCopy(byteBodyView(body));
    applyResponseState(response, statusCode, headers);
    return response;
}

HttpResponse Context::body(std::span<const std::byte> body, ResponseInit init) const {
    HttpResponse response(resource());
    response.setBodyCopy(byteBodyView(body));
    applyResponseState(response, init.status, init.headers);
    return response;
}

HttpResponse Context::text(
    std::string_view body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::text(
    std::string_view body,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, headers);
    return response;
}

HttpResponse Context::text(std::string_view body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, init.status, init.headers);
    return response;
}

HttpResponse Context::text(
    std::pmr::string& body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::text(
    std::pmr::string& body,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, headers);
    return response;
}

HttpResponse Context::text(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, init.status, init.headers);
    return response;
}

HttpResponse Context::textStaticView(
    std::string_view body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=UTF-8");
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::jsonSerialized(
    std::pmr::string& body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "application/json");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::html(
    std::string_view body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::html(
    std::string_view body,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, headers);
    return response;
}

HttpResponse Context::html(std::string_view body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    response.setBodyView(body);
    applyResponseState(response, init.status, init.headers);
    return response;
}

HttpResponse Context::html(
    std::pmr::string& body,
    std::optional<std::uint16_t> statusCode) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode);
    return response;
}

HttpResponse Context::html(
    std::pmr::string& body,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, statusCode, headers);
    return response;
}

HttpResponse Context::html(std::pmr::string& body, ResponseInit init) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=UTF-8");
    detail::setResponseBodyOwned(response, std::move(body));
    applyResponseState(response, init.status, init.headers);
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
    detail::setResponseHeaderStableView(responseMetadata_, name, value);
    return *this;
}

void Context::applyResponseState(
    HttpResponse& response,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    const auto finalStatusCode = statusCode.value_or(responseMetadata_.status());
    response.status(finalStatusCode);
    if (response_ != nullptr && response_ != &response) {
        mergeResponseSlotHeaders(response, *response_);
    }
    const auto contextHeaderCount = responseMetadata_.headers().size();
    if (contextHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + contextHeaderCount);
    }
    for (const auto& header : responseMetadata_.headers()) {
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
