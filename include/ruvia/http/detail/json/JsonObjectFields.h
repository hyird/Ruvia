#pragma once

#include <concepts>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/json/JsonScanner.h"
#include "ruvia/http/detail/json/JsonString.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

template <typename Visitor>
[[nodiscard]] bool dispatchJsonObjectFieldVisitor(
    Visitor& visitor,
    std::string_view key,
    std::string_view value) {
    if constexpr (requires { { visitor(key, value) } -> std::convertible_to<bool>; }) {
        return static_cast<bool>(visitor(key, value));
    } else {
        visitor(key, value);
        return true;
    }
}

template <typename Visitor>
[[nodiscard]] bool visitJsonObjectFields(
    std::string_view body,
    std::pmr::memory_resource* resource,
    Visitor&& visitor) {
    auto input = body;
    if (!consumeJsonChar(input, '{')) {
        return false;
    }
    skipJsonWhitespace(input);
    if (!input.empty() && input.front() == '}') {
        return true;
    }

    auto& visitorRef = visitor;
    while (true) {
        std::string_view key;
        bool keyEscaped = false;
        if (!parseJsonStringRaw(input, key, keyEscaped) ||
            !consumeJsonChar(input, ':')) {
            return false;
        }

        const auto valueStart = input;
        if (!skipJsonValue(input)) {
            return false;
        }
        const auto consumed = valueStart.size() - input.size();
        const auto value = valueStart.substr(0, consumed);
        if (keyEscaped) {
            std::pmr::string decodedKey(pmrResourceOrDefault(resource));
            if (!decodeJsonString(key, decodedKey)) {
                return false;
            }
            if (!dispatchJsonObjectFieldVisitor(visitorRef, std::string_view(decodedKey), value)) {
                return true;
            }
        } else if (!dispatchJsonObjectFieldVisitor(visitorRef, key, value)) {
            return true;
        }

        skipJsonWhitespace(input);
        if (!input.empty() && input.front() == '}') {
            return true;
        }
        if (!consumeJsonChar(input, ',')) {
            return false;
        }
    }
}

}  // namespace ruvia::detail
