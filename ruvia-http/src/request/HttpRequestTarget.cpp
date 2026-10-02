#include "ruvia/http/HttpRequestTarget.h"

#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/parser/HttpUriGrammar.h"

namespace ruvia {

bool isValidHttpOriginFormTarget(std::string_view target) noexcept {
    return detail::isValidOriginFormTarget(target);
}

bool isValidHttpConnectAuthority(std::string_view authority) noexcept {
    detail::RequestTargetView target;
    return detail::parseRequestTarget(HttpKnownMethod::kConnect, authority, target);
}

bool isValidHttpIpv4Literal(std::string_view value) noexcept {
    return detail::parseIpv4Address(value);
}

bool isValidHttpIpv6Literal(std::string_view value) noexcept {
    return detail::isValidIpv6Literal(value);
}

std::optional<HttpAuthorityView> parseHttpAuthority(BorrowedText value) noexcept {
    const auto parsed = detail::parseHttpAuthority(value.view());
    if (!parsed) {
        return std::nullopt;
    }
    return HttpAuthorityView{.host = parsed->host(), .port = parsed->port()};
}

std::optional<std::string_view> parseHttpAuthorityHost(BorrowedText value) noexcept {
    const auto authority = detail::parseHttpAuthority(value.view());
    if (!authority) {
        return std::nullopt;
    }
    return authority->host();
}

bool httpAuthoritiesEqual(BorrowedText left, BorrowedText right, std::uint16_t defaultPort) noexcept {
    return detail::authorityMatchesHost(left.view(), right.view(), defaultPort);
}

}  // namespace ruvia
