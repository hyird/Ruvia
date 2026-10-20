#include "ruvia/http/http_request_target.h"

#include "parser/http_request_target.h"
#include "parser/http_uri_grammar.h"

namespace ruvia {
namespace detail {

struct http_authority_view_access final {
    [[nodiscard]] static constexpr http_authority_view make(
        std::string_view host, http_authority_port_kind port_kind, std::uint16_t port = 0) noexcept {
        return http_authority_view(host, port_kind, port);
    }
};

}  // namespace detail

bool is_valid_http_origin_form_target(std::string_view target) noexcept {
    return detail::is_valid_origin_form_target(target);
}

bool is_valid_http_connect_authority(std::string_view authority) noexcept {
    detail::request_target_view target;
    return detail::parse_request_target(http_known_method::connect, authority, target);
}

bool is_valid_http_ipv4_literal(std::string_view value) noexcept {
    return detail::parse_ipv4_address(value).has_value();
}

bool is_valid_http_ipv6_literal(std::string_view value) noexcept {
    return detail::parse_ipv6_address(value).has_value();
}

std::optional<http_authority_view> parse_http_authority(borrowed_text text) noexcept {
    const auto value = text.view();
    if (value.empty()) {
        return std::nullopt;
    }

    std::string_view host;
    std::string_view port_text;
    bool has_port_delimiter = false;
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = value.substr(0, close + 1);
        const auto remainder = value.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') {
                return std::nullopt;
            }
            has_port_delimiter = true;
            port_text = remainder.substr(1);
        }
    } else {
        const auto colon = value.find(':');
        host = colon == std::string_view::npos ? value : value.substr(0, colon);
        if (colon != std::string_view::npos) {
            has_port_delimiter = true;
            port_text = value.substr(colon + 1);
        }
    }

    if (!detail::is_valid_http_host(host)) {
        return std::nullopt;
    }
    if (!has_port_delimiter) {
        return detail::http_authority_view_access::make(host, http_authority_port_kind::absent);
    }
    if (port_text.empty()) {
        return detail::http_authority_view_access::make(host, http_authority_port_kind::empty);
    }

    const auto port = detail::parse_port_value(port_text);
    if ((port.index() != 0)) {
        return std::nullopt;
    }
    return detail::http_authority_view_access::make(host, http_authority_port_kind::value, std::get<0>(port));
}

std::optional<std::string_view> parse_http_authority_host(borrowed_text value) noexcept {
    if (value.empty()) {
        return value.view();
    }
    const auto authority = parse_http_authority(value);
    if (!authority) {
        return std::nullopt;
    }
    return authority->host();
}

bool http_authorities_equal(borrowed_text left, borrowed_text right, std::uint16_t default_port) noexcept {
    return detail::authority_matches_host(left.view(), right.view(), default_port);
}

}  // namespace ruvia
