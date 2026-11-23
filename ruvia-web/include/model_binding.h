#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "http_content_negotiation.h"
#include "http_error.h"
#include "http_header.h"
#include "http_status.h"
#include "middleware.h"
#include "model.h"
#include "task.h"

namespace ruvia {

enum class validation_target : std::uint8_t {
    json,
    form,
    query,
    param,
    header,
    cookie,
};

struct request_model_parse_options final {
    bool reject_unknown_fields_{false};
    http_status_code validation_error_status_{http_status::bad_request};
};

namespace detail {

using ::ruvia::request_model_parse_options;
using ::ruvia::validation_target;

[[noreturn]] void throw_invalid_json_content_type();
[[noreturn]] void throw_invalid_json_body();
[[noreturn]] void throw_invalid_form_content_type();

[[nodiscard]] inline constexpr std::string_view trim_http_ows(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

// Validates optional Content-Type parameters: if a `charset` parameter is
// present, only UTF-8 is accepted (RFC 8259 §8.1 / WHATWG URL form encoding).
[[nodiscard]] inline bool content_type_parameters_allow_utf8(std::string_view content_type) noexcept {
    const auto first_semi = content_type.find(';');
    if (first_semi == std::string_view::npos) {
        return true;
    }
    auto rest = content_type.substr(first_semi + 1);
    while (!rest.empty()) {
        std::size_t idx = 0;
        bool in_quotes = false;
        while (idx < rest.size()) {
            const char ch = rest[idx];
            if (in_quotes) {
                if (ch == '\\' && idx + 1 < rest.size()) {
                    idx += 2;
                    continue;
                }
                if (ch == '"') {
                    in_quotes = false;
                }
            } else if (ch == '"') {
                in_quotes = true;
            } else if (ch == ';') {
                break;
            }
            ++idx;
        }
        const auto param = trim_http_ows(rest.substr(0, idx));
        rest = idx < rest.size() ? rest.substr(idx + 1) : std::string_view{};
        if (param.empty()) {
            continue;
        }
        const auto eq = param.find('=');
        if (eq == std::string_view::npos) {
            return false;
        }
        const auto name = param.substr(0, eq);
        auto value = param.substr(eq + 1);
        if (!http_ascii_equals_ignore_case(name, "charset")) {
            continue;
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
            // Quoted-pair inside "utf-8" (e.g. \"u...) is rejected unless plain ASCII utf-8.
            if (value.find('\\') != std::string_view::npos) {
                return false;
            }
        }
        if (!http_ascii_equals_ignore_case(value, "utf-8")) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

// Matches `application/json` and structured syntax suffixes `application/*+json`
// (RFC 6838 §4.2.8), requiring valid Content-Type grammar and UTF-8 if charset is named.
[[nodiscard]] inline bool is_json_media_type(std::string_view content_type) noexcept {
    if (!is_valid_http_content_type_field_value(content_type) || !detail::content_type_parameters_allow_utf8(content_type)) {
        return false;
    }
    const auto media = http_media_type_only(content_type);
    if (http_ascii_equals_ignore_case(media, "application/json")) {
        return true;
    }
    constexpr std::string_view prefix = "application/";
    constexpr std::string_view suffix = "+json";
    return media.size() > prefix.size() + suffix.size() && http_ascii_equals_ignore_case(media.substr(0, prefix.size()), prefix) && http_ascii_equals_ignore_case(media.substr(media.size() - suffix.size()), suffix);
}

[[nodiscard]] inline bool is_form_urlencoded_media_type(std::string_view content_type) noexcept {
    if (!is_valid_http_content_type_field_value(content_type) || !detail::content_type_parameters_allow_utf8(content_type)) {
        return false;
    }
    return http_ascii_equals_ignore_case(http_media_type_only(content_type), "application/x-www-form-urlencoded");
}

namespace detail {

template <validation_target target>
[[nodiscard]] bool request_has_body_media_type(std::optional<std::string_view> content_type) noexcept {
    if (!content_type.has_value()) {
        return false;
    }
    if constexpr (target == validation_target::json) {
        return is_json_media_type(*content_type);
    } else if constexpr (target == validation_target::form) {
        return is_form_urlencoded_media_type(*content_type);
    } else {
        static_assert(always_false<std::integral_constant<validation_target, target>>, "unsupported body validation target");
    }
}

template <validation_target target>
[[noreturn]] void throw_invalid_content_type() {
    if constexpr (target == validation_target::json) {
        throw_invalid_json_content_type();
    } else if constexpr (target == validation_target::form) {
        throw_invalid_form_content_type();
    } else {
        static_assert(always_false<std::integral_constant<validation_target, target>>, "unsupported body validation target");
    }
}

template <typename t_type>
void validate_or_throw_request_model(const t_type& value, std::vector<validation_issue>&& parse_issues, http_status_code failure_status) {
    validator checks;
    if constexpr (model_validation_access::contains_model<t_type>()) {
        (void)model_validation_access::validate_value(value, checks);
    }
    checks.prepend_issues(std::move(parse_issues));
    if (!checks.ok()) {
        throw http_error({
            .status_ = failure_status,
            .code_ = "validation_failed",
            .message_ = "request validation failed",
            .validation_issues_ = checks.issues(),
        });
    }
}

template <validation_target target, typename body_type>
[[nodiscard]] body_type parse_and_validate_request_model(std::string_view body, request_model_parse_options options = {}, model_string_storage storage_mode = model_string_storage::borrowed) {
    if constexpr (target == validation_target::json) {
        static_assert(is_model_json_value<body_type>, "JSON request binding requires a RUVIA_MODEL, tagged_variant, array, json_value, or json_object");
        std::vector<validation_issue> issues;
        bool had_recorded_issues = false;
        auto parsed = parse_complete_json_value<body_type>(body, storage_mode, {.reject_unknown_fields_ = options.reject_unknown_fields_}, &issues, &had_recorded_issues);
        if (!parsed.has_value()) {
            if (!issues.empty()) {
                throw http_error({
                    .status_ = options.validation_error_status_,
                    .code_ = "validation_failed",
                    .message_ = "request validation failed",
                    .validation_issues_ = issues,
                });
            }
            throw_invalid_json_body();
        }
        validate_or_throw_request_model(*parsed, std::move(issues), options.validation_error_status_);
        return std::move(*parsed);
    } else if constexpr (target == validation_target::form) {
        static_assert(is_model<body_type>, "form request binding requires a RUVIA_MODEL");
        std::vector<validation_issue> issues;
        auto parsed = model_parse_access::parse_form<body_type>(form_body_input{.body_ = body, .string_storage_ = storage_mode}, &issues);
        validate_or_throw_request_model(parsed, std::move(issues), options.validation_error_status_);
        return parsed;
    } else {
        static_assert(always_false<std::integral_constant<validation_target, target>>, "unsupported body validation target");
    }
}

template <validation_target target, typename body_type>
[[nodiscard]] body_type parse_and_validate_request_model(std::span<const http_header_view> fields, request_model_parse_options options = {}) {
    static_assert(is_model<body_type>, "request parameter/header/cookie binding requires a RUVIA_MODEL");
    static_assert(target == validation_target::query || target == validation_target::param || target == validation_target::header || target == validation_target::cookie, "unsupported field validation target");
    std::vector<validation_issue> issues;
    constexpr bool ignore_case = target == validation_target::header;
    auto parsed = model_parse_access::parse_form<body_type>(request_fields_input{.fields_ = fields, .case_insensitive_names_ = ignore_case}, &issues);
    validate_or_throw_request_model(parsed, std::move(issues), options.validation_error_status_);
    return parsed;
}

template <validation_target target, typename body_type, bool reject_unknown_fields = false, std::uint16_t validation_status = 400>
class model_validation_middleware final {
    static_assert(validation_status >= 100 && validation_status <= 599, "HTTP validation error status code must be in 100..599");

public:
    using ruvia_validation_body_type = body_type;

    [[nodiscard]] task<void> invoke(context& c, next& next_value);
};

}  // namespace detail

template <typename body_type, bool reject_unknown_fields = false, std::uint16_t validation_status = 400>
using json_body = detail::model_validation_middleware<validation_target::json, body_type, reject_unknown_fields, validation_status>;

template <typename body_type, std::uint16_t validation_status = 400>
using strict_json_body = json_body<body_type, true, validation_status>;

template <typename body_type, std::uint16_t validation_status = 400>
using form_body = detail::model_validation_middleware<validation_target::form, body_type, false, validation_status>;

template <typename body_type, std::uint16_t validation_status = 400>
using query_model = detail::model_validation_middleware<validation_target::query, body_type, false, validation_status>;

template <typename body_type, std::uint16_t validation_status = 400>
using path_model = detail::model_validation_middleware<validation_target::param, body_type, false, validation_status>;

template <typename body_type, std::uint16_t validation_status = 400>
using header_model = detail::model_validation_middleware<validation_target::header, body_type, false, validation_status>;

template <typename body_type, std::uint16_t validation_status = 400>
using cookie_model = detail::model_validation_middleware<validation_target::cookie, body_type, false, validation_status>;

}  // namespace ruvia
