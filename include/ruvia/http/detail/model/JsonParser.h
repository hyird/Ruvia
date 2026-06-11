#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/http/JsonUtils.h"
#include "ruvia/http/detail/model/Traits.h"

// Internal JSON scanner/value parser layer for RUVIA_MODEL.

namespace ruvia::detail {

template <typename ValueT>
[[nodiscard]] std::size_t jsonSizeHintValue(const ValueT& value) {
    using T = std::remove_cvref_t<ValueT>;
    if constexpr (isRuviaScalar<T>) {
        return jsonSizeHintValue(value.value);
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? 4 : 5;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<std::size_t>(std::numeric_limits<T>::digits10) + 3;
    } else if constexpr (std::is_floating_point_v<T>) {
        return 32;
    } else if constexpr (isRuviaString<T>) {
        return jsonStringSizeHint(value.view());
    } else if constexpr (requires { value.ruviaJsonSizeHint(); }) {
        return value.ruviaJsonSizeHint();
    } else if constexpr (isRuviaArray<T> || isRuviaList<T>) {
        std::size_t size = 2;
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                ++size;
            }
            first = false;
            size += jsonSizeHintValue(item);
        }
        return size;
    } else {
        static_assert(alwaysFalse<T>, "RUVIA_MODEL field type is not JSON serializable");
    }
}

template <typename ValueT>
void appendJsonValue(std::pmr::string& output, const ValueT& value) {
    using T = std::remove_cvref_t<ValueT>;
    if constexpr (isRuviaScalar<T>) {
        appendJsonValue(output, value.value);
    } else if constexpr (std::is_same_v<T, bool>) {
        output.append(value ? "true" : "false");
    } else if constexpr (std::is_integral_v<T>) {
        char buffer[32]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        char buffer[64]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    } else if constexpr (isRuviaString<T>) {
        appendJsonString(output, value.view());
    } else if constexpr (requires { value.ruviaAppendJson(output); }) {
        value.ruviaAppendJson(output);
    } else if constexpr (isRuviaArray<T>) {
        output.push_back('[');
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                output.push_back(',');
            }
            first = false;
            appendJsonValue(output, item);
        }
        output.push_back(']');
    } else if constexpr (isRuviaList<T>) {
        output.push_back('[');
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                output.push_back(',');
            }
            first = false;
            appendJsonValue(output, item);
        }
        output.push_back(']');
    } else {
        static_assert(alwaysFalse<T>, "RUVIA_MODEL field type is not JSON serializable");
    }
}

inline void skipJsonWhitespace(std::string_view& input) noexcept {
    while (!input.empty()) {
        const char c = input.front();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return;
        }
        input.remove_prefix(1);
    }
}

[[nodiscard]] inline bool consumeJsonChar(std::string_view& input, char expected) noexcept {
    skipJsonWhitespace(input);
    if (input.empty() || input.front() != expected) {
        return false;
    }
    input.remove_prefix(1);
    return true;
}

[[nodiscard]] inline bool consumeJsonLiteral(std::string_view& input, std::string_view literal) noexcept {
    skipJsonWhitespace(input);
    if (!input.starts_with(literal)) {
        return false;
    }
    input.remove_prefix(literal.size());
    return true;
}

[[nodiscard]] inline int hexValue(char c) noexcept;

[[nodiscard]] inline bool parseJsonStringRaw(
    std::string_view& input,
    std::string_view& value,
    bool& escaped) noexcept {
    skipJsonWhitespace(input);
    if (input.empty() || input.front() != '"') {
        return false;
    }
    input.remove_prefix(1);

    escaped = false;
    const char* const begin = input.data();
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '\\') {
            escaped = true;
            if (i + 1 >= input.size()) {
                return false;
            }
            const char escape = input[i + 1];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
                escape == 'n' || escape == 'r' || escape == 't') {
                ++i;
                continue;
            }
            if (escape == 'u') {
                if (i + 5 >= input.size() || hexValue(input[i + 2]) < 0 || hexValue(input[i + 3]) < 0 ||
                    hexValue(input[i + 4]) < 0 || hexValue(input[i + 5]) < 0) {
                    return false;
                }
                i += 5;
                continue;
            }
            return false;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
        if (c == '"') {
            value = std::string_view(begin, i);
            input.remove_prefix(i + 1);
            return true;
        }
    }

    return false;
}

[[nodiscard]] inline bool parseJsonStringView(std::string_view& input, std::string_view& value) noexcept {
    bool escaped = false;
    if (!parseJsonStringRaw(input, value, escaped)) {
        return false;
    }
    return !escaped;
}

template <typename OutputT>
void appendUtf8(OutputT& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

[[nodiscard]] inline bool readJsonHex4(std::string_view input, std::uint32_t& value) noexcept {
    if (input.size() < 4) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto hex = hexValue(input[i]);
        if (hex < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(hex);
    }
    return true;
}

template <typename OutputT>
[[nodiscard]] bool decodeJsonString(std::string_view input, OutputT& output) {
    output.clear();
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c != '\\') {
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            output.push_back(c);
            continue;
        }

        if (i + 1 >= input.size()) {
            return false;
        }
        const char escape = input[++i];
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escape);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                std::uint32_t codePoint = 0;
                if (!readJsonHex4(input.substr(i + 1), codePoint)) {
                    return false;
                }
                i += 4;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (i + 6 >= input.size() || input[i + 1] != '\\' || input[i + 2] != 'u') {
                        return false;
                    }
                    std::uint32_t low = 0;
                    if (!readJsonHex4(input.substr(i + 3), low) || low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }
                    i += 6;
                    codePoint = 0x10000 + (((codePoint - 0xD800) << 10) | (low - 0xDC00));
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    return false;
                }
                appendUtf8(output, codePoint);
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool skipJsonString(std::string_view& input) noexcept {
    std::string_view ignored;
    bool escaped = false;
    return parseJsonStringRaw(input, ignored, escaped);
}

[[nodiscard]] inline bool skipJsonValue(std::string_view& input) noexcept;
[[nodiscard]] inline bool skipJsonValue(std::string_view& input, std::size_t depth) noexcept;

[[nodiscard]] inline std::size_t scanJsonNumberLength(std::string_view input) noexcept {
    skipJsonWhitespace(input);
    std::size_t index = 0;
    if (index < input.size() && input[index] == '-') {
        ++index;
    }
    if (index >= input.size()) {
        return 0;
    }
    if (input[index] == '0') {
        ++index;
        if (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            return 0;
        }
    } else if (input[index] >= '1' && input[index] <= '9') {
        do {
            ++index;
        } while (index < input.size() && input[index] >= '0' && input[index] <= '9');
    } else {
        return 0;
    }

    if (index < input.size() && input[index] == '.') {
        ++index;
        const auto fractionBegin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            ++index;
        }
        if (index == fractionBegin) {
            return 0;
        }
    }
    if (index < input.size() && (input[index] == 'e' || input[index] == 'E')) {
        ++index;
        if (index < input.size() && (input[index] == '+' || input[index] == '-')) {
            ++index;
        }
        const auto exponentBegin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            ++index;
        }
        if (index == exponentBegin) {
            return 0;
        }
    }
    return index;
}

[[nodiscard]] inline bool skipJsonNumber(std::string_view& input) noexcept {
    skipJsonWhitespace(input);
    const auto index = scanJsonNumberLength(input);
    if (index == 0) {
        return false;
    }
    input.remove_prefix(index);
    return true;
}

[[nodiscard]] inline bool skipJsonArray(std::string_view& input, std::size_t depth) noexcept {
    if (depth > kMaxJsonDepth) {
        return false;
    }
    if (!consumeJsonChar(input, '[')) {
        return false;
    }
    skipJsonWhitespace(input);
    if (!input.empty() && input.front() == ']') {
        input.remove_prefix(1);
        return true;
    }

    while (skipJsonValue(input, depth + 1)) {
        skipJsonWhitespace(input);
        if (!input.empty() && input.front() == ']') {
            input.remove_prefix(1);
            return true;
        }
        if (!consumeJsonChar(input, ',')) {
            return false;
        }
    }

    return false;
}

[[nodiscard]] inline bool skipJsonArray(std::string_view& input) noexcept {
    return skipJsonArray(input, 0);
}

[[nodiscard]] inline bool skipJsonObject(std::string_view& input, std::size_t depth) noexcept {
    if (depth > kMaxJsonDepth) {
        return false;
    }
    if (!consumeJsonChar(input, '{')) {
        return false;
    }
    skipJsonWhitespace(input);
    if (!input.empty() && input.front() == '}') {
        input.remove_prefix(1);
        return true;
    }

    while (skipJsonString(input)) {
        if (!consumeJsonChar(input, ':') || !skipJsonValue(input, depth + 1)) {
            return false;
        }
        skipJsonWhitespace(input);
        if (!input.empty() && input.front() == '}') {
            input.remove_prefix(1);
            return true;
        }
        if (!consumeJsonChar(input, ',')) {
            return false;
        }
    }

    return false;
}

[[nodiscard]] inline bool skipJsonObject(std::string_view& input) noexcept {
    return skipJsonObject(input, 0);
}

[[nodiscard]] inline bool skipJsonValue(std::string_view& input, std::size_t depth) noexcept {
    if (depth > kMaxJsonDepth) {
        return false;
    }
    skipJsonWhitespace(input);
    if (input.empty()) {
        return false;
    }

    switch (input.front()) {
        case '"':
            return skipJsonString(input);
        case '{':
            return skipJsonObject(input, depth + 1);
        case '[':
            return skipJsonArray(input, depth + 1);
        case 't':
            return consumeJsonLiteral(input, "true");
        case 'f':
            return consumeJsonLiteral(input, "false");
        case 'n':
            return consumeJsonLiteral(input, "null");
        default:
            return skipJsonNumber(input);
    }
}

[[nodiscard]] inline bool skipJsonValue(std::string_view& input) noexcept {
    return skipJsonValue(input, 0);
}

class JsonScanner final {
public:
    explicit JsonScanner(std::string_view input) noexcept : input_(input) {}

    [[nodiscard]] bool consumeObject() noexcept {
        return skipJsonObject(input_);
    }

    void skipWhitespace() noexcept {
        skipJsonWhitespace(input_);
    }

    [[nodiscard]] bool empty() const noexcept {
        return input_.empty();
    }

    [[nodiscard]] std::string_view remaining() const noexcept {
        return input_;
    }

private:
    std::string_view input_;
};

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(
    std::string_view& input,
    std::pmr::memory_resource* resource,
    std::size_t depth = 0);

template <typename VectorT>
[[nodiscard]] bool parseJsonArrayValue(
    std::string_view& input,
    VectorT& value,
    std::pmr::memory_resource* resource,
    std::size_t depth) {
    using ElementT = typename RuviaArrayTraits<std::remove_cvref_t<VectorT>>::value_type;

    if (depth > kMaxJsonDepth) {
        return false;
    }
    if (!consumeJsonChar(input, '[')) {
        return false;
    }

    value.clear();
    skipJsonWhitespace(input);
    if (!input.empty() && input.front() == ']') {
        input.remove_prefix(1);
        return true;
    }

    for (;;) {
        auto element = parseJsonValue<ElementT>(input, resource, depth + 1);
        if (!element) {
            return false;
        }
        value.emplace_back(std::move(*element));

        skipJsonWhitespace(input);
        if (!input.empty() && input.front() == ']') {
            input.remove_prefix(1);
            return true;
        }
        if (!consumeJsonChar(input, ',')) {
            return false;
        }
    }
}

template <typename ListT>
[[nodiscard]] bool parseJsonListValue(
    std::string_view& input,
    ListT& value,
    std::pmr::memory_resource* resource,
    std::size_t depth) {
    using ElementT = typename RuviaListTraits<std::remove_cvref_t<ListT>>::value_type;

    if (depth > kMaxJsonDepth) {
        return false;
    }
    if (!consumeJsonChar(input, '[')) {
        return false;
    }

    value.clear();
    skipJsonWhitespace(input);
    if (!input.empty() && input.front() == ']') {
        input.remove_prefix(1);
        return true;
    }

    for (;;) {
        auto element = parseJsonValue<ElementT>(input, resource, depth + 1);
        if (!element) {
            return false;
        }
        value.emplaceMove(std::move(*element));

        skipJsonWhitespace(input);
        if (!input.empty() && input.front() == ']') {
            input.remove_prefix(1);
            return true;
        }
        if (!consumeJsonChar(input, ',')) {
            return false;
        }
    }
}

template <typename T>
[[nodiscard]] bool parseJsonValue(
    std::string_view& input,
    T& value,
    std::pmr::memory_resource* resource,
    std::size_t depth = 0) {
    using FieldT = std::remove_cvref_t<T>;
    if (depth > kMaxJsonDepth) {
        return false;
    }
    if constexpr (isRuviaString<FieldT>) {
        std::string_view raw;
        bool escaped = false;
        if (!parseJsonStringRaw(input, raw, escaped)) {
            return false;
        }
        if (!escaped) {
            value.assignView(raw);
            return true;
        }
        return decodeJsonString(raw, value.resetOwned());
    } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
        return parseJsonStringView(input, value);
    } else if constexpr (isRuviaArray<FieldT>) {
        return parseJsonArrayValue(input, value, resource, depth);
    } else if constexpr (isRuviaList<FieldT>) {
        return parseJsonListValue(input, value, resource, depth);
    } else if constexpr (isRuviaScalar<FieldT>) {
        using ScalarT = typename RuviaScalarTraits<FieldT>::value_type;
        ScalarT parsed{};
        if constexpr (std::is_same_v<ScalarT, bool>) {
            if (consumeJsonLiteral(input, "true")) {
                value.value = true;
                return true;
            }
            if (consumeJsonLiteral(input, "false")) {
                value.value = false;
                return true;
            }
            return false;
        } else {
            skipJsonWhitespace(input);
            const auto length = scanJsonNumberLength(input);
            if (length == 0) {
                return false;
            }
            const auto number = input.substr(0, length);
            const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), parsed);
            if (ec != std::errc{} || ptr != number.data() + number.size()) {
                return false;
            }
            input.remove_prefix(length);
            value.value = parsed;
            return true;
        }
    } else if constexpr (std::is_same_v<FieldT, bool>) {
        if (consumeJsonLiteral(input, "true")) {
            value = true;
            return true;
        }
        if (consumeJsonLiteral(input, "false")) {
            value = false;
            return true;
        }
        return false;
    } else if constexpr (std::is_integral_v<FieldT>) {
        skipJsonWhitespace(input);
        const auto length = scanJsonNumberLength(input);
        if (length == 0) {
            return false;
        }
        const auto number = input.substr(0, length);
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc{} || ptr != number.data() + number.size()) {
            return false;
        }
        input.remove_prefix(length);
        return true;
    } else if constexpr (std::is_floating_point_v<FieldT>) {
        skipJsonWhitespace(input);
        const auto length = scanJsonNumberLength(input);
        if (length == 0) {
            return false;
        }
        const auto number = input.substr(0, length);
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc{} || ptr != number.data() + number.size()) {
            return false;
        }
        input.remove_prefix(length);
        return true;
    } else if constexpr (JsonBody<FieldT>::value) {
        std::string_view object = input;
        if (!skipJsonObject(input, depth + 1)) {
            return false;
        }
        object = object.substr(0, object.size() - input.size());
        if (auto nested = JsonBody<FieldT>::parseDepth(object, resource, depth + 1); nested) {
            value = std::move(*nested);
            return true;
        }
        return false;
    } else {
        static_assert(alwaysFalse<FieldT>, "RUVIA_MODEL JSON getter type is not supported");
    }
}

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(
    std::string_view& input,
    std::pmr::memory_resource* resource,
    std::size_t depth) {
    using FieldT = std::remove_cvref_t<T>;
    if (depth > kMaxJsonDepth) {
        return std::nullopt;
    }
    if constexpr (JsonBody<FieldT>::value) {
        std::string_view object = input;
        if (!skipJsonObject(input, depth + 1)) {
            return std::nullopt;
        }
        object = object.substr(0, object.size() - input.size());
        return JsonBody<FieldT>::parseDepth(object, resource, depth + 1);
    } else {
        FieldT value = makeRequestValue<FieldT>(resource);
        if (!parseJsonValue(input, value, resource, depth)) {
            return std::nullopt;
        }
        return value;
    }
}

[[nodiscard]] inline int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

}  // namespace ruvia::detail
