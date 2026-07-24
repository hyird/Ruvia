#pragma once

#include <concepts>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/json/JsonScanner.h"
#include "ruvia/web/detail/json/JsonString.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

template <typename Visitor>
[[nodiscard]] bool dispatchJsonObjectFieldVisitor(Visitor& visitor, std::string_view key, std::string_view value) {
    if constexpr (requires {
                      { visitor(key, value) } -> std::convertible_to<bool>;
                  }) {
        return static_cast<bool>(visitor(key, value));
    } else {
        visitor(key, value);
        return true;
    }
}

template <typename Visitor>
[[nodiscard]] bool visitJsonObjectFields(ResolvedPmrResourceTag, std::string_view body, std::pmr::memory_resource* resource, Visitor&& visitor) {
    auto input = body;
    if (!consumeJsonChar(input, '{')) {
        return false;
    }
    skipJsonWhitespace(input);
    if (!input.empty() && input.front() == '}') {
        input.remove_prefix(1);
        skipJsonWhitespace(input);
        return input.empty();
    }

    auto& visitorRef = visitor;
    while (true) {
        const auto key = parseJsonString(input);
        if (!key.has_value() || !consumeJsonChar(input, ':')) {
            return false;
        }

        const auto valueStart = input;
        if (!skipJsonValue(input)) {
            return false;
        }
        const auto consumed = valueStart.size() - input.size();
        const auto value = valueStart.substr(0, consumed);
        if (key->encoding() == JsonStringEncoding::kEscaped) {
            auto decodedKey = decodeJsonString(key->raw(), resource);
            if (!decodedKey.has_value()) {
                return false;
            }
            if (!dispatchJsonObjectFieldVisitor(visitorRef, std::string_view(*decodedKey), value)) {
                return true;
            }
        } else if (!dispatchJsonObjectFieldVisitor(visitorRef, key->raw(), value)) {
            return true;
        }

        skipJsonWhitespace(input);
        if (!input.empty() && input.front() == '}') {
            input.remove_prefix(1);
            skipJsonWhitespace(input);
            return input.empty();
        }
        if (!consumeJsonChar(input, ',')) {
            return false;
        }
    }
}

template <typename Visitor>
[[nodiscard]] bool visitJsonObjectFields(std::string_view body, std::pmr::memory_resource* resource, Visitor&& visitor) {
    return visitJsonObjectFields(ResolvedPmrResourceTag{}, body, pmrResourceOrDefault(resource), std::forward<Visitor>(visitor));
}

}  // namespace ruvia::detail
