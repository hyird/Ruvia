#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {
struct http_structured_item {
    enum class kind_type { other,
        integer,
        boolean } kind_{kind_type::other};
    std::int64_t integer_{0};
    bool boolean_{false};
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
        while (!input_.empty() && (input_.peek() == ' ' || input_.peek() == '\t')) {
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
            while (!input_.empty()) {
                const auto ch = static_cast<unsigned char>(input_.peek());
                input_.advance();
                if (ch == '"') {
                    return true;
                }
                if (ch == '\\') {
                    if (input_.empty() || (input_.peek() != '\\' && input_.peek() != '"')) {
                        return false;
                    }
                    input_.advance();
                } else if (ch < 0x20 || ch > 0x7e) {
                    return false;
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
                } else if (padding || !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                          (ch >= '0' && ch <= '9') || ch == '+' || ch == '/')) {
                    return false;
                }
                ++count;
            }
            // RFC 8941 section 4.2.7 allows missing padding, including the
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
        const auto first = input_.peek();
        input_.advance();
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '*')) {
            return false;
        }
        constexpr std::string_view extra{"!#$%&'*+-.^_`|~:/"};
        while (!input_.empty()) {
            const auto ch = input_.peek();
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || extra.find(ch) != std::string_view::npos)) {
                break;
            }
            input_.advance();
        }
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
