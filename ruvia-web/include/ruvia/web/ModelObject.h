#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/json/JsonObjectFields.h"
#include "ruvia/web/detail/json/JsonSkip.h"
#include "ruvia/web/detail/model/parse/JsonParser.h"
#include "ruvia/web/detail/model/parse/JsonWriter.h"

namespace ruvia {
class JsonValue;
class JsonObject;
}  // namespace ruvia

namespace ruvia::detail {

template <typename ViewT>
[[nodiscard]] std::optional<ViewT> parseJsonViewValue(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, ModelStringStorage stringStorage,
    bool requireObject);

}

namespace ruvia {

// JsonValue and JsonObject borrow their complete input body by default. An
// owning string passed to parse() must therefore outlive the parsed view;
// basic_string rvalues are rejected before a dangling view can be created.
// Model fields may copy the JSON token when the parse owns its strings.
// Dynamic JSON token, not an ordinary field-validation schema. parse() borrows
// the complete token; fromJson<Model>() owns it in the model resource instead.
// Move construction preserves borrowing/ownership. Move assignment keeps the
// target resource and owns the result (copying borrowed/incompatible storage).
// Views and potentially borrowed get<T>() results must not outlive their token.
class JsonValue final {
public:
    enum class Kind : unsigned char { kObject,
        kArray,
        kString,
        kNumber,
        kBoolean,
        kNull };

    explicit JsonValue(ModelOptions options = {})
        : JsonValue(detail::ResolvedPmrResourceTag{}, {},
              detail::pmrResourceOrDefault(options.resource)) {}

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

    JsonValue(const JsonValue&) = delete;
    JsonValue& operator=(const JsonValue&) = delete;

    JsonValue(JsonValue&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    JsonValue& operator=(JsonValue&& other) {
        if (this == &other) {
            return *this;
        }

        auto rebound = std::move(other).rebindForModel(resource_);
        std::destroy_at(&storage_);
        std::construct_at(&storage_, std::move(rebound.storage_));
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&storage_)) {
            return *borrowed;
        }
        const auto& owned = std::get<std::pmr::string>(storage_);
        return std::string_view(owned);
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] Kind kind() const noexcept {
        auto input = view();
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

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const;

private:
    friend struct detail::ModelValueFactory;
    friend struct detail::ModelValueRebindAccess;
    friend class JsonObject;
    template <typename ViewT>
    friend std::optional<ViewT> detail::parseJsonViewValue(std::string_view&,
        std::pmr::memory_resource*, std::size_t, detail::ModelStringStorage, bool);

    using Storage = std::variant<std::string_view, std::pmr::string>;

    JsonValue(detail::ResolvedPmrResourceTag, std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : resource_(resource),
          storage_(std::in_place_type<std::string_view>, body) {}

    void assignOwned(std::string_view value) {
        storage_.template emplace<std::pmr::string>(value, resource_);
    }

    [[nodiscard]] JsonValue rebindForModel(std::pmr::memory_resource* resource) const& {
        JsonValue rebound(detail::ResolvedPmrResourceTag{}, {}, resource);
        rebound.assignOwned(view());
        return rebound;
    }

    [[nodiscard]] JsonValue rebindForModel(std::pmr::memory_resource* resource) && {
        JsonValue rebound(detail::ResolvedPmrResourceTag{}, {}, resource);
        if (auto* owned = std::get_if<std::pmr::string>(&storage_)) {
            if (owned->get_allocator().resource() == resource) {
                rebound.storage_.template emplace<std::pmr::string>(std::move(*owned));
                return rebound;
            }
        }
        rebound.assignOwned(view());
        return rebound;
    }

    std::pmr::memory_resource* resource_;
    Storage storage_;
};

// Object-only dynamic token with the same borrowing, ownership and move
// contract as JsonValue. Supplying a resource to parse() does not copy input.
class JsonObject final {
public:
    explicit JsonObject(ModelOptions options = {})
        : JsonObject(detail::ResolvedPmrResourceTag{}, {},
              detail::pmrResourceOrDefault(options.resource)) {}

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

    JsonObject(const JsonObject&) = delete;
    JsonObject& operator=(const JsonObject&) = delete;

    JsonObject(JsonObject&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    JsonObject& operator=(JsonObject&& other) {
        if (this == &other) {
            return *this;
        }

        auto rebound = std::move(other).rebindForModel(resource_);
        std::destroy_at(&storage_);
        std::construct_at(&storage_, std::move(rebound.storage_));
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&storage_)) {
            return *borrowed;
        }
        const auto& owned = std::get<std::pmr::string>(storage_);
        return std::string_view(owned);
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        auto* const resource = resource_;
        std::optional<T> result;
        bool lastMatchFailed = false;
        const bool valid = detail::visitJsonObjectFields(detail::ResolvedPmrResourceTag{}, view(),
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
    friend struct detail::ModelValueFactory;
    friend struct detail::ModelValueRebindAccess;
    template <typename ViewT>
    friend std::optional<ViewT> detail::parseJsonViewValue(std::string_view&,
        std::pmr::memory_resource*, std::size_t, detail::ModelStringStorage, bool);

    using Storage = std::variant<std::string_view, std::pmr::string>;

    JsonObject(detail::ResolvedPmrResourceTag, std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : resource_(resource),
          storage_(std::in_place_type<std::string_view>, body) {}

    void assignOwned(std::string_view value) {
        storage_.template emplace<std::pmr::string>(value, resource_);
    }

    [[nodiscard]] JsonObject rebindForModel(std::pmr::memory_resource* resource) const& {
        JsonObject rebound(detail::ResolvedPmrResourceTag{}, {}, resource);
        rebound.assignOwned(view());
        return rebound;
    }

    [[nodiscard]] JsonObject rebindForModel(std::pmr::memory_resource* resource) && {
        JsonObject rebound(detail::ResolvedPmrResourceTag{}, {}, resource);
        if (auto* owned = std::get_if<std::pmr::string>(&storage_)) {
            if (owned->get_allocator().resource() == resource) {
                rebound.storage_.template emplace<std::pmr::string>(std::move(*owned));
                return rebound;
            }
        }
        rebound.assignOwned(view());
        return rebound;
    }

    std::pmr::memory_resource* resource_;
    Storage storage_;
};

template <typename T>
[[nodiscard]] inline std::optional<T> JsonValue::get(std::string_view field) const {
    if (!isObject()) {
        return std::nullopt;
    }
    return JsonObject(detail::ResolvedPmrResourceTag{}, view(), resource_).get<T>(field);
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename ViewT>
[[nodiscard]] std::optional<ViewT> parseJsonViewValue(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, ModelStringStorage stringStorage,
    bool requireObject) {
    skipJsonWhitespace(input);
    const auto start = input;
    if (!skipJsonValue(input, depth)) {
        return std::nullopt;
    }
    const auto token = start.substr(0, start.size() - input.size());
    if (requireObject) {
        auto probe = token;
        skipJsonWhitespace(probe);
        if (probe.empty() || probe.front() != '{') {
            input = start;
            return std::nullopt;
        }
    }
    ViewT value(ModelOptions{.resource = resource});
    if (stringStorage == ModelStringStorage::kOwned) {
        value.assignOwned(token);
    } else {
        value.storage_.template emplace<std::string_view>(token);
    }
    return value;
}

template <>
inline std::optional<JsonValue> parseJsonValue<JsonValue>(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, ModelStringStorage stringStorage) {
    return parseJsonViewValue<JsonValue>(input, resource, depth, stringStorage, false);
}

template <>
inline std::optional<JsonObject> parseJsonValue<JsonObject>(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, ModelStringStorage stringStorage) {
    return parseJsonViewValue<JsonObject>(input, resource, depth, stringStorage, true);
}

template <>
inline std::size_t jsonSizeHintValue<JsonValue>(const JsonValue& value) {
    return value.view().size();
}

template <>
inline std::size_t jsonSizeHintValue<JsonObject>(const JsonObject& value) {
    return value.view().size();
}

template <>
inline void appendJsonValue<JsonValue>(std::pmr::string& output, const JsonValue& value) {
    output.append(value.view());
}

template <>
inline void appendJsonValue<JsonObject>(std::pmr::string& output, const JsonObject& value) {
    output.append(value.view());
}

}  // namespace ruvia::detail
