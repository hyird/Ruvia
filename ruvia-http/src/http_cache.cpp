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
#include "http_content_coding.h"
#include "http_date.h"
#include "http_field_text.h"
#include "http_header.h"
#include "http_proxy_forwarding.h"
#include "http_representation.h"
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
        return parse_saturating_decimal(value);
    }
    std::uint64_t result_value = 0;
    bool any_digit = false;
    http_quoted_string_cursor cursor(value);
    unsigned char ch = 0;
    while (cursor.next(ch)) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        result_value = accumulate_decimal_digit(result_value, static_cast<unsigned>(ch - '0'));
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
                    value_.set(cache_control_directive::public_);
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
                        value_.set(cache_control_directive::private_);
                        value_.set(cache_control_directive::private_unqualified);
                        value_.private_fields_.clear();
                    } else if (detail::is_http_token_or_quoted_string(arg)) {
                        value_.set(cache_control_directive::private_);
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

cache_control parse_cache_control(const http_header_range_view& headers) noexcept {
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

bool http_cache_control_has_no_transform(const http_header_range_view& headers) noexcept {
    return parse_cache_control(headers).no_transform();
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
std::optional<cache_control> parse_targeted_cache_control_from_input(input_type input) {
    http_structured_parser<input_type> parser{std::move(input)};
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
        http_structured_item item{
            .kind_ = http_structured_item::kind_type::boolean,
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
            if (item.kind_ == http_structured_item::kind_type::boolean) {
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
            if (item.kind_ == http_structured_item::kind_type::integer && item.integer_ >= 0) {
                target = static_cast<std::uint64_t>(item.integer_);
            } else {
                target.reset();
            }
        };
        const auto apply_qualified_directive = [&](cache_control_directive flag_dir, cache_control_directive unqual_dir, cache_control::nominated_field_set& fields) noexcept {
            if (item.kind_ == http_structured_item::kind_type::boolean) {
                fields.clear();
                if (item.boolean_) {
                    result.set(flag_dir);
                    result.set(unqual_dir);
                } else {
                    result.clear_directive(flag_dir);
                    result.clear_directive(unqual_dir);
                }
            } else if (item.kind_ == http_structured_item::kind_type::string || item.kind_ == http_structured_item::kind_type::token) {
                fields.clear();
                result.clear_directive(unqual_dir);
                result.set(flag_dir);
                std::array<char, 256> unescaped{};
                std::string_view list_text = item.text();
                if (item.kind_ == http_structured_item::kind_type::string && item.has_escapes_) {
                    std::size_t out_len = 0;
                    for (std::size_t i = 0; i < item.text().size(); ++i) {
                        if (item.text()[i] == '\\' && i + 1U < item.text().size()) {
                            ++i;
                        }
                        if (out_len >= unescaped.size()) {
                            result.set(unqual_dir);
                            fields.clear();
                            return;
                        }
                        unescaped[out_len++] = item.text()[i];
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
            apply_bool_directive(cache_control_directive::public_);
        } else if (key == "immutable") {
            apply_bool_directive(cache_control_directive::immutable);
        } else if (key == "only-if-cached") {
            apply_bool_directive(cache_control_directive::only_if_cached);
        } else if (key == "private") {
            apply_qualified_directive(cache_control_directive::private_, cache_control_directive::private_unqualified, result.private_fields_);
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
            if (item.kind_ == http_structured_item::kind_type::boolean && item.boolean_) {
                result.set(cache_control_directive::max_stale_any);
                result.max_stale_.reset();
            } else if (item.kind_ == http_structured_item::kind_type::integer && item.integer_ >= 0) {
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
    return parse_targeted_cache_control_from_input(http_structured_text_input{value});
}

std::optional<cache_control> parse_targeted_cache_control(const http_header_range_view& headers, std::string_view header_name) {
    if (header_name.empty()) {
        return std::nullopt;
    }
    return parse_targeted_cache_control_from_input(http_structured_header_input{headers.begin(), headers.end(), header_name});
}

targeted_cache_control_selection resolve_targeted_cache_control(const http_header_range_view& headers, std::span<const std::string_view> target_headers) {
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
                    .control_ = parse_cache_control(headers),
                    .selected_header_ = target_name,
                    .targeted_ = false,
                };
            }
            continue;
        }
        if (const auto targeted = parse_targeted_cache_control(headers, target_name)) {
            return {
                .control_ = *targeted,
                .selected_header_ = target_name,
                .targeted_ = true,
            };
        }
    }
    return {
        .control_ = parse_cache_control(headers),
        .selected_header_ = has_cache_control() ? std::string_view("Cache-Control") : std::string_view{},
        .targeted_ = false,
    };
}

bool strip_http_cache_private_headers(editable_http_headers headers, const cache_control& control, std::span<const std::string_view> protected_fields) noexcept {
    if (control.no_store() || control.is_unqualified_private()) {
        return false;
    }
    for (const auto name : protected_fields) {
        if (control.has_private_field(name)) {
            return false;
        }
    }
    const auto count = control.private_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove(control.private_field(i));
    }
    return true;
}

bool strip_http_cache_no_cache_headers(editable_http_headers headers, const cache_control& control) noexcept {
    if (control.no_store() || control.is_unqualified_no_cache()) {
        return false;
    }
    const auto count = control.no_cache_field_count();
    for (std::size_t i = 0; i < count; ++i) {
        headers.remove(control.no_cache_field(i));
    }
    return true;
}

namespace {

void append_preserved_cache_control_directives(std::string_view field_value, std::string& output) {
    detail::http_visit_comma_separated_quoted(field_value, [&](std::string_view token) {
        const auto eq = token.find('=');
        const auto name = http_trim_ows(eq == std::string_view::npos ? token : token.substr(0, eq));
        if (name.empty() || http_ascii_equals_ignore_case(name, "max-age")) {
            return true;
        }
        if (!output.empty()) {
            output.append(", ");
        }
        output.append(token);
        return true;
    });
}

void finish_rewritten_cache_control_max_age(std::string& output, std::optional<std::uint64_t> client_max_age) {
    if (!client_max_age) {
        return;
    }
    if (!output.empty()) {
        output.append(", ");
    }
    output.append("max-age=");
    append_decimal(output, *client_max_age);
}

void append_normalized_vary_field_value(std::string_view raw, std::string& output) {
    const auto trimmed = http_trim_ows(raw);
    bool in_ows = false;
    for (const char ch : trimmed) {
        if (ch == ' ' || ch == '\t') {
            if (!in_ows) {
                output.push_back(' ');
                in_ows = true;
            }
        } else {
            output.push_back(ch);
            in_ows = false;
        }
    }
}

// Fields a message carries once; a later 304 line replaces the earlier one.
[[nodiscard]] bool is_single_valued_304_field(std::string_view name) noexcept {
    return http_ascii_equals_ignore_case(name, "Date") || http_ascii_equals_ignore_case(name, "ETag") || http_ascii_equals_ignore_case(name, "Expires") || http_ascii_equals_ignore_case(name, "Content-Location") || http_ascii_equals_ignore_case(name, "Last-Modified");
}
}  // namespace

std::string rewrite_http_cache_control_max_age(std::string_view field_value, std::optional<std::uint64_t> client_max_age) {
    std::string output;
    append_preserved_cache_control_directives(field_value, output);
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

std::string rewrite_http_cache_control_max_age(const http_header_range_view& headers, std::optional<std::uint64_t> client_max_age) {
    std::string output;
    for (const auto field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "Cache-Control")) {
            append_preserved_cache_control_directives(field.value(), output);
        }
    }
    finish_rewritten_cache_control_max_age(output, client_max_age);
    return output;
}

void rewrite_http_response_cache_max_age(editable_http_headers response_headers, std::optional<std::uint64_t> client_max_age) {
    const auto rewritten = rewrite_http_cache_control_max_age(response_headers.fields(), client_max_age);
    response_headers.transact([&](editable_http_headers candidate) {
        if (rewritten.empty()) {
            candidate.remove("Cache-Control");
        } else {
            candidate.set("Cache-Control", rewritten);
        }
        if (client_max_age) {
            candidate.remove("Expires");
        }
    });
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

void http_vary_spec::update_headers(const http_header_range_view& headers) {
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

http_vary_spec parse_http_vary(const http_header_range_view& headers) {
    http_vary_spec spec;
    spec.update_headers(headers);
    return spec;
}

std::optional<std::string> format_http_vary_cache_key(const http_vary_spec& vary, const http_header_range_view& request_headers) {
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
            append_normalized_vary_field_value(header.value(), key);
        }
        if (!present) {
            key.append(":0");
        }
        key.push_back('\n');
    }
    return key;
}

bool http_vary_requests_match(const http_vary_spec& vary, const http_header_range_view& first_request, const http_header_range_view& second_request) {
    const auto first_key = format_http_vary_cache_key(vary, first_request);
    const auto second_key = format_http_vary_cache_key(vary, second_request);
    return first_key.has_value() && second_key.has_value() && *first_key == *second_key;
}

std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept {
    const auto trimmed = http_trim_ows(field_value);
    if (trimmed.empty() || trimmed.front() == '"') {
        return std::nullopt;
    }
    return parse_delta_seconds(trimmed);
}

std::uint64_t compute_http_cache_age(const http_header_range_view& response_headers, std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
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

void sanitize_http_304_response_headers(editable_http_headers headers) noexcept {
    headers.remove_if(is_http_304_forbidden_header);
}

namespace {

// Stream the coding stack across field lines without joining or allocating.
class cache_content_encoding_cursor final {
public:
    explicit cache_content_encoding_cursor(const http_header_range_view& headers) noexcept :
        field_(headers.begin()),
        end_(headers.end()) {}

    [[nodiscard]] std::string_view next() noexcept {
        while (true) {
            if (!remaining_.empty()) {
                const auto comma = remaining_.find(',');
                const auto token = http_trim_ows(remaining_.substr(0, comma));
                remaining_ = comma == std::string_view::npos ? std::string_view{} : remaining_.substr(comma + 1);
                if (!token.empty()) {
                    if (!is_valid_http_header_name(token)) {
                        valid_ = false;
                        return {};
                    }
                    return token;
                }
            } else {
                if (field_ == end_) {
                    return {};
                }
                const auto field = *field_++;
                if (http_ascii_equals_ignore_case(field.name(), "Content-Encoding")) {
                    present_ = true;
                    remaining_ = field.value();
                }
            }
        }
    }

    bool valid_{true};
    bool present_{false};

private:
    http_header_range_view::iterator field_;
    http_header_range_view::iterator end_;
    std::string_view remaining_;
};

[[nodiscard]] bool cache_content_encodings_match(const http_header_range_view& cached, const http_header_range_view& validating) noexcept {
    cache_content_encoding_cursor left(cached);
    cache_content_encoding_cursor right(validating);
    auto right_token = right.next();
    if (!right.present_) {
        return true;
    }
    while (true) {
        const auto left_token = left.next();
        const auto left_coding = http_content_coding_from_token(left_token);
        const auto right_coding = http_content_coding_from_token(right_token);
        const bool equal = http_ascii_equals_ignore_case(left_token, right_token) || (left_coding && right_coding && left_coding == right_coding);
        if (!left.valid_ || !right.valid_ || !equal) {
            return false;
        }
        if (left_token.empty()) {
            return true;
        }
        right_token = right.next();
    }
}

struct cache_validator_fields final {
    http_unique_field_lookup etag_;
    http_unique_field_lookup modified_;
    http_unique_field_lookup date_;

    explicit cache_validator_fields(const http_header_range_view& headers) noexcept {
        for (const auto field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), "ETag")) {
                etag_.add_occurrence(http_trim_ows(field.value()));
            } else if (http_ascii_equals_ignore_case(field.name(), "Last-Modified")) {
                modified_.add_occurrence(field.value());
            } else if (http_ascii_equals_ignore_case(field.name(), "Date")) {
                date_.add_occurrence(field.value());
            }
        }
    }
};

// Every name the owned 304 lines carry is retired from the stored block, then
// the lines are written back in order.
void replace_stored_304_fields(editable_http_headers stored_headers, std::span<const http_header> replacements) {
    const auto repeats_earlier_line = [replacements](std::size_t index) noexcept {
        const auto name = replacements[index].name();
        return std::ranges::any_of(replacements.first(index), [name](const http_header& earlier) noexcept {
            return http_ascii_equals_ignore_case(earlier.name(), name);
        });
    };
    for (std::size_t index = 0; index < replacements.size(); ++index) {
        if (!repeats_earlier_line(index)) {
            stored_headers.remove(replacements[index].name());
        }
    }
    for (std::size_t index = 0; index < replacements.size(); ++index) {
        const auto& line = replacements[index];
        // The first line of a name restores the field; later lines join it
        // unless the field is single-valued.
        if (repeats_earlier_line(index) && !is_single_valued_304_field(line.name())) {
            stored_headers.append(line.name(), line.value());
        } else {
            stored_headers.set(line.name(), line.value());
        }
    }
}

}  // namespace

http_cache_validator_match match_http_cache_validators(const http_header_range_view& cached_headers, const http_header_range_view& validating_headers) noexcept {
    if (!cache_content_encodings_match(cached_headers, validating_headers)) {
        return http_cache_validator_match::none;
    }
    const cache_validator_fields cached(cached_headers);
    const cache_validator_fields validating(validating_headers);
    if (cached.etag_.status_ == unique_lookup_status::repeated || validating.etag_.status_ == unique_lookup_status::repeated) {
        return http_cache_validator_match::none;
    }
    if (validating.etag_.status_ == unique_lookup_status::found) {
        if (cached.etag_.status_ != unique_lookup_status::found || !is_valid_http_entity_tag(cached.etag_.value_) || !is_valid_http_entity_tag(validating.etag_.value_) || !http_weak_etag_equals(cached.etag_.value_, validating.etag_.value_)) {
            return http_cache_validator_match::none;
        }
        if (is_strong_http_entity_tag(validating.etag_.value_)) {
            return http_strong_etag_equals(cached.etag_.value_, validating.etag_.value_) ? http_cache_validator_match::strong : http_cache_validator_match::none;
        }
        return http_cache_validator_match::weak;
    }
    if (cached.modified_.status_ == unique_lookup_status::repeated || validating.modified_.status_ == unique_lookup_status::repeated) {
        return http_cache_validator_match::none;
    }
    if (validating.modified_.status_ == unique_lookup_status::found) {
        const auto modified = parse_http_date(cached.modified_.value_);
        const auto validated = parse_http_date(validating.modified_.value_);
        if (!modified || !validated || *modified != *validated) {
            return http_cache_validator_match::none;
        }
        const auto date = cached.date_.status_ == unique_lookup_status::found ? parse_http_date(cached.date_.value_) : std::nullopt;
        const bool strong = date && *date >= *modified && static_cast<std::uint64_t>(*date) - static_cast<std::uint64_t>(*modified) >= 60;
        return strong ? http_cache_validator_match::strong : http_cache_validator_match::weak;
    }
    return cached.etag_.status_ == unique_lookup_status::absent && cached.modified_.status_ == unique_lookup_status::absent ? http_cache_validator_match::unvalidated : http_cache_validator_match::none;
}

bool weaken_http_cache_etag(editable_http_headers headers) {
    const auto etag = find_unique_http_header(headers.fields(), "ETag");
    if (etag.status_ == unique_lookup_status::repeated || (etag.status_ == unique_lookup_status::found && !is_valid_http_entity_tag(etag.value_))) {
        return false;
    }
    if (etag.status_ == unique_lookup_status::found && is_strong_http_entity_tag(etag.value_)) {
        const auto weak = weaken_http_etag(etag.value_);
        headers.set("ETag", weak);
    }
    return true;
}

bool merge_http_304_response_headers(editable_http_headers cached_headers, const http_header_range_view& not_modified_headers) {
    if (match_http_cache_validators(cached_headers.fields(), not_modified_headers) == http_cache_validator_match::none) {
        return false;
    }
    const auto nominated = parse_http_connection_nominated_fields(not_modified_headers);
    if (nominated.status() != http_connection_fields_status::valid) {
        return false;
    }
    // The 304 may view the cached block itself, wholly or in part, so the
    // lines to write are copied out before the first cached line is retired.
    std::vector<http_header> replacements;
    for (const auto field : not_modified_headers) {
        if (is_valid_http_header_name(field.name()) && is_valid_http_header_value(field.value()) && !is_http_hop_by_hop_or_nominated_field(field.name(), nominated) && !is_http_304_forbidden_header(field.name())) {
            replacements.emplace_back(field.name(), field.value());
        }
    }
    if (replacements.empty()) {
        return true;
    }
    // Several lines are rewritten: an http_response takes all of them or none.
    cached_headers.transact([&replacements](editable_http_headers candidate) {
        replace_stored_304_fields(candidate, replacements);
    }, replacements.size());
    return true;
}

}  // namespace ruvia
