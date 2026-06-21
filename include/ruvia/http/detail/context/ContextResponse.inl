#pragma once

namespace ruvia {

inline Context& Context::status(std::uint16_t statusCode, std::string_view statusText) {
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

inline Context& Context::setHeader(std::string_view name, std::string_view value) {
    if (!isValidHttpHeaderName(name)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!isValidHttpHeaderValue(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto knownBit = HttpResponse::classifyKnownHeader(name);
    for (auto& header : responseHeaders_) {
        if ((knownBit != 0 && header.knownBit == knownBit) ||
            (knownBit == 0 && detail::httpAsciiEqualsIgnoreCase(header.name(), name))) {
            responseHeaders_.assign(header, name, value, knownBit);
            return *this;
        }
    }

    responseHeaders_.add(name, value, knownBit);
    return *this;
}

inline Context& Context::setCookie(std::string_view name, std::string_view value, const CookieOptions& options) {
    detail::validateCookie(name, value, options);
    const bool hasMaxAge = options.maxAge >= 0;
    const auto maxAgeValue = hasMaxAge ? static_cast<std::uint64_t>(options.maxAge) : std::uint64_t{0};
    const auto decimalSize = [](std::uint64_t number) noexcept {
        std::size_t size = 1;
        while (number >= 10) {
            number /= 10;
            ++size;
        }
        return size;
    };
    const auto maxAgeSize = hasMaxAge ? decimalSize(maxAgeValue) : std::size_t{0};
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

    auto& header = responseHeaders_.addUninitializedValue(
        "Set-Cookie",
        cookieSize,
        HttpResponse::kKnownHeaderSetCookie);
    auto* cursor = const_cast<char*>(header.bytes) + header.nameSize;
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

inline HttpResponse Context::text(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    return textView(body, statusCode, statusText);
}

inline HttpResponse Context::textView(
    std::string_view body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    response.setHeaderStableView("Content-Type", "text/plain; charset=utf-8");
    response.setBodyView(body);
    applyResponseState(response, statusCode, statusText);
    return response;
}

inline HttpResponse Context::text(
    std::pmr::string& body,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    response.setHeaderStableView("Content-Type", "text/plain; charset=utf-8");
    response.setBody(std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

template <std::size_t N>
inline HttpResponse Context::text(
    const char (&body)[N],
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    response.setHeaderStableView("Content-Type", "text/plain; charset=utf-8");
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    response.setBodyStaticView(std::string_view(body, size));
    applyResponseState(response, statusCode, statusText);
    return response;
}

template <typename T>
inline HttpResponse Context::json(
    const T& value,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    response.setHeaderStableView("Content-Type", "application/json; charset=utf-8");
    std::pmr::string body(allocator<char>());
    appendJson(body, value);
    response.setBody(std::move(body));
    applyResponseState(response, statusCode, statusText);
    return response;
}

inline HttpResponse Context::redirect(
    std::string_view location,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    HttpResponse response(resource());
    response.setHeader("Location", location);
    applyResponseState(response, statusCode, statusText);
    return response;
}

inline HttpResponse Context::error(
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

inline HttpResponse Context::streamingHead(std::string_view contentType) const {
    HttpResponse response(resource());
    if (!contentType.empty()) {
        response.setHeader("Content-Type", contentType);
    }
    applyResponseState(response, 0, {});
    return response;
}

inline Context& Context::setResponseHeaderStableView(std::string_view name, std::string_view value) {
    const auto knownBit = HttpResponse::classifyKnownHeader(name);
    for (auto& header : responseHeaders_) {
        if ((knownBit != 0 && header.knownBit == knownBit) ||
            (knownBit == 0 && detail::httpAsciiEqualsIgnoreCase(header.name(), name))) {
            responseHeaders_.assignStableView(header, name, value, knownBit);
            return *this;
        }
    }

    responseHeaders_.addStableView(name, value, knownBit);
    return *this;
}

inline void Context::applyResponseState(
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
        if (header.knownBit == HttpResponse::kKnownHeaderSetCookie) {
            response.appendHeaderValidated(header.name(), header.value(), header.knownBit);
        } else {
            response.setHeaderValidated(header.name(), header.value(), header.knownBit);
        }
    }
}

}  // namespace ruvia
