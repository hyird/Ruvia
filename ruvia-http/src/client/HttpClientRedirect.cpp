#include "ruvia/http/HttpClientRedirect.h"

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/PmrResource.h"
#include "ruvia/http/detail/client/HttpOrigin.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

#include <utility>

namespace ruvia {
namespace {

[[nodiscard]] bool isHttpClientUriScheme(std::string_view value) noexcept {
    if (value.empty() ||
        !((value.front() >= 'A' && value.front() <= 'Z') ||
          (value.front() >= 'a' && value.front() <= 'z'))) {
        return false;
    }
    for (const char ch : value.substr(1)) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

void removeHttpClientLastPathSegment(std::pmr::string& path) noexcept {
    const auto slash = path.rfind('/');
    if (slash == std::pmr::string::npos) {
        path.clear();
        return;
    }
    path.erase(slash);
}

[[nodiscard]] bool normalizeHttpClientAbsolutePath(
    std::string_view path,
    std::pmr::string& normalized) {
    if (path.empty() || path.front() != '/') {
        return false;
    }

    normalized.clear();
    normalized.reserve(path.size());
    std::size_t cursor = 0;
    while (cursor < path.size()) {
        if (path[cursor] != '/') {
            return false;
        }
        const auto nextSlash = path.find('/', cursor + 1);
        const bool last = nextSlash == std::string_view::npos;
        const auto segment = path.substr(
            cursor + 1,
            last ? std::string_view::npos : nextSlash - cursor - 1);

        if (segment == ".") {
            if (last && (normalized.empty() || normalized.back() != '/')) {
                normalized.push_back('/');
            }
        } else if (segment == "..") {
            removeHttpClientLastPathSegment(normalized);
            if (last && (normalized.empty() || normalized.back() != '/')) {
                normalized.push_back('/');
            }
        } else {
            normalized.push_back('/');
            normalized.append(segment.data(), segment.size());
        }

        if (last) {
            break;
        }
        cursor = nextSlash;
    }

    if (normalized.empty()) {
        normalized.push_back('/');
    }
    return true;
}

}  // namespace

bool isValidHttpClientOriginTarget(std::string_view target) noexcept {
    return detail::isValidOriginFormTarget(target);
}

bool isHttpClientRedirectStatus(std::uint16_t status) noexcept {
    return status == 301 || status == 302 || status == 303 ||
        status == 307 || status == 308;
}

HttpClientResponseHeaderLookupResult lookupUniqueHttpClientResponseHeader(
    const HttpClientResponse& response,
    std::string_view name) noexcept {
    std::string_view found;
    bool seen = false;
    for (const auto& header : response.headers()) {
        if (!detail::httpAsciiEqualsIgnoreCase(header.name(), name)) {
            continue;
        }
        if (seen) {
            return HttpClientResponseHeaderLookupResult::makeRepeated();
        }
        seen = true;
        found = header.value();
    }
    return seen
        ? HttpClientResponseHeaderLookupResult::makeFound(found)
        : HttpClientResponseHeaderLookupResult::makeAbsent();
}

HttpClientRedirectRequestPlan planHttpClientRedirectRequest(
    const HttpClientRequest& request,
    std::uint16_t status) noexcept {
    if (status == 303) {
        return HttpClientRedirectRequestPlan(
            request.method == "HEAD" ? request.method : std::string_view("GET"),
            HttpClientRedirectContentDisposition::kDrop);
    }
    if ((status == 301 || status == 302) && request.method == "POST") {
        return HttpClientRedirectRequestPlan(
            "GET",
            HttpClientRedirectContentDisposition::kDrop);
    }
    return HttpClientRedirectRequestPlan(
        request.method,
        HttpClientRedirectContentDisposition::kPreserve);
}

HttpClientOriginAuthorityStatus classifyHttpClientOriginAuthority(
    const HttpOrigin& origin,
    std::string_view authority) noexcept {
    if (authority.find('@') != std::string_view::npos) {
        return HttpClientOriginAuthorityStatus::kInvalidAuthority;
    }
    const auto parsed = detail::parseHttpAuthority(authority);
    if (!parsed) {
        return HttpClientOriginAuthorityStatus::kInvalidAuthority;
    }
    return parsed->effectivePort(detail::httpSchemeDefaultPort(origin.scheme())) ==
                origin.port() &&
            detail::httpUriHostEquals(parsed->host(), origin.host())
        ? HttpClientOriginAuthorityStatus::kSameOrigin
        : HttpClientOriginAuthorityStatus::kDifferentOrigin;
}

HttpClientRedirectTargetResult resolveHttpClientSameOriginRedirectTarget(
    const HttpOrigin& origin,
    std::string_view currentTarget,
    std::string_view location,
    std::pmr::memory_resource* resource) {
    if (currentTarget.empty() || currentTarget.front() != '/' ||
        !isValidHttpClientOriginTarget(currentTarget)) {
        return HttpClientRedirectTargetResult::makeFailure(
            HttpClientRedirectTargetError::kInvalidCurrentTarget);
    }

    location = detail::httpTrimOws(location);
    if (const auto hash = location.find('#'); hash != std::string_view::npos) {
        location = location.substr(0, hash);
    }

    bool hasAuthority = false;
    bool schemeMatchesOrigin = true;
    std::string_view reference = location;
    const auto colon = reference.find(':');
    const auto firstPathOrQuery = reference.find_first_of("/?");
    if (colon != std::string_view::npos &&
        (firstPathOrQuery == std::string_view::npos || colon < firstPathOrQuery)) {
        const auto scheme = reference.substr(0, colon);
        if (!isHttpClientUriScheme(scheme)) {
            return HttpClientRedirectTargetResult::makeFailure(
                HttpClientRedirectTargetError::kInvalidLocation);
        }
        const bool isHttps = detail::httpAsciiEqualsIgnoreCase(scheme, "https");
        if (!isHttps && !detail::httpAsciiEqualsIgnoreCase(scheme, "http")) {
            return HttpClientRedirectTargetResult::makeFailure(
                HttpClientRedirectTargetError::kNotSameOrigin);
        }
        const auto locationScheme = isHttps ? HttpScheme::kHttps : HttpScheme::kHttp;
        schemeMatchesOrigin = locationScheme == origin.scheme();
        reference.remove_prefix(colon + 1);
        if (!reference.starts_with("//")) {
            return HttpClientRedirectTargetResult::makeFailure(
                HttpClientRedirectTargetError::kInvalidLocation);
        }
        reference.remove_prefix(2);
        hasAuthority = true;
    } else if (reference.starts_with("//")) {
        reference.remove_prefix(2);
        hasAuthority = true;
    }

    if (hasAuthority) {
        const auto authorityEnd = reference.find_first_of("/?");
        const auto authority = authorityEnd == std::string_view::npos
            ? reference
            : reference.substr(0, authorityEnd);
        const auto authorityStatus = classifyHttpClientOriginAuthority(origin, authority);
        if (!schemeMatchesOrigin) {
            return HttpClientRedirectTargetResult::makeFailure(
                authorityStatus == HttpClientOriginAuthorityStatus::kInvalidAuthority
                    ? HttpClientRedirectTargetError::kInvalidLocation
                    : HttpClientRedirectTargetError::kNotSameOrigin);
        }
        switch (authorityStatus) {
            case HttpClientOriginAuthorityStatus::kSameOrigin:
                break;
            case HttpClientOriginAuthorityStatus::kDifferentOrigin:
                return HttpClientRedirectTargetResult::makeFailure(
                    HttpClientRedirectTargetError::kNotSameOrigin);
            case HttpClientOriginAuthorityStatus::kInvalidAuthority:
                return HttpClientRedirectTargetResult::makeFailure(
                    HttpClientRedirectTargetError::kInvalidLocation);
        }
        reference = authorityEnd == std::string_view::npos
            ? std::string_view{}
            : reference.substr(authorityEnd);
    }

    const auto queryAt = reference.find('?');
    const bool hasReferenceQuery = queryAt != std::string_view::npos;
    const auto referencePath = hasReferenceQuery
        ? reference.substr(0, queryAt)
        : reference;
    const auto referenceQuery = hasReferenceQuery
        ? reference.substr(queryAt + 1)
        : std::string_view{};

    auto* const targetResource = detail::httpPmrResourceOrDefault(resource);
    std::pmr::string mergedPath(targetResource);
    std::string_view selectedQuery;
    bool hasSelectedQuery = hasReferenceQuery;

    if (hasAuthority) {
        if (!referencePath.empty() && referencePath.front() != '/') {
            return HttpClientRedirectTargetResult::makeFailure(
                HttpClientRedirectTargetError::kInvalidLocation);
        }
        mergedPath.assign(referencePath.empty() ? "/" : referencePath);
        selectedQuery = referenceQuery;
    } else {
        const auto baseQueryAt = currentTarget.find('?');
        const bool hasBaseQuery = baseQueryAt != std::string_view::npos;
        const auto basePath = hasBaseQuery
            ? currentTarget.substr(0, baseQueryAt)
            : currentTarget;
        const auto baseQuery = hasBaseQuery
            ? currentTarget.substr(baseQueryAt + 1)
            : std::string_view{};

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

    std::pmr::string resolved(targetResource);
    if (!normalizeHttpClientAbsolutePath(mergedPath, resolved)) {
        return HttpClientRedirectTargetResult::makeFailure(
            HttpClientRedirectTargetError::kInvalidLocation);
    }
    if (hasSelectedQuery) {
        resolved.push_back('?');
        resolved.append(selectedQuery.data(), selectedQuery.size());
    }
    if (!isValidHttpClientOriginTarget(resolved)) {
        return HttpClientRedirectTargetResult::makeFailure(
            HttpClientRedirectTargetError::kInvalidLocation);
    }

    return HttpClientRedirectTargetResult::makeTarget(std::move(resolved));
}

}  // namespace ruvia
