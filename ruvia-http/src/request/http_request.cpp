#include "ruvia/http/http_request.h"

#include <system_error>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/url_encoding.h"

#include "request/http_request_access.h"

namespace ruvia {
std::optional<std::string_view> http_request::header(std::string_view name) const noexcept {
    const auto kind = detail::classify_request_header(name);
    if (kind != detail::request_header_kind::other) {
        const auto known_slot = detail::request_header_kind_known_slot(kind);
        const auto index = cached_headers_[known_slot];
        if (index == 0 || index > headers_.size()) {
            return std::nullopt;
        }
        return headers_[index - 1].value();
    }

    for (std::size_t i = headers_.size(); i > 0; --i) {
        const auto index = i - 1;
        if (detail::http_ascii_equals_ignore_case(headers_[index].name(), name)) {
            return headers_[index].value();
        }
    }

    return std::nullopt;
}

std::optional<std::string_view> http_request::last_raw_query_value(
    std::string_view raw_name) const noexcept {
    std::optional<std::string_view> result;
    (void)visit_url_encoded_pairs(
        query_string_, [&](std::string_view name, std::string_view value) noexcept {
            if (name == raw_name) {
                result = value;
            }
            return true;
        });
    return result;
}

std::optional<std::string_view> http_request::cookie(std::string_view name) const noexcept {
    if (!detail::request_has_known_header(*this, detail::request_header_kind::cookie)) {
        return std::nullopt;
    }
    const auto last_cookie =
        detail::request_known_header(*this, detail::request_header_kind::cookie);
    if (auto value = detail::http_find_semicolon_parameter(last_cookie, name)) {
        return value;
    }
    for (std::size_t i = headers_.size(); i > 0; --i) {
        const auto index = i - 1;
        const auto header_value = headers_[index];
        if (headers_.kind_at(index) !=
                static_cast<std::uint8_t>(detail::request_header_kind::cookie) ||
            header_value.value().data() == last_cookie.data()) {
            continue;
        }
        if (auto value = detail::http_find_semicolon_parameter(header_value.value(), name)) {
            return value;
        }
    }
    return std::nullopt;
}

std::pmr::memory_resource* http_request::resource() const noexcept {
    return detail::http_pmr_resource_or_default(resource_);
}

}  // namespace ruvia
