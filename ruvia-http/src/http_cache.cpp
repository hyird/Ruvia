#include "ruvia/http/http_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_cors.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_response.h"

#include "http_connection_fields.h"
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

[[nodiscard]] bool parse_nominated_field_list_arg(
    std::string_view arg, auto&& add_field) noexcept {
    if (arg.empty()) {
        return false;
    }
    std::array<char, 256> unescaped_buf{};
    std::string_view list_text = arg;
    if (arg.front() == '"') {
        http_quoted_string_cursor cursor(arg);
        std::size_t len = 0;
        unsigned char ch = 0;
        while (cursor.next(ch)) {
            if (len >= unescaped_buf.size()) {
                return false;
            }
            unescaped_buf[len++] = static_cast<char>(ch);
        }
        if (!cursor.closed() || cursor.consumed() != arg.size()) {
            return false;
        }
        list_text = std::string_view(unescaped_buf.data(), len);
    } else if (!is_valid_http_header_name(arg)) {
        return false;
    }

    http_field_name_list names(list_text);
    bool saw_name = false;
    while (const auto name = names.next()) {
        saw_name = true;
        if (!add_field(*name)) {
            return false;
        }
    }
    return saw_name && names.valid();
}
}  // namespace

void cache_control_field_parser::update(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view token) noexcept {
        const auto eq = token.find('=');
        const bool has_argument = eq != std::string_view::npos;
        const auto name = eq == std::string_view::npos ? token : token.substr(0, eq);
        const auto arg = eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);
        switch (name.size()) {
            case 6U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "public")) {
                    value_.set(cache_control_directive::public_value);
                }
                break;
            case 7U:
                if (http_ascii_equals_ignore_case(name, "max-age")) {
                    if (!max_age_seen_) {
                        max_age_seen_ = true;
                        value_.max_age_ = parse_delta_seconds(arg);
                    }
                } else if (http_ascii_equals_ignore_case(name, "private")) {
                    if (!has_argument) {
                        value_.set(cache_control_directive::private_value);
                        value_.set(cache_control_directive::private_unqualified);
                        value_.private_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::private_value);
                        if (!value_.is_unqualified_private()) {
                            const bool added = parse_nominated_field_list_arg(
                                arg, [this](std::string_view field_name) noexcept {
                                    return value_.private_fields_.add_lowercase(field_name);
                                });
                            if (!added) {
                                value_.set(cache_control_directive::private_unqualified);
                                value_.private_fields_.clear();
                            }
                        }
                    }
                }
                break;
            case 8U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-store")) {
                    value_.set(cache_control_directive::no_store);
                } else if (http_ascii_equals_ignore_case(name, "no-cache")) {
                    if (!has_argument) {
                        value_.set(cache_control_directive::no_cache);
                        value_.set(cache_control_directive::no_cache_unqualified);
                        value_.no_cache_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::no_cache);
                        if (!value_.is_unqualified_no_cache()) {
                            const bool added = parse_nominated_field_list_arg(
                                arg, [this](std::string_view field_name) noexcept {
                                    return value_.no_cache_fields_.add_lowercase(field_name);
                                });
                            if (!added) {
                                value_.set(cache_control_directive::no_cache_unqualified);
                                value_.no_cache_fields_.clear();
                            }
                        }
                    }
                } else if (http_ascii_equals_ignore_case(name, "s-maxage")) {
                    if (!s_max_age_seen_) {
                        s_max_age_seen_ = true;
                        value_.s_max_age_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 9U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "immutable")) {
                    value_.set(cache_control_directive::immutable);
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
                }
                break;
            case 12U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "no-transform")) {
                    value_.set(cache_control_directive::no_transform);
                }
                break;
            case 14U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "only-if-cached")) {
                    value_.set(cache_control_directive::only_if_cached);
                } else if (http_ascii_equals_ignore_case(name, "stale-if-error")) {
                    if (!stale_if_error_seen_) {
                        stale_if_error_seen_ = true;
                        value_.stale_if_error_ = parse_delta_seconds(arg);
                    }
                }
                break;
            case 15U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "must-revalidate")) {
                    value_.set(cache_control_directive::must_revalidate);
                }
                break;
            case 16U:
                if (!has_argument && http_ascii_equals_ignore_case(name, "proxy-revalidate")) {
                    value_.set(cache_control_directive::proxy_revalidate);
                }
                break;
            case 22U:
                if (http_ascii_equals_ignore_case(name, "stale-while-revalidate")) {
                    if (!stale_while_revalidate_seen_) {
                        stale_while_revalidate_seen_ = true;
                        value_.stale_while_revalidate_ = parse_delta_seconds(arg);
                    }
                }
                break;
            default:
                break;
        }
        return true;
    });
}

cache_control parse_cache_control(std::string_view value) noexcept {
    cache_control_field_parser parser;
    parser.update(value);
    return parser.finish();
}

cache_control detail::parse_cache_control_headers(detail::http_header_range_view headers) noexcept {
    cache_control_field_parser parser;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            parser.update(field.value());
        }
    }
    return parser.finish();
}

bool http_cache_control_has_no_transform(std::string_view field_value) noexcept {
    return parse_cache_control(field_value).no_transform();
}

bool cache_control::nominated_field_set::contains(std::string_view field_name) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (http_ascii_equals_ignore_case(at(i), field_name)) {
            return true;
        }
    }
    return false;
}

bool cache_control::nominated_field_set::add_lowercase(std::string_view field_name) noexcept {
    if (field_name.empty() || field_name.size() > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    if (contains(field_name)) {
        return true;
    }
    if (count_ >= max_nominated_fields ||
        field_name.size() > max_nominated_field_bytes - storage_used_) {
        return false;
    }
    offsets_[count_] = storage_used_;
    lengths_[count_] = static_cast<std::uint8_t>(field_name.size());
    for (std::size_t i = 0; i < field_name.size(); ++i) {
        storage_[storage_used_ + i] =
            static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(field_name[i])));
    }
    storage_used_ = static_cast<std::uint16_t>(storage_used_ + field_name.size());
    ++count_;
    return true;
}

bool cache_control::has_private_field(std::string_view field_name) const noexcept {
    return !is_unqualified_private() && private_fields_.contains(field_name);
}

bool cache_control::has_no_cache_field(std::string_view field_name) const noexcept {
    return !is_unqualified_no_cache() && no_cache_fields_.contains(field_name);
}

template <typename input_type>
std::optional<cache_control> parse_targeted_cache_control_from_input(input_type input) noexcept {
    detail::http_structured_parser<input_type> parser{std::move(input)};
    parser.ows();
    if (parser.empty()) {
        return std::nullopt;
    }
    cache_control result;
    bool saw_member = false;
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return std::nullopt;
        }
        saw_member = true;
        detail::http_structured_item item{
            .kind_ = detail::http_structured_item::kind_type::boolean,
            .boolean_ = true,
        };
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return std::nullopt;
            }
        } else if (!parser.parameters()) {
            return std::nullopt;
        }

        const auto apply_bool_directive = [&](cache_control_directive dir) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                if (item.boolean_) {
                    result.set(dir);
                } else {
                    result.clear_directive(dir);
                }
            } else {
                result.clear_directive(dir);
            }
        };
        const auto apply_delta_directive =
            [&](std::optional<std::uint64_t>& target) noexcept {
                if (item.kind_ == detail::http_structured_item::kind_type::integer &&
                    item.integer_ >= 0) {
                    target = static_cast<std::uint64_t>(item.integer_);
                } else {
                    target.reset();
                }
            };
        const auto apply_qualified_directive = [&](cache_control_directive flag_dir,
                                                   cache_control_directive unqual_dir,
                                                   cache_control::nominated_field_set& fields) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                fields.clear();
                if (item.boolean_) {
                    result.set(flag_dir);
                    result.set(unqual_dir);
                } else {
                    result.clear_directive(flag_dir);
                    result.clear_directive(unqual_dir);
                }
            } else if (item.kind_ == detail::http_structured_item::kind_type::string ||
                       item.kind_ == detail::http_structured_item::kind_type::token) {
                fields.clear();
                result.clear_directive(unqual_dir);
                result.set(flag_dir);
                std::array<char, 256> unescaped{};
                std::string_view list_text = item.text_;
                if (item.kind_ == detail::http_structured_item::kind_type::string &&
                    item.has_escapes_) {
                    std::size_t out_len = 0;
                    for (std::size_t i = 0; i < item.text_.size(); ++i) {
                        if (item.text_[i] == '\\' && i + 1U < item.text_.size()) {
                            ++i;
                        }
                        if (out_len >= unescaped.size()) {
                            result.set(unqual_dir);
                            fields.clear();
                            return;
                        }
                        unescaped[out_len++] = item.text_[i];
                    }
                    list_text = std::string_view(unescaped.data(), out_len);
                }
                http_field_name_list names(list_text);
                bool saw_name = false;
                bool ok = true;
                while (const auto field_name = names.next()) {
                    saw_name = true;
                    if (!fields.add_lowercase(*field_name)) {
                        ok = false;
                        break;
                    }
                }
                if (!saw_name || !names.valid() || !ok) {
                    result.set(unqual_dir);
                    fields.clear();
                }
            } else {
                fields.clear();
                result.clear_directive(flag_dir);
                result.clear_directive(unqual_dir);
            }
        };

        if (key == "no-store") {
            apply_bool_directive(cache_control_directive::no_store);
        } else if (key == "no-transform") {
            apply_bool_directive(cache_control_directive::no_transform);
        } else if (key == "must-revalidate") {
            apply_bool_directive(cache_control_directive::must_revalidate);
        } else if (key == "proxy-revalidate") {
            apply_bool_directive(cache_control_directive::proxy_revalidate);
        } else if (key == "public") {
            apply_bool_directive(cache_control_directive::public_value);
        } else if (key == "immutable") {
            apply_bool_directive(cache_control_directive::immutable);
        } else if (key == "only-if-cached") {
            apply_bool_directive(cache_control_directive::only_if_cached);
        } else if (key == "private") {
            apply_qualified_directive(cache_control_directive::private_value,
                cache_control_directive::private_unqualified, result.private_fields_);
        } else if (key == "no-cache") {
            apply_qualified_directive(cache_control_directive::no_cache,
                cache_control_directive::no_cache_unqualified, result.no_cache_fields_);
        } else if (key == "max-age") {
            apply_delta_directive(result.max_age_);
        } else if (key == "s-maxage") {
            apply_delta_directive(result.s_max_age_);
        } else if (key == "stale-while-revalidate") {
            apply_delta_directive(result.stale_while_revalidate_);
        } else if (key == "stale-if-error") {
            apply_delta_directive(result.stale_if_error_);
        } else if (key == "min-fresh") {
            apply_delta_directive(result.min_fresh_);
        } else if (key == "max-stale") {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean && item.boolean_) {
                result.set(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            } else if (item.kind_ == detail::http_structured_item::kind_type::integer &&
                       item.integer_ >= 0) {
                result.clear_directive(cache_control_directive::max_stale_any);
                result.max_stale_ = static_cast<std::uint64_t>(item.integer_);
            } else {
                result.clear_directive(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            }
        }

        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
            return std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            return std::nullopt;
        }
    }
    if (!saw_member) {
        return std::nullopt;
    }
    return result;
}

std::optional<cache_control> parse_targeted_cache_control(std::string_view value) noexcept {
    return parse_targeted_cache_control_from_input(detail::http_structured_text_input{value});
}

std::optional<cache_control> detail::parse_targeted_cache_control_headers(
    detail::http_header_range_view headers, std::string_view header_name) noexcept {
    if (header_name.empty()) {
        return std::nullopt;
    }
    return parse_targeted_cache_control_from_input(
        detail::http_structured_header_input{headers.begin(), headers.end(), header_name});
}

targeted_cache_control_selection detail::resolve_targeted_cache_control_headers(
    detail::http_header_range_view headers,
    std::span<const std::string_view> target_headers) noexcept {
    const auto has_cache_control = [&headers]() noexcept {
        for (const auto field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
                return true;
            }
        }
        return false;
    };
    for (const auto target_name : target_headers) {
        if (target_name.empty()) {
            continue;
        }
        if (http_ascii_equals_ignore_case(target_name, "Cache-Control")) {
            if (has_cache_control()) {
                return {
                    .control_ = parse_cache_control_headers(headers),
                    .selected_header_ = target_name,
                    .targeted_ = false,
                };
            }
            continue;
        }
        if (const auto targeted = parse_targeted_cache_control_headers(headers, target_name)) {
            return {
                .control_ = *targeted,
                .selected_header_ = target_name,
                .targeted_ = true,
            };
        }
    }
    return {
        .control_ = parse_cache_control_headers(headers),
        .selected_header_ = has_cache_control() ? std::string_view("Cache-Control")
                                                : std::string_view{},
        .targeted_ = false,
    };
}

bool strip_http_cache_private_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    if (control.is_field_qualified_private()) {
        std::erase_if(headers, [&control](const http_header& field) noexcept {
            return control.has_private_field(field.name());
        });
    }
    return true;
}

bool strip_http_cache_private_headers(
    http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    const auto count = control.private_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.private_field(i));
    }
    return true;
}

bool strip_http_cache_private_headers(
    http_response& response, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    if (!control.is_field_qualified_private()) {
        return true;
    }
    auto staged = response.headers().clone();
    (void)strip_http_cache_private_headers(staged, control);
    response.commit_headers(std::move(staged));
    return true;
}

bool strip_http_cache_private_headers(std::vector<http_header>& headers) noexcept {
    return strip_http_cache_private_headers(headers, parse_cache_control(headers));
}

bool strip_http_cache_private_headers(http_response_headers& headers) {
    return strip_http_cache_private_headers(headers, parse_cache_control(headers));
}

bool strip_http_cache_private_headers(http_response& response) {
    return strip_http_cache_private_headers(response, parse_cache_control(response));
}

bool strip_http_cache_no_cache_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    if (control.is_field_qualified_no_cache()) {
        std::erase_if(headers, [&control](const http_header& field) noexcept {
            return control.has_no_cache_field(field.name());
        });
    }
    return true;
}

bool strip_http_cache_no_cache_headers(
    http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    const auto count = control.no_cache_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.no_cache_field(i));
    }
    return true;
}

bool strip_http_cache_no_cache_headers(
    http_response& response, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    if (!control.is_field_qualified_no_cache()) {
        return true;
    }
    auto staged = response.headers().clone();
    (void)strip_http_cache_no_cache_headers(staged, control);
    response.commit_headers(std::move(staged));
    return true;
}

namespace {

void append_preserved_cache_control_directives(
    std::string_view field_value, std::string& output) {
    detail::http_visit_comma_separated_quoted(field_value, [&](std::string_view token) {
        const auto eq = token.find('=');
        const auto name =
            http_trim_ows(eq == std::string_view::npos ? token : token.substr(0, eq));
        if (name.empty() || http_ascii_equals_ignore_case(name, "max-age") ||
            http_ascii_equals_ignore_case(name, "s-maxage")) {
            return true;
        }
        if (!output.empty()) {
            output.append(", ");
        }
        output.append(token);
        return true;
    });
}

void finish_rewritten_cache_control_max_age(
    std::string& output, std::uint64_t client_max_age) {
    if (!output.empty()) {
        output.append(", ");
    }
    output.append("max-age=");
    http_append_unsigned(output, client_max_age);
}

[[nodiscard]] std::string normalize_vary_field_value(std::string_view raw) {
    const auto trimmed = http_trim_ows(raw);
    std::string normalized;
    normalized.reserve(trimmed.size());
    bool in_ows = false;
    for (const char ch : trimmed) {
        if (ch == ' ' || ch == '\t') {
            if (!in_ows) {
                normalized.push_back(' ');
                in_ows = true;
            }
        } else {
            normalized.push_back(ch);
            in_ows = false;
        }
    }
    return normalized;
}

template <typename target_headers_type>
void merge_304_headers_impl(
    target_headers_type& cached_headers, detail::http_header_range_view not_modified_headers) {
    const auto nominated = parse_http_connection_nominated_fields(not_modified_headers);
    std::vector<std::string_view> updated_names;
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(field.value()) ||
            is_http_hop_by_hop_or_nominated_field(name, nominated) ||
            is_http_304_forbidden_header(name)) {
            continue;
        }
        const bool already_recorded =
            std::ranges::any_of(updated_names, [name](std::string_view existing) noexcept {
                return http_ascii_equals_ignore_case(existing, name);
            });
        if (!already_recorded) {
            updated_names.push_back(name);
        }
    }
    if (updated_names.empty()) {
        return;
    }
    if constexpr (std::same_as<target_headers_type, http_response_headers>) {
        for (const auto name : updated_names) {
            cached_headers.remove_header(name);
        }
    } else {
        std::erase_if(cached_headers, [&](const http_header& existing) noexcept {
            return std::ranges::any_of(updated_names, [&](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(existing.name(), updated);
            });
        });
    }
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        const bool should_copy =
            std::ranges::any_of(updated_names, [name](std::string_view updated) noexcept {
                return http_ascii_equals_ignore_case(name, updated);
            });
        if (!should_copy) {
            continue;
        }
        if constexpr (std::same_as<target_headers_type, http_response_headers>) {
            if (cached_headers.header(name).has_value() &&
                !http_ascii_equals_ignore_case(name, "Date") &&
                !http_ascii_equals_ignore_case(name, "ETag") &&
                !http_ascii_equals_ignore_case(name, "Expires") &&
                !http_ascii_equals_ignore_case(name, "Content-Location") &&
                !http_ascii_equals_ignore_case(name, "Last-Modified")) {
                cached_headers.header(
                    name, field.value(), {.mode_ = http_response_header_mode::append});
            } else {
                cached_headers.header(name, field.value());
            }
        } else {
            cached_headers.emplace_back(name, field.value());
        }
    }
}
}  // namespace

std::string rewrite_http_cache_control_max_age(
    std::string_view field_value, std::uint64_t client_max_age) {
    std::string output;
    append_preserved_cache_control_directives(field_value, output);
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

std::string detail::rewrite_http_cache_control_max_age_headers(
    detail::http_header_range_view headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

void rewrite_http_response_cache_max_age(
    std::vector<http_header>& response_headers, std::uint64_t client_max_age) {
    auto rewritten = rewrite_http_cache_control_max_age(response_headers, client_max_age);
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return http_ascii_equals_ignore_case(field.name(), "Cache-Control") ||
               http_ascii_equals_ignore_case(field.name(), "Expires");
    });
    response_headers.emplace_back("Cache-Control", rewritten);
}

void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto& field : response_headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    response_headers.remove_header("Expires");
    response_headers.header("Cache-Control", output);
}

void rewrite_http_response_cache_max_age(
    http_response& response, std::uint64_t client_max_age) {
    auto staged = response.headers().clone(1);
    rewrite_http_response_cache_max_age(staged, client_max_age);
    response.commit_headers(std::move(staged));
}

void http_vary_spec::update(std::string_view field_value) {
    if (wildcard_) {
        return;
    }
    detail::http_visit_comma_separated_tokens(field_value, [&](std::string_view token) {
        if (token == "*") {
            wildcard_ = true;
            field_names_.clear();
            return false;
        }
        if (!is_valid_http_header_name(token)) {
            return true;
        }
        std::string lower(token);
        for (auto& ch : lower) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        const auto it = std::ranges::lower_bound(field_names_, lower);
        if (it == field_names_.end() || *it != lower) {
            field_names_.insert(it, std::move(lower));
        }
        return true;
    });
}

void http_vary_spec::update_header_range(detail::http_header_range_view headers) {
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Vary")) {
            update(field.value());
        }
    }
}

http_vary_spec parse_http_vary(std::string_view field_value) {
    http_vary_spec spec;
    spec.update(field_value);
    return spec;
}

std::optional<std::string> detail::format_http_vary_cache_key_headers(
    const http_vary_spec& vary, detail::http_header_range_view request_headers) {
    if (vary.is_wildcard()) {
        return std::nullopt;
    }
    std::string key;
    for (const auto& field_name : vary.field_names()) {
        key.append(field_name);
        bool present = false;
        for (const auto header : request_headers) {
            if (!http_ascii_equals_ignore_case(header.name(), field_name)) {
                continue;
            }
            if (!present) {
                key.append(":1:");
                present = true;
            } else {
                key.append(", ");
            }
            key.append(normalize_vary_field_value(header.value()));
        }
        if (!present) {
            key.append(":0");
        }
        key.push_back('\n');
    }
    return key;
}

std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept {
    const auto trimmed = http_trim_ows(field_value);
    if (trimmed.empty() || trimmed.front() == '"') {
        return std::nullopt;
    }
    return parse_delta_seconds(trimmed);
}

std::uint64_t detail::compute_http_cache_age_headers(
    detail::http_header_range_view response_headers, std::time_t request_time,
    std::time_t response_time, std::time_t now) noexcept {
    std::optional<std::time_t> date_value;
    std::optional<std::uint64_t> age_value;
    for (const auto field : response_headers) {
        if (!date_value.has_value() && http_ascii_equals_ignore_case(field.name(), "Date")) {
            date_value = parse_http_date(field.value());
        } else if (!age_value.has_value() && http_ascii_equals_ignore_case(field.name(), "Age")) {
            age_value = parse_http_age(field.value());
        }
    }
    return compute_http_cache_age(date_value, age_value, request_time, response_time, now);
}

bool is_http_304_forbidden_header(std::string_view field_name) noexcept {
    switch (field_name.size()) {
        case 6U:
            return http_ascii_equals_ignore_case(field_name, "Digest");
        case 7U:
            return http_ascii_equals_ignore_case(field_name, "Trailer");
        case 11U:
            return http_ascii_equals_ignore_case(field_name, "Content-MD5") ||
                   http_ascii_equals_ignore_case(field_name, "Repr-Digest");
        case 12U:
            return http_ascii_equals_ignore_case(field_name, "Content-Type");
        case 13U:
            return http_ascii_equals_ignore_case(field_name, "Content-Range");
        case 14U:
            return http_ascii_equals_ignore_case(field_name, "Content-Length") ||
                   http_ascii_equals_ignore_case(field_name, "Content-Digest");
        case 16U:
            return http_ascii_equals_ignore_case(field_name, "Content-Encoding") ||
                   http_ascii_equals_ignore_case(field_name, "Content-Language");
        case 17U:
            return http_ascii_equals_ignore_case(field_name, "Transfer-Encoding");
        case 19U:
            return http_ascii_equals_ignore_case(field_name, "Content-Disposition");
        default:
            return false;
    }
}

void sanitize_http_304_response_headers(std::vector<http_header>& headers) {
    std::erase_if(headers, [](const http_header& field) noexcept {
        return is_http_304_forbidden_header(field.name());
    });
}

void sanitize_http_304_response_headers(http_response_headers& headers) {
    std::vector<std::string> to_remove;
    for (const auto& field : headers) {
        if (is_http_304_forbidden_header(field.name())) {
            to_remove.emplace_back(field.name());
        }
    }
    for (const auto& name : to_remove) {
        headers.remove_header(name);
    }
}

void sanitize_http_304_response_headers(http_response& response) {
    auto staged = response.headers().clone();
    sanitize_http_304_response_headers(staged);
    response.commit_headers(std::move(staged));
}

void detail::merge_http_304_response_headers_into(
    std::vector<http_header>& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}

void detail::merge_http_304_response_headers_into(
    http_response_headers& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}
void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response) {
    auto staged = cached_response.headers().clone(not_modified_response.headers().size());
    merge_http_304_response_headers(staged, not_modified_response.headers());
    cached_response.commit_headers(std::move(staged));
}

}  // namespace ruvia

namespace ruvia::detail {

// UTC calendar fields for an HTTP-date, or no value outside years 0000-9999
// (the four-digit IMF-fixdate year). Civil conversion is independent of the C
// runtime's date range and locale; in particular, Windows gmtime_s cannot
// represent pre-1970 UTC timestamps. The range check runs before calendar
// conversion so huge time_t values cannot overflow it.
[[nodiscard]] std::optional<std::tm> http_utc_tm(std::time_t time) noexcept {
    using namespace std::chrono;
    constexpr auto first = duration_cast<seconds>(sys_days{year{0} / January / 1}.time_since_epoch()).count();
    constexpr auto end = duration_cast<seconds>(sys_days{year{10000} / January / 1}.time_since_epoch()).count();
    if (std::cmp_less(time, first) || std::cmp_greater_equal(time, end)) {
        return std::nullopt;
    }
    const sys_seconds instant{seconds{static_cast<seconds::rep>(time)}};
    const auto date = floor<days>(instant);
    const year_month_day calendar{date};
    const hh_mm_ss clock{instant - date};
    std::tm utc{};
    utc.tm_year = static_cast<int>(calendar.year()) - 1900;
    utc.tm_mon = static_cast<int>(static_cast<unsigned>(calendar.month())) - 1;
    utc.tm_mday = static_cast<int>(static_cast<unsigned>(calendar.day()));
    utc.tm_wday = static_cast<int>(weekday{date}.c_encoding());
    utc.tm_hour = static_cast<int>(clock.hours().count());
    utc.tm_min = static_cast<int>(clock.minutes().count());
    utc.tm_sec = static_cast<int>(clock.seconds().count());
    return utc;
}

std::optional<std::time_t> http_civil_to_time_t(
    int year, int month, int day, int hour, int minute, int second) noexcept {
    if (year < static_cast<int>((std::chrono::year::min)()) || year > static_cast<int>((std::chrono::year::max)()) ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days_since_epoch = static_cast<std::int64_t>(std::chrono::sys_days{date}.time_since_epoch().count());
    const auto total = days_since_epoch * 86400 + static_cast<std::int64_t>(hour) * 3600 +
                       static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    if (!std::in_range<std::time_t>(total)) {
        return std::nullopt;
    }
    return static_cast<std::time_t>(total);
}

namespace {

// "Date: <value>\r\n" -- the cache is the single owner of this wire format.
// Both the full-line accessor (HTTP/1 text response) and the
// value-only accessor (HPACK :date for HTTP/2) derive their views from these
// constants, so the framing lives in exactly one place.
inline constexpr std::string_view date_header_prefix = "Date: ";
inline constexpr std::string_view date_header_suffix = "\r\n";

struct date_cache final {
    std::array<char, 64> line_{};
    std::size_t size_{0};
    std::time_t second_{};
    bool initialized_{false};
};

[[nodiscard]] date_cache& worker_date_cache() noexcept {
    thread_local date_cache cache;
    return cache;
}

}  // namespace

std::string_view cached_date_header() noexcept {
    const auto now = std::time(nullptr);
    auto& cache = worker_date_cache();
    if (!cache.initialized_ || cache.second_ != now) {
        cache.second_ = now;
        cache.initialized_ = true;
        cache.size_ = 0;
        // RFC 9110 section 6.6.1: do not generate Date without a usable clock.
        if (now != std::time_t{-1}) {
            if (const auto date = format_http_date(now)) {
                std::memcpy(cache.line_.data(), date_header_prefix.data(), date_header_prefix.size());
                std::memcpy(cache.line_.data() + date_header_prefix.size(), date->data(), date->size());
                cache.size_ = date_header_prefix.size() + date->size();
                cache.line_[cache.size_++] = date_header_suffix[0];
                cache.line_[cache.size_++] = date_header_suffix[1];
            }
        }
    }
    return std::string_view(cache.line_.data(), cache.size_);
}

std::string_view cached_date_value() noexcept {
    const auto line = cached_date_header();
    constexpr std::size_t framing = date_header_prefix.size() + date_header_suffix.size();
    if (line.size() <= framing) {
        return {};
    }
    return line.substr(date_header_prefix.size(), line.size() - framing);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

// IMF-fixdate day names (RFC 9110 section 5.6.7), indexed by tm_wday.
constexpr std::array<std::string_view, 7> http_weekday_names{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

[[nodiscard]] int month_index(std::string_view value) noexcept {
    for (std::size_t i = 0; i < detail::http_month_names.size(); ++i) {
        if (value == detail::http_month_names[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

[[nodiscard]] bool is_short_weekday(std::string_view value) noexcept {
    return std::ranges::find(http_weekday_names, value) != http_weekday_names.end();
}

[[nodiscard]] bool is_long_weekday(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 7> weekdays{
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    return std::ranges::find(weekdays, value) != weekdays.end();
}

[[nodiscard]] std::optional<int> parse_fixed_digits(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed_value = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        parsed_value = parsed_value * 10 + (c - '0');
    }
    return parsed_value;
}

[[nodiscard]] std::optional<std::time_t> parse_imf_fixdate(std::string_view value) noexcept {
    if (value.size() != http_imf_fixdate_size || !is_short_weekday(value.substr(0, 3)) || value[3] != ',' ||
        value[4] != ' ' || value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || value[25] != ' ' || value.substr(26, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = parse_fixed_digits(value.substr(5, 2));
    const auto month = month_index(value.substr(8, 3));
    const auto year = parse_fixed_digits(value.substr(12, 4));
    const auto hour = parse_fixed_digits(value.substr(17, 2));
    const auto minute = parse_fixed_digits(value.substr(20, 2));
    const auto second = parse_fixed_digits(value.substr(23, 2));
    if (!day || month == 0 || !year || !hour || !minute || !second) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] std::optional<std::time_t> parse_asctime_date(std::string_view value) noexcept {
    if (value.size() != 24 || value[3] != ' ' || value[7] != ' ' || value[10] != ' ' ||
        value[13] != ':' || value[16] != ':' || value[19] != ' ' ||
        !is_short_weekday(value.substr(0, 3))) {
        return std::nullopt;
    }
    const auto month = month_index(value.substr(4, 3));
    auto day_field = value.substr(8, 2);
    if (day_field.front() == ' ') {
        day_field.remove_prefix(1);
    }
    const auto day = parse_fixed_digits(day_field);
    const auto hour = parse_fixed_digits(value.substr(11, 2));
    const auto minute = parse_fixed_digits(value.substr(14, 2));
    const auto second = parse_fixed_digits(value.substr(17, 2));
    const auto year = parse_fixed_digits(value.substr(20, 4));
    if (month == 0 || !day || !hour || !minute || !second || !year) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] int resolve_rfc850_year(int short_year, int current_year) noexcept {
    const auto current_century = current_year - current_year % 100;
    auto year = current_century + short_year;
    // RFC 9110 section 5.6.7: an obsolete two-digit date that appears more
    // than 50 years in the future denotes the most recent past year with the
    // same final two digits. This is a rolling pivot, not the fixed POSIX
    // 68/69 split.
    if (year > current_year + 50) {
        year -= 100;
    }
    return year;
}

[[nodiscard]] std::optional<std::time_t> parse_rfc850_date(std::string_view value) noexcept {
    const auto comma = value.find(", ");
    if (comma == std::string_view::npos || !is_long_weekday(value.substr(0, comma))) {
        return std::nullopt;
    }
    const auto body = value.substr(comma + 2);
    if (body.size() != 22 || body[2] != '-' || body[6] != '-' || body[9] != ' ' ||
        body[12] != ':' || body[15] != ':' || body[18] != ' ' || body.substr(19, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = parse_fixed_digits(body.substr(0, 2));
    const auto month = month_index(body.substr(3, 3));
    const auto short_year = parse_fixed_digits(body.substr(7, 2));
    const auto hour = parse_fixed_digits(body.substr(10, 2));
    const auto minute = parse_fixed_digits(body.substr(13, 2));
    const auto second = parse_fixed_digits(body.substr(16, 2));
    if (!day || month == 0 || !short_year || !hour || !minute || !second) {
        return std::nullopt;
    }
    const auto now = std::time(nullptr);
    if (now == std::time_t{-1}) {
        return std::nullopt;
    }
    const auto current_utc = detail::http_utc_tm(now);
    if (!current_utc) {
        return std::nullopt;
    }
    const int current_year = current_utc->tm_year + 1900;
    auto year = resolve_rfc850_year(*short_year, current_year);
    // RFC 9110 section 5.6.7 applies the rolling 50-year pivot to the full
    // timestamp. Equal years alone do not establish that a date is in range.
    const std::array date_fields{month, *day, *hour, *minute, *second};
    const std::array reference_fields{current_utc->tm_mon + 1, current_utc->tm_mday, current_utc->tm_hour,
        current_utc->tm_min, current_utc->tm_sec};
    if (year == current_year + 50 && date_fields > reference_fields) {
        year -= 100;
    }
    return detail::http_civil_to_time_t(year, month, *day, *hour, *minute, *second);
}

}  // namespace

// Allocation-free wire value; an unrepresentable date must not be truncated or
// substituted with a different timestamp. Fixed English names are independent
// of the process locale, as required by RFC 9110 section 5.6.7.
std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept {
    const auto utc = detail::http_utc_tm(value);
    if (!utc) {
        return std::nullopt;
    }
    std::array<char, http_imf_fixdate_size> output{};
    auto* out = output.data();
    const long year = static_cast<long>(utc->tm_year) + 1900;

    std::size_t i = 0;
    const auto put2 = [out, &i](int field) noexcept {
        out[i++] = static_cast<char>('0' + (field / 10) % 10);
        out[i++] = static_cast<char>('0' + field % 10);
    };
    const auto put3 = [out, &i](std::string_view name) noexcept {
        out[i++] = name[0];
        out[i++] = name[1];
        out[i++] = name[2];
    };

    put3(http_weekday_names[static_cast<std::size_t>(utc->tm_wday)]);
    out[i++] = ',';
    out[i++] = ' ';
    put2(utc->tm_mday);
    out[i++] = ' ';
    put3(detail::http_month_names[static_cast<std::size_t>(utc->tm_mon)]);
    out[i++] = ' ';
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 1000) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 100) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 10) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>(year % 10));
    out[i++] = ' ';
    put2(utc->tm_hour);
    out[i++] = ':';
    put2(utc->tm_min);
    out[i++] = ':';
    put2(utc->tm_sec);
    out[i++] = ' ';
    out[i++] = 'G';
    out[i++] = 'M';
    out[i++] = 'T';
    return output;
}

std::optional<std::time_t> parse_http_date(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (const auto imf = parse_imf_fixdate(value)) {
        return imf;
    }
    if (const auto rfc850 = parse_rfc850_date(value)) {
        return rfc850;
    }
    return parse_asctime_date(value);
}

}  // namespace ruvia
