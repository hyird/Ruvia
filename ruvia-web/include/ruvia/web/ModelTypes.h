#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/Attributes.h"
#include "ruvia/web/FixedString.h"
#include "ruvia/web/detail/model/model_text_storage.h"

namespace ruvia {

class RequestNameValueList;

namespace detail {

template <typename T>
inline constexpr bool isModelNarrowInteger =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> &&
    !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>;

class ModelInput;
struct ModelValueFactory;
struct ModelValueRebindAccess;

enum class ModelStringStorage : std::uint8_t {
    kBorrowed,
    kOwned,
};

struct ModelValueRebindAccess final {
    template <typename T>
    [[nodiscard]] static consteval bool hasRebindForModel() {
        using ValueT = std::remove_cvref_t<T>;
        if constexpr (requires { typename ValueT::RuviaModelSchema; }) {
            return true;
        } else {
            return requires(const ValueT& source, std::pmr::memory_resource* target) {
                source.rebindForModel(target);
            };
        }
    }

    template <typename T>
    [[nodiscard]] static std::remove_cvref_t<T> own(
        T&& value, std::pmr::memory_resource* resource) {
        using ValueT = std::remove_cvref_t<T>;
        if constexpr (requires { typename ValueT::RuviaModelSchema; }) {
            return ValueT(std::forward<T>(value).fields_.rebind(resource));
        } else if constexpr (requires(ValueT& source, std::pmr::memory_resource* target) {
                                 source.rebindForModel(target);
                             }) {
            if constexpr (std::is_lvalue_reference_v<T&&>) {
                return value.rebindForModel(resource);
            } else {
                return std::move(value).rebindForModel(resource);
            }
        } else {
            return std::forward<T>(value);
        }
    }
};

template <typename T>
[[nodiscard]] std::remove_cvref_t<T> rebindModelValue(
    T&& value, std::pmr::memory_resource* resource) {
    return ModelValueRebindAccess::own(std::forward<T>(value), resource);
}

}  // namespace detail

struct ModelOptions final {
    std::pmr::memory_resource* resource{nullptr};
};

namespace detail {

// Construction is shared; containers decide how the normalized value is owned
// (inline vector element or address-stable PMR box).
template <typename value_type, typename insert_type, typename... argument_types>
value_type& emplace_model_value(std::pmr::memory_resource* resource,
    insert_type insert, argument_types&&... arguments) {
    auto own_and_insert = [&insert, resource](auto&& value) -> value_type& {
        // Borrowed/const inputs are cloned; only mutable rvalues may transfer.
        using input_type = std::conditional_t<
            std::is_lvalue_reference_v<decltype(value)> ||
                std::is_const_v<std::remove_reference_t<decltype(value)>>,
            const value_type&, value_type&&>;
        return insert(rebindModelValue(static_cast<input_type>(value), resource));
    };
    if constexpr (sizeof...(argument_types) == 1 &&
                  (std::same_as<std::remove_cvref_t<argument_types>, value_type> && ...)) {
        return own_and_insert(std::forward<argument_types>(arguments)...);
    } else if constexpr (sizeof...(argument_types) == 0 && std::constructible_from<value_type, ModelOptions>) {
        return own_and_insert(value_type(ModelOptions{.resource = resource}));
    } else if constexpr (requires {
                             value_type(std::forward<argument_types>(arguments)...,
                                 ModelOptions{.resource = resource});
                         }) {
        return own_and_insert(value_type(std::forward<argument_types>(arguments)...,
            ModelOptions{.resource = resource}));
    } else {
        return own_and_insert(value_type(std::forward<argument_types>(arguments)...));
    }
}

}  // namespace detail

struct ModelParseOptions final {
    std::pmr::memory_resource* resource{nullptr};
    // Totals across the complete typed JSON document, including nested arrays.
    std::size_t max_array_elements{64 * 1024};
    std::size_t max_representation_bytes{16 * 1024 * 1024};
};

struct ModelSerializeOptions final {
    std::pmr::memory_resource* resource{nullptr};
};

class String final {
public:
    explicit String(ModelOptions options = {})
        : String(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(options.resource)) {
    }

    String(std::string_view value, ModelOptions options = {})
        : resource_(detail::pmrResourceOrDefault(options.resource)) {
        storage_.assign_owned(value, resource_);
    }

    String(const String&) = delete;
    String& operator=(const String&) = delete;

    String(String&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    String& operator=(String&& other) {
        storage_.assign_from(std::move(other.storage_), resource_);
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] const char* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return view().data();
    }
    [[nodiscard]] const char* data() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return view().size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return view().empty();
    }

    operator std::string_view() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    operator std::string_view() const&& = delete;

    friend bool operator==(const String& left, const String& right) noexcept {
        return left.view() == right.view();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    void assignOwned(std::string_view value) {
        storage_.assign_owned(value, resource_);
    }

    void assignOwned(std::pmr::string&& value) {
        storage_.assign_owned(std::move(value), resource_);
    }

private:
    friend struct detail::ModelValueFactory;
    friend struct detail::ModelValueRebindAccess;

    String(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : resource_(resource) {}

    String(detail::ResolvedPmrResourceTag, std::string_view value, std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(value) {}

    String(detail::ResolvedPmrResourceTag, detail::model_text_storage&& storage,
        std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(std::move(storage)) {}

    [[nodiscard]] String rebindForModel(std::pmr::memory_resource* resource) const& {
        return String(detail::ResolvedPmrResourceTag{}, storage_.rebind(resource), resource);
    }

    [[nodiscard]] String rebindForModel(std::pmr::memory_resource* resource) && {
        return String(detail::ResolvedPmrResourceTag{}, std::move(storage_).rebind(resource), resource);
    }

    std::pmr::memory_resource* resource_;
    detail::model_text_storage storage_;
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

struct Int8 final {
    std::int8_t value{0};
    constexpr Int8() noexcept = default;
    constexpr Int8(std::int8_t input) noexcept
        : value(input) {}
    template <typename T>
        requires(detail::isModelNarrowInteger<T> &&
                 !std::is_same_v<std::remove_cv_t<T>, std::int8_t>)
    constexpr Int8(T input)
        : value(checked(input)) {}
    template <typename T>
        requires(!detail::isModelNarrowInteger<T>)
    Int8(T) = delete;
    [[nodiscard]] constexpr operator std::int8_t() const noexcept {
        return value;
    }

private:
    template <typename T>
    static constexpr std::int8_t checked(T input) {
        if (!std::in_range<std::int8_t>(input)) {
            throw std::out_of_range("Int8 value is out of range");
        }
        return static_cast<std::int8_t>(input);
    }
};

struct UInt8 final {
    std::uint8_t value{0};
    constexpr UInt8() noexcept = default;
    constexpr UInt8(std::uint8_t input) noexcept
        : value(input) {}
    template <typename T>
        requires(detail::isModelNarrowInteger<T> &&
                 !std::is_same_v<std::remove_cv_t<T>, std::uint8_t>)
    constexpr UInt8(T input)
        : value(checked(input)) {}
    template <typename T>
        requires(!detail::isModelNarrowInteger<T>)
    UInt8(T) = delete;
    [[nodiscard]] constexpr operator std::uint8_t() const noexcept {
        return value;
    }

private:
    template <typename T>
    static constexpr std::uint8_t checked(T input) {
        if (!std::in_range<std::uint8_t>(input)) {
            throw std::out_of_range("UInt8 value is out of range");
        }
        return static_cast<std::uint8_t>(input);
    }
};

struct Int16 final {
    std::int16_t value{0};
    constexpr Int16() noexcept = default;
    constexpr Int16(std::int16_t input) noexcept
        : value(input) {}
    template <typename T>
        requires(detail::isModelNarrowInteger<T> &&
                 !std::is_same_v<std::remove_cv_t<T>, std::int16_t>)
    constexpr Int16(T input)
        : value(checked(input)) {}
    template <typename T>
        requires(!detail::isModelNarrowInteger<T>)
    Int16(T) = delete;
    [[nodiscard]] constexpr operator std::int16_t() const noexcept {
        return value;
    }

private:
    template <typename T>
    static constexpr std::int16_t checked(T input) {
        if (!std::in_range<std::int16_t>(input)) {
            throw std::out_of_range("Int16 value is out of range");
        }
        return static_cast<std::int16_t>(input);
    }
};

struct UInt16 final {
    std::uint16_t value{0};
    constexpr UInt16() noexcept = default;
    constexpr UInt16(std::uint16_t input) noexcept
        : value(input) {}
    template <typename T>
        requires(detail::isModelNarrowInteger<T> &&
                 !std::is_same_v<std::remove_cv_t<T>, std::uint16_t>)
    constexpr UInt16(T input)
        : value(checked(input)) {}
    template <typename T>
        requires(!detail::isModelNarrowInteger<T>)
    UInt16(T) = delete;
    [[nodiscard]] constexpr operator std::uint16_t() const noexcept {
        return value;
    }

private:
    template <typename T>
    static constexpr std::uint16_t checked(T input) {
        if (!std::in_range<std::uint16_t>(input)) {
            throw std::out_of_range("UInt16 value is out of range");
        }
        return static_cast<std::uint16_t>(input);
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

class Bytes final {
public:
    explicit Bytes(ModelOptions options = {})
        : resource_(detail::pmrResourceOrDefault(options.resource)),
          items_(resource_) {}

    Bytes(std::span<const std::uint8_t> value, ModelOptions options = {})
        : resource_(detail::pmrResourceOrDefault(options.resource)),
          items_(value.begin(), value.end(), resource_) {}

    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;

    Bytes(Bytes&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    Bytes& operator=(Bytes&& other) {
        if (this == &other) {
            return *this;
        }
        auto rebound = std::move(other).rebindForModel(resource_);
        items_ = std::move(rebound.items_);
        return *this;
    }

    [[nodiscard]] std::span<const std::uint8_t> view() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_;
    }
    [[nodiscard]] std::span<const std::uint8_t> view() const&& = delete;
    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    void assignOwned(std::span<const std::uint8_t> value) {
        std::pmr::vector<std::uint8_t> owned(value.begin(), value.end(), resource_);
        items_ = std::move(owned);
    }

    void assignOwned(std::pmr::vector<std::uint8_t>&& value) {
        if (value.get_allocator().resource() == resource_) {
            items_ = std::move(value);
            return;
        }
        std::pmr::vector<std::uint8_t> owned(value.begin(), value.end(), resource_);
        items_ = std::move(owned);
    }

    friend bool operator==(const Bytes& left, const Bytes& right) noexcept {
        return left.size() == right.size() &&
               std::equal(left.view().begin(), left.view().end(), right.view().begin());
    }

private:
    friend struct detail::ModelValueRebindAccess;

    [[nodiscard]] Bytes rebindForModel(std::pmr::memory_resource* resource) const& {
        return Bytes(view(), {.resource = resource});
    }

    [[nodiscard]] Bytes rebindForModel(std::pmr::memory_resource* resource) && {
        Bytes rebound(ModelOptions{.resource = resource});
        if (resource_ == resource) {
            rebound.items_ = std::move(items_);
        } else {
            rebound.assignOwned(view());
        }
        return rebound;
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<std::uint8_t> items_;
};

template <typename T>
class Array final {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using iterator = typename std::pmr::vector<T>::iterator;
    using const_iterator = typename std::pmr::vector<T>::const_iterator;

    explicit Array(ModelOptions options = {})
        : Array(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(options.resource)) {}

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    Array(Array&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    Array& operator=(Array&& other) {
        if (this == &other) {
            return *this;
        }

        auto rebound = [&]() {
            if constexpr (detail::ModelValueRebindAccess::hasRebindForModel<T>()) {
                return other.rebindForModel(resource_);
            } else {
                return std::move(other).rebindForModel(resource_);
            }
        }();
        other.clear();
        items_ = std::move(rebound.items_);
        return *this;
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] size_type size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] size_type capacity() const noexcept {
        return items_.capacity();
    }

    [[nodiscard]] reference operator[](size_type index) & noexcept RUVIA_LIFETIMEBOUND {
        return items_[index];
    }
    [[nodiscard]] const_reference operator[](size_type index) const& noexcept RUVIA_LIFETIMEBOUND {
        return items_[index];
    }
    [[nodiscard]] const_reference operator[](size_type) const&& = delete;

    [[nodiscard]] reference front() & noexcept RUVIA_LIFETIMEBOUND {
        return items_.front();
    }
    [[nodiscard]] const_reference front() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.front();
    }
    [[nodiscard]] const_reference front() const&& = delete;

    [[nodiscard]] reference back() & noexcept RUVIA_LIFETIMEBOUND {
        return items_.back();
    }
    [[nodiscard]] const_reference back() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.back();
    }
    [[nodiscard]] const_reference back() const&& = delete;

    [[nodiscard]] iterator begin() & noexcept RUVIA_LIFETIMEBOUND {
        return items_.begin();
    }
    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.begin();
    }
    iterator begin() const&& = delete;

    [[nodiscard]] iterator end() & noexcept RUVIA_LIFETIMEBOUND {
        return items_.end();
    }
    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.end();
    }
    iterator end() const&& = delete;

    [[nodiscard]] std::pmr::polymorphic_allocator<T> get_allocator() const noexcept {
        return items_.get_allocator();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    void reserve(size_type count) {
        items_.reserve(count);
    }

    void clear() noexcept {
        items_.clear();
    }

    void resize(size_type count) {
        if (count < size()) {
            items_.resize(count);
            return;
        }
        while (size() < count) {
            emplace_back();
        }
    }

    void resize(size_type count, const T& value) {
        if (count <= size()) {
            if (count < size()) {
                items_.resize(count);
            }
            return;
        }
        T stable = detail::rebindModelValue(value, resource_);
        while (size() < count) {
            push_back(stable);
        }
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) & {
        return detail::emplace_model_value<T>(resource_, [this](T&& value) -> T& { return emplaceParsed(std::move(value)); }, std::forward<Args>(args)...);
    }

    void push_back(const T& value) & {
        (void)emplace_back(value);
    }

    void push_back(T&& value) & {
        (void)emplace_back(std::move(value));
    }

    friend bool operator==(const Array& left, const Array& right) {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }

private:
    friend struct detail::ModelValueFactory;
    friend struct detail::ModelValueRebindAccess;

    Array(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : resource_(resource),
          items_(resource_) {}

    T& emplaceParsed(T&& value) {
        return items_.emplace_back(std::move(value));
    }

    [[nodiscard]] Array rebindForModel(std::pmr::memory_resource* resource) const& {
        Array rebound(detail::ResolvedPmrResourceTag{}, resource);
        rebound.reserve(size());
        for (const auto& value : items_) {
            (void)rebound.emplaceParsed(detail::rebindModelValue(value, resource));
        }
        return rebound;
    }

    [[nodiscard]] Array rebindForModel(std::pmr::memory_resource* resource) && {
        Array rebound(detail::ResolvedPmrResourceTag{}, resource);
        rebound.reserve(size());
        for (auto& value : items_) {
            (void)rebound.emplaceParsed(detail::rebindModelValue(std::move(value), resource));
        }
        return rebound;
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<T> items_;
};

template <typename T>
class BoxedArray final {
    template <bool Const>
    class Iterator;

public:
    using value_type = T;

    explicit BoxedArray(ModelOptions options = {})
        : BoxedArray(
              detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(options.resource)) {}

    BoxedArray(const BoxedArray&) = delete;
    BoxedArray& operator=(const BoxedArray&) = delete;

    BoxedArray(BoxedArray&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    BoxedArray& operator=(BoxedArray&& other) {
        if (this == &other) {
            return *this;
        }

        auto rebound = [&]() {
            if constexpr (detail::ModelValueRebindAccess::hasRebindForModel<T>()) {
                return other.rebindForModel(resource_);
            } else {
                return std::move(other).rebindForModel(resource_);
            }
        }();
        other.clear();
        clear();
        items_ = std::move(rebound.items_);
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

    [[nodiscard]] const T& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_[index];
    }
    [[nodiscard]] T& operator[](std::size_t index) & noexcept RUVIA_LIFETIMEBOUND {
        return *items_[index];
    }
    [[nodiscard]] const T& operator[](std::size_t) const&& = delete;

    [[nodiscard]] const T& front() const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_.front();
    }
    [[nodiscard]] T& front() & noexcept RUVIA_LIFETIMEBOUND {
        return *items_.front();
    }
    [[nodiscard]] const T& front() const&& = delete;

    [[nodiscard]] const T& back() const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_.back();
    }
    [[nodiscard]] T& back() & noexcept RUVIA_LIFETIMEBOUND {
        return *items_.back();
    }
    [[nodiscard]] const T& back() const&& = delete;

    [[nodiscard]] auto begin() & noexcept RUVIA_LIFETIMEBOUND {
        return Iterator<false>(items_.begin());
    }
    [[nodiscard]] auto begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return Iterator<true>(items_.begin());
    }
    void begin() const&& = delete;

    [[nodiscard]] auto end() & noexcept RUVIA_LIFETIMEBOUND {
        return Iterator<false>(items_.end());
    }
    [[nodiscard]] auto end() const& noexcept RUVIA_LIFETIMEBOUND {
        return Iterator<true>(items_.end());
    }
    void end() const&& = delete;

    void clear() noexcept {
        for (auto* value : items_) {
            detail::destroyPmrObject(detail::ResolvedPmrResourceTag{}, value, resource_);
        }
        items_.clear();
    }

    void reserve(std::size_t count) {
        items_.reserve(count);
    }

    void resize(std::size_t count) {
        if (count < size()) {
            while (size() > count) {
                auto* value = items_.back();
                items_.pop_back();
                detail::destroyPmrObject(detail::ResolvedPmrResourceTag{}, value, resource_);
            }
            return;
        }
        while (size() < count) {
            emplace();
        }
    }

    template <typename... Args>
    T& emplace(Args&&... args) & {
        return detail::emplace_model_value<T>(resource_, [this](T&& value) -> T& { return emplaceParsed(std::move(value)); }, std::forward<Args>(args)...);
    }

    void push_back(const T& value) & {
        (void)emplace(value);
    }

    void push_back(T&& value) & {
        (void)emplace(std::move(value));
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    friend bool operator==(const BoxedArray& left, const BoxedArray& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (!(left[i] == right[i])) {
                return false;
            }
        }
        return true;
    }

private:
    friend struct detail::ModelValueFactory;
    friend struct detail::ModelValueRebindAccess;

    BoxedArray(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : resource_(resource),
          items_(resource_) {}

    template <bool Const>
    class Iterator final {
    public:
        using InnerIterator = std::conditional_t<Const,
            typename std::pmr::vector<T*>::const_iterator,
            typename std::pmr::vector<T*>::iterator>;
        using difference_type = typename InnerIterator::difference_type;
        using value_type = T;
        using reference = std::conditional_t<Const, const T&, T&>;
        using pointer = std::conditional_t<Const, const T*, T*>;
        using iterator_category = std::forward_iterator_tag;

        Iterator() noexcept = default;

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
        InnerIterator current_{};
    };

    T& emplaceParsed(T&& value) {
        auto* const stored = detail::constructPmrObject<T>(
            detail::ResolvedPmrResourceTag{}, resource_, std::move(value));
        try {
            items_.push_back(stored);
        } catch (...) {
            detail::destroyPmrObject(detail::ResolvedPmrResourceTag{}, stored, resource_);
            throw;
        }
        return *stored;
    }

    [[nodiscard]] BoxedArray rebindForModel(std::pmr::memory_resource* resource) const& {
        BoxedArray rebound(detail::ResolvedPmrResourceTag{}, resource);
        rebound.reserve(size());
        for (const auto& value : *this) {
            rebound.emplaceParsed(detail::rebindModelValue(value, resource));
        }
        return rebound;
    }

    [[nodiscard]] BoxedArray rebindForModel(std::pmr::memory_resource* resource) && {
        BoxedArray rebound(detail::ResolvedPmrResourceTag{}, resource);
        rebound.reserve(size());
        for (auto& value : *this) {
            rebound.emplaceParsed(detail::rebindModelValue(std::move(value), resource));
        }
        return rebound;
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<T*> items_;
};

namespace detail {

struct ModelValueFactory final {
    [[nodiscard]] static String makeString(std::pmr::memory_resource* resource) {
        return String(ResolvedPmrResourceTag{}, resource);
    }

    [[nodiscard]] static String makeString(
        std::string_view value, std::pmr::memory_resource* resource) {
        return String(ResolvedPmrResourceTag{}, value, resource);
    }

    template <typename ListT>
    [[nodiscard]] static ListT makeBoxedArray(std::pmr::memory_resource* resource) {
        return ListT(ResolvedPmrResourceTag{}, resource);
    }

    template <typename TargetT>
    static void emplaceParsed(TargetT& target, typename TargetT::value_type&& value) {
        target.emplaceParsed(std::move(value));
    }
};

}  // namespace detail

}  // namespace ruvia
