#include "ruvia/http/Context.h"

#include "CookieValidation.h"
#include "HttpRequestInternal.h"
#include "HttpResponseBodyAccess.h"
#include "HttpResponseHeaderAccess.h"
#include "HttpResponseHeaderState.h"
#include "HeaderAcceptUtils.h"
#include "ruvia/detail/NumberFormat.h"
#include "ResponseHeaderIndexCache.h"

#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia {

bool Context::accepts(std::string_view mediaType) const noexcept {
    return detail::httpAcceptsMediaType(
        detail::requestKnownHeader(request_, detail::RequestKnownHeader::kAccept),
        mediaType);
}

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
    return *this;
}

Context& Context::setHeader(std::string_view name, std::string_view value) {
    if (!isValidHttpHeaderName(name)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto knownBit = detail::classifyResponseKnownHeader(name);
    if (auto* const header = findResponseHeaderForUpdate(name, knownBit)) {
        responseHeaders_.assign(*header, name, value, knownBit);
        return *this;
    }

    const auto index = responseHeaders_.size();
    responseHeaders_.add(name, value, knownBit);
    recordResponseKnownHeaderIndex(knownBit, index);
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
    return *this;
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
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    detail::setResponseHeaderStableView(response, "Content-Type", "text/plain; charset=utf-8");
    response.setBodyOwned(std::move(body));
    applyResponseState(response, statusCode, statusText);
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
    return makeErrorResponse(
        resource(),
        HttpErrorInfo{
            .statusCode = statusCode,
            .statusText = statusText,
            .code = code,
            .message = message});
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
    std::string_view statusText) const {
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
    for (const auto& header : responseHeaders_) {
        const auto knownBit = detail::responseHeaderKnownBit(header);
        if (knownBit == detail::kResponseHeaderSetCookie) {
            detail::appendResponseHeaderValidated(response, header.name(), header.value(), knownBit);
        } else {
            detail::setResponseHeaderValidated(response, header.name(), header.value(), knownBit);
        }
    }
}

}  // namespace ruvia
