#include "ruvia/http/HttpRequest.h"

#include <system_error>
#include <utility>

#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

#include "request/HttpRequestAccess.h"

namespace ruvia {
std::optional<std::string_view> HttpRequest::header(std::string_view name) const noexcept {
    const auto kind = detail::classifyRequestHeader(name);
    if (kind != detail::RequestHeaderKind::kOther) {
        const auto knownSlot = detail::requestHeaderKindKnownSlot(kind);
        const auto index = cachedHeaders_[knownSlot];
        if (index == 0 || index > headers_.size()) {
            return std::nullopt;
        }
        return headers_[index - 1].value();
    }

    for (std::size_t i = headers_.size(); i > 0; --i) {
        const auto index = i - 1;
        if (detail::httpAsciiEqualsIgnoreCase(headers_[index].name(), name)) {
            return headers_[index].value();
        }
    }

    return std::nullopt;
}

std::optional<std::string_view> HttpRequest::lastRawQueryValue(
    std::string_view rawName) const noexcept {
    std::optional<std::string_view> result;
    (void)visitUrlEncodedPairs(
        queryString_, [&](std::string_view name, std::string_view value) noexcept {
            if (name == rawName) {
                result = value;
            }
            return true;
        });
    return result;
}

std::optional<std::string_view> HttpRequest::cookie(std::string_view name) const noexcept {
    if (!detail::requestHasKnownHeader(*this, detail::RequestHeaderKind::kCookie)) {
        return std::nullopt;
    }
    const auto lastCookie =
        detail::requestKnownHeader(*this, detail::RequestHeaderKind::kCookie);
    if (auto value = detail::httpFindSemicolonParameter(lastCookie, name)) {
        return value;
    }
    for (std::size_t i = headers_.size(); i > 0; --i) {
        const auto index = i - 1;
        const auto header = headers_[index];
        if (headers_.kindAt(index) !=
                std::to_underlying(detail::RequestHeaderKind::kCookie) ||
            header.value().data() == lastCookie.data()) {
            continue;
        }
        if (auto value = detail::httpFindSemicolonParameter(header.value(), name)) {
            return value;
        }
    }
    return std::nullopt;
}

std::pmr::memory_resource* HttpRequest::resource() const noexcept {
    return detail::httpPmrResourceOrDefault(resource_);
}

}  // namespace ruvia
