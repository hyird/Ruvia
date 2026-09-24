#include "ruvia/http/HttpRequestTarget.h"

#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/parser/HttpUriGrammar.h"

namespace ruvia {

bool isValidHttpOriginFormTarget(std::string_view target) noexcept {
    return detail::isValidOriginFormTarget(target);
}

bool isValidHttpIpv4Literal(std::string_view value) noexcept {
    return detail::parseIpv4Address(value);
}

bool isValidHttpIpv6Literal(std::string_view value) noexcept {
    return detail::isValidIpv6Literal(value);
}

std::optional<std::string_view> parseHttpAuthorityHost(BorrowedText value) noexcept {
    const auto authority = detail::parseHttpAuthority(value.view());
    if (!authority) {
        return std::nullopt;
    }
    return authority->host();
}

}  // namespace ruvia
