#include "ruvia/http/Context.h"

#include "CookieValidation.h"
#include "HttpRequestInternal.h"
#include "HttpResponseBodyAccess.h"
#include "HttpResponseHeaderAccess.h"
#include "HttpResponseHeaderState.h"
#include "ruvia/detail/NumberFormat.h"
#include "ResponseHeaderIndexCache.h"

#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia {

namespace {

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
        } else {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }
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

Context& Context::status(std::uint16_t statusCode, std::string_view statusText) {
    if (statusCode < 100 || statusCode > 999) {
        throw std::invalid_argument("invalid HTTP status code");
    }
    if (!isValidHttpStatusText(statusText)) {
        throw std::invalid_argument("invalid HTTP status text");
    }
    responseStatusCode_ = statusCode;
    if (statusText.empty()) {
        responseStatusText_.clear();
    } else {
        responseStatusText_.assign(statusText.data(), statusText.size());
    }
    if (response_ != nullptr) {
        response_->setStatus(statusCode, statusText);
    }
    return *this;
}

Context& Context::header(std::string_view name, std::string_view value, HeaderOptions options) {
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
        return *this;
    }

    if (auto* const header = findResponseHeaderForUpdate(name, knownBit)) {
        responseHeaders_.assign(*header, name, value, knownBit);
        if (response_ != nullptr) {
            detail::setResponseHeaderValidated(responseStorage(), name, value, knownBit);
        }
        return *this;
    }

    const auto index = responseHeaders_.size();
    responseHeaders_.add(name, value, knownBit);
    recordResponseKnownHeaderIndex(knownBit, index);
    if (response_ != nullptr) {
        detail::setResponseHeaderValidated(responseStorage(), name, value, knownBit);
    }
    return *this;
}

Context& Context::setCookie(std::string_view name, std::string_view value, const CookieOptions& options) {
    detail::validateCookie(name, value, options);
    const bool hasMaxAge = options.maxAge >= 0;
    const auto maxAgeValue = hasMaxAge ? static_cast<std::uint64_t>(options.maxAge) : std::uint64_t{0};
    const auto maxAgeSize = hasMaxAge ? detail::unsignedDecimalSize(maxAgeValue) : std::size_t{0};
    std::size_t cookieSize = name.size() + 1 + value.size();
    if (!options.path.empty()) {
        cookieSize += std::string_view("; Path=").size() + options.path.size();
    }
    if (!options.domain.empty()) {
        cookieSize += std::string_view("; Domain=").size() + options.domain.size();
    }
    if (hasMaxAge) {
        cookieSize += std::string_view("; Max-Age=").size() + maxAgeSize;
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
    if (response_ != nullptr) {
        detail::appendResponseHeaderValidated(
            responseStorage(),
            "Set-Cookie",
            header.value(),
            detail::kResponseHeaderSetCookie);
    }
    return *this;
}

Context& Context::deleteCookie(std::string_view name, CookieOptions options) {
    options.maxAge = 0;
    return setCookie(name, "", options);
}

void Context::storeResponse(HttpResponse&& response) {
    if (response_ != nullptr && response_ != &response) {
        mergeResponseSlotHeaders(response, *response_);
    }

    if (!responseStatusText_.empty()) {
        const auto statusText = std::string_view(responseStatusText_);
        if (responseStatusCode_ != 200 || statusText != "OK") {
            response.setStatus(responseStatusCode_, statusText);
        }
    } else if (responseStatusCode_ != 200) {
        response.setStatus(responseStatusCode_, {});
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
            if (!responseHasHeaderValue(response, name, value)) {
                detail::appendResponseHeaderValidated(response, name, value, knownBit);
            }
        } else {
            detail::setResponseHeaderValidated(response, name, value, knownBit);
        }
    }

    responseStorage() = std::move(response);
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

HttpResponse Context::text(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=utf-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::text(
    std::string_view body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=utf-8");
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
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=utf-8");
    response.setBodyView(body);
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::text(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=utf-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::text(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=utf-8");
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
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=utf-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::textStaticView(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=utf-8");
    detail::setResponseBodyStaticView(response, body);
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::jsonSerialized(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "application/json; charset=utf-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::html(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=utf-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::html(
    std::string_view body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=utf-8");
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
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=utf-8");
    response.setBodyView(body);
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

HttpResponse Context::html(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=utf-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

HttpResponse Context::html(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=utf-8");
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
    detail::setResponseHeaderStableView(response, "Content-Type", "text/html; charset=utf-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, init.status, init.statusText, init.headers);
    return response;
}

Context& Context::setRenderer(Renderer renderer) noexcept {
    renderer_ = renderer;
    return *this;
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
    response.setHeader("Location", location);
    applyResponseState(response, statusCode, statusText);
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
    } else if (statusCode == 0 && !responseStatusText_.empty()) {
        const auto finalStatusText = std::string_view(responseStatusText_);
        if (finalStatusCode != 200 || finalStatusText != "OK") {
            response.setStatus(finalStatusCode, finalStatusText);
        }
    } else if (finalStatusCode != 200) {
        response.setStatus(finalStatusCode, {});
    }
    const auto contextHeaderCount = responseHeaders_.size();
    if (contextHeaderCount > 0) {
        detail::reserveResponseHeaders(response, response.headers().size() + contextHeaderCount);
    }
    for (const auto& header : responseHeaders_) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        if (knownBit == detail::kResponseHeaderSetCookie || detail::responseHeaderAppend(header)) {
            detail::appendResponseHeaderValidated(response, header.name(), header.value(), knownBit);
        } else {
            detail::setResponseHeaderValidated(response, header.name(), header.value(), knownBit);
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
