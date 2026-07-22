#include "ruvia/http/HttpResponse.h"

#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeadersAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/ResponseHeaderIndexCache.h"

// Set-Cookie is the one response field that neither replaces nor appends by
// field name: a second cookie of the SAME cookie-name must replace the earlier
// one while cookies of other names stay (RFC 6265). These operations own that
// rule, including finding the earlier line and dropping the ones it shadows.

namespace ruvia {
namespace {

[[nodiscard]] bool setCookieValueHasWireName(
    std::string_view value,
    std::string_view wirePrefix,
    std::string_view cookieName) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    if (!value.starts_with(wirePrefix)) {
        return false;
    }
    value.remove_prefix(wirePrefix.size());
    if (!value.starts_with(cookieName)) {
        return false;
    }
    value.remove_prefix(cookieName.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    return !value.empty() && value.front() == '=';
}

[[nodiscard]] std::string_view setCookieWireName(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {};
    }
    auto name = value.substr(0, equals);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.remove_suffix(1);
    }
    return isValidHttpHeaderName(name) ? name : std::string_view{};
}

}  // namespace

HttpResponseHeader& HttpResponse::upsertSetCookieHeaderUninitializedValue(
    std::string_view wirePrefix,
    std::string_view cookieName,
    std::size_t valueSize) {
    auto* retained = findSetCookieHeader(wirePrefix, cookieName);
    if (retained == nullptr) {
        return appendHeaderUninitializedValue(
            "Set-Cookie", valueSize, detail::kResponseHeaderSetCookie);
    }

    headers_.assignUninitializedValue(
        *retained, "Set-Cookie", valueSize, detail::kResponseHeaderSetCookie);
    detail::setResponseHeaderAppend(*retained, true);
    eraseLaterSetCookieHeaders(*retained, wirePrefix, cookieName);
    return *retained;
}

void HttpResponse::upsertSetCookieHeaderValidated(std::string_view value) {
    const auto cookieName = setCookieWireName(value);
    if (cookieName.empty()) {
        appendHeaderValidated(
            "Set-Cookie", value, detail::kResponseHeaderSetCookie);
        return;
    }

    auto* retained = findSetCookieHeader({}, cookieName);
    if (retained == nullptr) {
        appendHeaderValidated(
            "Set-Cookie", value, detail::kResponseHeaderSetCookie);
        return;
    }

    headers_.assign(
        *retained, "Set-Cookie", value, detail::kResponseHeaderSetCookie);
    detail::setResponseHeaderAppend(*retained, true);
    eraseLaterSetCookieHeaders(*retained, {}, cookieName);
}

HttpResponseHeader* HttpResponse::findSetCookieHeader(
    std::string_view wirePrefix,
    std::string_view cookieName) noexcept {
    for (auto& header : headers_) {
        if (detail::responseHeaderKnownBit(header) == detail::kResponseHeaderSetCookie &&
            setCookieValueHasWireName(header.value(), wirePrefix, cookieName)) {
            return &header;
        }
    }
    return nullptr;
}

void HttpResponse::eraseLaterSetCookieHeaders(
    HttpResponseHeader& retained,
    std::string_view wirePrefix,
    std::string_view cookieName) noexcept {
    // A response might already contain duplicates introduced through the raw
    // header API. Once an authoritative cookie path owns this name, collapse
    // every later occurrence so the final response has one value.
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* write = &retained + 1;
    for (auto* read = &retained + 1; read != end; ++read) {
        if (detail::responseHeaderKnownBit(*read) == detail::kResponseHeaderSetCookie &&
            setCookieValueHasWireName(read->value(), wirePrefix, cookieName)) {
            headers_.releaseHeader(*read);
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != end) {
        detail::HttpResponseHeadersAccess::truncate(headers_, begin, write);
    }
    rebuildKnownHeaderIndex();
}

}  // namespace ruvia
