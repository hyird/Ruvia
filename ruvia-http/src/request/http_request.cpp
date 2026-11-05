#include "ruvia/http/http_request.h"

#include "ruvia/http/cookies.h"
#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_accept_encoding.h"
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
        if (headers_.kind_at(index) == detail::request_header_kind::other &&
            http_ascii_equals_ignore_case(headers_[index].name(), name)) {
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
    // The last matching pair within one field line wins.
    const auto find_in = [name](std::string_view field_value) noexcept {
        std::optional<std::string_view> result;
        http_visit_cookie_pairs(field_value, [name, &result](std::string_view key, std::string_view value) noexcept {
            if (key == name) {
                result = value;
            }
            return true;
        });
        return result;
    };
    const auto last_cookie =
        detail::request_known_header(*this, detail::request_header_kind::cookie);
    if (auto value = find_in(last_cookie)) {
        return value;
    }
    if (!detail::http_request_access::has_repeated_known_header(
            *this, detail::request_header_kind::cookie)) {
        return std::nullopt;
    }
    const auto last_cookie_index =
        cached_headers_[detail::request_header_kind_known_slot(detail::request_header_kind::cookie)];
    // The one-based cache points to the last Cookie field; only earlier fields can contribute.
    for (std::size_t i = last_cookie_index - 1; i > 0; --i) {
        const auto index = i - 1;
        if (headers_.kind_at(index) != detail::request_header_kind::cookie) {
            continue;
        }
        if (auto value = find_in(headers_[index].value())) {
            return value;
        }
    }
    return std::nullopt;
}

http_response_coding_selection http_response_coding_selection_for(const http_request& request) noexcept {
    http_response_coding_qualities qualities;
    if (detail::request_has_known_header(request, detail::request_header_kind::accept_encoding)) {
        if (!detail::http_request_access::has_repeated_known_header(
                request, detail::request_header_kind::accept_encoding)) {
            qualities.update(
                detail::request_known_header(request, detail::request_header_kind::accept_encoding));
        } else {
            // Every field line contributes: RFC 9110 section 5.3 combines repeated
            // list fields, and the request cache records only the last line.
            const auto headers = request.headers();
            const auto last_index = detail::http_request_access::known_header_index(
                request, detail::request_header_kind::accept_encoding);
            const auto limit = last_index <= headers.size() ? last_index : headers.size();
            for (std::size_t i = 0; i < limit; ++i) {
                if (detail::http_request_access::header_kind(request, i) ==
                    detail::request_header_kind::accept_encoding) {
                    qualities.update(headers[i].value());
                }
            }
        }
    }
    return http_response_coding_selection::select(qualities);
}

std::pmr::memory_resource* http_request::resource() const noexcept {
    return detail::http_pmr_resource_or_default(resource_);
}

}  // namespace ruvia
