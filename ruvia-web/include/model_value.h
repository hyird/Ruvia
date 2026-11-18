#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "binary_text_encoding.h"
#include "borrowed_text.h"
#include "http_header.h"

// Model value types shared by RUVIA_MODEL, SQL entities and Redis entities.
// Kept free of HTTP routing, validation and JSON so data-access headers do not
// inherit them; only unique_lookup_status comes from http_header.h.

namespace ruvia {

class bytes;
}  // namespace ruvia

namespace ruvia::detail {

[[nodiscard]] inline bool model_address_within(const void* address, const void* begin, std::size_t bytes_value) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto first = reinterpret_cast<std::uintptr_t>(begin);
    return value >= first && value - first < bytes_value;
}

// Text semantics (ordinary string or JSON token) belong to the facade. This
// value owns the common borrow/clone/transfer and strong publication rules.
class model_text_storage final {
public:
    explicit model_text_storage(std::string_view borrowed = {}) noexcept :
        value_(std::in_place_type<std::string_view>, borrowed) {}

    model_text_storage(const model_text_storage&) = delete;
    model_text_storage& operator=(const model_text_storage&) = delete;
    model_text_storage(model_text_storage&& other) noexcept :
        value_(std::move(other.value_)) {
        other.assign_borrowed({});
    }

    model_text_storage& operator=(model_text_storage&& other) {
        if (this != &other) {
            if (const auto* borrowed = std::get_if<std::string_view>(&other.value_)) {
                const auto* owned = std::get_if<std::string>(&value_);
                if (owned != nullptr && model_address_within(borrowed->data(), owned->data(), owned->size())) {
                    // Copy before releasing the destination's bytes. If it
                    // throws, neither storage has changed.
                    assign_owned(*borrowed);
                } else {
                    assign_borrowed(*borrowed);
                }
            } else {
                assign_owned(std::move(std::get<std::string>(other.value_)));
            }
            other.assign_borrowed({});
        }
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&value_)) {
            return *borrowed;
        }
        return std::get<std::string>(value_);
    }
    std::string_view view() const&& = delete;

    void assign_borrowed(std::string_view value) noexcept {
        value_.emplace<std::string_view>(value);
    }

    void assign_owned(std::string_view value) {
        std::string owned(value);
        value_.emplace<std::string>(std::move(owned));
    }

    void assign_owned(std::string&& value) noexcept {
        value_.emplace<std::string>(std::move(value));
    }

    void own_borrowed_in_place() {
        if (const auto* borrowed = std::get_if<std::string_view>(&value_)) {
            assign_owned(*borrowed);
        }
    }

    [[nodiscard]] model_text_storage rebind() const& {
        return model_text_storage(std::string(view()));
    }

    [[nodiscard]] std::string take() && {
        own_borrowed_in_place();
        std::string taken(std::move(std::get<std::string>(value_)));
        assign_borrowed({});
        return taken;
    }

    [[nodiscard]] model_text_storage rebind() && {
        return model_text_storage(std::move(*this).take());
    }

    void assign_owned_from(model_text_storage&& source_value) {
        if (this == &source_value) {
            return;
        }
        source_value.own_borrowed_in_place();
        *this = std::move(source_value);
    }

private:
    explicit model_text_storage(std::string&& owned) noexcept :
        value_(std::in_place_type<std::string>, std::move(owned)) {}

    std::variant<std::string_view, std::string> value_;
};

template <typename bytes_type = ::ruvia::bytes>
inline void append_model_binary(std::string& output, const bytes_type& value) {
    const auto encoded_size = http_base64_encoded_size(value.size());
    const auto start = output.size();
    output.resize(start + encoded_size + 2);
    output[start] = '"';
    http_base64_encode(output.data() + start + 1, value.view());
    output[start + encoded_size + 1] = '"';
}

template <typename bytes_type = ::ruvia::bytes>
[[nodiscard]] inline std::optional<bytes_type> decode_model_binary(std::string_view encoded) {
    if (encoded.empty()) {
        return bytes_type();
    }
    if (encoded.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> decoded(http_base64_decoded_size_bound(encoded.size()));
    const auto decoded_size = http_base64_decode(encoded, decoded, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded_size) {
        return std::nullopt;
    }
    decoded.resize(*decoded_size);
    return bytes_type(std::move(decoded));
}

}  // namespace ruvia::detail

namespace ruvia {

namespace detail {

template <typename t_type>
inline constexpr bool is_model_integer = std::is_integral_v<t_type> && !std::is_same_v<t_type, bool> && !std::is_same_v<t_type, char> && !std::is_same_v<t_type, wchar_t> && !std::is_same_v<t_type, char8_t> && !std::is_same_v<t_type, char16_t> && !std::is_same_v<t_type, char32_t>;

template <typename target_type, typename input_type>
[[nodiscard]] constexpr target_type checked_model_integer(input_type input) {
    if (!std::in_range<target_type>(input)) {
        throw std::out_of_range("model integer value is out of range");
    }
    return static_cast<target_type>(input);
}

template <typename target_type, std::floating_point input_type>
[[nodiscard]] constexpr target_type checked_model_float(input_type input) noexcept((std::numeric_limits<input_type>::max)() <= (std::numeric_limits<target_type>::max)()) {
    constexpr auto source_max = (std::numeric_limits<input_type>::max)();
    constexpr auto target_max = (std::numeric_limits<target_type>::max)();
    if constexpr (source_max > target_max) {
        if ((input > target_max || input < -target_max) && input <= source_max && input >= -source_max) {
            throw std::out_of_range("model floating value is out of range");
        }
    }
    return static_cast<target_type>(input);
}

struct model_value_factory;
struct model_value_rebind_access;
struct borrowed_model_token_tag final {};

enum class model_string_storage : std::uint8_t {
    borrowed,
    owned,
};

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
            return requires(const value_type& source_value) { source_value.rebind_for_model(); };
        }
    }

    template <typename t_type>
    static void own_in_place(t_type& value) {
        if constexpr (requires { typename t_type::ruvia_model_schema_type; }) {
            value.fields_.own_in_place();
        } else if constexpr (requires(t_type& target) { target.own_for_model(); }) {
            value.own_for_model();
        }
    }

    template <typename t_type>
    [[nodiscard]] static std::remove_cvref_t<t_type> own(t_type&& value) {
        using value_type = std::remove_cvref_t<t_type>;
        if constexpr (requires { typename value_type::ruvia_model_schema_type; }) {
            return value_type(std::forward<t_type>(value).fields_.rebind());
        } else if constexpr (requires(value_type& source_value) { source_value.rebind_for_model(); }) {
            if constexpr (std::is_lvalue_reference_v<t_type&&> || std::is_const_v<std::remove_reference_t<t_type>>) {
                return value.rebind_for_model();
            } else {
                return std::move(value).rebind_for_model();
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
            return requires(const value_type& source_value, const void* address) { source_value.owns_for_model(address); };
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
[[nodiscard]] std::remove_cvref_t<t_type> rebind_model_value(t_type&& value) {
    return model_value_rebind_access::own(std::forward<t_type>(value));
}

template <typename value_type, typename factory_type>
struct model_value_construction final {
    factory_type factory_;

    operator value_type() && {
        return factory_();
    }
};

// Borrowed/move-only values are normalized only after the container has allocated
// their final storage. In particular, a failed growth must not consume an input
// that aliases an existing element. The captured arguments are consumed inline.
template <typename value_type, typename... argument_types>
auto prepare_model_value(const void* destination, argument_types&&... arguments) {
    if constexpr (sizeof...(argument_types) == 1 && ((std::same_as<std::remove_cvref_t<argument_types>, value_type> && !std::is_lvalue_reference_v<argument_types> && !std::is_const_v<std::remove_reference_t<argument_types>>) && ...)) {
        if ((model_value_rebind_access::contains(arguments, destination) || ...)) {
            throw_model_move_source_owns_destination();
        }
    }
    auto construct = [&arguments...]() -> value_type {
        if constexpr (sizeof...(argument_types) == 1 && (std::same_as<std::remove_cvref_t<argument_types>, value_type> && ...)) {
            return [&]<typename input_type>(input_type&& value) -> value_type {
                if constexpr (std::is_lvalue_reference_v<input_type&&> || std::is_const_v<std::remove_reference_t<input_type>>) {
                    return rebind_model_value(static_cast<const value_type&>(value));
                } else {
                    return rebind_model_value(std::move(value));
                }
            }(std::forward<argument_types>(arguments)...);
        } else if constexpr (sizeof...(argument_types) == 0 && std::default_initializable<value_type>) {
            return value_type();
        } else {
            return value_type(std::forward<argument_types>(arguments)...);
        }
    };
    if constexpr (model_value_rebind_access::has_rebind_for_model<value_type>()) {
        return model_value_construction<value_type, decltype(construct)>{std::move(construct)};
    } else {
        // Scalar constructors intentionally reject non-numeric conversions.
        return construct();
    }
}

}  // namespace detail

class string final {
public:
    string() noexcept = default;

    string(std::string_view value) {
        storage_.assign_owned(value);
    }

    template <std::same_as<std::string> string_type>
    string(string_type&& value) noexcept {
        storage_.assign_owned(std::move(value));
    }

    string(const string&) = delete;
    string& operator=(const string&) = delete;

    string(string&& other) noexcept = default;

    string& operator=(string&& other) {
        storage_.assign_owned_from(std::move(other.storage_));
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

    void assign_owned(std::string_view value) & {
        storage_.assign_owned(value);
    }
    void assign_owned(std::string_view) && = delete;

    template <std::same_as<std::string> string_type>
    void assign_owned(string_type&& value) & noexcept {
        storage_.assign_owned(std::move(value));
    }
    template <std::same_as<std::string> string_type>
    void assign_owned(string_type&&) && = delete;

    [[nodiscard]] std::string take_string() && {
        return std::move(storage_).take();
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    string(detail::borrowed_model_token_tag, std::string_view value) noexcept :
        storage_(value) {}

    explicit string(detail::model_text_storage&& storage) noexcept :
        storage_(std::move(storage)) {}

    void own_for_model() {
        storage_.own_borrowed_in_place();
    }

    [[nodiscard]] string rebind_for_model() const& {
        return string(storage_.rebind());
    }

    [[nodiscard]] string rebind_for_model() && {
        return string(std::move(storage_).rebind());
    }

    detail::model_text_storage storage_;
};

struct bool_value final {
    bool value_{false};
    constexpr bool_value() noexcept = default;
    constexpr bool_value(bool input) noexcept :
        value_(input) {}
    [[nodiscard]] constexpr operator bool() const noexcept {
        return value_;
    }
};

struct double_value;

// Finite floating inputs are checked before narrowing; explicit nonfinite
// inputs and the conversion semantics of user-defined inputs are preserved.
struct float_value final {
    float value_{0};
    constexpr float_value() noexcept = default;
    constexpr float_value(float input) noexcept :
        value_(input) {}
    template <std::floating_point input_type>
        requires(!std::same_as<input_type, float>)
    constexpr float_value(input_type input) noexcept(noexcept(detail::checked_model_float<float>(input))) :
        value_(detail::checked_model_float<float>(input)) {}
    constexpr float_value(const double_value& input);
    [[nodiscard]] constexpr operator float() const noexcept {
        return value_;
    }
};

struct double_value final {
    double value_{0};
    constexpr double_value() noexcept = default;
    constexpr double_value(double input) noexcept :
        value_(input) {}
    template <std::floating_point input_type>
        requires(!std::same_as<input_type, double>)
    constexpr double_value(input_type input) noexcept(noexcept(detail::checked_model_float<double>(input))) :
        value_(detail::checked_model_float<double>(input)) {}
    [[nodiscard]] constexpr operator double() const noexcept {
        return value_;
    }
};

constexpr float_value::float_value(const double_value& input) :
    value_(detail::checked_model_float<float>(input.value_)) {}

// Accepts its exact integer type unchecked, other integers after a range
// check, and rejects every non-integer type at compile time.
template <typename t_type>
struct integer_value final {
    t_type value_{0};
    constexpr integer_value() noexcept = default;
    constexpr integer_value(t_type input) noexcept :
        value_(input) {}
    template <typename input_type>
        requires(detail::is_model_integer<input_type> && !std::is_same_v<std::remove_cv_t<input_type>, t_type>)
    constexpr integer_value(input_type input) :
        value_(detail::checked_model_integer<t_type>(input)) {}
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
    bytes() noexcept = default;

    bytes(std::span<const std::uint8_t> value) :
        items_(value.begin(), value.end()) {}

    template <std::same_as<std::vector<std::uint8_t>> vector_type>
    bytes(vector_type&& value) noexcept :
        items_(std::move(value)) {}

    bytes(const bytes&) = delete;
    bytes& operator=(const bytes&) = delete;

    bytes(bytes&& other) noexcept :
        items_(std::move(other.items_)) {
        other.items_.clear();
    }

    bytes& operator=(bytes&& other) noexcept {
        if (this != &other) {
            items_ = std::move(other.items_);
            other.items_.clear();
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::uint8_t> view() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_;
    }
    [[nodiscard]] std::span<const std::uint8_t> view() const&& = delete;
    [[nodiscard]] const std::uint8_t* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    [[nodiscard]] const std::uint8_t* data() const&& = delete;
    operator std::span<const std::uint8_t>() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    operator std::span<const std::uint8_t>() const&& = delete;
    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    void assign_owned(std::span<const std::uint8_t> value) & {
        std::vector<std::uint8_t> owned(value.begin(), value.end());
        items_ = std::move(owned);
    }
    void assign_owned(std::span<const std::uint8_t>) && = delete;

    template <std::same_as<std::vector<std::uint8_t>> vector_type>
    void assign_owned(vector_type&& value) & noexcept {
        items_ = std::move(value);
    }
    template <std::same_as<std::vector<std::uint8_t>> vector_type>
    void assign_owned(vector_type&&) && = delete;

    [[nodiscard]] std::vector<std::uint8_t> take_bytes() && noexcept {
        std::vector<std::uint8_t> taken(std::move(items_));
        items_.clear();
        return taken;
    }

    friend bool operator==(const bytes& left, const bytes& right) noexcept {
        return left.size() == right.size() && std::equal(left.view().begin(), left.view().end(), right.view().begin());
    }

private:
    friend struct detail::model_value_rebind_access;

    [[nodiscard]] bytes rebind_for_model() const& {
        return bytes(view());
    }

    [[nodiscard]] bytes rebind_for_model() && noexcept {
        bytes rebound;
        rebound.items_ = std::move(items_);
        items_.clear();
        return rebound;
    }

    std::vector<std::uint8_t> items_;
};

// array and boxed_array leave a distinct source empty on move assignment.
// Strings and models are rebound to own their data. A source that owns the
// destination is rejected; failed rebinding preserves the destination.
template <typename t_type>
class array final {
public:
    using value_type = t_type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = t_type&;
    using const_reference = const t_type&;
    using iterator = typename std::vector<t_type>::iterator;
    using const_iterator = typename std::vector<t_type>::const_iterator;

    array() noexcept = default;

    array(const array&) = delete;
    array& operator=(const array&) = delete;

    array(array&& other) noexcept :
        items_(std::move(other.items_)) {
        other.items_.clear();
    }

    array& operator=(array&& other) {
        if (this == &other) {
            return *this;
        }
        // Consuming a source that owns this array would destroy the destination.
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }

        auto rebound = std::move(other).rebind_for_model();
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

    void reserve(size_type count) & {
        items_.reserve(count);
    }
    void reserve(size_type) && = delete;

    void clear() & noexcept {
        items_.clear();
    }
    void clear() && = delete;

    void resize(size_type count) & {
        const auto old_size = size();
        if (count < old_size) {
            items_.resize(count);
            return;
        }
        reserve(count);
        try {
            while (size() < count) {
                emplace_back();
            }
        } catch (...) {
            items_.resize(old_size);
            throw;
        }
    }
    void resize(size_type) && = delete;

    void resize(size_type count, const t_type& value) & {
        const auto old_size = size();
        if (count <= old_size) {
            if (count < old_size) {
                items_.resize(count);
            }
            return;
        }
        t_type stable = detail::rebind_model_value(value);
        reserve(count);
        try {
            while (size() < count) {
                push_back(stable);
            }
        } catch (...) {
            items_.resize(old_size);
            throw;
        }
    }
    void resize(size_type, const t_type&) && = delete;

    [[nodiscard]] std::vector<t_type> take_items() && {
        own_for_model();
        return std::exchange(items_, {});
    }

    template <typename... args_type>
    t_type& emplace_back(args_type&&... args) & RUVIA_LIFETIMEBOUND {
        return items_.emplace_back(detail::prepare_model_value<t_type>(this, std::forward<args_type>(args)...));
    }
    template <typename... args_type>
    t_type& emplace_back(args_type&&...) && = delete;

    void push_back(const t_type& value) & {
        (void)emplace_back(value);
    }
    void push_back(const t_type&) && = delete;

    void push_back(t_type&& value) & {
        (void)emplace_back(std::move(value));
    }
    void push_back(t_type&&) && = delete;

    friend bool operator==(const array& left, const array& right) {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    t_type& emplace_parsed(t_type&& value) {
        return items_.emplace_back(std::move(value));
    }

    [[nodiscard]] array rebind_for_model() const& {
        array rebound;
        rebound.reserve(size());
        for (const auto& value : items_) {
            (void)rebound.emplace_parsed(detail::rebind_model_value(value));
        }
        return rebound;
    }

    void own_for_model() {
        if constexpr (detail::model_value_rebind_access::has_rebind_for_model<t_type>()) {
            for (auto& value : items_) {
                detail::model_value_rebind_access::own_in_place(value);
            }
        }
    }

    [[nodiscard]] array rebind_for_model() && {
        own_for_model();
        array rebound;
        rebound.items_ = std::move(items_);
        clear();
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

    std::vector<t_type> items_;
};

template <typename t_type>
class boxed_array final {
    template <bool const_value>
    class iterator_type;

public:
    using value_type = t_type;

    boxed_array() noexcept = default;

    boxed_array(const boxed_array&) = delete;
    boxed_array& operator=(const boxed_array&) = delete;

    boxed_array(boxed_array&& other) noexcept :
        items_(std::move(other.items_)) {
        other.items_.clear();
    }

    boxed_array& operator=(boxed_array&& other) {
        if (this == &other) {
            return *this;
        }
        // Consuming a source that owns this array would destroy the destination.
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }

        auto rebound = std::move(other).rebind_for_model();
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

    void clear() & noexcept {
        for (auto* value : items_) {
            delete value;
        }
        items_.clear();
    }
    void clear() && = delete;

    void reserve(std::size_t count) & {
        items_.reserve(count);
    }
    void reserve(std::size_t) && = delete;

    void resize(std::size_t count) & {
        const auto old_size = size();
        if (count < old_size) {
            while (size() > count) {
                auto* value = items_.back();
                items_.pop_back();
                delete value;
            }
            return;
        }
        reserve(count);
        try {
            while (size() < count) {
                emplace_back();
            }
        } catch (...) {
            while (size() > old_size) {
                auto* value = items_.back();
                items_.pop_back();
                delete value;
            }
            throw;
        }
    }
    void resize(std::size_t) && = delete;

    template <typename... args_type>
    t_type& emplace_back(args_type&&... args) & RUVIA_LIFETIMEBOUND {
        return emplace_parsed(detail::prepare_model_value<t_type>(this, std::forward<args_type>(args)...));
    }
    template <typename... args_type>
    t_type& emplace_back(args_type&&...) && = delete;

    void push_back(const t_type& value) & {
        (void)emplace_back(value);
    }
    void push_back(const t_type&) && = delete;

    void push_back(t_type&& value) & {
        (void)emplace_back(std::move(value));
    }
    void push_back(t_type&&) && = delete;

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

    template <bool const_value>
    class iterator_type final {
    public:
        using inner_iterator_type = std::conditional_t<const_value, typename std::vector<t_type*>::const_iterator, typename std::vector<t_type*>::iterator>;
        using difference_type = typename inner_iterator_type::difference_type;
        using value_type = t_type;
        using reference = std::conditional_t<const_value, const t_type&, t_type&>;
        using pointer = std::conditional_t<const_value, const t_type*, t_type*>;
        using iterator_category = std::forward_iterator_tag;

        iterator_type() noexcept = default;

        explicit iterator_type(inner_iterator_type current) noexcept :
            current_(current) {}

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

    template <typename input_type>
    t_type& emplace_parsed(input_type&& value) {
        // Admit the pointer slot before constructing a box can consume its input.
        if (size() == items_.capacity()) {
            if (size() == items_.max_size()) {
                throw std::length_error("model boxed array is too large");
            }
            const auto growth = std::min(std::max(size(), std::size_t{1}), items_.max_size() - size());
            items_.reserve(size() + growth);
        }
        auto* const stored = new t_type(std::forward<input_type>(value));
        items_.push_back(stored);
        return *stored;
    }

    [[nodiscard]] boxed_array rebind_for_model() const& {
        boxed_array rebound;
        rebound.reserve(size());
        for (const auto& value : *this) {
            rebound.emplace_parsed(detail::rebind_model_value(value));
        }
        return rebound;
    }

    void own_for_model() {
        if constexpr (detail::model_value_rebind_access::has_rebind_for_model<t_type>()) {
            for (auto* value : items_) {
                detail::model_value_rebind_access::own_in_place(*value);
            }
        }
    }

    [[nodiscard]] boxed_array rebind_for_model() && {
        own_for_model();
        boxed_array rebound;
        rebound.items_ = std::move(items_);
        items_.clear();
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

    std::vector<t_type*> items_;
};

namespace detail {

struct model_value_factory final {
    [[nodiscard]] static string make_borrowed_string(std::string_view value) noexcept {
        return string(borrowed_model_token_tag{}, value);
    }

    template <typename target_type>
    static void emplace_parsed(target_type& target, typename target_type::value_type&& value) {
        target.emplace_parsed(std::move(value));
    }
};

template <typename>
inline constexpr bool always_false = false;

template <typename t_type>
inline constexpr bool is_ruvia_string = std::is_same_v<std::remove_cvref_t<t_type>, string>;

template <typename t_type>
inline constexpr bool is_ruvia_bytes = std::is_same_v<std::remove_cvref_t<t_type>, bytes>;

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

}  // namespace detail
}  // namespace ruvia
