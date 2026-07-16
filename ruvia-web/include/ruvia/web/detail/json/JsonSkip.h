#pragma once

#include "ruvia/web/detail/json/JsonLex.h"
#include "ruvia/web/detail/json/JsonLimits.h"
#include "ruvia/web/detail/json/JsonNumber.h"
#include "ruvia/web/detail/json/JsonString.h"
#include "ruvia/web/detail/BorrowedView.h"

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline bool skipJsonValue(std::string_view& input) noexcept;
[[nodiscard]] inline bool skipJsonValue(std::string_view& input, std::size_t depth) noexcept;

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

    while (parseJsonString(input).has_value()) {
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
            return parseJsonString(input).has_value();
        case '{':
            // skipJsonObject/skipJsonArray already add depth+1 for their child values; passing
            // depth+1 here too would double-count and enforce half the documented kMaxJsonDepth.
            return skipJsonObject(input, depth);
        case '[':
            return skipJsonArray(input, depth);
        case 't':
            return consumeJsonLiteral(input, "true");
        case 'f':
            return consumeJsonLiteral(input, "false");
        case 'n':
            return consumeJsonLiteral(input, "null");
        default:
            return skipJsonNumberToken(input);
    }
}

[[nodiscard]] inline bool skipJsonValue(std::string_view& input) noexcept {
    return skipJsonValue(input, 0);
}

class JsonScanner final {
public:
    explicit JsonScanner(std::string_view input) noexcept : input_(input) {}

    template <RvalueCharBasicString Input>
    JsonScanner(Input&&) = delete;

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

}  // namespace ruvia::detail
