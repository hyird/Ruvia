#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {
struct HttpStructuredItem {
    enum class Kind { kOther,
        kInteger,
        kBoolean } kind{Kind::kOther};
    std::int64_t integer{0};
    bool boolean{false};
};
struct http_structured_text_input {
    std::string_view remaining;

    [[nodiscard]] bool empty() const noexcept {
        return remaining.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining.data();
    }
    void advance() noexcept {
        remaining.remove_prefix(1);
    }
};

template <typename input_type>
struct HttpStructuredParser {
    input_type input;

    [[nodiscard]] bool empty() const noexcept {
        return input.empty();
    }
    bool take(char ch) {
        if (!input.empty() && input.peek() == ch) {
            input.advance();
            return true;
        }
        return false;
    }
    void spaces() {
        while (take(' ')) {
        }
    }
    void ows() {
        while (!input.empty() && (input.peek() == ' ' || input.peek() == '\t')) {
            input.advance();
        }
    }
    std::string_view key() {
        if (input.empty() || !((input.peek() >= 'a' && input.peek() <= 'z') || input.peek() == '*')) {
            return {};
        }
        const auto* const start = input.data();
        std::size_t length = 1;
        input.advance();
        while (!input.empty()) {
            const auto ch = input.peek();
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '*')) {
                break;
            }
            ++length;
            input.advance();
        }
        // Field-line boundaries inject commas, so a key always stays contiguous.
        return {start, length};
    }
    bool item(HttpStructuredItem& value) {
        if (input.empty()) {
            return false;
        }
        if (take('?')) {
            value.kind = HttpStructuredItem::Kind::kBoolean;
            if (take('1')) {
                value.boolean = true;
                return true;
            }
            return take('0');
        }
        if (take('"')) {
            while (!input.empty()) {
                const auto ch = static_cast<unsigned char>(input.peek());
                input.advance();
                if (ch == '"') {
                    return true;
                }
                if (ch == '\\') {
                    if (input.empty() || (input.peek() != '\\' && input.peek() != '"')) {
                        return false;
                    }
                    input.advance();
                } else if (ch < 0x20 || ch > 0x7e) {
                    return false;
                }
            }
            return false;
        }
        if (take(':')) {
            std::size_t count = 0, padding = 0;
            while (!input.empty() && input.peek() != ':') {
                const auto ch = input.peek();
                input.advance();
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
        if (input.peek() == '-' || (input.peek() >= '0' && input.peek() <= '9')) {
            const bool negative = take('-');
            std::size_t digits = 0;
            std::int64_t number = 0;
            while (!input.empty() && input.peek() >= '0' && input.peek() <= '9') {
                if (++digits > 15) {
                    return false;
                }
                number = number * 10 + input.peek() - '0';
                input.advance();
            }
            if (!digits) {
                return false;
            }
            if (take('.')) {
                if (digits > 12) {
                    return false;
                }
                std::size_t fraction = 0;
                while (!input.empty() && input.peek() >= '0' && input.peek() <= '9') {
                    input.advance();
                    if (++fraction > 3) {
                        return false;
                    }
                }
                return fraction != 0;
            }
            value.kind = HttpStructuredItem::Kind::kInteger;
            value.integer = negative ? -number : number;
            return true;
        }
        const auto first = input.peek();
        input.advance();
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '*')) {
            return false;
        }
        constexpr std::string_view extra{"!#$%&'*+-.^_`|~:/"};
        while (!input.empty()) {
            const auto ch = input.peek();
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || extra.find(ch) != std::string_view::npos)) {
                break;
            }
            input.advance();
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
                HttpStructuredItem ignored;
                if (!item(ignored)) {
                    return false;
                }
            }
        }
        return true;
    }
    bool member(HttpStructuredItem& value) {
        if (!take('(')) {
            return item(value) && parameters();
        }
        spaces();
        while (!take(')')) {
            HttpStructuredItem ignored;
            if (!item(ignored) || !parameters()) {
                return false;
            }
            if (input.empty() || (input.peek() != ' ' && input.peek() != ')')) {
                return false;
            }
            spaces();
        }
        return parameters();
    }
};
}  // namespace ruvia::detail
