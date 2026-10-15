#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include "ruvia/http/detail/field/header_token_utils.h"

namespace ruvia::detail {

enum class http_content_length_parse_status : std::uint8_t { ok,
    invalid,
    conflicting };

// Incremental Content-Length accumulator. Accepts comma-combined values with
// OWS and commits only when every repeated value agrees.
class http_content_length_state final {
public:
    [[nodiscard]] http_content_length_parse_status parse_field(std::string_view field_value) noexcept {
        auto status = http_content_length_parse_status::ok;
        bool saw_value = false;
        auto parsed_value = value_;
        http_visit_comma_separated_quoted_field_items(field_value, [&parsed_value, &status, &saw_value](
                                                                       std::string_view item) noexcept {
            status = accumulate(item, parsed_value);
            saw_value = true;
            return status == http_content_length_parse_status::ok;
        });
        if (status == http_content_length_parse_status::ok && !saw_value) {
            return http_content_length_parse_status::invalid;
        }
        if (status == http_content_length_parse_status::ok) {
            value_ = parsed_value;
        }
        return status;
    }

    [[nodiscard]] http_content_length_parse_status parse_single_value(std::string_view input) noexcept {
        auto parsed = value_;
        const auto status = accumulate(input, parsed);
        if (status == http_content_length_parse_status::ok) {
            value_ = parsed;
        }
        return status;
    }

    [[nodiscard]] std::optional<std::size_t> value() const noexcept {
        return value_;
    }

private:
    [[nodiscard]] static http_content_length_parse_status accumulate(
        std::string_view item, std::optional<std::size_t>& value) noexcept {
        if (item.empty()) {
            return http_content_length_parse_status::invalid;
        }
        std::size_t parsed_value = 0;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed_value);
        if (ec != std::errc{} || end != item.data() + item.size()) {
            return http_content_length_parse_status::invalid;
        }
        if (value.has_value() && *value != parsed_value) {
            return http_content_length_parse_status::conflicting;
        }
        value = parsed_value;
        return http_content_length_parse_status::ok;
    }

    std::optional<std::size_t> value_;
};

}  // namespace ruvia::detail
