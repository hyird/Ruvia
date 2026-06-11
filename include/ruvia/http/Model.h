#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ruvia {

// Compile-time model metadata. RUVIA_MODEL specializes JsonBody/FormBody through
// generated parse functions and serializes through ruviaAppendJson().

template <typename T, typename = void>
struct JsonBody {
    static constexpr bool value = false;
};

template <typename T>
struct JsonBody<T, std::void_t<decltype(T::ruviaParseJsonBody(
                       std::declval<std::string_view>(),
                       std::declval<std::pmr::memory_resource*>()))>> {
    static constexpr bool value = true;

    static std::optional<T> parse(
        std::string_view body,
        std::pmr::memory_resource* resource) {
        return T::ruviaParseJsonBody(body, resource);
    }

    static std::optional<T> parseDepth(
        std::string_view body,
        std::pmr::memory_resource* resource,
        std::size_t depth) {
        if constexpr (requires { T::ruviaParseJsonBodyDepth(body, resource, depth); }) {
            return T::ruviaParseJsonBodyDepth(body, resource, depth);
        } else {
            (void)depth;
            return T::ruviaParseJsonBody(body, resource);
        }
    }
};

template <typename T, typename = void>
struct FormBody {
    static constexpr bool value = false;
};

template <typename T>
struct FormBody<T, std::void_t<decltype(T::ruviaParseFormBody(
                       std::declval<std::string_view>(),
                       std::declval<std::pmr::memory_resource*>()))>> {
    static constexpr bool value = true;

    static std::optional<T> parse(
        std::string_view body,
        std::pmr::memory_resource* resource) {
        return T::ruviaParseFormBody(body, resource);
    }
};

template <std::size_t N>
struct FixedString {
    char value[N]{};

    constexpr FixedString(const char (&text)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(value, N - 1);
    }
};

template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

template <std::size_t LeftN, std::size_t RightN>
[[nodiscard]] constexpr bool operator==(
    const FixedString<LeftN>& left,
    const FixedString<RightN>& right) noexcept {
    if constexpr (LeftN != RightN) {
        return false;
    } else {
        for (std::size_t i = 0; i < LeftN; ++i) {
            if (left.value[i] != right.value[i]) {
                return false;
            }
        }
        return true;
    }
}

class String final {
public:
    explicit String(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : owned_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    String(
        std::string_view value,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : view_(value), owned_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    [[nodiscard]] std::string_view view() const noexcept {
        if (ownedActive_) {
            return std::string_view(owned_.data(), owned_.size());
        }
        return view_;
    }

    [[nodiscard]] const char* data() const noexcept {
        return view().data();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return view().size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return view().empty();
    }

    operator std::string_view() const noexcept {
        return view();
    }

    void assignView(std::string_view value) noexcept {
        view_ = value;
        owned_.clear();
        ownedActive_ = false;
    }

    void assignOwned(std::string_view value) {
        auto& owned = resetOwned();
        owned.assign(value.data(), value.size());
    }

    void assignOwned(std::pmr::string&& value) {
        owned_ = std::move(value);
        ownedActive_ = true;
        view_ = {};
    }

    [[nodiscard]] std::pmr::string& resetOwned() {
        owned_.clear();
        ownedActive_ = true;
        view_ = {};
        return owned_;
    }

private:
    std::string_view view_;
    std::pmr::string owned_;
    bool ownedActive_{false};
};

struct Bool final {
    bool value{false};
    constexpr Bool() noexcept = default;
    constexpr Bool(bool input) noexcept : value(input) {}
    [[nodiscard]] constexpr operator bool() const noexcept { return value; }
};

struct Float final {
    float value{0};
    constexpr Float() noexcept = default;
    constexpr Float(float input) noexcept : value(input) {}
    [[nodiscard]] constexpr operator float() const noexcept { return value; }
};

struct Double final {
    double value{0};
    constexpr Double() noexcept = default;
    constexpr Double(double input) noexcept : value(input) {}
    [[nodiscard]] constexpr operator double() const noexcept { return value; }
};

struct Int32 final {
    std::int32_t value{0};
    constexpr Int32() noexcept = default;
    constexpr Int32(std::int32_t input) noexcept : value(input) {}
    [[nodiscard]] constexpr operator std::int32_t() const noexcept { return value; }
};

struct UInt32 final {
    std::uint32_t value{0};
    constexpr UInt32() noexcept = default;
    constexpr UInt32(std::uint32_t input) noexcept : value(input) {}
    [[nodiscard]] constexpr operator std::uint32_t() const noexcept { return value; }
};

struct Int64 final {
    std::int64_t value{0};
    constexpr Int64() noexcept = default;
    constexpr Int64(std::int64_t input) noexcept : value(input) {}
    [[nodiscard]] constexpr operator std::int64_t() const noexcept { return value; }
};

struct UInt64 final {
    std::uint64_t value{0};
    constexpr UInt64() noexcept = default;
    constexpr UInt64(std::uint64_t input) noexcept : value(input) {}
    [[nodiscard]] constexpr operator std::uint64_t() const noexcept { return value; }
};

template <typename T>
using Array = std::pmr::vector<T>;

template <typename T>
class List final {
public:
    using value_type = T;

    explicit List(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
          items_(resource_) {}

    List(const List&) = delete;
    List& operator=(const List&) = delete;

    List(List&&) noexcept = default;
    List& operator=(List&&) noexcept = default;

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] const T& operator[](std::size_t index) const noexcept {
        return *items_[index];
    }

    [[nodiscard]] const T& front() const noexcept {
        return *items_.front();
    }

    [[nodiscard]] auto begin() const noexcept {
        return Iterator(items_.begin());
    }

    [[nodiscard]] auto end() const noexcept {
        return Iterator(items_.end());
    }

    void clear() noexcept {
        items_.clear();
    }

    template <typename... Args>
    T& emplace(Args&&... args) {
        auto* memory = resource_->allocate(sizeof(T), alignof(T));
        T* value = nullptr;
        if constexpr (sizeof...(Args) == 0 && std::constructible_from<T, std::pmr::memory_resource*>) {
            value = std::construct_at(static_cast<T*>(memory), resource_);
        } else {
            value = std::construct_at(static_cast<T*>(memory), std::forward<Args>(args)...);
        }
        items_.push_back(value);
        return *value;
    }

    T& emplaceMove(T&& value) {
        return emplace(std::move(value));
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    class Iterator final {
    public:
        using InnerIterator = typename std::pmr::vector<T*>::const_iterator;
        using difference_type = typename InnerIterator::difference_type;
        using value_type = T;
        using reference = const T&;
        using pointer = const T*;
        using iterator_category = std::forward_iterator_tag;

        explicit Iterator(InnerIterator current) noexcept : current_(current) {}

        reference operator*() const noexcept {
            return **current_;
        }

        pointer operator->() const noexcept {
            return *current_;
        }

        Iterator& operator++() noexcept {
            ++current_;
            return *this;
        }

        Iterator operator++(int) noexcept {
            auto copy = *this;
            ++current_;
            return copy;
        }

        friend bool operator==(const Iterator& left, const Iterator& right) noexcept {
            return left.current_ == right.current_;
        }

    private:
        InnerIterator current_;
    };

    std::pmr::memory_resource* resource_;
    std::pmr::vector<T*> items_;
};

class JsonObject;
class FormObject;
class RequestObject;

}  // namespace ruvia

#include "ruvia/http/detail/model/Traits.h"
#include "ruvia/http/detail/model/Parser.h"

namespace ruvia {

template <typename T>
class ModelFieldConstRef final {
public:
    explicit ModelFieldConstRef(const std::optional<T>& value) noexcept
        : value_(&value) {}

    [[nodiscard]] bool has_value() const noexcept {
        return value_->has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] const T* operator->() const noexcept {
        return value_->operator->();
    }

    [[nodiscard]] const T& operator*() const noexcept {
        return **value_;
    }

    [[nodiscard]] const T& value() const {
        return value_->value();
    }

    [[nodiscard]] const std::optional<T>& optional() const noexcept {
        return *value_;
    }

    [[nodiscard]] operator const std::optional<T>&() const noexcept {
        return *value_;
    }

private:
    const std::optional<T>* value_{nullptr};
};

template <typename T>
class ModelFieldRef final {
public:
    ModelFieldRef(
        std::optional<T>& value,
        detail::ModelFieldState& state,
        std::pmr::memory_resource* resource) noexcept
        : value_(&value),
          state_(&state),
          resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    [[nodiscard]] bool has_value() const noexcept {
        return value_->has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T* operator->() noexcept {
        return value_->operator->();
    }

    [[nodiscard]] const T* operator->() const noexcept {
        return value_->operator->();
    }

    [[nodiscard]] T& operator*() noexcept {
        return **value_;
    }

    [[nodiscard]] const T& operator*() const noexcept {
        return **value_;
    }

    [[nodiscard]] T& value() {
        return value_->value();
    }

    [[nodiscard]] const T& value() const {
        return value_->value();
    }

    [[nodiscard]] std::optional<T>& optional() noexcept {
        return *value_;
    }

    [[nodiscard]] const std::optional<T>& optional() const noexcept {
        return *value_;
    }

    [[nodiscard]] operator std::optional<T>&() noexcept {
        return *value_;
    }

    [[nodiscard]] operator const std::optional<T>&() const noexcept {
        return *value_;
    }

    [[nodiscard]] T& ensure() {
        if (!*value_) {
            value_->emplace(detail::makeRequestValue<T>(resource_));
        }
        *state_ = detail::ModelFieldState::kParsed;
        return **value_;
    }

    void reset() noexcept {
        *state_ = detail::ModelFieldState::kMissing;
        value_->reset();
    }

    void clear()
        requires detail::isRuviaString<T>
    {
        ensure().resetOwned().clear();
    }

    void clear()
        requires (!detail::isRuviaString<T> && requires (T& value) { value.clear(); })
    {
        ensure().clear();
    }

    template <typename... Args>
    decltype(auto) emplace(Args&&... args)
        requires requires (T& value) { value.emplace(std::forward<Args>(args)...); }
    {
        return ensure().emplace(std::forward<Args>(args)...);
    }

    template <typename... Args>
    decltype(auto) emplace_back(Args&&... args)
        requires requires (T& value) { value.emplace_back(std::forward<Args>(args)...); }
    {
        return ensure().emplace_back(std::forward<Args>(args)...);
    }

    void assignView(std::string_view value) noexcept
        requires detail::isRuviaString<T>
    {
        if (!*value_) {
            value_->emplace(resource_);
        }
        (*value_)->assignView(value);
        *state_ = detail::ModelFieldState::kParsed;
    }

    void assignOwned(std::string_view value)
        requires detail::isRuviaString<T>
    {
        if (!*value_) {
            value_->emplace(resource_);
        }
        (*value_)->assignOwned(value);
        *state_ = detail::ModelFieldState::kParsed;
    }

    void assignOwned(std::pmr::string&& value)
        requires detail::isRuviaString<T>
    {
        if (!*value_) {
            value_->emplace(resource_);
        }
        (*value_)->assignOwned(std::move(value));
        *state_ = detail::ModelFieldState::kParsed;
    }

private:
    std::optional<T>* value_{nullptr};
    detail::ModelFieldState* state_{nullptr};
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
};

class JsonObject final {
public:
    JsonObject() noexcept = default;

    explicit JsonObject(
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : body_(body), resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    [[nodiscard]] static std::optional<JsonObject> parse(
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept {
        detail::JsonScanner scanner(body);
        if (!scanner.consumeObject()) {
            return std::nullopt;
        }
        scanner.skipWhitespace();
        if (!scanner.empty()) {
            return std::nullopt;
        }
        return JsonObject(body, resource);
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        auto input = body_;
        if (!detail::consumeJsonChar(input, '{')) {
            return std::nullopt;
        }
        detail::skipJsonWhitespace(input);
        if (!input.empty() && input.front() == '}') {
            return std::nullopt;
        }

        while (true) {
            std::string_view key;
            bool keyEscaped = false;
            if (!detail::parseJsonStringRaw(input, key, keyEscaped) || !detail::consumeJsonChar(input, ':')) {
                return std::nullopt;
            }
            bool keyMatches = key == field;
            std::pmr::string decodedKey(resource_);
            if (keyEscaped) {
                keyMatches = detail::decodeJsonString(key, decodedKey) && decodedKey == field;
            }
            if (keyMatches) {
                auto value = detail::parseJsonValue<T>(input, resource_);
                if (!value) {
                    return std::nullopt;
                }
                return value;
            }
            if (!detail::skipJsonValue(input)) {
                return std::nullopt;
            }

            detail::skipJsonWhitespace(input);
            if (!input.empty() && input.front() == '}') {
                return std::nullopt;
            }
            if (!detail::consumeJsonChar(input, ',')) {
                return std::nullopt;
            }
        }
    }

private:
    std::string_view body_;
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
};

class FormObject final {
public:
    FormObject() noexcept = default;

    explicit FormObject(
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : body_(body), resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    [[nodiscard]] static std::optional<FormObject> parse(
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept {
        if (!detail::validateFormEncoding(body)) {
            return std::nullopt;
        }
        return FormObject(body, resource);
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        auto input = body_;
        while (true) {
            const auto pairEnd = input.find('&');
            const auto pair = pairEnd == std::string_view::npos ? input : input.substr(0, pairEnd);
            const auto equals = pair.find('=');
            const auto name = equals == std::string_view::npos ? pair : pair.substr(0, equals);
            const auto valueView = equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);

            if (detail::urlComponentEquals(name, field, detail::UrlDecodeMode::kForm)) {
                T value = detail::makeRequestValue<T>(resource_);
                if (!detail::parseFormValue(valueView, value, resource_)) {
                    return std::nullopt;
                }
                return value;
            }

            if (pairEnd == std::string_view::npos) {
                return std::nullopt;
            }
            input.remove_prefix(pairEnd + 1);
        }
    }

private:
    std::string_view body_;
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
};

enum class RequestObjectKind {
    kJson,
    kForm
};

class RequestObject final {
public:
    RequestObject() noexcept = default;

    RequestObject(
        RequestObjectKind kind,
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : kind_(kind),
          body_(body),
          resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    RequestObject(const RequestObject& other)
        : kind_(other.kind_),
          body_(other.body_),
          resource_(other.resource_) {}

    RequestObject& operator=(const RequestObject& other) {
        if (this == &other) {
            return *this;
        }
        kind_ = other.kind_;
        body_ = other.body_;
        resource_ = other.resource_;
        return *this;
    }

    RequestObject(RequestObject&&) noexcept = default;
    RequestObject& operator=(RequestObject&&) noexcept = default;

    [[nodiscard]] RequestObjectKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        if (kind_ == RequestObjectKind::kJson) {
            return JsonObject(body_, resource_).get<T>(field);
        }
        if constexpr (detail::isFormField<T>) {
            return FormObject(body_, resource_).get<T>(field);
        } else {
            (void)field;
            return std::nullopt;
        }
    }

private:
    RequestObjectKind kind_{RequestObjectKind::kJson};
    std::string_view body_;
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
};

template <typename T>
void appendJson(std::pmr::string& output, const T& value) {
    output.reserve(output.size() + detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
}

template <typename T>
[[nodiscard]] std::pmr::string toJson(
    const T& value,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    std::pmr::string output(resource);
    output.reserve(detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
    return output;
}

}  // namespace ruvia

#include "ruvia/http/detail/model/Rules.h"
#include "ruvia/http/detail/model/RequestObjectVisitors.h"
#include "ruvia/http/detail/model/Macros.h"
