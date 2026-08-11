#pragma once

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/response/HttpResponseKnownHeaders.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpHeader.h"

#include <algorithm>
#include <concepts>
#include <exception>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace ruvia::detail {

// A valid field name is a non-empty RFC 9110 token. isHttpTokenChar rejects ':',
// which also keeps HTTP/2 pseudo-headers out of a trailer section (RFC 9113
// §8.1). Reuses the parser's shared tchar table.
[[nodiscard]] inline bool isValidResponseTrailerName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    return std::ranges::all_of(name, [](char ch) noexcept { return isHttpTokenChar(static_cast<unsigned char>(ch)); });
}

[[nodiscard]] inline bool responseTrailerValueHasLeadingOrTrailingWhitespace(std::string_view value) noexcept {
    const auto whitespace = [](char ch) noexcept {
        return ch == ' ' || ch == '\t';
    };
    return !value.empty() && (whitespace(value.front()) || whitespace(value.back()));
}

// A trailer value must be a valid HTTP field value (RFC 9110 §5.5): field-vchar
// (VCHAR / obs-text) plus HTAB, and nothing else. The public API value also
// represents the parsed field value, not field-line OWS; rejecting leading and
// trailing SP/HTAB keeps the HTTP/1 chunked-trailer and HTTP/2 trailing-HEADERS
// sinks on the same normalized contract.
[[nodiscard]] inline bool isValidResponseTrailerValue(std::string_view value) noexcept {
    return !responseTrailerValueHasLeadingOrTrailingWhitespace(value) && std::ranges::all_of(value, [](char ch) noexcept { return isHttpFieldValueChar(static_cast<unsigned char>(ch)); });
}

// Fields that must never appear in a trailer section because they govern message
// framing, routing, authentication, response controls, or content format
// (RFC 9110 §6.5.1, RFC 9113 §8.1).
[[nodiscard]] inline bool isForbiddenResponseTrailerName(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classifyResponseHeaderName(name); known != 0 && known != kResponseHeaderEtag && known != kResponseHeaderAcceptRanges) {
        return true;
    }

    switch (classifyRequestHeader(name)) {
        case RequestHeaderKind::kHost:
        case RequestHeaderKind::kContentLength:
        case RequestHeaderKind::kTransferEncoding:
        case RequestHeaderKind::kConnection:
        case RequestHeaderKind::kContentEncoding:
        case RequestHeaderKind::kContentType:
        case RequestHeaderKind::kCookie:
        case RequestHeaderKind::kExpect:
        case RequestHeaderKind::kIfMatch:
        case RequestHeaderKind::kIfModifiedSince:
        case RequestHeaderKind::kIfNoneMatch:
        case RequestHeaderKind::kIfRange:
        case RequestHeaderKind::kIfUnmodifiedSince:
        case RequestHeaderKind::kRange:
        case RequestHeaderKind::kUpgrade:
        case RequestHeaderKind::kAuthorization:
            return true;
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kUserAgent:
        case RequestHeaderKind::kOrigin:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kSecWebSocketVersion:
            break;
    }

    switch (name.size()) {
        case 2:
            return httpAsciiEqualsIgnoreCase(name, "TE");
        case 3:
            // Response control data (RFC 9110 §7.4).
            return httpAsciiEqualsIgnoreCase(name, "Age");
        case 4:
            return httpAsciiEqualsIgnoreCase(name, "Date") || httpAsciiEqualsIgnoreCase(name, "Vary");
        case 6:
            return httpAsciiEqualsIgnoreCase(name, "Pragma");
        case 7:
            return httpAsciiEqualsIgnoreCase(name, "Trailer") || httpAsciiEqualsIgnoreCase(name, "Expires") || httpAsciiEqualsIgnoreCase(name, "Warning");
        case 8:
            return httpAsciiEqualsIgnoreCase(name, "Location");
        case 10:
            return httpAsciiEqualsIgnoreCase(name, "Keep-Alive") || httpAsciiEqualsIgnoreCase(name, "Set-Cookie");
        case 11:
            return httpAsciiEqualsIgnoreCase(name, "Retry-After");
        case 12:
            return httpAsciiEqualsIgnoreCase(name, "Max-Forwards");
        case 13:
            return httpAsciiEqualsIgnoreCase(name, "Cache-Control") || httpAsciiEqualsIgnoreCase(name, "Content-Range");
        case 15:
            return httpAsciiEqualsIgnoreCase(name, "X-Frame-Options") || httpAsciiEqualsIgnoreCase(name, "Referrer-Policy") || httpAsciiEqualsIgnoreCase(name, "Clear-Site-Data");
        case 16:
            return httpAsciiEqualsIgnoreCase(name, "X-XSS-Protection") || httpAsciiEqualsIgnoreCase(name, "WWW-Authenticate") || httpAsciiEqualsIgnoreCase(name, "Proxy-Connection");
        case 18:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authenticate") || httpAsciiEqualsIgnoreCase(name, "Permissions-Policy");
        case 19:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authorization") || httpAsciiEqualsIgnoreCase(name, "Content-Disposition");
        case 22:
            return httpAsciiEqualsIgnoreCase(name, "X-Content-Type-Options");
        case 23:
            return httpAsciiEqualsIgnoreCase(name, "Content-Security-Policy");
        case 25:
            return httpAsciiEqualsIgnoreCase(name, "Strict-Transport-Security");
        case 35:
            return httpAsciiEqualsIgnoreCase(name, "Content-Security-Policy-Report-Only");
        default:
            return false;
    }
}

[[nodiscard]] inline bool isValidHttpResponseTrailerFieldValue(std::string_view value, HttpFieldListRole role) noexcept {
    return isValidHttpTrailerFieldValue(value, role, [](std::string_view name) noexcept {
        return isForbiddenResponseTrailerName(name);
    });
}

// True if (name, value) is an acceptable response trailer field. Shared by the
// HTTP/1.1 chunked-trailer and HTTP/2 trailing-HEADERS sinks so both transports
// enforce the same rules.
[[nodiscard]] inline bool responseTrailerFieldValid(std::string_view name, std::string_view value) noexcept {
    return isValidResponseTrailerName(name) && !isForbiddenResponseTrailerName(name) && isValidResponseTrailerValue(value);
}

class HttpResponseTrailerSectionResult;
class HttpResponseTrailerSectionFailure;

class HttpResponseTrailerSectionError final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "invalid HTTP response trailer section";
    }

private:
    friend class HttpResponseTrailerSectionFailure;

    HttpResponseTrailerSectionError() noexcept = default;
};

// Borrowed proof that the complete terminal section passed the shared response-
// trailer rules. Protocol encoders accept this value instead of revalidating raw
// fields independently. The source span must outlive its synchronous consumption.
class HttpResponseTrailerSection final {
public:
    [[nodiscard]] std::span<const HttpHeaderView> fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

private:
    friend class HttpResponseTrailerSectionResult;
    friend HttpResponseTrailerSectionResult httpResponseTrailerSection(std::span<const HttpHeaderView>) noexcept;

    explicit HttpResponseTrailerSection(std::span<const HttpHeaderView> fields) noexcept
        : fields_(fields) {}

    std::span<const HttpHeaderView> fields_;
};

class HttpResponseTrailerSectionFailure final {
public:
    [[nodiscard]] HttpResponseTrailerSectionError exception() const noexcept {
        return HttpResponseTrailerSectionError();
    }

private:
    friend class HttpResponseTrailerSectionResult;
    friend HttpResponseTrailerSectionResult httpResponseTrailerSection(std::span<const HttpHeaderView>) noexcept;

    HttpResponseTrailerSectionFailure() noexcept = default;
};

class HttpResponseTrailerSectionResult final {
public:
    [[nodiscard]] const HttpResponseTrailerSection* section() const& noexcept {
        return std::get_if<HttpResponseTrailerSection>(&value_);
    }
    [[nodiscard]] const HttpResponseTrailerSection* section() const&& = delete;

    [[nodiscard]] const HttpResponseTrailerSectionFailure* failure() const& noexcept {
        return std::get_if<HttpResponseTrailerSectionFailure>(&value_);
    }
    [[nodiscard]] const HttpResponseTrailerSectionFailure* failure() const&& = delete;

private:
    friend HttpResponseTrailerSectionResult httpResponseTrailerSection(std::span<const HttpHeaderView>) noexcept;

    using Value = std::variant<HttpResponseTrailerSection, HttpResponseTrailerSectionFailure>;

    template <typename Alternative>
    explicit HttpResponseTrailerSectionResult(Alternative alternative) noexcept
        : value_(alternative) {}

    Value value_;
};

// A validated section retains the caller's header array until the synchronous
// protocol submission completes.  Letting a temporary std::array/vector convert
// to span here would return a proof object whose field storage had already died.
template <typename Range>
concept HttpTemporaryOwningResponseTrailerRange =
    !std::is_lvalue_reference_v<Range&&> &&
    std::ranges::contiguous_range<Range> &&
    !std::ranges::borrowed_range<Range> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<Range>>, HttpHeaderView>;

template <HttpTemporaryOwningResponseTrailerRange Headers>
HttpResponseTrailerSectionResult httpResponseTrailerSection(Headers&&) noexcept = delete;

// Validate the whole section before head, encoder, output, or stream mutation.
[[nodiscard]] inline HttpResponseTrailerSectionResult httpResponseTrailerSection(std::span<const HttpHeaderView> trailers) noexcept {
    if (trailers.size() > kMaxHttpHeaderFields) {
        return HttpResponseTrailerSectionResult(HttpResponseTrailerSectionFailure());
    }
    HttpHeaderSectionSize sectionSize;
    for (const auto& trailer : trailers) {
        if (!responseTrailerFieldValid(trailer.name(), trailer.value()) || !sectionSize.add(trailer.name(), trailer.value())) {
            return HttpResponseTrailerSectionResult(HttpResponseTrailerSectionFailure());
        }
    }
    return HttpResponseTrailerSectionResult(HttpResponseTrailerSection(trailers));
}

template <HttpTemporaryOwningResponseTrailerRange Headers>
HttpResponseTrailerSectionResult validatedResponseTrailerSection(Headers&&) = delete;

// Validate a caller's trailers, throwing the typed failure. The caller keeps the
// returned result: the section it exposes borrows from it.
[[nodiscard]] inline HttpResponseTrailerSectionResult validatedResponseTrailerSection(std::span<const HttpHeaderView> trailers) {
    auto result = httpResponseTrailerSection(trailers);
    if (const auto* failure = result.failure()) {
        throw failure->exception();
    }
    return result;
}

}  // namespace ruvia::detail
