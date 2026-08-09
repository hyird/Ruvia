#include "ruvia/http/HttpClientRedirect.h"

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/detail/client/HttpOriginView.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

#include <utility>

namespace ruvia {
namespace {

[[nodiscard]] bool isHttpClientUriScheme(std::string_view value) noexcept {
    if (value.empty() || !((value.front() >= 'A' && value.front() <= 'Z') || (value.front() >= 'a' && value.front() <= 'z'))) {
        return false;
    }
    for (const char ch : value.substr(1)) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool isValidHttpClientUriFragment(std::string_view fragment) noexcept {
    if (fragment.empty()) {
        return true;
    }
    // RFC 3986 section 3.5 permits pchar, '/', and '?'. The shared
    // request-target byte validator covers that grammar and percent encoding,
    // except that its authority union also admits IP-literal brackets.
    return fragment.find_first_of("[]") == std::string_view::npos && detail::isValidRequestTargetBytes(fragment);
}

void removeHttpClientLastPathSegment(std::pmr::string& path) noexcept {
    const auto slash = path.rfind('/');
    if (slash == std::pmr::string::npos) {
        path.clear();
        return;
    }
    path.erase(slash);
}

[[nodiscard]] bool normalizeHttpClientAbsolutePath(std::string_view path, std::pmr::string& normalized) {
    if (path.empty() || path.front() != '/') {
        return false;
    }

    normalized.clear();
    normalized.reserve(path.size());
    auto remaining = path;
    while (!remaining.empty()) {
        // RFC 3986 section 5.2.4 deliberately moves one path segment at a
        // time. Empty segments are significant: reducing the path to a stack
        // of non-dot segments would turn "/a//." into "/a/" instead of the
        // required "/a//".
        if (remaining.starts_with("../")) {
            remaining.remove_prefix(3);
        } else if (remaining.starts_with("./")) {
            remaining.remove_prefix(2);
        } else if (remaining.starts_with("/./")) {
            remaining.remove_prefix(2);
        } else if (remaining == "/.") {
            remaining = "/";
        } else if (remaining.starts_with("/../")) {
            remaining.remove_prefix(3);
            removeHttpClientLastPathSegment(normalized);
        } else if (remaining == "/..") {
            remaining = "/";
            removeHttpClientLastPathSegment(normalized);
        } else if (remaining == "." || remaining == "..") {
            remaining = {};
        } else {
            const auto nextSlash = remaining.front() == '/' ? remaining.find('/', 1) : remaining.find('/');
            const auto segmentBytes = nextSlash == std::string_view::npos ? remaining.size() : nextSlash;
            normalized.append(remaining.substr(0, segmentBytes));
            remaining.remove_prefix(segmentBytes);
        }
    }

    if (normalized.empty()) {
        normalized.push_back('/');
    }
    return true;
}

// Merges the reference's path/query with the current origin-form target per
// RFC 3986 section 5.3 and validates the resolved origin-form target. Returns
// false when the location's path or the merged product is not a valid target.
// `reference` is the URI-reference with any scheme/authority prefix and the
// fragment already removed.
[[nodiscard]] bool resolveHttpClientRedirectPathAndQuery(bool hasAuthority, std::string_view reference, std::string_view currentTarget, std::pmr::memory_resource* targetResource, std::pmr::string& resolved) {
    const auto queryAt = reference.find('?');
    const bool hasReferenceQuery = queryAt != std::string_view::npos;
    const auto referencePath = hasReferenceQuery ? reference.substr(0, queryAt) : reference;
    const auto referenceQuery = hasReferenceQuery ? reference.substr(queryAt + 1) : std::string_view{};

    std::pmr::string mergedPath(targetResource);
    std::string_view selectedQuery;
    bool hasSelectedQuery = hasReferenceQuery;

    if (hasAuthority) {
        if (!referencePath.empty() && referencePath.front() != '/') {
            return false;
        }
        mergedPath.assign(referencePath.empty() ? "/" : referencePath);
        selectedQuery = referenceQuery;
    } else {
        const auto baseQueryAt = currentTarget.find('?');
        const bool hasBaseQuery = baseQueryAt != std::string_view::npos;
        const auto basePath = hasBaseQuery ? currentTarget.substr(0, baseQueryAt) : currentTarget;
        const auto baseQuery = hasBaseQuery ? currentTarget.substr(baseQueryAt + 1) : std::string_view{};

        if (referencePath.empty()) {
            mergedPath.assign(basePath);
            if (hasReferenceQuery) {
                selectedQuery = referenceQuery;
            } else {
                hasSelectedQuery = hasBaseQuery;
                selectedQuery = baseQuery;
            }
        } else if (referencePath.front() == '/') {
            mergedPath.assign(referencePath);
            selectedQuery = referenceQuery;
        } else {
            const auto lastSlash = basePath.rfind('/');
            mergedPath.assign(basePath.substr(0, lastSlash + 1));
            mergedPath.append(referencePath.data(), referencePath.size());
            selectedQuery = referenceQuery;
        }
    }

    if (!normalizeHttpClientAbsolutePath(mergedPath, resolved)) {
        return false;
    }
    if (hasSelectedQuery) {
        resolved.push_back('?');
        resolved.append(selectedQuery.data(), selectedQuery.size());
    }
    return isValidHttpClientOriginTarget(resolved);
}

}  // namespace

bool isValidHttpClientOriginTarget(std::string_view target) noexcept {
    return detail::isValidOriginFormTarget(target);
}

bool isHttpClientRedirectStatus(HttpStatusCode status) noexcept {
    return status == http_status::kMovedPermanently || status == http_status::kFound || status == http_status::kSeeOther || status == http_status::kTemporaryRedirect || status == http_status::kPermanentRedirect;
}

HttpClientResponseHeaderLookupResult lookupUniqueHttpClientResponseHeader(const HttpClientResponseHead& head, std::string_view name) noexcept {
    std::string_view found;
    bool seen = false;
    for (const auto& header : head.headers()) {
        if (!detail::httpAsciiEqualsIgnoreCase(header.name(), name)) {
            continue;
        }
        if (seen) {
            return HttpClientResponseHeaderLookupResult::makeRepeated();
        }
        seen = true;
        found = header.value();
    }
    return seen ? HttpClientResponseHeaderLookupResult::makeFound(found) : HttpClientResponseHeaderLookupResult::makeAbsent();
}

HttpClientRedirectRequestPlan::HttpClientRedirectRequestPlan(std::string_view method, HttpClientRedirectContentDisposition contentDisposition, std::pmr::memory_resource* resource)
    : method_(method, detail::httpPmrResourceOrDefault(resource)),
      contentDisposition_(contentDisposition) {}

HttpClientRedirectRequestPlan planHttpClientRedirectRequest(const HttpClientRequestView& request, HttpStatusCode status, std::pmr::memory_resource* resource) {
    if (status == http_status::kSeeOther) {
        return HttpClientRedirectRequestPlan(request.method == "HEAD" ? request.method.view() : std::string_view("GET"), HttpClientRedirectContentDisposition::kDrop, resource);
    }
    if ((status == http_status::kMovedPermanently || status == http_status::kFound) && request.method == "POST") {
        return HttpClientRedirectRequestPlan("GET", HttpClientRedirectContentDisposition::kDrop, resource);
    }
    return HttpClientRedirectRequestPlan(request.method.view(), HttpClientRedirectContentDisposition::kPreserve, resource);
}

HttpClientOriginAuthorityStatus classifyHttpClientOriginAuthority(const HttpOriginView& origin, std::string_view authority) noexcept {
    if (authority.find('@') != std::string_view::npos) {
        return HttpClientOriginAuthorityStatus::kInvalidAuthority;
    }
    const auto parsed = detail::parseHttpAuthority(authority);
    if (!parsed) {
        return HttpClientOriginAuthorityStatus::kInvalidAuthority;
    }
    return parsed->effectivePort(detail::httpSchemeDefaultPort(origin.scheme())) == origin.port() && detail::httpUriHostEquals(parsed->host(), origin.host()) ? HttpClientOriginAuthorityStatus::kSameOrigin : HttpClientOriginAuthorityStatus::kDifferentOrigin;
}


HttpOriginView HttpClientResolvedRedirect::origin() const& {
    return scheme_ == HttpScheme::kHttps ? HttpOriginView::https(host(), port_) : HttpOriginView::http(host(), port_);
}

HttpClientRedirectResolutionResult resolveHttpClientRedirectTarget(const HttpOriginView& origin, std::string_view currentTarget, std::string_view location, std::pmr::memory_resource* resource) {
    if (currentTarget.empty() || currentTarget.front() != '/' || !isValidHttpClientOriginTarget(currentTarget)) {
        return HttpClientRedirectResolutionResult::makeFailure(HttpClientRedirectResolutionError::kInvalidCurrentTarget);
    }

    location = detail::httpTrimOws(location);
    if (const auto hash = location.find('#'); hash != std::string_view::npos) {
        if (!isValidHttpClientUriFragment(location.substr(hash + 1))) {
            return HttpClientRedirectResolutionResult::makeFailure(HttpClientRedirectResolutionError::kInvalidLocation);
        }
        location = location.substr(0, hash);
    }

    bool hasAuthority = false;
    auto targetScheme = origin.scheme();
    std::string_view reference = location;
    const auto colon = reference.find(':');
    const auto firstPathOrQuery = reference.find_first_of("/?");
    if (colon != std::string_view::npos && (firstPathOrQuery == std::string_view::npos || colon < firstPathOrQuery)) {
        const auto scheme = reference.substr(0, colon);
        if (!isHttpClientUriScheme(scheme)) {
            return HttpClientRedirectResolutionResult::makeFailure(HttpClientRedirectResolutionError::kInvalidLocation);
        }
        const bool isHttps = detail::httpAsciiEqualsIgnoreCase(scheme, "https");
        if (!isHttps && !detail::httpAsciiEqualsIgnoreCase(scheme, "http")) {
            return HttpClientRedirectResolutionResult::makeFailure(HttpClientRedirectResolutionError::kUnsupportedScheme);
        }
        targetScheme = isHttps ? HttpScheme::kHttps : HttpScheme::kHttp;
        reference.remove_prefix(colon + 1);
        if (!reference.starts_with("//")) {
            return HttpClientRedirectResolutionResult::makeFailure(HttpClientRedirectResolutionError::kInvalidLocation);
        }
        reference.remove_prefix(2);
        hasAuthority = true;
    } else if (reference.starts_with("//")) {
        reference.remove_prefix(2);
        hasAuthority = true;
    }

    auto* const targetResource = detail::httpPmrResourceOrDefault(resource);
    std::pmr::string host(targetResource);
    auto port = origin.port();
    bool crossOrigin = targetScheme != origin.scheme();
    if (hasAuthority) {
        const auto authorityEnd = reference.find_first_of("/?");
        const auto authority = authorityEnd == std::string_view::npos ? reference : reference.substr(0, authorityEnd);
        if (authority.find('@') != std::string_view::npos) {
            return HttpClientRedirectResolutionResult::makeFailure(HttpClientRedirectResolutionError::kInvalidLocation);
        }
        const auto parsed = detail::parseHttpAuthority(authority);
        if (!parsed) {
            return HttpClientRedirectResolutionResult::makeFailure(HttpClientRedirectResolutionError::kInvalidLocation);
        }
        host.assign(parsed->host().data(), parsed->host().size());
        port = parsed->effectivePort(detail::httpSchemeDefaultPort(targetScheme));
        crossOrigin = crossOrigin || port != origin.port() || !detail::httpUriHostEquals(parsed->host(), origin.host());
        reference = authorityEnd == std::string_view::npos ? std::string_view{} : reference.substr(authorityEnd);
    } else {
        host.assign(origin.host().data(), origin.host().size());
    }

    std::pmr::string resolved(targetResource);
    if (!resolveHttpClientRedirectPathAndQuery(hasAuthority, reference, currentTarget, targetResource, resolved)) {
        return HttpClientRedirectResolutionResult::makeFailure(HttpClientRedirectResolutionError::kInvalidLocation);
    }

    return HttpClientRedirectResolutionResult::makeResolved(targetScheme, std::move(host), port, std::move(resolved), crossOrigin);
}

}  // namespace ruvia
