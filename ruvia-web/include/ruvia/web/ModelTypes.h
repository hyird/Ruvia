#pragma once

#include <algorithm>
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
#include <variant>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia {

class RequestNameValueList;
class JsonValue;

namespace detail {

class ModelInput;
struct ModelValueFactory;

enum class ModelStringStorage : std::uint8_t {
    kBorrowed,
    kOwned,
};

}  // namespace detail

template <typename T, typename = void>
struct JsonBody : std::false_type {};

template <typename T>
    requires requires { typename T::RuviaRequestModelSchema; }
struct JsonBody<T, void> : std::true_type {};

template <typename T, typename = void>
struct FormBody : std::false_type {};

template <typename T>
    requires requires { typename T::RuviaRequestModelSchema; }
struct FormBody<T, void> : std::true_type {};

template <std::size_t N>
struct FixedString {
    char value[N]{};

    constexpr FixedString(const char (&text)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const& noexcept {
        return std::string_view(value, N - 1);
    }
    [[nodiscard]] constexpr std::string_view view() const&& = delete;
};

template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

template <std::size_t LeftN, std::size_t RightN>
[[nodiscard]] constexpr bool operator==(const FixedString<LeftN>& left, const FixedString<RightN>& right) noexcept {
    if constexpr (LeftN != RightN) {
        return false;
    } else {
        return std::ranges::equal(left.value, right.value);
    }
}

class String final {
public:
    explicit String(std::pmr::memory_resource* resource = nullptr)
        : String(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

    String(std::string_view value, std::pmr::memory_resource* resource = nullptr)
        : resource_(detail::pmrResourceOrDefault(resource)),
          storage_(std::in_place_type<std::pmr::string>, value, resource_) {}

    String(const String&) = delete;
    String& operator=(const String&) = delete;

    String(String&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    String& operator=(String&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        std::destroy_at(&storage_);
        resource_ = other.resource_;
        std::construct_at(&storage_, std::move(other.storage_));
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept {
        if (const auto* borrowed = std::get_if<std::string_view>(&storage_)) {
            return *borrowed;
        }
        const auto& owned = std::get<std::pmr::string>(storage_);
        return std::string_view(owned);
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] const char* data() const& noexcept {
        return view().data();
    }
    [[nodiscard]] const char* data() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return view().size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return view().empty();
    }

    operator std::string_view() const& noexcept {
        return view();
    }
    operator std::string_view() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    void assignOwned(std::string_view value) {
        std::pmr::string owned(value, resource_);
        storage_.template emplace<std::pmr::string>(std::move(owned));
    }

    void assignOwned(std::pmr::string&& value) {
        std::pmr::string owned(std::move(value), resource_);
        storage_.template emplace<std::pmr::string>(std::move(owned));
    }

private:
    friend struct detail::ModelValueFactory;

    using Storage = std::variant<std::string_view, std::pmr::string>;

    String(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(std::in_place_type<std::string_view>) {}

    String(detail::ResolvedPmrResourceTag, std::string_view value, std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(std::in_place_type<std::string_view>, value) {}

    std::pmr::memory_resource* resource_;
    Storage storage_;
};

struct Bool final {
    bool value{false};
    constexpr Bool() noexcept = default;
    constexpr Bool(bool input) noexcept
        : value(input) {}
    [[nodiscard]] constexpr operator bool() const noexcept {
        return value;
    }
};

struct Float final {
    float value{0};
    constexpr Float() noexcept = default;
    constexpr Float(float input) noexcept
        : value(input) {}
    [[nodiscard]] constexpr operator float() const noexcept {
        return value;
    }
};

struct Double final {
    double value{0};
    constexpr Double() noexcept = default;
    constexpr Double(double input) noexcept
        : value(input) {}
    [[nodiscard]] constexpr operator double() const noexcept {
        return value;
    }
};

struct Int32 final {
    std::int32_t value{0};
    constexpr Int32() noexcept = default;
    constexpr Int32(std::int32_t input) noexcept
        : value(input) {}
    [[nodiscard]] constexpr operator std::int32_t() const noexcept {
        return value;
    }
};

struct UInt32 final {
    std::uint32_t value{0};
    constexpr UInt32() noexcept = default;
    constexpr UInt32(std::uint32_t input) noexcept
        : value(input) {}
    [[nodiscard]] constexpr operator std::uint32_t() const noexcept {
        return value;
    }
};

struct Int64 final {
    std::int64_t value{0};
    constexpr Int64() noexcept = default;
    constexpr Int64(std::int64_t input) noexcept
        : value(input) {}
    [[nodiscard]] constexpr operator std::int64_t() const noexcept {
        return value;
    }
};

struct UInt64 final {
    std::uint64_t value{0};
    constexpr UInt64() noexcept = default;
    constexpr UInt64(std::uint64_t input) noexcept
        : value(input) {}
    [[nodiscard]] constexpr operator std::uint64_t() const noexcept {
        return value;
    }
};

template <typename T>
using Array = std::pmr::vector<T>;

template <typename T>
class BoxedArray final {
public:
    using value_type = T;

    explicit BoxedArray(std::pmr::memory_resource* resource = nullptr)
        : BoxedArray(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

    BoxedArray(const BoxedArray&) = delete;
    BoxedArray& operator=(const BoxedArray&) = delete;

    BoxedArray(BoxedArray&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    BoxedArray& operator=(BoxedArray&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        clear();
        // polymorphic_allocator does not propagate on move assignment. Rebuild
        // the pointer table so its allocator follows the resource that owns the
        // transferred elements.
        std::destroy_at(&items_);
        resource_ = other.resource_;
        std::construct_at(&items_, std::move(other.items_));
        return *this;
    }

    ~BoxedArray() {
        clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] const T& operator[](std::size_t index) const& noexcept {
        return *items_[index];
    }
    [[nodiscard]] const T& operator[](std::size_t) const&& = delete;

    [[nodiscard]] const T& front() const& noexcept {
        return *items_.front();
    }
    [[nodiscard]] const T& front() const&& = delete;

    [[nodiscard]] auto begin() const& noexcept {
        return Iterator(items_.begin());
    }
    void begin() const&& = delete;

    [[nodiscard]] auto end() const& noexcept {
        return Iterator(items_.end());
    }
    void end() const&& = delete;

    void clear() noexcept {
        for (auto* value : items_) {
            detail::destroyPmrObject(detail::ResolvedPmrResourceTag{}, value, resource_);
        }
        items_.clear();
    }

    template <typename... Args>
    T& emplace(Args&&... args) & {
        T* value = nullptr;
        if constexpr (sizeof...(Args) == 0 && std::constructible_from<T, std::pmr::memory_resource*>) {
            value = detail::constructPmrObject<T>(detail::ResolvedPmrResourceTag{}, resource_, resource_);
        } else {
            value = detail::constructPmrObject<T>(detail::ResolvedPmrResourceTag{}, resource_, std::forward<Args>(args)...);
        }
        try {
            items_.push_back(value);
        } catch (...) {
            detail::destroyPmrObject(detail::ResolvedPmrResourceTag{}, value, resource_);
            throw;
        }
        return *value;
    }

    T& emplaceMove(T&& value) & {
        return emplace(std::move(value));
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    friend struct detail::ModelValueFactory;

    BoxedArray(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
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

        explicit Iterator(InnerIterator current) noexcept
            : current_(current) {}

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

    [[nodiscard]] static String makeString(std::string_view value, std::pmr::memory_resource* resource) {
        return String(ResolvedPmrResourceTag{}, value, resource);
    }

    template <typename ListT>
    [[nodiscard]] static ListT makeBoxedArray(std::pmr::memory_resource* resource) {
        return ListT(ResolvedPmrResourceTag{}, resource);
    }
};

}  // namespace detail

class JsonObject;
class FormObject;

}  // namespace ruvia
