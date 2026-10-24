#pragma once

#include <algorithm>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
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

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/detail/model/model_text_storage.h"
#include "ruvia/web/fixed_string.h"

namespace ruvia {

namespace detail {

template <typename t_type>
inline constexpr bool is_model_integer =
    std::is_integral_v<t_type> && !std::is_same_v<t_type, bool> && !std::is_same_v<t_type, char> &&
    !std::is_same_v<t_type, wchar_t> && !std::is_same_v<t_type, char8_t> &&
    !std::is_same_v<t_type, char16_t> && !std::is_same_v<t_type, char32_t>;

template <typename target_type, typename input_type>
[[nodiscard]] constexpr target_type checked_model_integer(input_type input) {
    if (!std::in_range<target_type>(input)) {
        throw std::out_of_range("model integer value is out of range");
    }
    return static_cast<target_type>(input);
}

struct model_value_factory;
struct model_value_rebind_access;

enum class model_string_storage : std::uint8_t {
    borrowed,
    owned,
};

[[nodiscard]] inline bool model_address_within(
    const void* address, const void* begin, std::size_t bytes_value) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto first = reinterpret_cast<std::uintptr_t>(begin);
    return value >= first && value - first < bytes_value;
}

[[noreturn]] inline void throw_model_move_source_owns_destination() {
    throw std::invalid_argument("model move source owns its destination");
}

struct model_value_rebind_access final {
    template <typename t_type>
    [[nodiscard]] static consteval bool has_rebind_for_model() {
        using value_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_type::ruvia_model_schema_type; }) {
            return true;
        } else {
            return requires(const value_type& source_value, std::pmr::memory_resource* target) {
                source_value.rebind_for_model(target);
            };
        }
    }

    template <typename t_type>
    [[nodiscard]] static std::remove_cvref_t<t_type> own(
        t_type&& value, std::pmr::memory_resource* resource) {
        using value_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_type::ruvia_model_schema_type; }) {
            return value_type(std::forward<t_type>(value).fields_.rebind(resource));
        } else if constexpr (requires(value_type& source_value, std::pmr::memory_resource* target) {
                                 source_value.rebind_for_model(target);
                             }) {
            if constexpr (std::is_lvalue_reference_v<t_type&&>) {
                return value.rebind_for_model(resource);
            } else {
                return std::move(value).rebind_for_model(resource);
            }
        } else {
            return std::forward<t_type>(value);
        }
    }

    template <typename t_type>
    [[nodiscard]] static consteval bool can_own() {
        using value_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_type::ruvia_model_schema_type; }) {
            return true;
        } else {
            return requires(const value_type& source_value, const void* address) {
                source_value.owns_for_model(address);
            };
        }
    }

    // Whether `address` lies in storage that `value` owns, excluding its own
    // inline bytes. Consuming a value destroys everything it owns.
    template <typename t_type>
    [[nodiscard]] static bool owns(const t_type& value, const void* address) noexcept {
        if constexpr (requires { typename t_type::ruvia_model_schema_type; }) {
            return value.fields_.owns(address);
        } else if constexpr (can_own<t_type>()) {
            return value.owns_for_model(address);
        } else {
            return false;
        }
    }

    // Whether consuming `value` would destroy the object at `address`.
    template <typename t_type>
    [[nodiscard]] static bool contains(const t_type& value, const void* address) noexcept {
        return model_address_within(address, std::addressof(value), sizeof(t_type)) || owns(value, address);
    }
};

template <typename t_type>
[[nodiscard]] std::remove_cvref_t<t_type> rebind_model_value(
    t_type&& value, std::pmr::memory_resource* resource) {
    return model_value_rebind_access::own(std::forward<t_type>(value), resource);
}

}  // namespace detail

struct model_options final {
    std::pmr::memory_resource* resource_{nullptr};
};

namespace detail {

// Construction is shared; containers decide how the normalized value is owned
// (inline vector element or address-stable PMR box). `destination` is the
// container, which a transferred value must not own.
template <typename value_type, typename insert_type, typename... argument_types>
value_type& emplace_model_value(std::pmr::memory_resource* resource, const void* destination,
    insert_type insert, argument_types&&... arguments) {
    if constexpr (sizeof...(argument_types) == 1 &&
                  (std::same_as<std::remove_cvref_t<argument_types>, value_type> && ...)) {
        return [&]<typename input_type>(input_type&& value) -> value_type& {
            if constexpr (std::is_lvalue_reference_v<input_type&&> ||
                          std::is_const_v<std::remove_reference_t<input_type>>) {
                // Borrowed/const inputs are cloned.
                return insert(rebind_model_value(static_cast<const value_type&>(value), resource));
            } else {
                // Mutable rvalues transfer, unless consuming one would destroy the container.
                if (model_value_rebind_access::contains(value, destination)) {
                    throw_model_move_source_owns_destination();
                }
                return insert(rebind_model_value(std::move(value), resource));
            }
        }(std::forward<argument_types>(arguments)...);
    } else if constexpr (sizeof...(argument_types) == 0 && std::constructible_from<value_type, model_options>) {
        // Fresh values are built owning on the destination resource.
        return insert(value_type(model_options{.resource_ = resource}));
    } else if constexpr (requires {
                             value_type(std::forward<argument_types>(arguments)...,
                                 model_options{.resource_ = resource});
                         }) {
        return insert(value_type(std::forward<argument_types>(arguments)...,
            model_options{.resource_ = resource}));
    } else {
        return insert(rebind_model_value(value_type(std::forward<argument_types>(arguments)...), resource));
    }
}

}  // namespace detail

struct model_parse_options final {
    std::pmr::memory_resource* resource_{nullptr};
    // Totals across the complete typed or dynamic JSON document, including nested arrays.
    std::size_t max_array_elements_{64 * 1024};
    std::size_t max_representation_bytes_{16 * 1024 * 1024};
};

class string final {
public:
    explicit string(model_options options = {})
        : string(resolved_pmr_resource_tag{}, pmr_resource_or_default(options.resource_)) {
    }

    string(std::string_view value, model_options options = {})
        : resource_(pmr_resource_or_default(options.resource_)) {
        storage_.assign_owned(value, resource_);
    }

    string(const string&) = delete;
    string& operator=(const string&) = delete;

    string(string&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    string& operator=(string&& other) {
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

    friend bool operator==(const string& left, const string& right) noexcept {
        return left.view() == right.view();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    void assign_owned(std::string_view value) {
        storage_.assign_owned(value, resource_);
    }

    void assign_owned(std::pmr::string&& value) {
        storage_.assign_owned(std::move(value), resource_);
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    string(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : resource_(resource) {}

    string(resolved_pmr_resource_tag, std::string_view value, std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(value) {}

    string(resolved_pmr_resource_tag, detail::model_text_storage&& storage,
        std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(std::move(storage)) {}

    [[nodiscard]] string rebind_for_model(std::pmr::memory_resource* resource) const& {
        return string(resolved_pmr_resource_tag{}, storage_.rebind(resource), resource);
    }

    [[nodiscard]] string rebind_for_model(std::pmr::memory_resource* resource) && {
        return string(resolved_pmr_resource_tag{}, std::move(storage_).rebind(resource), resource);
    }

    std::pmr::memory_resource* resource_;
    detail::model_text_storage storage_;
};

struct bool_value final {
    bool value_{false};
    constexpr bool_value() noexcept = default;
    constexpr bool_value(bool input) noexcept
        : value_(input) {}
    [[nodiscard]] constexpr operator bool() const noexcept {
        return value_;
    }
};

struct float_value final {
    float value_{0};
    constexpr float_value() noexcept = default;
    constexpr float_value(float input) noexcept
        : value_(input) {}
    [[nodiscard]] constexpr operator float() const noexcept {
        return value_;
    }
};

struct double_value final {
    double value_{0};
    constexpr double_value() noexcept = default;
    constexpr double_value(double input) noexcept
        : value_(input) {}
    [[nodiscard]] constexpr operator double() const noexcept {
        return value_;
    }
};

// Accepts its exact integer type unchecked, other integers after a range
// check, and rejects every non-integer type at compile time.
template <typename t_type>
struct integer_value final {
    t_type value_{0};
    constexpr integer_value() noexcept = default;
    constexpr integer_value(t_type input) noexcept
        : value_(input) {}
    template <typename input_type>
        requires(detail::is_model_integer<input_type> &&
                 !std::is_same_v<std::remove_cv_t<input_type>, t_type>)
    constexpr integer_value(input_type input)
        : value_(detail::checked_model_integer<t_type>(input)) {}
    template <typename input_type>
        requires(!detail::is_model_integer<input_type>)
    integer_value(input_type) = delete;
    [[nodiscard]] constexpr operator t_type() const noexcept {
        return value_;
    }
};

using int8 = integer_value<std::int8_t>;
using uint8 = integer_value<std::uint8_t>;
using int16 = integer_value<std::int16_t>;
using uint16 = integer_value<std::uint16_t>;
using int32 = integer_value<std::int32_t>;
using uint32 = integer_value<std::uint32_t>;
using int64 = integer_value<std::int64_t>;
using uint64 = integer_value<std::uint64_t>;

class bytes final {
public:
    explicit bytes(model_options options = {})
        : resource_(pmr_resource_or_default(options.resource_)),
          items_(resource_) {}

    bytes(std::span<const std::uint8_t> value, model_options options = {})
        : resource_(pmr_resource_or_default(options.resource_)),
          items_(value.begin(), value.end(), resource_) {}

    bytes(const bytes&) = delete;
    bytes& operator=(const bytes&) = delete;

    bytes(bytes&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    bytes& operator=(bytes&& other) {
        if (this == &other) {
            return *this;
        }
        auto rebound = std::move(other).rebind_for_model(resource_);
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

    void assign_owned(std::span<const std::uint8_t> value) {
        std::pmr::vector<std::uint8_t> owned(value.begin(), value.end(), resource_);
        items_ = std::move(owned);
    }

    void assign_owned(std::pmr::vector<std::uint8_t>&& value) {
        if (value.get_allocator().resource() == resource_) {
            items_ = std::move(value);
            return;
        }
        std::pmr::vector<std::uint8_t> owned(value.begin(), value.end(), resource_);
        items_ = std::move(owned);
    }

    friend bool operator==(const bytes& left, const bytes& right) noexcept {
        return left.size() == right.size() &&
               std::equal(left.view().begin(), left.view().end(), right.view().begin());
    }

private:
    friend struct detail::model_value_rebind_access;

    [[nodiscard]] bytes rebind_for_model(std::pmr::memory_resource* resource) const& {
        return bytes(view(), {.resource_ = resource});
    }

    [[nodiscard]] bytes rebind_for_model(std::pmr::memory_resource* resource) && {
        bytes rebound(model_options{.resource_ = resource});
        if (resource_ == resource) {
            rebound.items_ = std::move(items_);
        } else {
            rebound.assign_owned(view());
        }
        return rebound;
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<std::uint8_t> items_;
};

// array and boxed_array keep the destination resource on move assignment and
// leave a distinct source empty on success. Scalars on the same resource transfer storage;
// strings and models are rebound to own their data. A source that owns the
// destination is rejected; failed rebinding preserves the destination.
template <typename t_type>
class array final {
public:
    using value_type = t_type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = t_type&;
    using const_reference = const t_type&;
    using iterator = typename std::pmr::vector<t_type>::iterator;
    using const_iterator = typename std::pmr::vector<t_type>::const_iterator;

    explicit array(model_options options = {})
        : array(resolved_pmr_resource_tag{}, pmr_resource_or_default(options.resource_)) {}

    array(const array&) = delete;
    array& operator=(const array&) = delete;

    array(array&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    array& operator=(array&& other) {
        if (this == &other) {
            return *this;
        }
        // Consuming a source that owns this array would destroy the destination.
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }

        auto rebound = [&]() {
            if constexpr (detail::model_value_rebind_access::has_rebind_for_model<t_type>()) {
                return other.rebind_for_model(resource_);
            } else {
                return std::move(other).rebind_for_model(resource_);
            }
        }();
        other.clear();
        items_ = std::move(rebound.items_);
        return *this;
    }

    // User-provided, not defaulted: a defaulted destructor would be constexpr
    // and clang instantiates the vector destructor at first use, while a
    // forward-declared element type may still be incomplete.
    ~array() {}

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

    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> get_allocator() const noexcept {
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

    void resize(size_type count, const t_type& value) {
        if (count <= size()) {
            if (count < size()) {
                items_.resize(count);
            }
            return;
        }
        t_type stable = detail::rebind_model_value(value, resource_);
        while (size() < count) {
            push_back(stable);
        }
    }

    template <typename... args_type>
    t_type& emplace_back(args_type&&... args) & {
        return detail::emplace_model_value<t_type>(resource_, this, [this](t_type&& value) -> t_type& { return emplace_parsed(std::move(value)); }, std::forward<args_type>(args)...);
    }

    void push_back(const t_type& value) & {
        (void)emplace_back(value);
    }

    void push_back(t_type&& value) & {
        (void)emplace_back(std::move(value));
    }

    friend bool operator==(const array& left, const array& right) {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    array(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : resource_(resource),
          items_(resource_) {}

    t_type& emplace_parsed(t_type&& value) {
        return items_.emplace_back(std::move(value));
    }

    [[nodiscard]] array rebind_for_model(std::pmr::memory_resource* resource) const& {
        array rebound(resolved_pmr_resource_tag{}, resource);
        rebound.reserve(size());
        for (const auto& value : items_) {
            (void)rebound.emplace_parsed(detail::rebind_model_value(value, resource));
        }
        return rebound;
    }

    [[nodiscard]] array rebind_for_model(std::pmr::memory_resource* resource) && {
        array rebound(resolved_pmr_resource_tag{}, resource);
        if constexpr (!detail::model_value_rebind_access::has_rebind_for_model<t_type>()) {
            if (resource == resource_) {
                rebound.items_ = std::move(items_);
                return rebound;
            }
        }
        rebound.reserve(size());
        for (auto& value : items_) {
            (void)rebound.emplace_parsed(detail::rebind_model_value(std::move(value), resource));
        }
        return rebound;
    }

    [[nodiscard]] bool owns_for_model(const void* address) const noexcept {
        if (detail::model_address_within(address, items_.data(), items_.size() * sizeof(t_type))) {
            return true;
        }
        if constexpr (detail::model_value_rebind_access::can_own<t_type>()) {
            for (const auto& value : items_) {
                if (detail::model_value_rebind_access::owns(value, address)) {
                    return true;
                }
            }
        }
        return false;
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<t_type> items_;
};

template <typename t_type>
class boxed_array final {
    template <bool const_value>
    class iterator_type;

public:
    using value_type = t_type;

    explicit boxed_array(model_options options = {})
        : boxed_array(
              resolved_pmr_resource_tag{}, pmr_resource_or_default(options.resource_)) {}

    boxed_array(const boxed_array&) = delete;
    boxed_array& operator=(const boxed_array&) = delete;

    boxed_array(boxed_array&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    boxed_array& operator=(boxed_array&& other) {
        if (this == &other) {
            return *this;
        }
        // Consuming a source that owns this array would destroy the destination.
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }

        auto rebound = [&]() {
            if constexpr (detail::model_value_rebind_access::has_rebind_for_model<t_type>()) {
                return other.rebind_for_model(resource_);
            } else {
                return std::move(other).rebind_for_model(resource_);
            }
        }();
        other.clear();
        clear();
        items_ = std::move(rebound.items_);
        return *this;
    }

    ~boxed_array() {
        clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] const t_type& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_[index];
    }
    [[nodiscard]] t_type& operator[](std::size_t index) & noexcept RUVIA_LIFETIMEBOUND {
        return *items_[index];
    }
    [[nodiscard]] const t_type& operator[](std::size_t) const&& = delete;

    [[nodiscard]] const t_type& front() const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_.front();
    }
    [[nodiscard]] t_type& front() & noexcept RUVIA_LIFETIMEBOUND {
        return *items_.front();
    }
    [[nodiscard]] const t_type& front() const&& = delete;

    [[nodiscard]] const t_type& back() const& noexcept RUVIA_LIFETIMEBOUND {
        return *items_.back();
    }
    [[nodiscard]] t_type& back() & noexcept RUVIA_LIFETIMEBOUND {
        return *items_.back();
    }
    [[nodiscard]] const t_type& back() const&& = delete;

    [[nodiscard]] auto begin() & noexcept RUVIA_LIFETIMEBOUND {
        return iterator_type<false>(items_.begin());
    }
    [[nodiscard]] auto begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return iterator_type<true>(items_.begin());
    }
    void begin() const&& = delete;

    [[nodiscard]] auto end() & noexcept RUVIA_LIFETIMEBOUND {
        return iterator_type<false>(items_.end());
    }
    [[nodiscard]] auto end() const& noexcept RUVIA_LIFETIMEBOUND {
        return iterator_type<true>(items_.end());
    }
    void end() const&& = delete;

    void clear() noexcept {
        for (auto* value : items_) {
            destroy_pmr_object(resolved_pmr_resource_tag{}, value, resource_);
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
                destroy_pmr_object(resolved_pmr_resource_tag{}, value, resource_);
            }
            return;
        }
        while (size() < count) {
            emplace_back();
        }
    }

    template <typename... args_type>
    t_type& emplace_back(args_type&&... args) & {
        return detail::emplace_model_value<t_type>(resource_, this, [this](t_type&& value) -> t_type& { return emplace_parsed(std::move(value)); }, std::forward<args_type>(args)...);
    }

    void push_back(const t_type& value) & {
        (void)emplace_back(value);
    }

    void push_back(t_type&& value) & {
        (void)emplace_back(std::move(value));
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    friend bool operator==(const boxed_array& left, const boxed_array& right) {
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
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    boxed_array(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : resource_(resource),
          items_(resource_) {}

    template <bool const_value>
    class iterator_type final {
    public:
        using inner_iterator_type = std::conditional_t<const_value,
            typename std::pmr::vector<t_type*>::const_iterator,
            typename std::pmr::vector<t_type*>::iterator>;
        using difference_type = typename inner_iterator_type::difference_type;
        using value_type = t_type;
        using reference = std::conditional_t<const_value, const t_type&, t_type&>;
        using pointer = std::conditional_t<const_value, const t_type*, t_type*>;
        using iterator_category = std::forward_iterator_tag;

        iterator_type() noexcept = default;

        explicit iterator_type(inner_iterator_type current) noexcept
            : current_(current) {}

        reference operator*() const noexcept {
            return **current_;
        }

        pointer operator->() const noexcept {
            return *current_;
        }

        iterator_type& operator++() noexcept {
            ++current_;
            return *this;
        }

        iterator_type operator++(int) noexcept {
            auto copy = *this;
            ++current_;
            return copy;
        }

        friend bool operator==(const iterator_type& left, const iterator_type& right) noexcept {
            return left.current_ == right.current_;
        }

    private:
        inner_iterator_type current_{};
    };

    t_type& emplace_parsed(t_type&& value) {
        auto* const stored = construct_pmr_object<t_type>(
            resolved_pmr_resource_tag{}, resource_, std::move(value));
        try {
            items_.push_back(stored);
        } catch (...) {
            destroy_pmr_object(resolved_pmr_resource_tag{}, stored, resource_);
            throw;
        }
        return *stored;
    }

    [[nodiscard]] boxed_array rebind_for_model(std::pmr::memory_resource* resource) const& {
        boxed_array rebound(resolved_pmr_resource_tag{}, resource);
        rebound.reserve(size());
        for (const auto& value : *this) {
            rebound.emplace_parsed(detail::rebind_model_value(value, resource));
        }
        return rebound;
    }

    [[nodiscard]] boxed_array rebind_for_model(std::pmr::memory_resource* resource) && {
        boxed_array rebound(resolved_pmr_resource_tag{}, resource);
        if constexpr (!detail::model_value_rebind_access::has_rebind_for_model<t_type>()) {
            if (resource == resource_) {
                rebound.items_ = std::move(items_);
                items_.clear();
                return rebound;
            }
        }
        rebound.reserve(size());
        for (auto& value : *this) {
            rebound.emplace_parsed(detail::rebind_model_value(std::move(value), resource));
        }
        return rebound;
    }

    [[nodiscard]] bool owns_for_model(const void* address) const noexcept {
        for (const auto* value : items_) {
            if (detail::model_address_within(address, value, sizeof(t_type))) {
                return true;
            }
            if constexpr (detail::model_value_rebind_access::can_own<t_type>()) {
                if (detail::model_value_rebind_access::owns(*value, address)) {
                    return true;
                }
            }
        }
        return false;
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<t_type*> items_;
};

namespace detail {

struct model_value_factory final {
    [[nodiscard]] static string make_string(std::pmr::memory_resource* resource) {
        return string(resolved_pmr_resource_tag{}, resource);
    }

    [[nodiscard]] static string make_string(
        std::string_view value, std::pmr::memory_resource* resource) {
        return string(resolved_pmr_resource_tag{}, value, resource);
    }

    template <typename list_type>
    [[nodiscard]] static list_type make_boxed_array(std::pmr::memory_resource* resource) {
        return list_type(resolved_pmr_resource_tag{}, resource);
    }

    template <typename target_type>
    static void emplace_parsed(target_type& target, typename target_type::value_type&& value) {
        target.emplace_parsed(std::move(value));
    }
};

}  // namespace detail

class json_object;
class json_value;

namespace detail {

enum class model_field_state : std::uint8_t {
    missing,
    parsed,
    null,
    invalid_type,
    duplicate
};

template <typename>
inline constexpr bool always_false = false;

template <typename t_type>
inline constexpr bool is_ruvia_string = std::is_same_v<std::remove_cvref_t<t_type>, string>;

template <typename t_type>
inline constexpr bool is_ruvia_bytes = std::is_same_v<std::remove_cvref_t<t_type>, bytes>;

template <typename t_type>
inline constexpr bool is_ruvia_json_value = std::is_same_v<std::remove_cvref_t<t_type>, json_value>;

template <typename t_type>
inline constexpr bool is_ruvia_json_object = std::is_same_v<std::remove_cvref_t<t_type>, json_object>;

template <typename t_type>
struct ruvia_array_traits : std::false_type {};

template <typename element_type>
struct ruvia_array_traits<array<element_type>> : std::true_type {
    using value_type = element_type;
};
template <typename t_type>
inline constexpr bool is_ruvia_array = ruvia_array_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
struct ruvia_boxed_array_traits : std::false_type {};

template <typename element_type>
struct ruvia_boxed_array_traits<boxed_array<element_type>> : std::true_type {
    using value_type = element_type;
};
template <typename t_type>
inline constexpr bool is_ruvia_boxed_array = ruvia_boxed_array_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
struct ruvia_scalar_traits : std::false_type {};

template <>
struct ruvia_scalar_traits<bool_value> : std::true_type {
    using value_type = bool;
};
template <typename integer_type>
struct ruvia_scalar_traits<integer_value<integer_type>> : std::true_type {
    using value_type = integer_type;
};
template <>
struct ruvia_scalar_traits<float_value> : std::true_type {
    using value_type = float;
};
template <>
struct ruvia_scalar_traits<double_value> : std::true_type {
    using value_type = double;
};

template <typename t_type>
inline constexpr bool is_ruvia_scalar = ruvia_scalar_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
using model_scalar_value_type = typename ruvia_scalar_traits<std::remove_cvref_t<t_type>>::value_type;

template <typename t_type>
inline constexpr bool is_form_field = is_ruvia_string<t_type> || is_ruvia_scalar<t_type>;

template <typename t_type>
inline constexpr bool is_model =
    requires { typename std::remove_cvref_t<t_type>::ruvia_model_schema_type; };

template <typename t_type>
inline constexpr bool model_field_value = is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_ruvia_scalar<t_type> ||
                                          is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type> || is_model<t_type>;

template <typename value_type>
inline constexpr bool model_field_value<array<value_type>> = model_field_value<std::remove_cvref_t<value_type>>;

template <typename value_type>
inline constexpr bool model_field_value<boxed_array<value_type>> = model_field_value<std::remove_cvref_t<value_type>>;

template <typename t_type>
inline constexpr bool is_model_field = model_field_value<std::remove_cvref_t<t_type>>;

template <typename t_type>
inline constexpr bool model_json_value =
    is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_ruvia_scalar<t_type> || is_model<t_type>;

template <typename value_type>
inline constexpr bool model_json_value<array<value_type>> = model_json_value<std::remove_cvref_t<value_type>>;

template <typename value_type>
inline constexpr bool model_json_value<boxed_array<value_type>> = model_json_value<std::remove_cvref_t<value_type>>;

template <typename t_type>
inline constexpr bool is_model_json_value = model_json_value<std::remove_cvref_t<t_type>>;

template <typename t_type>
[[nodiscard]] t_type make_request_value(resolved_pmr_resource_tag, std::pmr::memory_resource* resource) {
    if constexpr (is_ruvia_string<t_type>) {
        return model_value_factory::make_string(resource);
    } else if constexpr (is_ruvia_bytes<t_type>) {
        return t_type(model_options{.resource_ = resource});
    } else if constexpr (is_ruvia_array<t_type>) {
        return t_type(model_options{.resource_ = resource});
    } else if constexpr (is_ruvia_boxed_array<t_type>) {
        return model_value_factory::make_boxed_array<t_type>(resource);
    } else if constexpr (is_model<t_type> || is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        return t_type(model_options{.resource_ = resource});
    } else {
        (void)resource;
        return t_type{};
    }
}

template <typename t_type>
[[nodiscard]] t_type make_request_value(std::pmr::memory_resource* resource) {
    if constexpr (is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type> ||
                  is_model<t_type> || is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        return make_request_value<t_type>(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource));
    } else {
        (void)resource;
        return t_type{};
    }
}

template <typename t_type>
inline constexpr bool is_validation_number =
    std::is_arithmetic_v<std::remove_cvref_t<t_type>> || is_ruvia_scalar<t_type>;

template <typename integer_type, typename floating_type>
[[nodiscard]] std::partial_ordering compare_validation_integer_floating(
    integer_type integer, floating_type floating) noexcept {
    if (std::isnan(floating)) {
        return std::partial_ordering::unordered;
    }

    // The upper bound is exclusive. Unlike max(), this power of two is
    // exactly representable even when the float cannot represent every integer.
    const auto upper = std::ldexp(floating_type{1}, std::numeric_limits<integer_type>::digits);
    if (floating >= upper) {
        return std::partial_ordering::less;
    }
    const auto lower = std::is_signed_v<integer_type> ? -upper : floating_type{0};
    if (floating < lower) {
        return std::partial_ordering::greater;
    }

    // NaN, infinity and out-of-range values have all returned before the cast.
    const auto truncated = static_cast<integer_type>(floating);
    if (std::cmp_less(integer, truncated)) {
        return std::partial_ordering::less;
    }
    if (std::cmp_greater(integer, truncated)) {
        return std::partial_ordering::greater;
    }
    const auto whole = std::trunc(floating);
    if (whole < floating) {
        return std::partial_ordering::less;
    }
    if (whole > floating) {
        return std::partial_ordering::greater;
    }
    return std::partial_ordering::equivalent;
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_less(const left_type& left, const right_type& right) {
    if constexpr (!is_validation_number<left_type> || !is_validation_number<right_type>) {
        return left < right;
    } else if constexpr (is_ruvia_scalar<left_type>) {
        return validation_less(left.value_, right);
    } else if constexpr (is_ruvia_scalar<right_type>) {
        return validation_less(left, right.value_);
    } else if constexpr (std::is_integral_v<left_type> && std::is_integral_v<right_type>) {
        return std::cmp_less(+left, +right);
    } else if constexpr (std::is_integral_v<left_type> && std::is_floating_point_v<right_type>) {
        return compare_validation_integer_floating(+left, right) == std::partial_ordering::less;
    } else if constexpr (std::is_floating_point_v<left_type> && std::is_integral_v<right_type>) {
        return compare_validation_integer_floating(+right, left) == std::partial_ordering::greater;
    } else {
        return left < right;
    }
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_greater(const left_type& left, const right_type& right) {
    if constexpr (is_validation_number<left_type> && is_validation_number<right_type>) {
        return validation_less(right, left);
    } else {
        return left > right;
    }
}

}  // namespace detail

}  // namespace ruvia

#include "ruvia/web/detail/json/json_array_elements.h"
#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_object_fields.h"
#include "ruvia/web/detail/json/json_skip.h"
#include "ruvia/web/detail/model/macro/macros.h"
#include "ruvia/web/detail/model/parse/json_parser.h"
#include "ruvia/web/detail/model/parse/json_writer.h"
#include "ruvia/web/detail/model/parse/model_input_visitors.h"
#include "ruvia/web/detail/model/parse/parser.h"
#include "ruvia/web/detail/model/rule/rules.h"

namespace ruvia::detail {

template <typename view_type>
[[nodiscard]] std::optional<view_type> parse_json_view_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    bool require_object, json_parse_budget& budget);

}

namespace ruvia {

// json_value and json_object borrow their complete input body by default. An
// owning string passed to parse() must therefore outlive the parsed view;
// basic_string rvalues are rejected before a dangling view can be created.
// Model fields may copy the JSON token when the parse owns its strings.
// Dynamic JSON token, not an ordinary field-validation schema. parse() borrows
// the complete token; from_json<Model>() owns it in the model resource instead.
// Move construction preserves borrowing/ownership. Move assignment keeps the
// target resource and owns the result (copying borrowed/incompatible storage).
// Views and potentially borrowed get<T>() results must not outlive their token.
// Default construction represents JSON null.
class json_value final {
public:
    enum class kind_type : unsigned char { object,
        array,
        string,
        number,
        boolean,
        null };

    explicit json_value(model_options options = {})
        : json_value(resolved_pmr_resource_tag{}, "null",
              pmr_resource_or_default(options.resource_)) {}

    [[nodiscard]] static json_value parse(std::string_view body, model_parse_options options = {}) {
        auto input = body;
        detail::json_parse_budget budget(options);
        if (!budget.skip_value(input, 0)) {
            throw std::invalid_argument("invalid JSON value");
        }
        detail::skip_json_whitespace(input);
        if (!input.empty()) {
            throw std::invalid_argument("invalid JSON value");
        }
        return json_value(
            resolved_pmr_resource_tag{}, body, pmr_resource_or_default(options.resource_));
    }

    template <typename traits_type, typename allocator_type>
    static json_value parse(
        std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    template <typename traits_type, typename allocator_type>
    static json_value parse(
        const std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    json_value(const json_value&) = delete;
    json_value& operator=(const json_value&) = delete;

    json_value(json_value&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    json_value& operator=(json_value&& other) {
        storage_.assign_from(std::move(other.storage_), resource_);
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] kind_type kind() const noexcept {
        auto input = view();
        detail::skip_json_whitespace(input);
        if (input.empty()) {
            return kind_type::null;
        }
        switch (input.front()) {
            case '{':
                return kind_type::object;
            case '[':
                return kind_type::array;
            case '"':
                return kind_type::string;
            case 't':
            case 'f':
                return kind_type::boolean;
            case 'n':
                return kind_type::null;
            default:
                return kind_type::number;
        }
    }

    [[nodiscard]] bool is_object() const noexcept {
        return kind() == kind_type::object;
    }

    [[nodiscard]] bool is_array() const noexcept {
        return kind() == kind_type::array;
    }

    [[nodiscard]] bool is_string() const noexcept {
        return kind() == kind_type::string;
    }

    [[nodiscard]] bool is_number() const noexcept {
        return kind() == kind_type::number;
    }

    [[nodiscard]] bool is_boolean() const noexcept {
        return kind() == kind_type::boolean;
    }

    [[nodiscard]] bool is_null() const noexcept {
        return kind() == kind_type::null;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] std::optional<t_type> get() const {
        return detail::parse_json_document<t_type>(view(), resource_, detail::model_string_storage::borrowed);
    }

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] std::optional<t_type> get(std::string_view field) const;

    // The callback receives a json_value borrowing this object's token; it is
    // valid only for the duration of the callback.
    template <typename visitor_type>
    [[nodiscard]] bool for_each_element(visitor_type&& visitor) const;

    // The callback receives a field name and json_value borrowing this object's
    // token; both are valid only for the duration of the callback.
    template <typename visitor_type>
    [[nodiscard]] bool for_each_field(visitor_type&& visitor) const;

    // Compare retained token bytes exactly, ignoring allocator and JSON
    // normalization, to preserve the zero-cost view semantics.
    [[nodiscard]] bool operator==(const json_value& other) const noexcept {
        return view() == other.view();
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;
    friend class json_object;
    template <typename view_type>
    friend std::optional<view_type> detail::parse_json_view_value(std::string_view&,
        std::pmr::memory_resource*, std::size_t, detail::model_string_storage, bool, detail::json_parse_budget&);

    json_value(resolved_pmr_resource_tag, std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : resource_(resource),
          storage_(body) {}

    json_value(resolved_pmr_resource_tag, std::pmr::memory_resource* resource,
        detail::model_text_storage&& storage) noexcept
        : resource_(resource),
          storage_(std::move(storage)) {}

    void assign_owned(std::string_view value) {
        storage_.assign_owned(value, resource_);
    }

    [[nodiscard]] json_value rebind_for_model(std::pmr::memory_resource* resource) const& {
        return json_value(resolved_pmr_resource_tag{}, resource, storage_.rebind(resource));
    }

    [[nodiscard]] json_value rebind_for_model(std::pmr::memory_resource* resource) && {
        return json_value(resolved_pmr_resource_tag{}, resource, std::move(storage_).rebind(resource));
    }

    std::pmr::memory_resource* resource_;
    detail::model_text_storage storage_;
};

// Object-only dynamic token with the same borrowing, ownership and move
// contract as json_value. Supplying a resource to parse() does not copy input.
// Default construction represents an empty JSON object.
class json_object final {
public:
    explicit json_object(model_options options = {})
        : json_object(resolved_pmr_resource_tag{}, "{}",
              pmr_resource_or_default(options.resource_)) {}

    [[nodiscard]] static json_object parse(std::string_view body, model_parse_options options = {}) {
        auto input = body;
        detail::json_parse_budget budget(options);
        detail::skip_json_whitespace(input);
        if (input.empty() || input.front() != '{' || !budget.skip_value(input, 0)) {
            throw std::invalid_argument("invalid JSON object");
        }
        detail::skip_json_whitespace(input);
        if (!input.empty()) {
            throw std::invalid_argument("invalid JSON object");
        }
        return json_object(
            resolved_pmr_resource_tag{}, body, pmr_resource_or_default(options.resource_));
    }

    template <typename traits_type, typename allocator_type>
    static json_object parse(
        std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    template <typename traits_type, typename allocator_type>
    static json_object parse(
        const std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    json_object(const json_object&) = delete;
    json_object& operator=(const json_object&) = delete;

    json_object(json_object&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    json_object& operator=(json_object&& other) {
        storage_.assign_from(std::move(other.storage_), resource_);
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    // The callback receives a field name and json_value borrowing this object's
    // token; both are valid only for the duration of the callback.
    template <typename visitor_type>
    [[nodiscard]] bool for_each_field(visitor_type&& visitor) const;

    // Compare retained token bytes exactly, ignoring allocator and JSON
    // normalization, to preserve the zero-cost view semantics.
    [[nodiscard]] bool operator==(const json_object& other) const noexcept {
        return view() == other.view();
    }

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] std::optional<t_type> get(std::string_view field) const {
        auto* const resource = resource_;
        std::optional<t_type> result;
        bool last_match_failed = false;
        const auto visited = detail::visit_json_object_fields(resolved_pmr_resource_tag{}, view(),
            resource, [&](std::string_view key, std::string_view value_view) {
                if (key != field) {
                    return true;
                }

                detail::skip_json_whitespace(value_view);
                auto value = detail::parse_json_document<t_type>(value_view, resource,
                    detail::model_string_storage::borrowed);
                if (!value) {
                    result.reset();
                    last_match_failed = true;
                    return true;
                }
                result.emplace(std::move(*value));
                last_match_failed = false;
                return true;
            });

        if (visited != detail::json_object_visit_result::complete || last_match_failed) {
            return std::nullopt;
        }
        return result;
    }

private:
    friend class json_value;
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;
    template <typename view_type>
    friend std::optional<view_type> detail::parse_json_view_value(std::string_view&,
        std::pmr::memory_resource*, std::size_t, detail::model_string_storage, bool, detail::json_parse_budget&);

    json_object(resolved_pmr_resource_tag, std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : resource_(resource),
          storage_(body) {}

    json_object(resolved_pmr_resource_tag, std::pmr::memory_resource* resource,
        detail::model_text_storage&& storage) noexcept
        : resource_(resource),
          storage_(std::move(storage)) {}

    void assign_owned(std::string_view value) {
        storage_.assign_owned(value, resource_);
    }

    [[nodiscard]] json_object rebind_for_model(std::pmr::memory_resource* resource) const& {
        return json_object(resolved_pmr_resource_tag{}, resource, storage_.rebind(resource));
    }

    [[nodiscard]] json_object rebind_for_model(std::pmr::memory_resource* resource) && {
        return json_object(resolved_pmr_resource_tag{}, resource, std::move(storage_).rebind(resource));
    }

    std::pmr::memory_resource* resource_;
    detail::model_text_storage storage_;
};

template <typename t_type>
    requires detail::is_model_field<t_type>
[[nodiscard]] inline std::optional<t_type> json_value::get(std::string_view field) const {
    if (!is_object()) {
        return std::nullopt;
    }
    return json_object(resolved_pmr_resource_tag{}, view(), resource_).get<t_type>(field);
}

template <typename visitor_type>
[[nodiscard]] inline bool json_value::for_each_element(visitor_type&& visitor) const {
    if (!is_array()) {
        return false;
    }
    auto input = view();
    if (!detail::consume_json_array_elements(input, 0, [this, &visitor](std::string_view& element_input) {
            const auto start = element_input;
            if (!detail::skip_json_value(element_input, 1)) {
                return false;
            }
            json_value value(resolved_pmr_resource_tag{},
                start.substr(0, start.size() - element_input.size()), resource_);
            return static_cast<bool>(visitor(value));
        })) {
        return false;
    }
    detail::skip_json_whitespace(input);
    return input.empty();
}

template <typename visitor_type>
[[nodiscard]] inline bool json_value::for_each_field(visitor_type&& visitor) const {
    if (!is_object()) {
        return false;
    }

    return detail::visit_json_object_fields(resolved_pmr_resource_tag{}, view(),
               resource_, [&](std::string_view key, std::string_view value_view) {
                   detail::skip_json_whitespace(value_view);
                   json_value value(resolved_pmr_resource_tag{}, value_view, resource_);
                   return static_cast<bool>(visitor(key, value));
               }) == detail::json_object_visit_result::complete;
}

template <typename visitor_type>
[[nodiscard]] inline bool json_object::for_each_field(visitor_type&& visitor) const {
    json_value value(resolved_pmr_resource_tag{}, view(), resource_);
    return value.for_each_field(std::forward<visitor_type>(visitor));
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename view_type>
[[nodiscard]] std::optional<view_type> parse_json_view_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    bool require_object, json_parse_budget& budget) {
    auto remaining = input;
    skip_json_whitespace(remaining);
    if (require_object && (remaining.empty() || remaining.front() != '{')) {
        return std::nullopt;
    }
    const auto start = remaining;
    if (!budget.skip_value(remaining, depth)) {
        return std::nullopt;
    }
    const auto token = start.substr(0, start.size() - remaining.size());
    view_type value(model_options{.resource_ = resource});
    if (string_storage == model_string_storage::owned) {
        if (!budget.consume_bytes(token.size()) ||
            !budget.consume_bytes(token.size() + sizeof(std::pmr::string))) {
            return std::nullopt;
        }
        value.assign_owned(token);
    } else {
        value.storage_.assign_borrowed(token);
    }
    input = remaining;
    return value;
}

template <>
inline std::optional<json_value> parse_json_value<json_value>(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    json_parse_budget& budget) {
    return parse_json_view_value<json_value>(input, resource, depth, string_storage, false, budget);
}

template <>
inline std::optional<json_object> parse_json_value<json_object>(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    json_parse_budget& budget) {
    return parse_json_view_value<json_object>(input, resource, depth, string_storage, true, budget);
}

template <>
inline std::size_t json_size_hint_value<json_value>(const json_value& value) {
    return value.view().size();
}

template <>
inline std::size_t json_size_hint_value<json_object>(const json_object& value) {
    return value.view().size();
}

template <>
inline void append_json_value<json_value>(std::pmr::string& output, const json_value& value) {
    output.append(value.view());
}

template <>
inline void append_json_value<json_object>(std::pmr::string& output, const json_object& value) {
    output.append(value.view());
}

}  // namespace ruvia::detail

namespace ruvia {

// Serializes current values without invoking field rules or initializers.
// The output is always well-formed UTF-8 (RFC 8259 §8.1): a string value holding
// ill-formed UTF-8 has each maximal ill-formed subpart replaced by U+FFFD.
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] inline std::pmr::string to_json(const t_type& value, model_options options = {}) {
    std::pmr::string output(pmr_resource_or_default(options.resource_));
    // MSVC reserve(n) can leave no room for the trailing NUL, so resize(n)
    // allocates again. One extra byte stays in the same allocation.
    output.reserve(detail::json_size_hint_value(value) + 1);
    detail::append_json_value(output, value);
    return output;
}

}  // namespace ruvia

#include "ruvia/web/validation.h"
