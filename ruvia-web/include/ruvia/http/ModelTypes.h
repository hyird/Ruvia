#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/memory/PmrObject.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia {

class RequestNameValueList;
class JsonValue;

namespace detail {

struct ModelValueFactory;

}  // namespace detail

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

    static std::optional<T> parseFields(
        const RequestNameValueList& fields,
        std::pmr::memory_resource* resource) {
        if constexpr (requires { T::ruviaParseFormFields(fields, resource); }) {
            return T::ruviaParseFormFields(fields, resource);
        } else {
            (void)fields;
            (void)resource;
            return std::nullopt;
        }
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
    explicit String(std::pmr::memory_resource* resource = nullptr)
        : String(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

    String(
        std::string_view value,
        std::pmr::memory_resource* resource = nullptr)
        : String(detail::ResolvedPmrResourceTag{}, value, detail::pmrResourceOrDefault(resource)) {}

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
    friend struct detail::ModelValueFactory;

    String(
        detail::ResolvedPmrResourceTag,
        std::pmr::memory_resource* resource)
        : owned_(resource) {}

    String(
        detail::ResolvedPmrResourceTag,
        std::string_view value,
        std::pmr::memory_resource* resource)
        : view_(value), owned_(resource) {}

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

    explicit List(std::pmr::memory_resource* resource = nullptr)
        : List(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

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
        T* value = nullptr;
        if constexpr (sizeof...(Args) == 0 && std::constructible_from<T, std::pmr::memory_resource*>) {
            value = detail::constructPmrObject<T>(
                detail::ResolvedPmrResourceTag{},
                resource_,
                resource_);
        } else {
            value = detail::constructPmrObject<T>(
                detail::ResolvedPmrResourceTag{},
                resource_,
                std::forward<Args>(args)...);
        }
        try {
            items_.push_back(value);
        } catch (...) {
            detail::destroyPmrObject(detail::ResolvedPmrResourceTag{}, value, resource_);
            throw;
        }
        return *value;
    }

    T& emplaceMove(T&& value) {
        return emplace(std::move(value));
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    friend struct detail::ModelValueFactory;

    List(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : resource_(resource),
          items_(resource_) {}

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

namespace detail {

struct ModelValueFactory final {
    [[nodiscard]] static String makeString(std::pmr::memory_resource* resource) {
        return String(ResolvedPmrResourceTag{}, resource);
    }

    [[nodiscard]] static String makeString(
        std::string_view value,
        std::pmr::memory_resource* resource) {
        return String(ResolvedPmrResourceTag{}, value, resource);
    }

    template <typename ListT>
    [[nodiscard]] static ListT makeList(std::pmr::memory_resource* resource) {
        return ListT(ResolvedPmrResourceTag{}, resource);
    }
};

}  // namespace detail

class JsonObject;
class FormObject;
class RequestObject;

}  // namespace ruvia
