#include "ruvia/http/HttpRequest.h"

#include <system_error>
#include <utility>

#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"

namespace ruvia {
namespace {

static_assert(std::to_underlying(detail::RequestHeaderKind::kAccept) ==
              std::to_underlying(detail::RequestKnownHeader::kAccept) + 1);
static_assert(std::to_underlying(detail::RequestHeaderKind::kAuthorization) ==
              std::to_underlying(detail::RequestKnownHeader::kAuthorization) + 1);
static_assert(std::to_underlying(detail::RequestHeaderKind::kContentEncoding) ==
              std::to_underlying(detail::RequestKnownHeader::kContentEncoding) + 1);
static_assert(std::to_underlying(detail::RequestHeaderKind::kUserAgent) ==
              std::to_underlying(detail::RequestKnownHeader::kUserAgent) + 1);
static_assert(std::to_underlying(detail::RequestHeaderKind::kXForwardedProto) ==
              std::to_underlying(detail::RequestKnownHeader::kXForwardedProto) + 1);
static_assert(std::to_underlying(detail::RequestHeaderKind::kSecWebSocketExtensions) ==
              std::to_underlying(detail::RequestKnownHeader::kSecWebSocketExtensions) + 1);
static_assert(detail::kRequestHeaderKindCount ==
              std::to_underlying(detail::RequestKnownHeader::kSecWebSocketExtensions) + 2);

}  // namespace

std::optional<std::string_view> HttpRequest::header(std::string_view name) const noexcept {
    const auto kind = detail::classifyRequestHeader(name);
    if (kind != detail::RequestHeaderKind::kOther) {
        const auto knownSlot = std::to_underlying(kind) - 1;
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
    (void)detail::visitUrlEncodedPairs(
        queryString_, [&](std::string_view name, std::string_view value) noexcept {
            if (name == rawName) {
                result = value;
            }
            return true;
        });
    return result;
}

std::optional<std::string_view> HttpRequest::cookie(std::string_view name) const noexcept {
    if (!detail::requestHasKnownHeader(*this, detail::RequestKnownHeader::kCookie)) {
        return std::nullopt;
    }
    const auto lastCookie =
        detail::requestKnownHeader(*this, detail::RequestKnownHeader::kCookie);
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
