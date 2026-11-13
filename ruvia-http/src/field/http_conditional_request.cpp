#include "ruvia/http/http_conditional_request.h"

#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_date.h"
#include "ruvia/http/http_field_syntax.h"

#include "field/http_entity_tag.h"
#include "field/http_field_kind.h"
#include "request/http_request_access.h"

namespace ruvia {

http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept {
    return http_conditional_headers{
        detail::http_request_access::has_repeated_known_header(
            request, detail::http_field_kind::if_unmodified_since)
            ? std::string_view{}
            : detail::http_request_access::known_header(request, detail::http_field_kind::if_unmodified_since),
        detail::http_request_access::has_repeated_known_header(
            request, detail::http_field_kind::if_modified_since)
            ? std::string_view{}
            : detail::http_request_access::known_header(request, detail::http_field_kind::if_modified_since),
        detail::http_request_access::known_header(request, detail::http_field_kind::range),
        detail::http_request_access::known_header(request, detail::http_field_kind::if_range),
        detail::http_request_access::has_known_header(request, detail::http_field_kind::if_range)};
}

void http_etag_field_condition::update(
    std::string_view value, std::string_view expected, bool strong) noexcept {
    const bool already_present = present_;
    present_ = true;
    const auto trimmed = http_trim_ows(value);
    if (trimmed == "*") {
        wildcard_ = true;
        if (already_present) {
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
    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them line by line and
    // retain whole-list validity without allocating a joined string.
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::if_match, [&result, etag](std::string_view value) noexcept {
            result.if_match_.update(value, etag, true);
            return true;
        });
    (void)detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::if_none_match, [&result, etag](std::string_view value) noexcept {
            result.if_none_match_.update(value, etag, false);
            return true;
        });
    return result;
}

bool http_date_not_modified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = parse_http_date(header_value);
    return date.has_value() && modified_seconds <= *date;
}

bool http_date_unmodified(std::string_view header_value, std::time_t modified_seconds) noexcept {
    const auto date = parse_http_date(header_value);
    return !date.has_value() || modified_seconds <= *date;
}

bool http_if_range_allows(std::string_view header_value, std::string_view etag,
    std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept {
    if (header_value.empty()) {
        return false;
    }
    const auto value = http_trim_ows(header_value);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return detail::http_strong_etag_equals(value, etag);
    }
    if (!date_validator_strong || !modified_seconds) {
        return false;
    }
    const auto date = parse_http_date(value);
    return date.has_value() && *modified_seconds == *date;
}

bool is_valid_http_entity_tag(std::string_view value) noexcept {
    if (value.starts_with("W/")) {
        value.remove_prefix(2);
    }
    if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
        return false;
    }
    for (std::size_t i = 1U; i + 1U < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == '"' || (byte != 0x21U && !(byte >= 0x23U && byte <= 0x7eU) && byte < 0x80U)) {
            return false;
        }
    }
    return true;
}

bool is_weak_http_entity_tag(std::string_view value) noexcept {
    return value.starts_with("W/") && is_valid_http_entity_tag(value);
}

bool is_strong_http_entity_tag(std::string_view value) noexcept {
    return !value.starts_with("W/") && is_valid_http_entity_tag(value);
}

bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept {
    return is_strong_http_entity_tag(left) && is_strong_http_entity_tag(right) && left == right;
}

bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept {
    return is_valid_http_entity_tag(left) && is_valid_http_entity_tag(right) &&
           detail::http_weak_etag_equals(left, right);
}

std::pmr::string weaken_http_etag(std::string_view etag, std::pmr::memory_resource* resource) {
    auto* const target_resource = detail::http_pmr_resource_or_default(resource);
    etag = http_trim_ows(etag);
    if (is_strong_http_entity_tag(etag)) {
        std::pmr::string weakened(target_resource);
        weakened.reserve(2U + etag.size());
        weakened.append("W/");
        weakened.append(etag);
        return weakened;
    }
    return std::pmr::string(etag, target_resource);
}
}  // namespace ruvia
