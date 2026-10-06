#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpSetCookie.h"
#include "ruvia/http/HttpSetCookiePlan.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/HttpResponseHeadersAccess.h"
#include "ruvia/http/detail/response/ResponseHeaderIndexCache.h"
#include "ruvia/http/detail/util/AsciiCase.h"

// Set-Cookie is the one response field that neither replaces nor appends by
// field name: a second cookie for the same storage key (name, domain, path)
// replaces the earlier one while cookies of other scopes stay. These operations
// own that rule, including finding the earlier line and dropping the ones it
// shadows.

namespace ruvia {
namespace {

[[nodiscard]] bool setCookieWireNameMatches(
    std::string_view value, std::string_view wirePrefix, std::string_view cookieName) noexcept {
    if (value.size() != wirePrefix.size() + cookieName.size()) {
        return false;
    }
    if (!value.starts_with(wirePrefix)) {
        return false;
    }
    return value.substr(wirePrefix.size()) == cookieName;
}

[[nodiscard]] bool setCookieValueMatchesStorageKey(std::string_view value,
    std::string_view wirePrefix, std::string_view cookieName, bool hasPath, std::string_view path,
    std::string_view domain) noexcept {
    const auto parsed = parseSetCookie(value);
    return parsed.has_value() && isValidHttpHeaderName(parsed->name()) &&
           setCookieWireNameMatches(parsed->name(), wirePrefix, cookieName) &&
           parsed->has(HttpSetCookieAttribute::kPath) == hasPath &&
           (!hasPath || parsed->path() == path) &&
           detail::httpAsciiEqualsIgnoreCase(parsed->domain(), domain);
}

}  // namespace

void HttpResponse::setCookie(const SetCookiePlan& plan) {
    auto* retained = findSetCookieHeader(
        plan.wirePrefix(), plan.name(), !plan.path().empty(), plan.path(), plan.domain());
    if (retained == nullptr) {
        auto& header = appendHeaderUninitializedValue(
            "Set-Cookie", plan.size(), detail::kResponseHeaderSetCookie);
        plan.write(detail::responseHeaderValueBegin(header));
        return;
    }

    const bool borrows_retained =
        detail::response_header_storage_overlaps(*retained, plan.name_) ||
        detail::response_header_storage_overlaps(*retained, plan.value_) ||
        detail::response_header_storage_overlaps(*retained, plan.path_) ||
        detail::response_header_storage_overlaps(*retained, plan.domain_);
    SetCookiePlan::written_fields written;
    if (borrows_retained) {
        auto prepared = headers_.makeUninitializedHeader(
            "Set-Cookie", plan.size(), detail::kResponseHeaderSetCookie);
        written = plan.write_fields(detail::responseHeaderValueBegin(prepared));
        headers_.releaseHeader(*retained);
        *retained = prepared;
    } else {
        headers_.assignUninitializedValue(
            *retained, "Set-Cookie", plan.size(), detail::kResponseHeaderSetCookie);
        written = plan.write_fields(detail::responseHeaderValueBegin(*retained));
    }
    detail::setResponseHeaderAppend(*retained, true);
    eraseLaterSetCookieHeaders(*retained, written.wire_name_, !written.path_.empty(),
        written.path_, written.domain_);
}

void HttpResponse::upsertSetCookieHeaderValidated(std::string_view value) {
    const auto parsed = parseSetCookie(value);
    const auto cookieName = parsed.has_value() && isValidHttpHeaderName(parsed->name())
                                ? parsed->name()
                                : std::string_view{};
    if (cookieName.empty()) {
        appendHeaderValidated("Set-Cookie", value, detail::kResponseHeaderSetCookie);
        return;
    }

    const auto hasPath = parsed->has(HttpSetCookieAttribute::kPath);
    auto* retained = findSetCookieHeader({}, cookieName, hasPath, parsed->path(), parsed->domain());
    if (retained == nullptr) {
        appendHeaderValidated("Set-Cookie", value, detail::kResponseHeaderSetCookie);
        return;
    }

    const auto name_size = cookieName.size();
    const auto path_size = parsed->path().size();
    const auto domain_size = parsed->domain().size();
    const auto name_offset = static_cast<std::size_t>(cookieName.data() - value.data());
    const auto path_offset = parsed->path().empty() ? std::size_t{0}
                                                    : static_cast<std::size_t>(parsed->path().data() - value.data());
    const auto domain_offset = parsed->domain().empty() ? std::size_t{0}
                                                        : static_cast<std::size_t>(parsed->domain().data() - value.data());
    headers_.assign(*retained, "Set-Cookie", value, detail::kResponseHeaderSetCookie);
    detail::setResponseHeaderAppend(*retained, true);
    const auto copied_value = retained->value();
    eraseLaterSetCookieHeaders(*retained, copied_value.substr(name_offset, name_size), hasPath,
        copied_value.substr(path_offset, path_size), copied_value.substr(domain_offset, domain_size));
}

HttpResponseHeader* HttpResponse::findSetCookieHeader(std::string_view wirePrefix,
    std::string_view cookieName, bool hasPath, std::string_view path,
    std::string_view domain) noexcept {
    for (auto& header : headers_) {
        if (detail::responseHeaderKnownBit(header) == detail::kResponseHeaderSetCookie &&
            setCookieValueMatchesStorageKey(
                header.value(), wirePrefix, cookieName, hasPath, path, domain)) {
            return &header;
        }
    }
    return nullptr;
}

void HttpResponse::eraseLaterSetCookieHeaders(HttpResponseHeader& retained,
    std::string_view cookieName, bool hasPath, std::string_view path,
    std::string_view domain) noexcept {
    // A response might already contain duplicates introduced through the raw
    // header API. Once an authoritative cookie path owns this storage key,
    // collapse every later occurrence so the final response has one value.
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* write = &retained + 1;
    for (auto* read = &retained + 1; read != end; ++read) {
        if (detail::responseHeaderKnownBit(*read) == detail::kResponseHeaderSetCookie &&
            setCookieValueMatchesStorageKey(
                read->value(), {}, cookieName, hasPath, path, domain)) {
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
