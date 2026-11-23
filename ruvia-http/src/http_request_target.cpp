#include <cstddef>
#include <string_view>

#include "http_authority.h"
#include "http_request.h"
#include "http_uri_syntax.h"
#include "url_encoding.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_valid_connect_authority_form(std::string_view target) noexcept {
    const auto authority = parse_http_authority(target);
    if (!authority || authority->port_kind() != http_authority_port_kind::value) {
        return false;
    }
    // RFC 9110 section 9.3.6 requires a non-empty, valid tunnel destination
    // port. Port zero is reserved and cannot identify that destination.
    return *authority->port() != 0;
}

// Splits origin-form = absolute-path [ "?" query ] and validates both parts.
[[nodiscard]] bool split_origin_form_target(std::string_view target, std::string_view& path, std::string_view& query) noexcept {
    if (target.empty() || target.front() != '/') {
        return false;
    }
    const auto separator = target.find('?');
    path = separator == std::string_view::npos ? target : target.substr(0, separator);
    query = separator == std::string_view::npos ? std::string_view{} : target.substr(separator + 1);
    return is_valid_uri_component(path, true, false, true) && is_valid_uri_component(query, true, true);
}

}  // namespace

bool is_valid_request_target_bytes(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < target.size(); ++i) {
        const auto byte = static_cast<unsigned char>(target[i]);
        if (byte == '%') {
            if (!decode_percent_byte(target, i)) {
                return false;
            }
            i += 2;
            continue;
        }
        // RFC 3986 URI-reference is ASCII and consists only of unreserved or
        // reserved characters. A fragment delimiter is never part of an HTTP
        // request target. Brackets are admitted here because this low-level
        // union also covers an IP-literal authority; component validation below
        // rejects them from path and query.
        if (!is_uri_pchar(byte) && byte != '/' && byte != '?' && byte != '[' && byte != ']') {
            return false;
        }
    }
    return true;
}

bool is_valid_origin_form_target(std::string_view target) noexcept {
    std::string_view path;
    std::string_view query;
    return split_origin_form_target(target, path, query);
}

bool is_valid_origin_or_asterisk_form_target(http_known_method method, std::string_view target) noexcept {
    return target == "*" ? method == http_known_method::options : is_valid_origin_form_target(target);
}

namespace {

[[nodiscard]] bool parse_absolute_target(http_known_method method, std::string_view target, request_target_view& output) noexcept {
    const auto url = parse_http_url(target);
    if (!url || url->fragment() || (!http_ascii_equals_ignore_case(url->scheme(), "http") && !http_ascii_equals_ignore_case(url->scheme(), "https"))) {
        return false;
    }
    // RFC 9112 section 3.2.4 preserves server-wide OPTIONS semantics only
    // when both the path and query are absent.
    output.path_ = !url->has_path() && method == http_known_method::options && !url->query() ? std::string_view("*") : url->path();
    output.scheme_ = url->scheme();
    output.query_ = url->query().value_or(std::string_view{});
    output.authority_ = url->authority_text();
    output.form_ = http_request_target_form::absolute;
    return true;
}

}  // namespace

bool parse_request_target(http_known_method method, std::string_view target, request_target_view& output) noexcept {
    if (target == "*") {
        if (method != http_known_method::options) {
            return false;
        }
        output.path_ = "*";
        output.scheme_ = {};
        output.query_ = {};
        output.authority_ = {};
        output.form_ = http_request_target_form::asterisk;
        return true;
    }
    if (method == http_known_method::connect) {
        if (!is_valid_connect_authority_form(target)) {
            return false;
        }
        output.path_ = target;
        output.scheme_ = {};
        output.query_ = {};
        output.authority_ = target;
        output.form_ = http_request_target_form::authority;
        return true;
    }
    if (target.empty()) {
        return false;
    }
    if (target.front() == '/') {
        std::string_view path;
        std::string_view query;
        if (!split_origin_form_target(target, path, query)) {
            return false;
        }
        output.scheme_ = {};
        output.path_ = path;
        output.query_ = query;
        output.authority_ = {};
        output.form_ = http_request_target_form::origin;
        return true;
    }
    return parse_absolute_target(method, target, output);
}

}  // namespace ruvia::detail

namespace ruvia {

std::optional<http_request_target_view> parse_http_request_target(http_known_method method, borrowed_text target) noexcept {
    detail::request_target_view parsed;
    if (!detail::parse_request_target(method, target.view(), parsed)) {
        return std::nullopt;
    }
    return http_request_target_view{
        .scheme_ = parsed.scheme_,
        .authority_ = parsed.authority_,
        .path_ = parsed.path_,
        .query_ = parsed.query_,
        .form_ = parsed.form_,
    };
}

bool is_valid_http_connect_authority(std::string_view authority) noexcept {
    detail::request_target_view target;
    return detail::parse_request_target(http_known_method::connect, authority, target);
}

bool is_valid_http_origin_form_target(std::string_view target) noexcept {
    return detail::is_valid_origin_form_target(target);
}

}  // namespace ruvia
