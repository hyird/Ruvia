#pragma once

#include <concepts>
#include <exception>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpResponseTrailerSection.h"
#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/HttpResponseKnownHeaders.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/util/HttpOws.h"

namespace ruvia::detail {

// Fields that must never appear in a trailer section because they govern message
// framing, routing, authentication, response controls, or content format
// (RFC 9110 §6.5.1, RFC 9113 §8.1).
[[nodiscard]] inline bool isForbiddenResponseTrailerName(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classifyResponseHeaderName(name);
        known != 0 && known != kResponseHeaderEtag && known != kResponseHeaderAcceptRanges) {
        return true;
    }

    if (is_forbidden_common_trailer_name(name, classifyRequestHeader(name))) {
        return true;
    }

    switch (name.size()) {
        case 3:
            // Response control data (RFC 9110 §7.4).
            return httpAsciiEqualsIgnoreCase(name, "Age");
        case 6:
            return httpAsciiEqualsIgnoreCase(name, "Pragma");
        case 7:
            return httpAsciiEqualsIgnoreCase(name, "Expires") ||
                   httpAsciiEqualsIgnoreCase(name, "Warning");
        case 11:
            return httpAsciiEqualsIgnoreCase(name, "Retry-After");
        case 15:
            return httpAsciiEqualsIgnoreCase(name, "X-Frame-Options") ||
                   httpAsciiEqualsIgnoreCase(name, "Referrer-Policy") ||
                   httpAsciiEqualsIgnoreCase(name, "Clear-Site-Data");
        case 16:
            return httpAsciiEqualsIgnoreCase(name, "X-XSS-Protection") ||
                   httpAsciiEqualsIgnoreCase(name, "WWW-Authenticate");
        case 18:
            return httpAsciiEqualsIgnoreCase(name, "Permissions-Policy");
        case 19:
            return httpAsciiEqualsIgnoreCase(name, "Content-Disposition");
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

[[nodiscard]] inline bool isValidHttpResponseTrailerFieldValue(
    std::string_view value, HttpFieldListRole role) noexcept {
    return isValidHttpTrailerFieldValue(value, role,
        [](std::string_view name) noexcept { return isForbiddenResponseTrailerName(name); });
}

// Name syntax is established by the field API or the wire parser's token scan.
[[nodiscard]] inline bool response_trailer_content_valid(
    std::string_view name, std::string_view value) noexcept {
    return !isForbiddenResponseTrailerName(name) && is_valid_http_field_value(value);
}

// True if (name, value) is an acceptable response trailer field. Shared by the
// HTTP/1.1 chunked-trailer and HTTP/2 trailing-HEADERS sinks so both transports
// enforce the same rules.
[[nodiscard]] inline bool responseTrailerFieldValid(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_field_name(name) && response_trailer_content_valid(name, value);
}

// Visit a parsed HTTP/1 chunked response trailer block. The input is the bytes
// between the terminal zero-size chunk and the empty line that ends the trailer
// section; a final CRLF after the last field is also accepted for standalone
// validation. Values are exposed after HTTP optional whitespace has been
// stripped, matching the normalized public response-trailer contract.
template <typename Visitor>
[[nodiscard]] inline bool visitHttpResponseTrailerFields(
    std::string_view trailers, Visitor&& visitor) {
    if (trailers.size() > kMaxHttpHeaderBytes) {
        return false;
    }

    std::size_t cursor = 0;
    std::size_t fieldCount = 0;
    while (cursor < trailers.size()) {
        if (fieldCount == kMaxHttpHeaderFields) {
            return false;
        }
        ++fieldCount;

        const auto lineEnd = trailers.find("\r\n", cursor);
        const auto line = lineEnd == std::string_view::npos
                              ? trailers.substr(cursor)
                              : trailers.substr(cursor, lineEnd - cursor);
        // Finding the separator also proves that the complete name is a token.
        const auto colon = http_token_prefix_size(line);
        if (colon == 0 || colon == line.size() || line[colon] != ':') {
            return false;
        }

        const auto name = line.substr(0, colon);
        const auto value = httpTrimOws(line.substr(colon + 1));
        if (!response_trailer_content_valid(name, value) || !visitor(name, value)) {
            return false;
        }

        if (lineEnd == std::string_view::npos) {
            return true;
        }
        cursor = lineEnd + 2;
    }
    return true;
}

[[nodiscard]] inline bool httpResponseTrailerBlockValid(std::string_view trailers) {
    return visitHttpResponseTrailerFields(
        trailers, [](std::string_view, std::string_view) noexcept { return true; });
}

class HttpResponseTrailerSectionResult;
[[nodiscard]] HttpResponseTrailerSectionResult httpResponseTrailerSection(
    std::span<const HttpHeaderView>) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

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
using HttpResponseTrailerSection = ::ruvia::HttpResponseTrailerSection;

class HttpResponseTrailerSectionFailure final {
public:
    [[nodiscard]] HttpResponseTrailerSectionError exception() const noexcept {
        return HttpResponseTrailerSectionError();
    }

private:
    friend class HttpResponseTrailerSectionResult;
    friend HttpResponseTrailerSectionResult httpResponseTrailerSection(
        std::span<const HttpHeaderView>) noexcept;

    HttpResponseTrailerSectionFailure() noexcept = default;
};

class HttpResponseTrailerSectionResult final {
public:
    [[nodiscard]] const HttpResponseTrailerSection* section() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] const HttpResponseTrailerSection* section() const&& = delete;

    [[nodiscard]] const HttpResponseTrailerSectionFailure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] const HttpResponseTrailerSectionFailure* failure() const&& = delete;

private:
    friend HttpResponseTrailerSectionResult httpResponseTrailerSection(
        std::span<const HttpHeaderView>) noexcept;

    using Value = std::variant<HttpResponseTrailerSection, HttpResponseTrailerSectionFailure>;

    explicit HttpResponseTrailerSectionResult(HttpResponseTrailerSection section) noexcept
        : value_(section) {}

    explicit HttpResponseTrailerSectionResult(HttpResponseTrailerSectionFailure failure) noexcept
        : value_(failure) {}

    Value value_;
};

// A validated section retains the caller's header array until the synchronous
// protocol submission completes.  Letting a temporary std::array/vector convert
// to span here would return a proof object whose field storage had already died.
template <typename Range>
concept HttpTemporaryOwningResponseTrailerRange =
    !std::is_lvalue_reference_v<Range&&> && std::ranges::contiguous_range<Range> &&
    !std::ranges::borrowed_range<Range> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<Range>>, HttpHeaderView>;

template <HttpTemporaryOwningResponseTrailerRange Headers>
HttpResponseTrailerSectionResult httpResponseTrailerSection(Headers&&) noexcept = delete;

// Validate the whole section before head, encoder, output, or stream mutation.
[[nodiscard]] inline HttpResponseTrailerSectionResult httpResponseTrailerSection(
    std::span<const HttpHeaderView> trailers) noexcept {
    if (trailers.size() > kMaxHttpHeaderFields) {
        return HttpResponseTrailerSectionResult(HttpResponseTrailerSectionFailure());
    }
    HttpHeaderSectionSize sectionSize;
    for (const auto& trailer : trailers) {
        if (!responseTrailerFieldValid(trailer.name(), trailer.value()) ||
            !sectionSize.add(trailer.name(), trailer.value())) {
            return HttpResponseTrailerSectionResult(HttpResponseTrailerSectionFailure());
        }
    }
    return HttpResponseTrailerSectionResult(HttpResponseTrailerSection(trailers));
}

template <HttpTemporaryOwningResponseTrailerRange Headers>
HttpResponseTrailerSectionResult validatedResponseTrailerSection(Headers&&) = delete;

// Validate a caller's trailers, throwing the typed failure. The caller keeps the
// returned result: the section it exposes borrows from it.
[[nodiscard]] inline HttpResponseTrailerSectionResult validatedResponseTrailerSection(
    std::span<const HttpHeaderView> trailers) {
    auto result = httpResponseTrailerSection(trailers);
    if (const auto* failure = result.failure()) {
        throw failure->exception();
    }
    return result;
}

}  // namespace ruvia::detail
