#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "binary_text_encoding.h"
#include "borrowed_text.h"
#include "http_header.h"

// RFC 9651 Structured Field Values: a zero-allocation item / parameter /
// inner-list scanner over one field value or every line of a repeated field.
// Callers own the top-level list or dictionary walk.
namespace ruvia::detail {

struct http_structured_item {
    enum class kind_type { other,
        integer,
        boolean,
        token,
        string,
        inner_list } kind_{kind_type::other};
    std::int64_t integer_{0};
    bool boolean_{false};
    std::string_view text_{};
    bool has_escapes_{false};
};

struct http_structured_text_input {
    std::string_view remaining_;

    [[nodiscard]] bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining_.data();
    }
    void advance() noexcept {
        remaining_.remove_prefix(1);
    }
};

template <typename iterator_type>
class http_structured_header_input final {
public:
    http_structured_header_input(iterator_type first, iterator_type last, std::string_view field_name) noexcept :
        next_(first),
        last_(last),
        field_name_(field_name) {
        (void)select_next(remaining_);
        normalize();
    }
    [[nodiscard]] bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining_.data();
    }
    void advance() noexcept {
        remaining_.remove_prefix(1);
        normalize();
    }

private:
    [[nodiscard]] bool select_next(std::string_view& value) noexcept {
        while (next_ != last_) {
            const auto& header_value = *next_++;
            if (http_ascii_equals_ignore_case(header_value.name(), field_name_)) {
                value = header_value.value();
                return true;
            }
        }
        return false;
    }
    void normalize() noexcept {
        while (remaining_.empty()) {
            if (separator_) {
                separator_ = false;
                remaining_ = pending_;
            } else if (select_next(pending_)) {
                remaining_ = ", ";
                separator_ = true;
            } else {
                return;
            }
        }
    }

    iterator_type next_;
    iterator_type last_;
    std::string_view field_name_;
    std::string_view remaining_;
    std::string_view pending_;
    bool separator_{false};
};

template <typename input_type>
struct http_structured_parser {
    input_type input_;

    [[nodiscard]] bool empty() const noexcept {
        return input_.empty();
    }
    bool take(char ch) {
        if (!input_.empty() && input_.peek() == ch) {
            input_.advance();
            return true;
        }
        return false;
    }
    void spaces() {
        while (take(' ')) {
        }
    }
    void ows() {
        while (!input_.empty() && is_http_ows_char(input_.peek())) {
            input_.advance();
        }
    }
    std::string_view key() {
        if (input_.empty() || !((input_.peek() >= 'a' && input_.peek() <= 'z') || input_.peek() == '*')) {
            return {};
        }
        const auto* const start = input_.data();
        std::size_t length = 1;
        input_.advance();
        while (!input_.empty()) {
            const auto ch = input_.peek();
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '*')) {
                break;
            }
            ++length;
            input_.advance();
        }
        // Field-line boundaries inject commas, so a key always stays contiguous.
        return {start, length};
    }
    bool item(http_structured_item& value) {
        if (input_.empty()) {
            return false;
        }
        if (take('?')) {
            value.kind_ = http_structured_item::kind_type::boolean;
            if (take('1')) {
                value.boolean_ = true;
                return true;
            }
            return take('0');
        }
        if (take('"')) {
            const auto* const start = input_.data();
            std::size_t length = 0;
            bool has_escapes = false;
            while (!input_.empty()) {
                const auto ch = static_cast<unsigned char>(input_.peek());
                input_.advance();
                if (ch == '"') {
                    value.kind_ = http_structured_item::kind_type::string;
                    value.text_ = std::string_view(start, length);
                    value.has_escapes_ = has_escapes;
                    return true;
                }
                if (ch == '\\') {
                    if (input_.empty() || (input_.peek() != '\\' && input_.peek() != '"')) {
                        return false;
                    }
                    has_escapes = true;
                    input_.advance();
                    length += 2;
                } else if (ch < 0x20 || ch > 0x7e) {
                    return false;
                } else {
                    ++length;
                }
            }
            return false;
        }
        if (take(':')) {
            std::size_t count = 0, padding = 0;
            while (!input_.empty() && input_.peek() != ':') {
                const auto ch = input_.peek();
                input_.advance();
                if (ch == '=') {
                    if (++padding > 2) {
                        return false;
                    }
                } else if (padding || http_base64_decode_char(ch) < 0) {
                    return false;
                }
                ++count;
            }
            // RFC 9651 section 4.2.7 allows missing padding, including the
            // final '=' of a partially padded sequence. Validate the data
            // quartet and supplied padding as if missing padding were added.
            const auto data_remainder = (count - padding) % 4;
            return take(':') && data_remainder != 1 && padding <= (4 - data_remainder) % 4;
        }
        if (input_.peek() == '-' || (input_.peek() >= '0' && input_.peek() <= '9')) {
            const bool negative = take('-');
            std::size_t digits = 0;
            std::int64_t number = 0;
            while (!input_.empty() && input_.peek() >= '0' && input_.peek() <= '9') {
                if (++digits > 15) {
                    return false;
                }
                number = number * 10 + input_.peek() - '0';
                input_.advance();
            }
            if (!digits) {
                return false;
            }
            if (take('.')) {
                if (digits > 12) {
                    return false;
                }
                std::size_t fraction = 0;
                while (!input_.empty() && input_.peek() >= '0' && input_.peek() <= '9') {
                    input_.advance();
                    if (++fraction > 3) {
                        return false;
                    }
                }
                return fraction != 0;
            }
            value.kind_ = http_structured_item::kind_type::integer;
            value.integer_ = negative ? -number : number;
            return true;
        }
        const auto* const start = input_.data();
        const auto first = input_.peek();
        input_.advance();
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '*')) {
            return false;
        }
        std::size_t length = 1;
        // sf-token = ( ALPHA / "*" ) *( tchar / ":" / "/" )
        while (!input_.empty()) {
            const auto ch = input_.peek();
            if (!is_http_token_char(static_cast<unsigned char>(ch)) && ch != ':' && ch != '/') {
                break;
            }
            ++length;
            input_.advance();
        }
        value.kind_ = http_structured_item::kind_type::token;
        value.text_ = std::string_view(start, length);
        return true;
    }
    bool parameters() {
        while (take(';')) {
            spaces();
            if (key().empty()) {
                return false;
            }
            if (take('=')) {
                http_structured_item ignored;
                if (!item(ignored)) {
                    return false;
                }
            }
        }
        return true;
    }
    bool member(http_structured_item& value) {
        if (!take('(')) {
            return item(value) && parameters();
        }
        value.kind_ = http_structured_item::kind_type::inner_list;
        spaces();
        while (!take(')')) {
            http_structured_item ignored;
            if (!item(ignored) || !parameters()) {
                return false;
            }
            if (input_.empty() || (input_.peek() != ' ' && input_.peek() != ')')) {
                return false;
            }
            spaces();
        }
        return parameters();
    }
};

}  // namespace ruvia::detail
