#include "ruvia/http/http_cache.h"

#include <algorithm>
#include <limits>

#include "ruvia/http/detail/field/header_token_utils.h"  // http_trim_ows, http_ascii_equals_ignore_case
#include "ruvia/http/detail/parser/http_parser_syntax.h"

#include "field/http_date.h"

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Quoted-pairs have their RFC 9110 section 5.6.4 semantic value, so
// `"6\0"` is equivalent to `60`. Syntactically valid overflow saturates to the
// greatest convenient representation; non-digit / empty input remains invalid.
[[nodiscard]] std::optional<std::uint64_t> parse_delta_seconds(std::string_view value) noexcept {
    bool quoted = false;
    std::size_t begin = 0;
    std::size_t end = value.size();
    if (!value.empty() && value.front() == '"') {
        if (value.size() < 2 || value.back() != '"') {
            return std::nullopt;
        }
        quoted = true;
        begin = 1;
        --end;
    }
    if (begin == end) {
        return std::nullopt;
    }

    std::uint64_t result_value = 0;
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    for (std::size_t cursor_value = begin; cursor_value < end; ++cursor_value) {
        auto ch = static_cast<unsigned char>(value[cursor_value]);
        if (quoted && ch == '\\') {
            if (++cursor_value == end) {
                return std::nullopt;
            }
            ch = static_cast<unsigned char>(value[cursor_value]);
        }
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (result_value > (maximum - digit) / 10) {
            result_value = maximum;
        } else {
            result_value = result_value * 10 + digit;
        }
    }
    return result_value;
}

[[nodiscard]] bool is_valid_cache_directive_argument(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return std::ranges::all_of(value, [](char ch) noexcept {
            return detail::is_http_token_char(static_cast<unsigned char>(ch));
        });
    }
    if (value.size() < 2 || value.back() != '"') {
        return false;
    }
    for (std::size_t cursor_value = 1; cursor_value + 1 < value.size(); ++cursor_value) {
        const auto ch = static_cast<unsigned char>(value[cursor_value]);
        if (ch == '\\') {
            if (++cursor_value + 1 >= value.size()) {
                return false;
            }
            const auto escaped = static_cast<unsigned char>(value[cursor_value]);
            if (escaped != '\t' && escaped != ' ' && (escaped < 0x21 || escaped > 0x7e) &&
                escaped < 0x80) {
                return false;
            }
        } else if (ch == '"' ||
                   (ch != '\t' && ch != ' ' && ch != 0x21 && (ch < 0x23 || ch > 0x5b) &&
                       (ch < 0x5d || ch > 0x7e) && ch < 0x80)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t cache_directive_end(std::string_view value, std::size_t begin) noexcept {
    bool quoted = false;
    bool escaped = false;
    for (std::size_t cursor_value = begin; cursor_value < value.size(); ++cursor_value) {
        const auto ch = value[cursor_value];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                quoted = false;
            }
            continue;
        }
        if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            return cursor_value;
        }
    }
    return value.size();
}

}  // namespace

void cache_control_field_parser::update(std::string_view value) noexcept {
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto comma = cache_directive_end(value, pos);
        auto token = value.substr(pos, comma - pos);
        pos = comma == value.size() ? value.size() : comma + 1;

        token = detail::http_trim_ows(token);
        if (token.empty()) {
            continue;
        }
        const auto eq = token.find('=');
        const bool has_argument = eq != std::string_view::npos;
        const auto name = eq == std::string_view::npos ? token : token.substr(0, eq);
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (!has_argument && detail::http_ascii_equals_ignore_case(name, "no-store")) {
            value_.set(cache_control_directive::no_store);
        } else if ((!has_argument || is_valid_cache_directive_argument(arg)) &&
                   detail::http_ascii_equals_ignore_case(name, "no-cache")) {
            value_.set(cache_control_directive::no_cache);
        } else if (!has_argument && detail::http_ascii_equals_ignore_case(name, "no-transform")) {
            value_.set(cache_control_directive::no_transform);
        } else if (!has_argument && detail::http_ascii_equals_ignore_case(name, "must-revalidate")) {
            value_.set(cache_control_directive::must_revalidate);
        } else if (!has_argument && detail::http_ascii_equals_ignore_case(name, "proxy-revalidate")) {
            value_.set(cache_control_directive::proxy_revalidate);
        } else if ((!has_argument || is_valid_cache_directive_argument(arg)) &&
                   detail::http_ascii_equals_ignore_case(name, "private")) {
            value_.set(cache_control_directive::private_value);
        } else if (!has_argument && detail::http_ascii_equals_ignore_case(name, "public")) {
            value_.set(cache_control_directive::public_value);
        } else if (!has_argument && detail::http_ascii_equals_ignore_case(name, "immutable")) {
            value_.set(cache_control_directive::immutable);
        } else if (!has_argument && detail::http_ascii_equals_ignore_case(name, "only-if-cached")) {
            value_.set(cache_control_directive::only_if_cached);
        } else if (detail::http_ascii_equals_ignore_case(name, "max-age")) {
            if (!max_age_seen_) {
                max_age_seen_ = true;
                value_.max_age_ = parse_delta_seconds(arg);
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "max-stale")) {
            if (!max_stale_seen_) {
                max_stale_seen_ = true;
                if (!has_argument) {
                    value_.set(cache_control_directive::max_stale_any);
                } else {
                    value_.max_stale_ = parse_delta_seconds(arg);
                }
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "min-fresh")) {
            if (!min_fresh_seen_) {
                min_fresh_seen_ = true;
                value_.min_fresh_ = parse_delta_seconds(arg);
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "s-maxage")) {
            if (!s_max_age_seen_) {
                s_max_age_seen_ = true;
                value_.s_max_age_ = parse_delta_seconds(arg);
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "stale-while-revalidate")) {
            if (!stale_while_revalidate_seen_) {
                stale_while_revalidate_seen_ = true;
                value_.stale_while_revalidate_ = parse_delta_seconds(arg);
            }
        } else if (detail::http_ascii_equals_ignore_case(name, "stale-if-error")) {
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

std::optional<std::time_t> parse_http_date(std::string_view value) noexcept {
    return detail::http_parse_http_date(detail::http_trim_ows(value));
}

}  // namespace ruvia
