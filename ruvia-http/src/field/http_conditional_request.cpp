#include "ruvia/http/http_conditional_request.h"

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/http_ows.h"

#include "field/http_date.h"
#include "field/http_entity_tag.h"
#include "request/http_request_access.h"

namespace ruvia {

http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept {
    return http_conditional_headers{
        detail::request_known_header(request, detail::request_header_kind::if_unmodified_since),
        detail::request_known_header(request, detail::request_header_kind::if_modified_since),
        detail::request_known_header(request, detail::request_header_kind::range),
        detail::request_known_header(request, detail::request_header_kind::if_range),
        detail::request_has_known_header(request, detail::request_header_kind::if_range)};
}

void http_etag_field_condition::update(
    std::string_view value, std::string_view expected, bool strong) noexcept {
    present_ = true;
    ++line_count_;
    const auto trimmed = detail::http_trim_ows(value);
    if (trimmed == "*") {
        wildcard_ = true;
        if (line_count_ != 1) {
            valid_ = false;
        }
        return;
    }
    if (wildcard_) {
        valid_ = false;
    }
    const auto result_value = detail::http_parse_etag_list_matches(value, expected, strong);
    valid_ = valid_ && result_value.valid_;
    matched_ = matched_ || result_value.matched_;
}

http_etag_preconditions get_http_etag_preconditions(
    const http_request& request, std::string_view etag) noexcept {
    http_etag_preconditions result;
    const bool has_if_match = detail::request_has_known_header(request, detail::request_header_kind::if_match);
    const bool has_if_none_match =
        detail::request_has_known_header(request, detail::request_header_kind::if_none_match);
    if (!has_if_match && !has_if_none_match) {
        return result;
    }

    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them in one header scan and
    // retain whole-list validity without allocating a joined string.
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = detail::http_request_access::header_kind(request, i);
        if (has_if_match && kind == static_cast<std::uint8_t>(detail::request_header_kind::if_match)) {
            result.if_match_.update(headers[i].value(), etag, true);
        } else if (has_if_none_match &&
                   kind == static_cast<std::uint8_t>(detail::request_header_kind::if_none_match)) {
            result.if_none_match_.update(headers[i].value(), etag, false);
        }
    }
    return result;
}

bool http_date_not_modified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = detail::http_parse_http_date(detail::http_trim_ows(header_value));
    return date.has_value() && modified_seconds <= *date;
}

bool http_date_unmodified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = detail::http_parse_http_date(detail::http_trim_ows(header_value));
    return !date.has_value() || modified_seconds <= *date;
}

bool http_if_range_allows(std::string_view header_value, std::string_view etag,
    std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept {
    if (header_value.empty()) {
        return false;
    }
    const auto value = detail::http_trim_ows(header_value);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return detail::http_strong_etag_equals(value, etag);
    }
    if (!date_validator_strong || !modified_seconds) {
        return false;
    }
    const auto date = detail::http_parse_http_date(value);
    return date.has_value() && *modified_seconds == *date;
}

}  // namespace ruvia
