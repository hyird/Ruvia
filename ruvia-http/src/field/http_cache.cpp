#include "ruvia/http/http_cache.h"

#include <limits>

#include "ruvia/http/detail/field/header_token_utils.h"  // http_trim_ows, http_ascii_equals_ignore_case

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Quoted-pairs have their RFC 9110 section 5.6.4 semantic value, so
// `"6\0"` is equivalent to `60`. Syntactically valid overflow saturates to the
// greatest convenient representation; non-digit / empty input remains invalid.
[[nodiscard]] std::optional<std::uint64_t> parse_delta_seconds(std::string_view value) noexcept {
    std::uint64_t result_value = 0;
    bool any_digit = false;
    const auto accumulate = [&result_value, &any_digit](unsigned char ch) noexcept {
        if (ch < '0' || ch > '9') {
            return false;
        }
        constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        result_value = result_value > (maximum - digit) / 10 ? maximum : result_value * 10 + digit;
        any_digit = true;
        return true;
    };
    if (!value.empty() && value.front() == '"') {
        http_quoted_string_cursor cursor(value);
        unsigned char ch = 0;
        while (cursor.next(ch)) {
            if (!accumulate(ch)) {
                return std::nullopt;
            }
        }
        if (!cursor.closed() || cursor.consumed() != value.size()) {
            return std::nullopt;
        }
    } else {
        for (const auto ch : value) {
            if (!accumulate(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
        }
    }
    if (!any_digit) {
        return std::nullopt;
    }
    return result_value;
}

}  // namespace

void cache_control_field_parser::update(std::string_view value) noexcept {
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto comma = pos + http_find_unquoted_delimiter(value.substr(pos), 0, ',');
        auto token = value.substr(pos, comma - pos);
        pos = comma == value.size() ? value.size() : comma + 1;

        token = http_trim_ows(token);
        if (token.empty()) {
            continue;
        }
        const auto eq = token.find('=');
        const bool has_argument = eq != std::string_view::npos;
        const auto name = eq == std::string_view::npos ? token : token.substr(0, eq);
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (!has_argument && http_ascii_equals_ignore_case(name, "no-store")) {
            value_.set(cache_control_directive::no_store);
        } else if ((!has_argument || detail::is_http_token_or_quoted_string(arg)) &&
                   http_ascii_equals_ignore_case(name, "no-cache")) {
            value_.set(cache_control_directive::no_cache);
        } else if (!has_argument && http_ascii_equals_ignore_case(name, "no-transform")) {
            value_.set(cache_control_directive::no_transform);
        } else if (!has_argument && http_ascii_equals_ignore_case(name, "must-revalidate")) {
            value_.set(cache_control_directive::must_revalidate);
        } else if (!has_argument && http_ascii_equals_ignore_case(name, "proxy-revalidate")) {
            value_.set(cache_control_directive::proxy_revalidate);
        } else if ((!has_argument || detail::is_http_token_or_quoted_string(arg)) &&
                   http_ascii_equals_ignore_case(name, "private")) {
            value_.set(cache_control_directive::private_value);
        } else if (!has_argument && http_ascii_equals_ignore_case(name, "public")) {
            value_.set(cache_control_directive::public_value);
        } else if (!has_argument && http_ascii_equals_ignore_case(name, "immutable")) {
            value_.set(cache_control_directive::immutable);
        } else if (!has_argument && http_ascii_equals_ignore_case(name, "only-if-cached")) {
            value_.set(cache_control_directive::only_if_cached);
        } else if (http_ascii_equals_ignore_case(name, "max-age")) {
            if (!max_age_seen_) {
                max_age_seen_ = true;
                value_.max_age_ = parse_delta_seconds(arg);
            }
        } else if (http_ascii_equals_ignore_case(name, "max-stale")) {
            if (!max_stale_seen_) {
                max_stale_seen_ = true;
                if (!has_argument) {
                    value_.set(cache_control_directive::max_stale_any);
                } else {
                    value_.max_stale_ = parse_delta_seconds(arg);
                }
            }
        } else if (http_ascii_equals_ignore_case(name, "min-fresh")) {
            if (!min_fresh_seen_) {
                min_fresh_seen_ = true;
                value_.min_fresh_ = parse_delta_seconds(arg);
            }
        } else if (http_ascii_equals_ignore_case(name, "s-maxage")) {
            if (!s_max_age_seen_) {
                s_max_age_seen_ = true;
                value_.s_max_age_ = parse_delta_seconds(arg);
            }
        } else if (http_ascii_equals_ignore_case(name, "stale-while-revalidate")) {
            if (!stale_while_revalidate_seen_) {
                stale_while_revalidate_seen_ = true;
                value_.stale_while_revalidate_ = parse_delta_seconds(arg);
            }
        } else if (http_ascii_equals_ignore_case(name, "stale-if-error")) {
            if (!stale_if_error_seen_) {
                stale_if_error_seen_ = true;
                value_.stale_if_error_ = parse_delta_seconds(arg);
            }
        }
    }
}

cache_control parse_cache_control(std::string_view value) noexcept {
    cache_control_field_parser parser;
    parser.update(value);
    return parser.finish();
}

}  // namespace ruvia
