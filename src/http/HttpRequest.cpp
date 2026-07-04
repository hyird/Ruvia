#include "ruvia/http/HttpRequest.h"

#include "HttpRequestInternal.h"
#include "parser/HttpParserSyntax.h"
#include "HeaderTokenUtils.h"
#include "ruvia/http/UrlEncoding.h"

#include <system_error>

namespace ruvia {
namespace {

static_assert(
    static_cast<std::size_t>(detail::RequestHeaderKind::kAccept) ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kAccept) + 1);
static_assert(
    static_cast<std::size_t>(detail::RequestHeaderKind::kAuthorization) ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kAuthorization) + 1);
static_assert(
    static_cast<std::size_t>(detail::RequestHeaderKind::kContentEncoding) ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kContentEncoding) + 1);
static_assert(
    static_cast<std::size_t>(detail::RequestHeaderKind::kUserAgent) ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kUserAgent) + 1);
static_assert(
    detail::kRequestHeaderKindCount ==
    static_cast<std::size_t>(detail::RequestKnownHeader::kUserAgent) + 2);

}  // namespace

std::string_view HttpRequest::header(std::string_view name) const noexcept {
    const auto kind = detail::classifyRequestHeader(name);
    if (kind != detail::RequestHeaderKind::kOther) {
        const auto knownSlot = static_cast<std::size_t>(kind) - 1;
        return detail::requestKnownHeader(*this, static_cast<detail::RequestKnownHeader>(knownSlot));
    }

    for (std::size_t i = headerCount_; i > 0; --i) {
        const auto index = i - 1;
        if (detail::httpAsciiEqualsIgnoreCase(headers_[index].name(), name)) {
            return headers_[index].value();
        }
    }

    return {};
}

std::optional<std::string_view> HttpRequest::query(std::string_view name) const noexcept {
    return detail::findUrlEncodedValue(queryString_, name, detail::UrlDecodeMode::kForm);
}

std::optional<std::string_view> HttpRequest::cookie(std::string_view name) const noexcept {
    return detail::httpFindSemicolonParameter(
        detail::requestKnownHeader(*this, detail::RequestKnownHeader::kCookie),
        name);
}

std::pmr::memory_resource* HttpRequest::resource() const noexcept {
    return detail::pmrResourceOrDefault(resource_);
}

}  // namespace ruvia
