#include "ruvia/http/HttpResponse.h"

#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpSetCookie.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeadersAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/ResponseHeaderIndexCache.h"
#include "ruvia/http/detail/util/AsciiCase.h"

// Set-Cookie is the one response field that neither replaces nor appends by
// field name: a second cookie for the same storage key (name, domain, path)
// replaces the earlier one while cookies of other scopes stay. These operations
// own that rule, including finding the earlier line and dropping the ones it
// shadows.

namespace ruvia {
namespace {

[[nodiscard]] bool setCookieWireNameMatches(std::string_view value, std::string_view wirePrefix, std::string_view cookieName) noexcept {
    if (value.size() != wirePrefix.size() + cookieName.size()) {
        return false;
    }
    if (!value.starts_with(wirePrefix)) {
        return false;
    }
    return value.substr(wirePrefix.size()) == cookieName;
}

[[nodiscard]] bool setCookieValueMatchesStorageKey(
    std::string_view value,
    std::string_view wirePrefix,
    std::string_view cookieName,
    bool hasPath,
    std::string_view path,
    std::string_view domain) noexcept {
    const auto parsed = parseSetCookie(value);
    return parsed.has_value() &&
        isValidHttpHeaderName(parsed->name) &&
        setCookieWireNameMatches(parsed->name, wirePrefix, cookieName) &&
        parsed->hasPathAttribute == hasPath &&
        (!hasPath || parsed->path == path) &&
        detail::httpAsciiEqualsIgnoreCase(parsed->domain, domain);
}

}  // namespace

HttpResponseHeader& HttpResponse::upsertSetCookieHeaderUninitializedValue(
    std::string_view wirePrefix,
    std::string_view cookieName,
    std::string_view path,
    std::string_view domain,
    std::size_t valueSize) {
    const bool hasPath = !path.empty();
    auto* retained = findSetCookieHeader(wirePrefix, cookieName, hasPath, path, domain);
    if (retained == nullptr) {
        return appendHeaderUninitializedValue("Set-Cookie", valueSize, detail::kResponseHeaderSetCookie);
    }

    headers_.assignUninitializedValue(*retained, "Set-Cookie", valueSize, detail::kResponseHeaderSetCookie);
    detail::setResponseHeaderAppend(*retained, true);
    eraseLaterSetCookieHeaders(*retained, wirePrefix, cookieName, hasPath, path, domain);
    return *retained;
}

void HttpResponse::upsertSetCookieHeaderValidated(std::string_view value) {
    const auto parsed = parseSetCookie(value);
    const auto cookieName = parsed.has_value() && isValidHttpHeaderName(parsed->name) ? parsed->name : std::string_view{};
    if (cookieName.empty()) {
        appendHeaderValidated("Set-Cookie", value, detail::kResponseHeaderSetCookie);
        return;
    }

    auto* retained = findSetCookieHeader({}, cookieName, parsed->hasPathAttribute, parsed->path, parsed->domain);
    if (retained == nullptr) {
        appendHeaderValidated("Set-Cookie", value, detail::kResponseHeaderSetCookie);
        return;
    }

    headers_.assign(*retained, "Set-Cookie", value, detail::kResponseHeaderSetCookie);
    detail::setResponseHeaderAppend(*retained, true);
    eraseLaterSetCookieHeaders(*retained, {}, cookieName, parsed->hasPathAttribute, parsed->path, parsed->domain);
}

HttpResponseHeader* HttpResponse::findSetCookieHeader(
    std::string_view wirePrefix,
    std::string_view cookieName,
    bool hasPath,
    std::string_view path,
    std::string_view domain) noexcept {
    for (auto& header : headers_) {
        if (detail::responseHeaderKnownBit(header) == detail::kResponseHeaderSetCookie &&
            setCookieValueMatchesStorageKey(header.value(), wirePrefix, cookieName, hasPath, path, domain)) {
            return &header;
        }
    }
    return nullptr;
}

void HttpResponse::eraseLaterSetCookieHeaders(
    HttpResponseHeader& retained,
    std::string_view wirePrefix,
    std::string_view cookieName,
    bool hasPath,
    std::string_view path,
    std::string_view domain) noexcept {
    // A response might already contain duplicates introduced through the raw
    // header API. Once an authoritative cookie path owns this storage key,
    // collapse every later occurrence so the final response has one value.
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* write = &retained + 1;
    for (auto* read = &retained + 1; read != end; ++read) {
        if (detail::responseHeaderKnownBit(*read) == detail::kResponseHeaderSetCookie &&
            setCookieValueMatchesStorageKey(read->value(), wirePrefix, cookieName, hasPath, path, domain)) {
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
