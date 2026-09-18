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
#include "ruvia/web/Attributes.h"
#include "ruvia/web/FixedString.h"

namespace ruvia {

class RequestNameValueList;
class JsonValue;

namespace detail {

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
        return requires(const ValueT& source, std::pmr::memory_resource* target) {
            source.rebindForModel(target);
        };
    }

    template <typename T>
    [[nodiscard]] static std::remove_cvref_t<T> own(
        T&& value, std::pmr::memory_resource* resource) {
        using ValueT = std::remove_cvref_t<T>;
        if constexpr (requires(ValueT& source, std::pmr::memory_resource* target) {
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

struct ModelParseOptions final {
    std::pmr::memory_resource* resource{nullptr};
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
        : resource_(detail::pmrResourceOrDefault(options.resource)),
          storage_(std::in_place_type<std::pmr::string>, value, resource_) {}

    String(const String&) = delete;
    String& operator=(const String&) = delete;

    String(String&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    String& operator=(String&& other) {
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
    friend struct detail::ModelValueRebindAccess;

    using Storage = std::variant<std::string_view, std::pmr::string>;

    String(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(std::in_place_type<std::string_view>) {}

    String(
        detail::ResolvedPmrResourceTag, std::string_view value, std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(std::in_place_type<std::string_view>, value) {}

    [[nodiscard]] String rebindForModel(std::pmr::memory_resource* resource) const& {
        String rebound(detail::ResolvedPmrResourceTag{}, resource);
        if (const auto* owned = std::get_if<std::pmr::string>(&storage_)) {
            rebound.storage_.template emplace<std::pmr::string>(*owned, resource);
        } else {
            rebound.assignOwned(std::get<std::string_view>(storage_));
        }
        return rebound;
    }

    [[nodiscard]] String rebindForModel(std::pmr::memory_resource* resource) && {
        String rebound(detail::ResolvedPmrResourceTag{}, resource);
        if (auto* owned = std::get_if<std::pmr::string>(&storage_)) {
            if (owned->get_allocator().resource() == resource) {
                rebound.storage_.template emplace<std::pmr::string>(std::move(*owned));
            } else {
                rebound.storage_.template emplace<std::pmr::string>(*owned, resource);
            }
        } else {
            rebound.assignOwned(std::get<std::string_view>(storage_));
        }
        return rebound;
    }

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
        if constexpr (sizeof...(Args) == 1 &&
                      (std::same_as<std::remove_cvref_t<Args>, T> && ...)) {
            return emplaceOwned(std::forward<Args>(args)...);
        } else if constexpr (sizeof...(Args) == 0 && std::constructible_from<T, ModelOptions>) {
            return emplaceOwned(T(ModelOptions{.resource = resource_}));
        } else if constexpr (requires {
                                 T(std::forward<Args>(args)...,
                                     ModelOptions{.resource = resource_});
                             }) {
            return emplaceOwned(T(std::forward<Args>(args)...,
                ModelOptions{.resource = resource_}));
        } else {
            return emplaceOwned(T(std::forward<Args>(args)...));
        }
    }

    void push_back(const T& value) & {
        (void)emplaceOwned(value);
    }

    void push_back(T&& value) & {
        (void)emplaceOwned(std::move(value));
    }

private:
    friend struct detail::ModelValueFactory;
    friend struct detail::ModelValueRebindAccess;

    Array(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : resource_(resource),
          items_(resource_) {}

    void emplaceParsed(T&& value) {
        items_.emplace_back(std::move(value));
    }

    T& emplaceOwned(const T& value) {
        T rebound = detail::rebindModelValue(value, resource_);
        items_.push_back(std::move(rebound));
        return items_.back();
    }

    T& emplaceOwned(T&& value) {
        T rebound = detail::rebindModelValue(std::move(value), resource_);
        items_.push_back(std::move(rebound));
        return items_.back();
    }

    [[nodiscard]] Array rebindForModel(std::pmr::memory_resource* resource) const& {
        Array rebound(detail::ResolvedPmrResourceTag{}, resource);
        rebound.reserve(size());
        for (const auto& value : items_) {
            rebound.items_.push_back(detail::rebindModelValue(value, resource));
        }
        return rebound;
    }

    [[nodiscard]] Array rebindForModel(std::pmr::memory_resource* resource) && {
        Array rebound(detail::ResolvedPmrResourceTag{}, resource);
        rebound.reserve(size());
        for (auto& value : items_) {
            rebound.items_.push_back(detail::rebindModelValue(std::move(value), resource));
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
        if constexpr (sizeof...(Args) == 1 &&
                      (std::same_as<std::remove_cvref_t<Args>, T> && ...)) {
            return emplaceOwned(std::forward<Args>(args)...);
        } else if constexpr (sizeof...(Args) == 0 && std::constructible_from<T, ModelOptions>) {
            return emplaceOwned(T(ModelOptions{.resource = resource_}));
        } else if constexpr (requires {
                                 T(std::forward<Args>(args)...,
                                     ModelOptions{.resource = resource_});
                             }) {
            return emplaceOwned(T(std::forward<Args>(args)...,
                ModelOptions{.resource = resource_}));
        } else {
            return emplaceOwned(T(std::forward<Args>(args)...));
        }
    }

    void push_back(const T& value) & {
        (void)emplaceOwned(value);
    }

    void push_back(T&& value) & {
        (void)emplaceOwned(std::move(value));
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
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

    T& emplaceOwned(const T& value) {
        T rebound = detail::rebindModelValue(value, resource_);
        return emplaceParsed(std::move(rebound));
    }

    T& emplaceOwned(T&& value) {
        T rebound = detail::rebindModelValue(std::move(value), resource_);
        return emplaceParsed(std::move(rebound));
    }

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

class JsonObject;
class FormObject;

}  // namespace ruvia
