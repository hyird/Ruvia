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

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/attributes.h"
#include "ruvia/web/detail/model/model_text_storage.h"
#include "ruvia/web/fixed_string.h"

namespace ruvia {

class request_name_value_list;

namespace detail {

template <typename t_type>
inline constexpr bool is_model_narrow_integer =
    std::is_integral_v<t_type> && !std::is_same_v<t_type, bool> && !std::is_same_v<t_type, char> &&
    !std::is_same_v<t_type, wchar_t> && !std::is_same_v<t_type, char8_t> &&
    !std::is_same_v<t_type, char16_t> && !std::is_same_v<t_type, char32_t>;

class model_input;
struct model_value_factory;
struct model_value_rebind_access;

enum class model_string_storage : std::uint8_t {
    borrowed,
    owned,
};

struct model_value_rebind_access final {
    template <typename t_type>
    [[nodiscard]] static consteval bool has_rebind_for_model() {
        using value_t_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_t_type::ruvia_model_schema_type; }) {
            return true;
        } else {
            return requires(const value_t_type& source_value, std::pmr::memory_resource* target) {
                source_value.rebind_for_model(target);
            };
        }
    }

    template <typename t_type>
    [[nodiscard]] static std::remove_cvref_t<t_type> own(
        t_type&& value, std::pmr::memory_resource* resource) {
        using value_t_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_t_type::ruvia_model_schema_type; }) {
            return value_t_type(std::forward<t_type>(value).fields_.rebind(resource));
        } else if constexpr (requires(value_t_type& source_value, std::pmr::memory_resource* target) {
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
        return insert(rebind_model_value(static_cast<input_type>(value), resource));
    };
    if constexpr (sizeof...(argument_types) == 1 &&
                  (std::same_as<std::remove_cvref_t<argument_types>, value_type> && ...)) {
        return own_and_insert(std::forward<argument_types>(arguments)...);
    } else if constexpr (sizeof...(argument_types) == 0 && std::constructible_from<value_type, model_options>) {
        return own_and_insert(value_type(model_options{.resource_ = resource}));
    } else if constexpr (requires {
                             value_type(std::forward<argument_types>(arguments)...,
                                 model_options{.resource_ = resource});
                         }) {
        return own_and_insert(value_type(std::forward<argument_types>(arguments)...,
            model_options{.resource_ = resource}));
    } else {
        return own_and_insert(value_type(std::forward<argument_types>(arguments)...));
    }
}

}  // namespace detail

struct model_parse_options final {
    std::pmr::memory_resource* resource_{nullptr};
    // Totals across the complete typed JSON document, including nested arrays.
    std::size_t max_array_elements_{64 * 1024};
    std::size_t max_representation_bytes_{16 * 1024 * 1024};
};

struct model_serialize_options final {
    std::pmr::memory_resource* resource_{nullptr};
};

class string final {
public:
    explicit string(model_options options = {})
        : string(detail::resolved_pmr_resource_tag{}, detail::pmr_resource_or_default(options.resource_)) {
    }

    string(std::string_view value, model_options options = {})
        : resource_(detail::pmr_resource_or_default(options.resource_)) {
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

    string(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : resource_(resource) {}

    string(detail::resolved_pmr_resource_tag, std::string_view value, std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(value) {}

    string(detail::resolved_pmr_resource_tag, detail::model_text_storage&& storage,
        std::pmr::memory_resource* resource)
        : resource_(resource),
          storage_(std::move(storage)) {}

    [[nodiscard]] string rebind_for_model(std::pmr::memory_resource* resource) const& {
        return string(detail::resolved_pmr_resource_tag{}, storage_.rebind(resource), resource);
    }

    [[nodiscard]] string rebind_for_model(std::pmr::memory_resource* resource) && {
        return string(detail::resolved_pmr_resource_tag{}, std::move(storage_).rebind(resource), resource);
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

struct int8 final {
    std::int8_t value_{0};
    constexpr int8() noexcept = default;
    constexpr int8(std::int8_t input) noexcept
        : value_(input) {}
    template <typename t_type>
        requires(detail::is_model_narrow_integer<t_type> &&
                 !std::is_same_v<std::remove_cv_t<t_type>, std::int8_t>)
    constexpr int8(t_type input)
        : value_(checked(input)) {}
    template <typename t_type>
        requires(!detail::is_model_narrow_integer<t_type>)
    int8(t_type) = delete;
    [[nodiscard]] constexpr operator std::int8_t() const noexcept {
        return value_;
    }

private:
    template <typename t_type>
    static constexpr std::int8_t checked(t_type input) {
        if (!std::in_range<std::int8_t>(input)) {
            throw std::out_of_range("Int8 value is out of range");
        }
        return static_cast<std::int8_t>(input);
    }
};

struct uint8 final {
    std::uint8_t value_{0};
    constexpr uint8() noexcept = default;
    constexpr uint8(std::uint8_t input) noexcept
        : value_(input) {}
    template <typename t_type>
        requires(detail::is_model_narrow_integer<t_type> &&
                 !std::is_same_v<std::remove_cv_t<t_type>, std::uint8_t>)
    constexpr uint8(t_type input)
        : value_(checked(input)) {}
    template <typename t_type>
        requires(!detail::is_model_narrow_integer<t_type>)
    uint8(t_type) = delete;
    [[nodiscard]] constexpr operator std::uint8_t() const noexcept {
        return value_;
    }

private:
    template <typename t_type>
    static constexpr std::uint8_t checked(t_type input) {
        if (!std::in_range<std::uint8_t>(input)) {
            throw std::out_of_range("UInt8 value is out of range");
        }
        return static_cast<std::uint8_t>(input);
    }
};

struct int16 final {
    std::int16_t value_{0};
    constexpr int16() noexcept = default;
    constexpr int16(std::int16_t input) noexcept
        : value_(input) {}
    template <typename t_type>
        requires(detail::is_model_narrow_integer<t_type> &&
                 !std::is_same_v<std::remove_cv_t<t_type>, std::int16_t>)
    constexpr int16(t_type input)
        : value_(checked(input)) {}
    template <typename t_type>
        requires(!detail::is_model_narrow_integer<t_type>)
    int16(t_type) = delete;
    [[nodiscard]] constexpr operator std::int16_t() const noexcept {
        return value_;
    }

private:
    template <typename t_type>
    static constexpr std::int16_t checked(t_type input) {
        if (!std::in_range<std::int16_t>(input)) {
            throw std::out_of_range("Int16 value is out of range");
        }
        return static_cast<std::int16_t>(input);
    }
};

struct uint16 final {
    std::uint16_t value_{0};
    constexpr uint16() noexcept = default;
    constexpr uint16(std::uint16_t input) noexcept
        : value_(input) {}
    template <typename t_type>
        requires(detail::is_model_narrow_integer<t_type> &&
                 !std::is_same_v<std::remove_cv_t<t_type>, std::uint16_t>)
    constexpr uint16(t_type input)
        : value_(checked(input)) {}
    template <typename t_type>
        requires(!detail::is_model_narrow_integer<t_type>)
    uint16(t_type) = delete;
    [[nodiscard]] constexpr operator std::uint16_t() const noexcept {
        return value_;
    }

private:
    template <typename t_type>
    static constexpr std::uint16_t checked(t_type input) {
        if (!std::in_range<std::uint16_t>(input)) {
            throw std::out_of_range("UInt16 value is out of range");
        }
        return static_cast<std::uint16_t>(input);
    }
};

struct int32 final {
    std::int32_t value_{0};
    constexpr int32() noexcept = default;
    constexpr int32(std::int32_t input) noexcept
        : value_(input) {}
    [[nodiscard]] constexpr operator std::int32_t() const noexcept {
        return value_;
    }
};

struct uint32 final {
    std::uint32_t value_{0};
    constexpr uint32() noexcept = default;
    constexpr uint32(std::uint32_t input) noexcept
        : value_(input) {}
    [[nodiscard]] constexpr operator std::uint32_t() const noexcept {
        return value_;
    }
};

struct int64 final {
    std::int64_t value_{0};
    constexpr int64() noexcept = default;
    constexpr int64(std::int64_t input) noexcept
        : value_(input) {}
    [[nodiscard]] constexpr operator std::int64_t() const noexcept {
        return value_;
    }
};

struct uint64 final {
    std::uint64_t value_{0};
    constexpr uint64() noexcept = default;
    constexpr uint64(std::uint64_t input) noexcept
        : value_(input) {}
    [[nodiscard]] constexpr operator std::uint64_t() const noexcept {
        return value_;
    }
};

class bytes final {
public:
    explicit bytes(model_options options = {})
        : resource_(detail::pmr_resource_or_default(options.resource_)),
          items_(resource_) {}

    bytes(std::span<const std::uint8_t> value, model_options options = {})
        : resource_(detail::pmr_resource_or_default(options.resource_)),
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
        : array(detail::resolved_pmr_resource_tag{}, detail::pmr_resource_or_default(options.resource_)) {}

    array(const array&) = delete;
    array& operator=(const array&) = delete;

    array(array&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    array& operator=(array&& other) {
        if (this == &other) {
            return *this;
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
        return detail::emplace_model_value<t_type>(resource_, [this](t_type&& value) -> t_type& { return emplace_parsed(std::move(value)); }, std::forward<args_type>(args)...);
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

    array(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : resource_(resource),
          items_(resource_) {}

    t_type& emplace_parsed(t_type&& value) {
        return items_.emplace_back(std::move(value));
    }

    [[nodiscard]] array rebind_for_model(std::pmr::memory_resource* resource) const& {
        array rebound(detail::resolved_pmr_resource_tag{}, resource);
        rebound.reserve(size());
        for (const auto& value : items_) {
            (void)rebound.emplace_parsed(detail::rebind_model_value(value, resource));
        }
        return rebound;
    }

    [[nodiscard]] array rebind_for_model(std::pmr::memory_resource* resource) && {
        array rebound(detail::resolved_pmr_resource_tag{}, resource);
        rebound.reserve(size());
        for (auto& value : items_) {
            (void)rebound.emplace_parsed(detail::rebind_model_value(std::move(value), resource));
        }
        return rebound;
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
              detail::resolved_pmr_resource_tag{}, detail::pmr_resource_or_default(options.resource_)) {}

    boxed_array(const boxed_array&) = delete;
    boxed_array& operator=(const boxed_array&) = delete;

    boxed_array(boxed_array&& other) noexcept
        : resource_(other.resource_),
          items_(std::move(other.items_)) {}

    boxed_array& operator=(boxed_array&& other) {
        if (this == &other) {
            return *this;
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
            detail::destroy_pmr_object(detail::resolved_pmr_resource_tag{}, value, resource_);
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
                detail::destroy_pmr_object(detail::resolved_pmr_resource_tag{}, value, resource_);
            }
            return;
        }
        while (size() < count) {
            emplace();
        }
    }

    template <typename... args_type>
    t_type& emplace(args_type&&... args) & {
        return detail::emplace_model_value<t_type>(resource_, [this](t_type&& value) -> t_type& { return emplace_parsed(std::move(value)); }, std::forward<args_type>(args)...);
    }

    void push_back(const t_type& value) & {
        (void)emplace(value);
    }

    void push_back(t_type&& value) & {
        (void)emplace(std::move(value));
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

    boxed_array(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
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
        auto* const stored = detail::construct_pmr_object<t_type>(
            detail::resolved_pmr_resource_tag{}, resource_, std::move(value));
        try {
            items_.push_back(stored);
        } catch (...) {
            detail::destroy_pmr_object(detail::resolved_pmr_resource_tag{}, stored, resource_);
            throw;
        }
        return *stored;
    }

    [[nodiscard]] boxed_array rebind_for_model(std::pmr::memory_resource* resource) const& {
        boxed_array rebound(detail::resolved_pmr_resource_tag{}, resource);
        rebound.reserve(size());
        for (const auto& value : *this) {
            rebound.emplace_parsed(detail::rebind_model_value(value, resource));
        }
        return rebound;
    }

    [[nodiscard]] boxed_array rebind_for_model(std::pmr::memory_resource* resource) && {
        boxed_array rebound(detail::resolved_pmr_resource_tag{}, resource);
        rebound.reserve(size());
        for (auto& value : *this) {
            rebound.emplace_parsed(detail::rebind_model_value(std::move(value), resource));
        }
        return rebound;
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

    template <typename list_t_type>
    [[nodiscard]] static list_t_type make_boxed_array(std::pmr::memory_resource* resource) {
        return list_t_type(resolved_pmr_resource_tag{}, resource);
    }

    template <typename target_t_type>
    static void emplace_parsed(target_t_type& target, typename target_t_type::value_type&& value) {
        target.emplace_parsed(std::move(value));
    }
};

}  // namespace detail

}  // namespace ruvia
