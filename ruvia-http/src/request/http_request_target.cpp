#include "ruvia/http/http_request_target.h"

#include "parser/http_request_target.h"
#include "parser/http_uri_grammar.h"

namespace ruvia {

bool is_valid_http_origin_form_target(std::string_view target) noexcept {
    return detail::is_valid_origin_form_target(target);
}

bool is_valid_http_connect_authority(std::string_view authority) noexcept {
    detail::request_target_view target;
    return detail::parse_request_target(http_known_method::connect, authority, target);
}

bool is_valid_http_ipv4_literal(std::string_view value) noexcept {
    return detail::parse_ipv4_address(value);
}

bool is_valid_http_ipv6_literal(std::string_view value) noexcept {
    return detail::is_valid_ipv6_literal(value);
}

std::optional<http_authority_view> parse_http_authority(borrowed_text value) noexcept {
    const auto parsed_value = detail::parse_http_authority(value.view());
    if (!parsed_value) {
        return std::nullopt;
    }
    return http_authority_view{.host_ = parsed_value->host(), .port_ = parsed_value->port()};
}

std::optional<std::string_view> parse_http_authority_host(borrowed_text value) noexcept {
    if (value.empty()) {
        return value.view();
    }
    const auto authority = detail::parse_http_authority(value.view());
    if (!authority) {
        return std::nullopt;
    }
    return authority->host();
}

bool http_authorities_equal(borrowed_text left, borrowed_text right, std::uint16_t default_port) noexcept {
    return detail::authority_matches_host(left.view(), right.view(), default_port);
}

}  // namespace ruvia
