#pragma once

#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/detail/coding/http_transfer_coding.h"
#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_content_coding.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http_valid_transfer_parameter_value(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return std::ranges::all_of(value,
            [](char byte) noexcept { return is_http_token_char(static_cast<unsigned char>(byte)); });
    }
    if (value.size() < 2 || value.back() != '"') {
        return false;
    }
    const auto end = value.size() - 1;
    for (std::size_t cursor_value = 1; cursor_value < end; ++cursor_value) {
        auto byte = static_cast<unsigned char>(value[cursor_value]);
        if (byte == '\\') {
            if (++cursor_value == end) {
                return false;
            }
            byte = static_cast<unsigned char>(value[cursor_value]);
            if (byte != '\t' && byte != ' ' && (byte < 0x21 || byte > 0x7e) && byte < 0x80) {
                return false;
            }
        } else if (byte == '"' ||
                   (byte != '\t' && byte != ' ' && byte != 0x21 && (byte < 0x23 || byte > 0x5b) &&
                       (byte < 0x5d || byte > 0x7e) && byte < 0x80)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool http_parse_transfer_coding_syntax(
    std::string_view item, std::string_view& coding, bool& has_parameters) noexcept {
    const auto first_semicolon = http_find_unquoted_delimiter(item, 0, ';');
    coding = http_trim_ows(item.substr(0, first_semicolon));
    if (coding.empty()) {
        return false;
    }
    for (const auto byte : coding) {
        if (!is_http_token_char(static_cast<unsigned char>(byte))) {
            return false;
        }
    }

    has_parameters = first_semicolon < item.size();
    auto start = first_semicolon;
    while (start < item.size()) {
        ++start;
        const auto end = http_find_unquoted_delimiter(item, start, ';');
        const auto parameter = http_trim_ows(item.substr(start, end - start));
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }
        const auto name = http_trim_ows(parameter.substr(0, equals));
        const auto value = http_trim_ows(parameter.substr(equals + 1));
        if (name.empty()) {
            return false;
        }
        for (const auto byte : name) {
            if (!is_http_token_char(static_cast<unsigned char>(byte))) {
                return false;
            }
        }
        if (!http_valid_transfer_parameter_value(value)) {
            return false;
        }
        start = end;
    }
    return true;
}

template <http_temporary_owning_char_string item_type>
bool http_parse_transfer_coding_syntax(item_type&&, std::string_view&, bool&) = delete;

enum class http_transfer_encoding_parse_status : std::uint8_t { ok,
    malformed,
    unsupported };

class http_non_chunked_transfer_encoding final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

private:
    friend class http_transfer_encoding_value;

    explicit http_non_chunked_transfer_encoding(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

class http_final_chunked_transfer_encoding final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

private:
    friend class http_transfer_encoding_value;

    explicit http_final_chunked_transfer_encoding(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

class http_transfer_encoding_value final {
public:
    [[nodiscard]] const http_non_chunked_transfer_encoding* non_chunked() const& noexcept {
        return std::get_if<http_non_chunked_transfer_encoding>(&value_);
    }
    const http_non_chunked_transfer_encoding* non_chunked() const&& = delete;

    [[nodiscard]] const http_final_chunked_transfer_encoding* final_chunked() const& noexcept {
        return std::get_if<http_final_chunked_transfer_encoding>(&value_);
    }
    const http_final_chunked_transfer_encoding* final_chunked() const&& = delete;

private:
    friend class http_transfer_encoding_state;

    using value_type = std::variant<http_non_chunked_transfer_encoding, http_final_chunked_transfer_encoding>;

    [[nodiscard]] static http_transfer_encoding_value make_non_chunked(
        http_transfer_codings transfer_codings) {
        return http_transfer_encoding_value(http_non_chunked_transfer_encoding(std::move(transfer_codings)));
    }

    [[nodiscard]] static http_transfer_encoding_value make_final_chunked(
        http_transfer_codings transfer_codings) {
        return http_transfer_encoding_value(http_final_chunked_transfer_encoding(std::move(transfer_codings)));
    }

    explicit http_transfer_encoding_value(http_non_chunked_transfer_encoding value)
        : value_(std::move(value)) {}

    explicit http_transfer_encoding_value(http_final_chunked_transfer_encoding value)
        : value_(std::move(value)) {}

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_transfer_encoding_value>);

// Field lines form one ordered list. Unknown but syntactically valid codings
// are remembered while later items/fields are still checked for malformed syntax.
class http_transfer_encoding_state final {
public:
    explicit http_transfer_encoding_state(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    [[nodiscard]] http_transfer_encoding_parse_status parse_field(std::string_view field_value) {
        // The published value is the sole owner of the committed coding list.
        http_transfer_codings next_codings(resource_);
        bool final_chunked = false;
        if (value_) {
            if (const auto* chunked = value_->final_chunked()) {
                next_codings = chunked->transfer_codings();
                final_chunked = true;
            } else {
                next_codings = value_->non_chunked()->transfer_codings();
            }
        }
        bool unsupported = unsupported_;
        bool saw_item = false;
        bool malformed = false;
        http_visit_comma_separated_quoted_items(field_value,
            [&next_codings, &final_chunked, &unsupported, &saw_item, &malformed](
                std::string_view item) {
                saw_item = true;
                std::string_view coding;
                bool has_parameters = false;
                if (final_chunked || !http_parse_transfer_coding_syntax(item, coding, has_parameters)) {
                    malformed = true;
                    return false;
                }
                if (http_ascii_equals_ignore_case(coding, "chunked")) {
                    if (has_parameters) {
                        malformed = true;
                        return false;
                    }
                    final_chunked = true;
                    return true;
                }
                const bool gzip = http_is_gzip_coding_token(coding);
                const bool deflate = http_ascii_equals_ignore_case(coding, "deflate");
                if (gzip || deflate) {
                    if (has_parameters) {
                        malformed = true;
                        return false;
                    }
                    if (next_codings.values_.size() == max_transfer_codings) {
                        unsupported = true;
                        return true;
                    }
                    next_codings.values_.push_back(gzip ? http_transfer_coding::gzip
                                                        : http_transfer_coding::deflate);
                    return true;
                }
                unsupported = true;
                return true;
            });
        if (malformed || !saw_item) {
            return http_transfer_encoding_parse_status::malformed;
        }
        auto next_value = final_chunked ? http_transfer_encoding_value::make_final_chunked(std::move(next_codings))
                                        : http_transfer_encoding_value::make_non_chunked(std::move(next_codings));
        // All allocations precede publication. Sequence move construction only
        // transfers its PMR buffer, including with MSVC iterator debugging.
        value_.emplace(std::move(next_value));
        unsupported_ = unsupported;
        return unsupported_ ? http_transfer_encoding_parse_status::unsupported
                            : http_transfer_encoding_parse_status::ok;
    }

    [[nodiscard]] const std::optional<http_transfer_encoding_value>& value() const noexcept {
        return value_;
    }
    [[nodiscard]] bool unsupported() const noexcept {
        return unsupported_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    std::pmr::memory_resource* resource_;
    std::optional<http_transfer_encoding_value> value_;
    bool unsupported_{false};
};

}  // namespace ruvia::detail
