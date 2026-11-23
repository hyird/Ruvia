#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "binary_text_encoding.h"
#include "borrowed_text.h"
#include "byte_view.h"
#include "http_header.h"

// RFC 9651 Structured Field Values: item / parameter / inner-list scanner.
// Contiguous tokens and unescaped strings borrow input; strings spanning field
// lines own their text; copy-on-unescape allocates only when escapes exist.
// Callers own the top-level list or dictionary walk.
namespace ruvia {

// Exact RFC 9651 §3.3.2 decimal: signed fixed-point scaled by 1000 (up to 12
// integer digits and 3 fractional digits).
struct http_structured_decimal final {
    std::int64_t milli_value_{0};

    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(milli_value_) / 1000.0;
    }

    [[nodiscard]] friend constexpr bool operator==(const http_structured_decimal&, const http_structured_decimal&) noexcept = default;
};

struct http_structured_item final {
    enum class kind_type : std::uint8_t {
        other,
        integer,
        decimal,
        boolean,
        token,
        string,
        byte_sequence,
        inner_list,
    } kind_{kind_type::other};
    std::int64_t integer_{0};
    http_structured_decimal decimal_{};
    bool boolean_{false};
    std::variant<std::string_view, std::string> text_{std::string_view{}};
    std::vector<std::byte> bytes_{};
    bool has_escapes_{false};

    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&text_)) {
            return *borrowed;
        }
        return std::get<std::string>(text_);
    }
    std::string_view text() const&& = delete;

    [[nodiscard]] std::string take_text() && {
        if (auto* owned = std::get_if<std::string>(&text_)) {
            auto result = std::move(*owned);
            text_ = std::string_view{};
            return result;
        }
        std::string copied(std::get<std::string_view>(text_));
        text_ = std::string_view{};
        return copied;
    }

    // Copy-on-unescape for sf-string: borrows text() directly when no backslash
    // escapes are present, otherwise unescapes into caller-supplied `storage`.
    [[nodiscard]] std::string_view unescape(std::string& storage RUVIA_LIFETIMEBOUND) const& RUVIA_LIFETIMEBOUND {
        const auto raw = text();
        if (!has_escapes_) {
            return raw;
        }
        storage.clear();
        storage.reserve(raw.size());
        for (std::size_t index = 0; index < raw.size(); ++index) {
            if (raw[index] == '\\' && index + 1 < raw.size()) {
                ++index;
            }
            storage.push_back(raw[index]);
        }
        return storage;
    }
    std::string_view unescape(std::string&) const&& = delete;

    [[nodiscard]] std::string unescaped_string() const {
        std::string storage;
        const auto view = unescape(storage);
        return has_escapes_ ? std::move(storage) : std::string(view);
    }

    [[nodiscard]] byte_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return byte_view(bytes_);
    }
    byte_view bytes() const&& = delete;

    [[nodiscard]] std::vector<std::byte> take_bytes() && noexcept {
        return std::exchange(bytes_, {});
    }
};

struct http_structured_text_input final {
    std::string_view remaining_{};

    constexpr http_structured_text_input() noexcept = default;
    explicit constexpr http_structured_text_input(borrowed_text remaining RUVIA_LIFETIMEBOUND) noexcept :
        remaining_(remaining.view()) {}

    [[nodiscard]] constexpr bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] constexpr char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] constexpr const char* data() const noexcept {
        return remaining_.data();
    }
    [[nodiscard]] constexpr std::size_t contiguous_size() const noexcept {
        return remaining_.size();
    }
    constexpr void advance() noexcept {
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
    [[nodiscard]] std::size_t contiguous_size() const noexcept {
        return remaining_.size();
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
struct http_structured_parser final {
    input_type input_;

    [[nodiscard]] bool empty() const noexcept {
        return input_.empty();
    }
    bool consume(char ch) {
        if (!input_.empty() && input_.peek() == ch) {
            input_.advance();
            return true;
        }
        return false;
    }
    void spaces() {
        while (consume(' ')) {
        }
    }
    void ows() {
        while (!input_.empty() && is_http_ows_char(input_.peek())) {
            input_.advance();
        }
    }
    [[nodiscard]] std::string_view key() RUVIA_LIFETIMEBOUND {
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

    // Text is retained as raw Structured Fields spelling (escapes included).
    // Passing false validates and classifies without retaining text or bytes.
    bool item(http_structured_item& value, bool retain_text = true) {
        if (input_.empty()) {
            return false;
        }
        value = {};
        if (consume('?')) {
            value.kind_ = http_structured_item::kind_type::boolean;
            if (consume('1')) {
                value.boolean_ = true;
                return true;
            }
            return consume('0');
        }
        if (consume('"')) {
            const char* chunk = input_.data();
            std::size_t length = 0;
            std::optional<std::string> fragmented;
            const auto advance_text = [&] {
                if (!retain_text) {
                    input_.advance();
                    return;
                }
                const bool boundary = input_.contiguous_size() == 1;
                ++length;
                input_.advance();
                if (boundary && !input_.empty()) {
                    if (!fragmented) {
                        fragmented.emplace();
                    }
                    fragmented->append(chunk, length);
                    chunk = input_.data();
                    length = 0;
                }
            };
            while (!input_.empty()) {
                const auto ch = static_cast<unsigned char>(input_.peek());
                if (ch == '"') {
                    value.kind_ = http_structured_item::kind_type::string;
                    if (fragmented) {
                        fragmented->append(chunk, length);
                        value.text_ = std::move(*fragmented);
                    } else if (retain_text) {
                        value.text_ = std::string_view(chunk, length);
                    }
                    input_.advance();
                    return true;
                }
                if (ch == '\\') {
                    advance_text();
                    if (input_.empty() || (input_.peek() != '\\' && input_.peek() != '"')) {
                        return false;
                    }
                    value.has_escapes_ = true;
                    advance_text();
                } else if (ch < 0x20 || ch > 0x7e) {
                    return false;
                } else {
                    advance_text();
                }
            }
            return false;
        }
        if (consume(':')) {
            std::size_t data_count = 0;
            std::size_t padding = 0;
            std::array<int, 4> quartet{};
            std::vector<std::byte> decoded;
            while (!input_.empty() && input_.peek() != ':') {
                const auto ch = input_.peek();
                input_.advance();
                if (ch == '=') {
                    if (++padding > 2) {
                        return false;
                    }
                } else {
                    const int sextet = base64_decode_char(ch);
                    if (padding != 0 || sextet < 0) {
                        return false;
                    }
                    if (retain_text) {
                        const auto slot = data_count % 4;
                        quartet[slot] = sextet;
                        if (slot == 3) {
                            decoded.push_back(static_cast<std::byte>((quartet[0] << 2) | (quartet[1] >> 4)));
                            decoded.push_back(static_cast<std::byte>((quartet[1] << 4) | (quartet[2] >> 2)));
                            decoded.push_back(static_cast<std::byte>((quartet[2] << 6) | quartet[3]));
                        }
                    }
                    ++data_count;
                }
            }
            // RFC 9651 §4.2.7 permits missing/partial '=' padding and ignores
            // non-zero unused bits in the final quantum.
            const auto data_remainder = data_count % 4;
            if (!consume(':') || data_remainder == 1 || padding > (4 - data_remainder) % 4) {
                return false;
            }
            if (retain_text) {
                if (data_remainder >= 2) {
                    decoded.push_back(static_cast<std::byte>((quartet[0] << 2) | (quartet[1] >> 4)));
                }
                if (data_remainder == 3) {
                    decoded.push_back(static_cast<std::byte>((quartet[1] << 4) | (quartet[2] >> 2)));
                }
                value.bytes_ = std::move(decoded);
            }
            value.kind_ = http_structured_item::kind_type::byte_sequence;
            return true;
        }
        if (input_.peek() == '-' || (input_.peek() >= '0' && input_.peek() <= '9')) {
            const bool negative = consume('-');
            std::size_t digits = 0;
            std::int64_t number = 0;
            while (!input_.empty() && input_.peek() >= '0' && input_.peek() <= '9') {
                if (++digits > 15) {
                    return false;
                }
                number = number * 10 + (input_.peek() - '0');
                input_.advance();
            }
            if (digits == 0) {
                return false;
            }
            if (consume('.')) {
                if (digits > 12) {
                    return false;
                }
                std::size_t fraction_digits = 0;
                std::int64_t fraction = 0;
                while (!input_.empty() && input_.peek() >= '0' && input_.peek() <= '9') {
                    if (++fraction_digits > 3) {
                        return false;
                    }
                    fraction = fraction * 10 + (input_.peek() - '0');
                    input_.advance();
                }
                if (fraction_digits == 0) {
                    return false;
                }
                while (fraction_digits++ < 3) {
                    fraction *= 10;
                }
                const auto scaled = number * 1000 + fraction;
                value.kind_ = http_structured_item::kind_type::decimal;
                value.decimal_ = http_structured_decimal{negative ? -scaled : scaled};
                return true;
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
        if (retain_text) {
            value.text_ = std::string_view(start, length);
        }
        return true;
    }

    bool parameters() {
        while (consume(';')) {
            spaces();
            if (key().empty()) {
                return false;
            }
            if (consume('=')) {
                http_structured_item ignored;
                if (!item(ignored, false)) {
                    return false;
                }
            }
        }
        return true;
    }

    template <typename visitor_type>
        requires std::invocable<visitor_type&, std::string_view, const http_structured_item&>
    bool parameters(visitor_type&& visitor, bool retain_text = true) {
        while (consume(';')) {
            spaces();
            const auto param_key = key();
            if (param_key.empty()) {
                return false;
            }
            http_structured_item param_value{.kind_ = http_structured_item::kind_type::boolean, .boolean_ = true};
            if (consume('=')) {
                if (!item(param_value, retain_text)) {
                    return false;
                }
            }
            if constexpr (std::convertible_to<std::invoke_result_t<visitor_type&, std::string_view, const http_structured_item&>, bool>) {
                if (!static_cast<bool>(std::invoke(visitor, param_key, param_value))) {
                    return false;
                }
            } else {
                std::invoke(visitor, param_key, param_value);
            }
        }
        return true;
    }

    template <typename element_visitor_type, typename param_visitor_type>
        requires(std::invocable<element_visitor_type&, const http_structured_item&> || std::invocable<element_visitor_type&, const http_structured_item&, http_structured_parser&>) && std::invocable<param_visitor_type&, std::string_view, const http_structured_item&>
    bool inner_list(element_visitor_type&& on_element, param_visitor_type&& on_list_param, bool retain_text = true) {
        if (!consume('(')) {
            return false;
        }
        spaces();
        while (!consume(')')) {
            http_structured_item element;
            if (!item(element, retain_text)) {
                return false;
            }
            if constexpr (std::invocable<element_visitor_type&, const http_structured_item&, http_structured_parser&>) {
                if constexpr (std::convertible_to<std::invoke_result_t<element_visitor_type&, const http_structured_item&, http_structured_parser&>, bool>) {
                    if (!static_cast<bool>(std::invoke(on_element, element, *this))) {
                        return false;
                    }
                } else {
                    std::invoke(on_element, element, *this);
                }
            } else {
                if (!parameters()) {
                    return false;
                }
                if constexpr (std::convertible_to<std::invoke_result_t<element_visitor_type&, const http_structured_item&>, bool>) {
                    if (!static_cast<bool>(std::invoke(on_element, element))) {
                        return false;
                    }
                } else {
                    std::invoke(on_element, element);
                }
            }
            if (input_.empty() || (input_.peek() != ' ' && input_.peek() != ')')) {
                return false;
            }
            spaces();
        }
        return parameters(std::forward<param_visitor_type>(on_list_param), retain_text);
    }

    template <typename element_visitor_type>
        requires(std::invocable<element_visitor_type&, const http_structured_item&> || std::invocable<element_visitor_type&, const http_structured_item&, http_structured_parser&>)
    bool inner_list(element_visitor_type&& on_element, bool retain_text = true) {
        return inner_list(
            std::forward<element_visitor_type>(on_element),
            [](std::string_view, const http_structured_item&) noexcept {},
            retain_text);
    }

    bool member(http_structured_item& value, bool retain_text = true) {
        if (!consume('(')) {
            return item(value, retain_text) && parameters();
        }
        value = {};
        value.kind_ = http_structured_item::kind_type::inner_list;
        spaces();
        while (!consume(')')) {
            http_structured_item ignored;
            if (!item(ignored, false) || !parameters()) {
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

}  // namespace ruvia
