#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/json/JsonObjectFields.h"
#include "ruvia/web/detail/json/JsonSkip.h"
#include "ruvia/web/detail/model/parse/JsonParser.h"

namespace ruvia {

// JsonValue and JsonObject borrow their complete input body. An owning string
// passed to parse() must therefore outlive the parsed view; basic_string
// rvalues are rejected before a dangling view can be created.
class JsonValue final {
public:
    enum class Kind : unsigned char { kObject,
        kArray,
        kString,
        kNumber,
        kBoolean,
        kNull };

    [[nodiscard]] static std::optional<JsonValue> parse(
        std::string_view body, ModelParseOptions options = {}) noexcept {
        auto input = body;
        if (!detail::skipJsonValue(input)) {
            return std::nullopt;
        }
        detail::skipJsonWhitespace(input);
        if (!input.empty()) {
            return std::nullopt;
        }
        return JsonValue(
            detail::ResolvedPmrResourceTag{}, body, detail::pmrResourceOrDefault(options.resource));
    }

    template <typename Traits, typename Allocator>
    static std::optional<JsonValue> parse(
        std::basic_string<char, Traits, Allocator>&&, ModelParseOptions = {}) = delete;

    template <typename Traits, typename Allocator>
    static std::optional<JsonValue> parse(
        const std::basic_string<char, Traits, Allocator>&&, ModelParseOptions = {}) = delete;

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    [[nodiscard]] Kind kind() const noexcept {
        auto input = body_;
        detail::skipJsonWhitespace(input);
        if (input.empty()) {
            return Kind::kNull;
        }
        switch (input.front()) {
            case '{':
                return Kind::kObject;
            case '[':
                return Kind::kArray;
            case '"':
                return Kind::kString;
            case 't':
            case 'f':
                return Kind::kBoolean;
            case 'n':
                return Kind::kNull;
            default:
                return Kind::kNumber;
        }
    }

    [[nodiscard]] bool isObject() const noexcept {
        return kind() == Kind::kObject;
    }

    [[nodiscard]] bool isArray() const noexcept {
        return kind() == Kind::kArray;
    }

    [[nodiscard]] bool isString() const noexcept {
        return kind() == Kind::kString;
    }

    [[nodiscard]] bool isNumber() const noexcept {
        return kind() == Kind::kNumber;
    }

    [[nodiscard]] bool isBoolean() const noexcept {
        return kind() == Kind::kBoolean;
    }

    [[nodiscard]] bool isNull() const noexcept {
        return kind() == Kind::kNull;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const;

private:
    JsonValue() noexcept = default;

    JsonValue(detail::ResolvedPmrResourceTag, std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : body_(body),
          resource_(resource) {}

    std::string_view body_;
    std::pmr::memory_resource* resource_{nullptr};
};

class JsonObject final {
public:
    [[nodiscard]] static std::optional<JsonObject> parse(
        std::string_view body, ModelParseOptions options = {}) noexcept {
        detail::JsonScanner scanner(body);
        if (!scanner.consumeObject()) {
            return std::nullopt;
        }
        scanner.skipWhitespace();
        if (!scanner.empty()) {
            return std::nullopt;
        }
        return JsonObject(
            detail::ResolvedPmrResourceTag{}, body, detail::pmrResourceOrDefault(options.resource));
    }

    template <typename Traits, typename Allocator>
    static std::optional<JsonObject> parse(
        std::basic_string<char, Traits, Allocator>&&, ModelParseOptions = {}) = delete;

    template <typename Traits, typename Allocator>
    static std::optional<JsonObject> parse(
        const std::basic_string<char, Traits, Allocator>&&, ModelParseOptions = {}) = delete;

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        auto* const resource = resource_;
        std::optional<T> result;
        bool lastMatchFailed = false;
        const bool valid = detail::visitJsonObjectFields(detail::ResolvedPmrResourceTag{}, body_,
            resource, [&](std::string_view key, std::string_view valueView) {
                if (key != field) {
                    return true;
                }

                auto valueInput = valueView;
                auto value = detail::parseJsonValue<T>(valueInput, resource);
                detail::skipJsonWhitespace(valueInput);
                if (!value || !valueInput.empty()) {
                    result.reset();
                    lastMatchFailed = true;
                    return true;
                }
                result.emplace(std::move(*value));
                lastMatchFailed = false;
                return true;
            });

        if (!valid || lastMatchFailed) {
            return std::nullopt;
        }
        return result;
    }

private:
    friend class JsonValue;

    JsonObject() noexcept = default;

    JsonObject(detail::ResolvedPmrResourceTag, std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : body_(body),
          resource_(resource) {}

    std::string_view body_;
    std::pmr::memory_resource* resource_{nullptr};
};

template <typename T>
[[nodiscard]] inline std::optional<T> JsonValue::get(std::string_view field) const {
    if (!isObject()) {
        return std::nullopt;
    }
    return JsonObject(detail::ResolvedPmrResourceTag{}, body_, resource_).get<T>(field);
}

}  // namespace ruvia
