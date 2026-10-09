#include "ruvia/http/http_client_redirect.h"

#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_request_target.h"

#include "client/http_origin_view.h"
#include "parser/http_request_target.h"
#include "parser/http_uri_grammar.h"

namespace ruvia {
namespace {

void remove_http_client_last_path_segment(std::pmr::string& path) noexcept {
    const auto slash = path.rfind('/');
    if (slash == std::pmr::string::npos) {
        path.clear();
        return;
    }
    path.erase(slash);
}

[[nodiscard]] bool normalize_http_client_absolute_path(
    std::string_view path, std::pmr::string& normalized) {
    if (path.empty() || path.front() != '/') {
        return false;
    }

    normalized.clear();
    normalized.reserve(path.size());
    auto remaining = path;
    while (!remaining.empty()) {
        // RFC 3986 section 5.2.4 deliberately moves one path segment at a
        // time. Empty segments are significant: reducing the path to a stack
        // of non-dot segments would turn "/a//." into "/a/" instead of the
        // required "/a//".
        if (remaining.starts_with("../")) {
            remaining.remove_prefix(3);
        } else if (remaining.starts_with("./")) {
            remaining.remove_prefix(2);
        } else if (remaining.starts_with("/./")) {
            remaining.remove_prefix(2);
        } else if (remaining == "/.") {
            remaining = "/";
        } else if (remaining.starts_with("/../")) {
            remaining.remove_prefix(3);
            remove_http_client_last_path_segment(normalized);
        } else if (remaining == "/..") {
            remaining = "/";
            remove_http_client_last_path_segment(normalized);
        } else if (remaining == "." || remaining == "..") {
            remaining = {};
        } else {
            const auto next_slash =
                remaining.front() == '/' ? remaining.find('/', 1) : remaining.find('/');
            const auto segment_bytes =
                next_slash == std::string_view::npos ? remaining.size() : next_slash;
            normalized.append(remaining.substr(0, segment_bytes));
            remaining.remove_prefix(segment_bytes);
        }
    }

    if (normalized.empty()) {
        normalized.push_back('/');
    }
    return true;
}

// Merges the reference's path/query with the current origin-form target per
// RFC 3986 section 5.2 and validates the resolved origin-form target. Returns
// false when the location's path or the merged product is not a valid target.
// `reference` is the URI-reference with any scheme/authority prefix and the
// fragment already removed.
[[nodiscard]] bool resolve_http_client_redirect_path_and_query(bool has_authority,
    std::string_view reference, std::string_view current_target,
    std::pmr::memory_resource* target_resource, std::pmr::string& resolved) {
    const auto query_at = reference.find('?');
    const bool has_reference_query = query_at != std::string_view::npos;
    const auto reference_path = has_reference_query ? reference.substr(0, query_at) : reference;
    const auto reference_query =
        has_reference_query ? reference.substr(query_at + 1) : std::string_view{};
    // Dot-segment removal must not hide malformed bytes in discarded segments.
    if (!detail::is_valid_uri_component(reference_path, true, false) ||
        !detail::is_valid_uri_component(reference_query, true, true)) {
        return false;
    }

    std::pmr::string merged_path(target_resource);
    std::string_view selected_query;
    bool has_selected_query = has_reference_query;

    if (has_authority) {
        if (!reference_path.empty() && reference_path.front() != '/') {
            return false;
        }
        merged_path.assign(reference_path.empty() ? "/" : reference_path);
        selected_query = reference_query;
    } else {
        const auto base_query_at = current_target.find('?');
        const bool has_base_query = base_query_at != std::string_view::npos;
        const auto base_path = has_base_query ? current_target.substr(0, base_query_at) : current_target;
        const auto base_query =
            has_base_query ? current_target.substr(base_query_at + 1) : std::string_view{};

        if (reference_path.empty()) {
            merged_path.assign(base_path);
            if (has_reference_query) {
                selected_query = reference_query;
            } else {
                has_selected_query = has_base_query;
                selected_query = base_query;
            }
        } else if (reference_path.front() == '/') {
            merged_path.assign(reference_path);
            selected_query = reference_query;
        } else {
            const auto last_slash = base_path.rfind('/');
            merged_path.assign(base_path.substr(0, last_slash + 1));
            merged_path.append(reference_path.data(), reference_path.size());
            selected_query = reference_query;
        }
    }

    if (!has_authority && reference_path.empty()) {
        // RFC 3986 section 5.2.2 copies the base path for an empty reference path.
        resolved = std::move(merged_path);
    } else if (!normalize_http_client_absolute_path(merged_path, resolved)) {
        return false;
    }
    if (has_selected_query) {
        resolved.push_back('?');
        resolved.append(selected_query.data(), selected_query.size());
    }
    return is_valid_http_origin_form_target(resolved);
}

}  // namespace

bool is_http_client_redirect_status(http_status_code status) noexcept {
    return status == http_status::moved_permanently || status == http_status::found ||
           status == http_status::see_other || status == http_status::temporary_redirect ||
           status == http_status::permanent_redirect;
}

http_client_response_header_lookup_result lookup_unique_http_client_response_header(
    const http_client_response_head& head, std::string_view name) noexcept {
    std::string_view found;
    bool seen = false;
    for (const auto& header : head.headers()) {
        if (!detail::http_ascii_equals_ignore_case(header.name(), name)) {
            continue;
        }
        if (seen) {
            return http_client_response_header_lookup_result::make_repeated();
        }
        seen = true;
        found = header.value();
    }
    return seen ? http_client_response_header_lookup_result::make_found(found)
                : http_client_response_header_lookup_result::make_absent();
}

http_client_redirect_request_plan::http_client_redirect_request_plan(std::string_view method,
    http_client_redirect_content_disposition content_disposition, std::pmr::memory_resource* resource)
    : method_(method, detail::http_pmr_resource_or_default(resource)),
      content_disposition_(content_disposition) {}

http_client_redirect_request_plan plan_http_client_redirect_request(
    const http_client_request_view& request, http_client_redirect_request_plan_options options) {
    if (options.status_ == http_status::see_other) {
        return http_client_redirect_request_plan(
            request.method_ == "HEAD" ? request.method_.view() : std::string_view("GET"),
            http_client_redirect_content_disposition::drop, options.resource_);
    }
    if ((options.status_ == http_status::moved_permanently ||
            options.status_ == http_status::found) &&
        request.method_ == "POST") {
        return http_client_redirect_request_plan(
            "GET", http_client_redirect_content_disposition::drop, options.resource_);
    }
    return http_client_redirect_request_plan(
        request.method_.view(), http_client_redirect_content_disposition::preserve, options.resource_);
}

http_client_origin_authority_status classify_http_client_origin_authority(
    const http_origin_view& origin, std::string_view authority) noexcept {
    if ((authority.find('@') != std::string_view::npos)) {
        return http_client_origin_authority_status::invalid_authority;
    }
    const auto parsed_value = detail::parse_http_authority(authority);
    if (!parsed_value) {
        return http_client_origin_authority_status::invalid_authority;
    }
    return parsed_value->effective_port(detail::http_scheme_default_port(origin.scheme())) == origin.port() &&
                   detail::http_uri_host_equals(parsed_value->host(), origin.host())
               ? http_client_origin_authority_status::same_origin
               : http_client_origin_authority_status::different_origin;
}

http_origin_view http_client_resolved_redirect::origin() const& {
    return scheme_ == http_scheme::https ? http_origin_view::https({.host_ = host(), .port_ = port_})
                                         : http_origin_view::http({.host_ = host(), .port_ = port_});
}

http_client_redirect_resolution_result resolve_http_client_redirect_target(
    const http_origin_view& origin, http_client_redirect_target_options options) {
    const auto current_target = options.current_target_;
    auto location = options.location_;
    if (current_target.empty() || current_target.front() != '/' ||
        !is_valid_http_origin_form_target(current_target)) {
        return http_client_redirect_resolution_result::make_failure(
            http_client_redirect_resolution_error::invalid_current_target);
    }

    location = detail::http_trim_ows(location);
    if (const auto hash = location.find('#'); hash != std::string_view::npos) {
        if (!detail::is_valid_uri_component(location.substr(hash + 1), true, true)) {
            return http_client_redirect_resolution_result::make_failure(
                http_client_redirect_resolution_error::invalid_location);
        }
        location = location.substr(0, hash);
    }

    bool has_authority = false;
    auto target_scheme = origin.scheme();
    std::string_view reference = location;
    const auto colon = reference.find(':');
    const auto first_path_or_query = reference.find_first_of("/?");
    if (colon != std::string_view::npos &&
        (first_path_or_query == std::string_view::npos || colon < first_path_or_query)) {
        const auto scheme = reference.substr(0, colon);
        if (!detail::is_valid_uri_scheme(scheme)) {
            return http_client_redirect_resolution_result::make_failure(
                http_client_redirect_resolution_error::invalid_location);
        }
        const bool is_https = detail::http_ascii_equals_ignore_case(scheme, "https");
        if (!is_https && !detail::http_ascii_equals_ignore_case(scheme, "http")) {
            return http_client_redirect_resolution_result::make_failure(
                http_client_redirect_resolution_error::unsupported_scheme);
        }
        target_scheme = is_https ? http_scheme::https : http_scheme::http;
        reference.remove_prefix(colon + 1);
        if (!reference.starts_with("//")) {
            return http_client_redirect_resolution_result::make_failure(
                http_client_redirect_resolution_error::invalid_location);
        }
        reference.remove_prefix(2);
        has_authority = true;
    } else if (reference.starts_with("//")) {
        reference.remove_prefix(2);
        has_authority = true;
    }

    auto* const target_resource = detail::http_pmr_resource_or_default(options.resource_);
    std::pmr::string host(target_resource);
    auto port = origin.port();
    bool cross_origin = target_scheme != origin.scheme();
    if (has_authority) {
        const auto authority_end = reference.find_first_of("/?");
        const auto authority =
            authority_end == std::string_view::npos ? reference : reference.substr(0, authority_end);
        if ((authority.find('@') != std::string_view::npos)) {
            return http_client_redirect_resolution_result::make_failure(
                http_client_redirect_resolution_error::invalid_location);
        }
        const auto parsed_value = detail::parse_http_authority(authority);
        if (!parsed_value) {
            return http_client_redirect_resolution_result::make_failure(
                http_client_redirect_resolution_error::invalid_location);
        }
        host.assign(parsed_value->host().data(), parsed_value->host().size());
        port = parsed_value->effective_port(detail::http_scheme_default_port(target_scheme));
        cross_origin = cross_origin || port != origin.port() ||
                       !detail::http_uri_host_equals(parsed_value->host(), origin.host());
        reference = authority_end == std::string_view::npos ? std::string_view{}
                                                            : reference.substr(authority_end);
    } else {
        host.assign(origin.host().data(), origin.host().size());
    }

    std::pmr::string resolved(target_resource);
    if (!resolve_http_client_redirect_path_and_query(
            has_authority, reference, current_target, target_resource, resolved)) {
        return http_client_redirect_resolution_result::make_failure(
            http_client_redirect_resolution_error::invalid_location);
    }

    return http_client_redirect_resolution_result::make_resolved(
        target_scheme, std::move(host), port, std::move(resolved), cross_origin);
}

}  // namespace ruvia
