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
struct HttpStructuredParser {
    std::string_view text;
    std::size_t at{0};
    bool take(char ch) {
        if (at < text.size() && text[at] == ch) {
            ++at;
            return true;
        }
        return false;
    }
    void spaces() {
        while (take(' ')) {
        }
    }
    void ows() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
            ++at;
        }
    }
    std::string_view key() {
        auto start = at;
        if (at == text.size() || !((text[at] >= 'a' && text[at] <= 'z') || text[at] == '*')) {
            return {};
        }
        ++at;
        while (at < text.size()) {
            const auto ch = text[at];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '*')) {
                break;
            }
            ++at;
        }
        return text.substr(start, at - start);
    }
    bool item(HttpStructuredItem& value) {
        if (at == text.size()) {
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
            while (at < text.size()) {
                auto ch = static_cast<unsigned char>(text[at++]);
                if (ch == '"') {
                    return true;
                }
                if (ch == '\\') {
                    if (at == text.size() || (text[at] != '\\' && text[at] != '"')) {
                        return false;
                    }
                    ++at;
                } else if (ch < 0x20 || ch > 0x7e) {
                    return false;
                }
            }
            return false;
        }
        if (take(':')) {
            std::size_t count = 0, padding = 0;
            while (at < text.size() && text[at] != ':') {
                const auto ch = text[at++];
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
        if (text[at] == '-' || (text[at] >= '0' && text[at] <= '9')) {
            const bool negative = take('-');
            std::size_t digits = 0;
            std::int64_t number = 0;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
                if (++digits > 15) {
                    return false;
                }
                number = number * 10 + text[at++] - '0';
            }
            if (!digits) {
                return false;
            }
            if (take('.')) {
                if (digits > 12) {
                    return false;
                }
                std::size_t fraction = 0;
                while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
                    ++at;
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
        const auto start = at;
        const auto first = text[at++];
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '*')) {
            return false;
        }
        constexpr std::string_view extra{"!#$%&'*+-.^_`|~:/"};
        while (at < text.size()) {
            const auto ch = text[at];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || extra.find(ch) != std::string_view::npos)) {
                break;
            }
            ++at;
        }
        return at > start;
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
            if (at == text.size() || (text[at] != ' ' && text[at] != ')')) {
                return false;
            }
            spaces();
        }
        return parameters();
    }
};
}  // namespace ruvia::detail
