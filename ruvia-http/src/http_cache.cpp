#include "http_cache.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "decimal_text.h"
#include "http_date.h"
#include "http_header.h"
#include "http_proxy_forwarding.h"
#include "http_response.h"
#include "http_structured_field.h"

namespace ruvia {
namespace {

// Parse a delta-seconds value (RFC 9111 section 1.2.2): an optionally DQUOTE-wrapped non-negative
// integer. Quoted-pairs have their RFC 9110 section 5.6.4 semantic value, so
// `"6\0"` is equivalent to `60`. Syntactically valid overflow saturates to the
// greatest convenient representation; non-digit / empty input remains invalid.
[[nodiscard]] std::optional<std::uint64_t> parse_delta_seconds(std::string_view value) noexcept {
    if (value.empty() || value.front() != '"') {
        return http_parse_saturating_decimal(value);
    }
    std::uint64_t result_value = 0;
    bool any_digit = false;
    http_quoted_string_cursor cursor(value);
    unsigned char ch = 0;
    while (cursor.next(ch)) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        result_value = http_accumulate_decimal_digit(result_value, static_cast<unsigned>(ch - '0'));
        any_digit = true;
    }
    if (!cursor.closed() || cursor.consumed() != value.size() || !any_digit) {
        return std::nullopt;
    }
    return result_value;
}

[[nodiscard]] bool parse_nominated_field_list_arg(std::string_view arg, auto&& add_field) noexcept {
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
                            const bool added = parse_nominated_field_list_arg(arg, [this](std::string_view field_name) noexcept {
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
                            const bool added = parse_nominated_field_list_arg(arg, [this](std::string_view field_name) noexcept {
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
    if (count_ >= max_nominated_fields || field_name.size() > max_nominated_field_bytes - storage_used_) {
        return false;
    }
    offsets_[count_] = storage_used_;
    lengths_[count_] = static_cast<std::uint8_t>(field_name.size());
    for (std::size_t i = 0; i < field_name.size(); ++i) {
        storage_[storage_used_ + i] = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(field_name[i])));
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
        const auto apply_delta_directive = [&](std::optional<std::uint64_t>& target) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::integer && item.integer_ >= 0) {
                target = static_cast<std::uint64_t>(item.integer_);
            } else {
                target.reset();
            }
        };
        const auto apply_qualified_directive = [&](cache_control_directive flag_dir, cache_control_directive unqual_dir, cache_control::nominated_field_set& fields) noexcept {
            if (item.kind_ == detail::http_structured_item::kind_type::boolean) {
                fields.clear();
                if (item.boolean_) {
                    result.set(flag_dir);
                    result.set(unqual_dir);
                } else {
                    result.clear_directive(flag_dir);
                    result.clear_directive(unqual_dir);
                }
            } else if (item.kind_ == detail::http_structured_item::kind_type::string || item.kind_ == detail::http_structured_item::kind_type::token) {
                fields.clear();
                result.clear_directive(unqual_dir);
                result.set(flag_dir);
                std::array<char, 256> unescaped{};
                std::string_view list_text = item.text_;
                if (item.kind_ == detail::http_structured_item::kind_type::string && item.has_escapes_) {
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
            apply_qualified_directive(cache_control_directive::private_value, cache_control_directive::private_unqualified, result.private_fields_);
        } else if (key == "no-cache") {
            apply_qualified_directive(cache_control_directive::no_cache, cache_control_directive::no_cache_unqualified, result.no_cache_fields_);
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
            } else if (item.kind_ == detail::http_structured_item::kind_type::integer && item.integer_ >= 0) {
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

std::optional<cache_control> detail::parse_targeted_cache_control_headers(detail::http_header_range_view headers, std::string_view header_name) noexcept {
    if (header_name.empty()) {
        return std::nullopt;
    }
    return parse_targeted_cache_control_from_input(detail::http_structured_header_input{headers.begin(), headers.end(), header_name});
}

targeted_cache_control_selection detail::resolve_targeted_cache_control_headers(detail::http_header_range_view headers, std::span<const std::string_view> target_headers) noexcept {
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
        .selected_header_ = has_cache_control() ? std::string_view("Cache-Control") : std::string_view{},
        .targeted_ = false,
    };
}

bool strip_http_cache_private_headers(std::vector<http_header>& headers, const cache_control& control) noexcept {
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

bool strip_http_cache_private_headers(http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    const auto count = control.private_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.private_field(i));
    }
    return true;
}

bool strip_http_cache_private_headers(http_response& response, const cache_control& control) {
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

bool strip_http_cache_no_cache_headers(std::vector<http_header>& headers, const cache_control& control) noexcept {
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

bool strip_http_cache_no_cache_headers(http_response_headers& headers, const cache_control& control) {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    const auto count = control.no_cache_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove_header(control.no_cache_field(i));
    }
    return true;
}

bool strip_http_cache_no_cache_headers(http_response& response, const cache_control& control) {
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

void append_preserved_cache_control_directives(std::string_view field_value, std::string& output) {
    detail::http_visit_comma_separated_quoted(field_value, [&](std::string_view token) {
        const auto eq = token.find('=');
        const auto name = http_trim_ows(eq == std::string_view::npos ? token : token.substr(0, eq));
        if (name.empty() || http_ascii_equals_ignore_case(name, "max-age") || http_ascii_equals_ignore_case(name, "s-maxage")) {
            return true;
        }
        if (!output.empty()) {
            output.append(", ");
        }
        output.append(token);
        return true;
    });
}

void finish_rewritten_cache_control_max_age(std::string& output, std::uint64_t client_max_age) {
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
void merge_304_headers_impl(target_headers_type& cached_headers, detail::http_header_range_view not_modified_headers) {
    const auto nominated = parse_http_connection_nominated_fields(not_modified_headers);
    std::vector<std::string_view> updated_names;
    for (const auto field : not_modified_headers) {
        const auto name = field.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(field.value()) || is_http_hop_by_hop_or_nominated_field(name, nominated) || is_http_304_forbidden_header(name)) {
            continue;
        }
        const bool already_recorded = std::ranges::any_of(updated_names, [name](std::string_view existing) noexcept {
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
        const bool should_copy = std::ranges::any_of(updated_names, [name](std::string_view updated) noexcept {
            return http_ascii_equals_ignore_case(name, updated);
        });
        if (!should_copy) {
            continue;
        }
        if constexpr (std::same_as<target_headers_type, http_response_headers>) {
            if (cached_headers.header(name).has_value() && !http_ascii_equals_ignore_case(name, "Date") && !http_ascii_equals_ignore_case(name, "ETag") && !http_ascii_equals_ignore_case(name, "Expires") && !http_ascii_equals_ignore_case(name, "Content-Location") && !http_ascii_equals_ignore_case(name, "Last-Modified")) {
                cached_headers.header(name, field.value(), {.mode_ = http_response_header_mode::append});
            } else {
                cached_headers.header(name, field.value());
            }
        } else {
            cached_headers.emplace_back(name, field.value());
        }
    }
}
}  // namespace

std::string rewrite_http_cache_control_max_age(std::string_view field_value, std::uint64_t client_max_age) {
    std::string output;
    append_preserved_cache_control_directives(field_value, output);
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

std::string detail::rewrite_http_cache_control_max_age_headers(detail::http_header_range_view headers, std::uint64_t client_max_age) {
    std::string output;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

void rewrite_http_response_cache_max_age(std::vector<http_header>& response_headers, std::uint64_t client_max_age) {
    auto rewritten = rewrite_http_cache_control_max_age(response_headers, client_max_age);
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return http_ascii_equals_ignore_case(field.name(), "Cache-Control") || http_ascii_equals_ignore_case(field.name(), "Expires");
    });
    response_headers.emplace_back("Cache-Control", rewritten);
}

void rewrite_http_response_cache_max_age(http_response_headers& response_headers, std::uint64_t client_max_age) {
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

void rewrite_http_response_cache_max_age(http_response& response, std::uint64_t client_max_age) {
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
        std::string lower = http_ascii_lowercase(token);
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

std::optional<std::string> detail::format_http_vary_cache_key_headers(const http_vary_spec& vary, detail::http_header_range_view request_headers) {
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

std::uint64_t detail::compute_http_cache_age_headers(detail::http_header_range_view response_headers, std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
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
            return http_ascii_equals_ignore_case(field_name, "Content-MD5") || http_ascii_equals_ignore_case(field_name, "Repr-Digest");
        case 12U:
            return http_ascii_equals_ignore_case(field_name, "Content-Type");
        case 13U:
            return http_ascii_equals_ignore_case(field_name, "Content-Range");
        case 14U:
            return http_ascii_equals_ignore_case(field_name, "Content-Length") || http_ascii_equals_ignore_case(field_name, "Content-Digest");
        case 16U:
            return http_ascii_equals_ignore_case(field_name, "Content-Encoding") || http_ascii_equals_ignore_case(field_name, "Content-Language");
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

void detail::merge_http_304_response_headers_into(std::vector<http_header>& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}

void detail::merge_http_304_response_headers_into(http_response_headers& cached_headers, detail::http_header_range_view not_modified_headers) {
    merge_304_headers_impl(cached_headers, not_modified_headers);
}
void merge_http_304_response_headers(http_response& cached_response, const http_response& not_modified_response) {
    auto staged = cached_response.headers().clone(not_modified_response.headers().size());
    merge_http_304_response_headers(staged, not_modified_response.headers());
    cached_response.commit_headers(std::move(staged));
}

}  // namespace ruvia
