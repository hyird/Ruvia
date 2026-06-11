#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/model/Parser.h"

namespace ruvia::detail {

template <typename Visitor>
[[nodiscard]] bool visitRequestJsonFields(const RequestObject& body, Visitor&& visitor) {
    auto input = body.view();
    if (!consumeJsonChar(input, '{')) {
        return false;
    }
    skipJsonWhitespace(input);
    if (!input.empty() && input.front() == '}') {
        return true;
    }

    while (true) {
        std::string_view key;
        bool keyEscaped = false;
        if (!parseJsonStringRaw(input, key, keyEscaped) ||
            !consumeJsonChar(input, ':')) {
            return false;
        }

        std::pmr::string decodedKey(body.resource());
        auto field = key;
        if (keyEscaped) {
            if (!decodeJsonString(key, decodedKey)) {
                return false;
            }
            field = decodedKey;
        }

        const auto valueStart = input;
        if (!skipJsonValue(input)) {
            return false;
        }
        const auto consumed = valueStart.size() - input.size();
        std::forward<Visitor>(visitor)(field, valueStart.substr(0, consumed));

        skipJsonWhitespace(input);
        if (!input.empty() && input.front() == '}') {
            return true;
        }
        if (!consumeJsonChar(input, ',')) {
            return false;
        }
    }
}

template <typename Visitor>
[[nodiscard]] bool visitRequestFormFields(const RequestObject& body, Visitor&& visitor) {
    auto input = body.view();
    while (!input.empty()) {
        const auto pairEnd = input.find('&');
        const auto pair = pairEnd == std::string_view::npos ? input : input.substr(0, pairEnd);
        const auto equals = pair.find('=');
        const auto name = equals == std::string_view::npos ? pair : pair.substr(0, equals);
        const auto value = equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);

        if (!hasFormEncoding(name)) {
            std::forward<Visitor>(visitor)(name, value);
        } else {
            std::pmr::string decodedName(body.resource());
            if (!decodeFormComponent(name, decodedName)) {
                return false;
            }
            std::forward<Visitor>(visitor)(std::string_view(decodedName), value);
        }

        if (pairEnd == std::string_view::npos) {
            return true;
        }
        input.remove_prefix(pairEnd + 1);
    }
    return true;
}

}  // namespace ruvia::detail
