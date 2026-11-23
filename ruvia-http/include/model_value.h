#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include "byte_view.h"
#include "decimal_text.h"
#include "fixed_string.h"
#include "http_header.h"
#include "outbound_bytes.h"

// Model value types shared by RUVIA_MODEL, SQL entities and Redis entities.
// Pure Sans-I/O: native C++ scalars (bool, int8_t..int64_t, uint8_t..uint64_t,
// float, double), ruvia::string, ruvia::bytes (std::byte), ruvia::uuid (RFC 9562),
// ruvia::decimal, std::chrono time/date aliases, RUVIA_ENUM, ruvia::array,
// ruvia::boxed_array, and ruvia::map.

namespace ruvia {

class bytes;
class string;

enum class bytes_encoding : std::uint8_t {
    base64,
    base64url,
    hex,
};

using utc_seconds = std::chrono::sys_seconds;
using utc_milliseconds = std::chrono::sys_time<std::chrono::milliseconds>;
using utc_microseconds = std::chrono::sys_time<std::chrono::microseconds>;
using civil_date = std::chrono::year_month_day;

}  // namespace ruvia

namespace ruvia::detail {

[[nodiscard]] inline bool model_address_within(const void* address, const void* begin, std::size_t bytes_value) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto first = reinterpret_cast<std::uintptr_t>(begin);
    return value >= first && value - first < bytes_value;
}

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

template <typename t_type>
inline constexpr bool is_model_integer = std::is_integral_v<std::remove_cvref_t<t_type>> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, bool> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, char> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, wchar_t> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, char8_t> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, char16_t> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, char32_t>;

template <typename t_type>
inline constexpr bool is_model_floating = std::is_same_v<std::remove_cvref_t<t_type>, float> || std::is_same_v<std::remove_cvref_t<t_type>, double>;

template <typename t_type>
inline constexpr bool is_model_scalar = std::is_same_v<std::remove_cvref_t<t_type>, bool> || is_model_integer<t_type> || is_model_floating<t_type>;

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
        } else if constexpr (sizeof...(argument_types) == 1 && is_model_integer<value_type> && (is_model_integer<argument_types> && ...)) {
            return checked_model_integer<value_type>(std::forward<argument_types>(arguments)...);
        } else if constexpr (sizeof...(argument_types) == 1 && is_model_floating<value_type> && (std::floating_point<std::remove_cvref_t<argument_types>> && ...)) {
            return checked_model_float<value_type>(std::forward<argument_types>(arguments)...);
        } else if constexpr (sizeof...(argument_types) == 1 && std::same_as<value_type, bool>) {
            static_assert((std::same_as<std::remove_cvref_t<argument_types>, bool> && ...), "bool model elements require an exact bool value");
            return value_type(std::forward<argument_types>(arguments)...);
        } else {
            return value_type(std::forward<argument_types>(arguments)...);
        }
    };
    if constexpr (model_value_rebind_access::has_rebind_for_model<value_type>()) {
        return model_value_construction<value_type, decltype(construct)>{std::move(construct)};
    } else {
        return construct();
    }
}

}  // namespace ruvia::detail

namespace ruvia {

class string final {
public:
    string() noexcept = default;

    string(std::string_view value) {
        storage_.assign_owned(value);
    }

    string(const char* value) {
        storage_.assign_owned(value == nullptr ? std::string_view{} : std::string_view(value));
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

    [[nodiscard]] string clone() const {
        return rebind_for_model();
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

    friend bool operator==(const string& left, std::string_view right) noexcept {
        return left.view() == right;
    }

    void assign(std::string_view value) & {
        storage_.assign_owned(value);
    }
    void assign(std::string_view) && = delete;

    template <std::same_as<std::string> string_type>
    void assign(string_type&& value) & noexcept {
        storage_.assign_owned(std::move(value));
    }
    template <std::same_as<std::string> string_type>
    void assign(string_type&&) && = delete;

    [[nodiscard]] std::string take_string() && {
        return std::move(storage_).take();
    }
    std::string take_string() & = delete;

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

class bytes final {
public:
    bytes() noexcept = default;

    bytes(byte_view value) :
        items_(value.begin(), value.end()) {}

    template <std::same_as<std::vector<std::byte>> vector_type>
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

    [[nodiscard]] bytes clone() const {
        return bytes(view());
    }

    [[nodiscard]] byte_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return byte_view(items_);
    }
    byte_view view() const&& = delete;

    [[nodiscard]] const std::byte* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_.data();
    }
    const std::byte* data() const&& = delete;

    operator byte_view() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    operator byte_view() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }
    [[nodiscard]] std::byte operator[](std::size_t index) const noexcept {
        return items_[index];
    }

    void assign(byte_view value) & {
        std::vector<std::byte> owned(value.begin(), value.end());
        items_ = std::move(owned);
    }
    void assign(byte_view) && = delete;

    template <std::same_as<std::vector<std::byte>> vector_type>
    void assign(vector_type&& value) & noexcept {
        items_ = std::move(value);
    }
    template <std::same_as<std::vector<std::byte>> vector_type>
    void assign(vector_type&&) && = delete;

    [[nodiscard]] std::vector<std::byte> take_bytes() && noexcept {
        std::vector<std::byte> taken(std::move(items_));
        items_.clear();
        return taken;
    }
    std::vector<std::byte> take_bytes() & = delete;

    friend bool operator==(const bytes& left, const bytes& right) noexcept {
        return std::ranges::equal(left.view(), right.view());
    }

    friend bool operator==(const bytes& left, byte_view right) noexcept {
        return std::ranges::equal(left.view(), right);
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

    std::vector<std::byte> items_;
};

inline outbound_bytes::outbound_bytes(const class bytes& value) :
    storage_(std::in_place_type<std::string>, as_chars(value.view())) {}

inline outbound_bytes::outbound_bytes(class bytes&& value) noexcept :
    storage_(std::in_place_type<std::vector<std::byte>>, std::move(value).take_bytes()) {}

inline outbound_bytes::outbound_bytes(const string& value) :
    storage_(std::in_place_type<std::string>, value.view()) {}

inline outbound_bytes::outbound_bytes(string&& value) :
    storage_(std::in_place_type<std::string>, std::move(value).take_string()) {}

// RFC 9562 128-bit UUID value type. Canonical text representation is 36-byte
// lowercase 8-4-4-4-12 hexadecimal; input parsing accepts ASCII hex case-insensitively.
class uuid final {
public:
    static constexpr std::size_t byte_size = 16;
    static constexpr std::size_t text_size = 36;

    constexpr uuid() noexcept = default;

    explicit constexpr uuid(std::span<const std::byte, byte_size> octets) noexcept {
        std::ranges::copy(octets, bytes_.begin());
    }

    explicit constexpr uuid(std::string_view text) {
        const auto parsed = try_parse(text);
        if (!parsed) {
            throw std::invalid_argument("invalid RFC 9562 UUID text");
        }
        *this = *parsed;
    }

    [[nodiscard]] static constexpr std::optional<uuid> try_from_bytes(byte_view octets) noexcept {
        if (octets.size() != byte_size) {
            return std::nullopt;
        }
        uuid result;
        std::ranges::copy(octets, result.bytes_.begin());
        return result;
    }

    [[nodiscard]] static constexpr std::optional<uuid> try_parse(std::string_view text) noexcept {
        if (text.size() != text_size || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
            return std::nullopt;
        }
        uuid result;
        std::size_t out = 0;
        for (std::size_t i = 0; i < text_size;) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                ++i;
                continue;
            }
            const int hi = decode_hex_nibble(text[i]);
            const int lo = decode_hex_nibble(text[i + 1]);
            if ((hi | lo) < 0) {
                return std::nullopt;
            }
            result.bytes_[out++] = static_cast<std::byte>((hi << 4) | lo);
            i += 2;
        }
        return result;
    }

    [[nodiscard]] constexpr std::span<const std::byte, byte_size> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return bytes_;
    }
    std::span<const std::byte, byte_size> bytes() const&& = delete;

    [[nodiscard]] constexpr bool is_nil() const noexcept {
        for (const auto b : bytes_) {
            if (b != std::byte{0}) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr std::uint8_t version() const noexcept {
        return static_cast<std::uint8_t>(bytes_[6]) >> 4;
    }

    [[nodiscard]] constexpr std::string_view format_to(std::span<char, text_size> buffer RUVIA_LIFETIMEBOUND) const noexcept {
        std::size_t out = 0;
        for (std::size_t i = 0; i < byte_size; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                buffer[out++] = '-';
            }
            const auto b = static_cast<unsigned char>(bytes_[i]);
            buffer[out++] = lower_hex_digit(b >> 4);
            buffer[out++] = lower_hex_digit(b);
        }
        return std::string_view(buffer.data(), text_size);
    }

    void append_to(std::string& output) const {
        std::array<char, text_size> buffer{};
        output.append(format_to(buffer));
    }

    [[nodiscard]] std::string to_string() const {
        std::array<char, text_size> buffer{};
        return std::string(format_to(buffer));
    }

    [[nodiscard]] friend constexpr auto operator<=>(const uuid& left, const uuid& right) noexcept = default;
    [[nodiscard]] friend constexpr bool operator==(const uuid& left, const uuid& right) noexcept = default;

private:
    std::array<std::byte, byte_size> bytes_{};
};

// Exact base-10 fixed-point decimal (up to 28 significant digits, scale 0..28).
// Normalized on construction: trailing fractional zeros beyond scale 0? No —
// canonical value comparison normalizes trailing fractional zeros (-0.00 == 0,
// 1.20 == 1.2) while preserving exact fractional digits up to 28 total digits.
class decimal final {
public:
    static constexpr std::uint8_t max_digits = 28;

    constexpr decimal() noexcept = default;

    explicit constexpr decimal(std::int64_t integer) noexcept {
        if (integer < 0) {
            negative_ = true;
            const auto magnitude = static_cast<std::uint64_t>(-(integer + 1)) + 1ULL;
            lo_ = magnitude;
        } else {
            lo_ = static_cast<std::uint64_t>(integer);
        }
    }

    explicit decimal(std::string_view text) {
        const auto parsed = try_parse(text);
        if (!parsed) {
            throw std::invalid_argument("invalid decimal literal");
        }
        *this = *parsed;
    }

    [[nodiscard]] static constexpr decimal from_scaled(std::int64_t unscaled, std::uint8_t scale = 0) {
        if (scale > max_digits) {
            throw std::out_of_range("decimal scale exceeds 28");
        }
        decimal result;
        result.scale_ = scale;
        if (unscaled < 0) {
            result.negative_ = true;
            result.lo_ = static_cast<std::uint64_t>(-(unscaled + 1)) + 1ULL;
        } else {
            result.lo_ = static_cast<std::uint64_t>(unscaled);
        }
        result.normalize_zero();
        return result;
    }

    [[nodiscard]] static constexpr std::optional<decimal> try_parse(std::string_view text) noexcept {
        if (text.empty()) {
            return std::nullopt;
        }
        std::size_t index = 0;
        bool negative = false;
        if (text[index] == '-') {
            negative = true;
            ++index;
        } else if (text[index] == '+') {
            return std::nullopt;
        }
        if (index >= text.size()) {
            return std::nullopt;
        }
        std::uint64_t hi = 0;
        std::uint64_t lo = 0;
        std::size_t int_digits = 0;
        std::size_t frac_digits = 0;
        std::size_t significant_digits = 0;
        bool in_fraction = false;

        for (; index < text.size(); ++index) {
            const char ch = text[index];
            if (ch == '.') {
                if (in_fraction || int_digits == 0) {
                    return std::nullopt;
                }
                in_fraction = true;
                continue;
            }
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
            const auto digit = static_cast<std::uint32_t>(ch - '0');
            if (in_fraction) {
                ++frac_digits;
            } else {
                ++int_digits;
            }
            if (hi != 0 || lo != 0 || digit != 0 || in_fraction) {
                if (hi != 0 || lo != 0 || digit != 0) {
                    ++significant_digits;
                } else if (in_fraction) {
                    // Leading fractional zeros after 0. still consume scale.
                    ++significant_digits;
                }
            }
            if (frac_digits > max_digits || significant_digits > max_digits) {
                return std::nullopt;
            }
            multiply_add_10(hi, lo, digit);
        }
        if (int_digits == 0 || (in_fraction && frac_digits == 0)) {
            return std::nullopt;
        }
        decimal result;
        result.hi_ = hi;
        result.lo_ = lo;
        result.scale_ = static_cast<std::uint8_t>(frac_digits);
        result.negative_ = negative;
        result.normalize_zero();
        return result;
    }

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return hi_ == 0 && lo_ == 0;
    }

    [[nodiscard]] constexpr bool is_negative() const noexcept {
        return negative_;
    }

    [[nodiscard]] constexpr std::uint8_t scale() const noexcept {
        return scale_;
    }

    void append_to(std::string& output) const {
        if (negative_) {
            output.push_back('-');
        }
        std::array<char, 40> digits{};
        std::size_t count = 0;
        auto hi = hi_;
        auto lo = lo_;
        if (hi == 0 && lo == 0) {
            digits[count++] = '0';
        } else {
            while (hi != 0 || lo != 0) {
                digits[count++] = static_cast<char>('0' + divide_10(hi, lo));
            }
        }
        if (scale_ == 0) {
            for (std::size_t i = count; i > 0; --i) {
                output.push_back(digits[i - 1]);
            }
            return;
        }
        if (count <= scale_) {
            output.append("0.");
            output.append(scale_ - count, '0');
            for (std::size_t i = count; i > 0; --i) {
                output.push_back(digits[i - 1]);
            }
            return;
        }
        for (std::size_t i = count; i > scale_; --i) {
            output.push_back(digits[i - 1]);
        }
        output.push_back('.');
        for (std::size_t i = scale_; i > 0; --i) {
            output.push_back(digits[i - 1]);
        }
    }

    [[nodiscard]] std::string to_string() const {
        std::string result;
        result.reserve(32);
        append_to(result);
        return result;
    }

    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const decimal& left, const decimal& right) noexcept {
        if (left.negative_ != right.negative_) {
            return left.negative_ ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        auto l_hi = left.hi_;
        auto l_lo = left.lo_;
        auto l_scale = left.scale_;
        auto r_hi = right.hi_;
        auto r_lo = right.lo_;
        auto r_scale = right.scale_;
        strip_trailing_zeros(l_hi, l_lo, l_scale);
        strip_trailing_zeros(r_hi, r_lo, r_scale);
        while (l_scale < r_scale) {
            multiply_add_10(l_hi, l_lo, 0);
            ++l_scale;
        }
        while (r_scale < l_scale) {
            multiply_add_10(r_hi, r_lo, 0);
            ++r_scale;
        }
        const auto mag = l_hi != r_hi ? (l_hi <=> r_hi) : (l_lo <=> r_lo);
        if (mag == std::strong_ordering::equal) {
            return std::strong_ordering::equal;
        }
        return left.negative_ ? (0 <=> mag) : mag;
    }

    [[nodiscard]] friend constexpr bool operator==(const decimal& left, const decimal& right) noexcept {
        return (left <=> right) == std::strong_ordering::equal;
    }

private:
    constexpr void normalize_zero() noexcept {
        if (hi_ == 0 && lo_ == 0) {
            negative_ = false;
        }
    }

    static constexpr void multiply_add_10(std::uint64_t& hi, std::uint64_t& lo, std::uint32_t add) noexcept {
        const std::uint64_t lo_low = (lo & 0xFFFFFFFFULL) * 10ULL + add;
        const std::uint64_t lo_high = (lo >> 32) * 10ULL + (lo_low >> 32);
        lo = (lo_high << 32) | (lo_low & 0xFFFFFFFFULL);
        hi = hi * 10ULL + (lo_high >> 32);
    }

    [[nodiscard]] static constexpr std::uint32_t divide_10(std::uint64_t& hi, std::uint64_t& lo) noexcept {
        const std::uint64_t rem_hi = hi % 10ULL;
        hi /= 10ULL;
        const std::uint64_t mid = (rem_hi << 32) | (lo >> 32);
        const std::uint64_t q_mid = mid / 10ULL;
        const std::uint64_t rem_mid = mid % 10ULL;
        const std::uint64_t low = (rem_mid << 32) | (lo & 0xFFFFFFFFULL);
        const std::uint64_t q_low = low / 10ULL;
        const auto rem = static_cast<std::uint32_t>(low % 10ULL);
        lo = (q_mid << 32) | q_low;
        return rem;
    }

    static constexpr void strip_trailing_zeros(std::uint64_t& hi, std::uint64_t& lo, std::uint8_t& scale) noexcept {
        if (hi == 0 && lo == 0) {
            scale = 0;
            return;
        }
        while (scale > 0) {
            auto candidate_hi = hi;
            auto candidate_lo = lo;
            if (divide_10(candidate_hi, candidate_lo) != 0) {
                break;
            }
            hi = candidate_hi;
            lo = candidate_lo;
            --scale;
        }
    }

    std::uint64_t hi_{0};
    std::uint64_t lo_{0};
    std::uint8_t scale_{0};
    bool negative_{false};
};

// Enum string reflection (ADL-discovered, works inside any namespace).
template <typename enum_type>
struct enum_entry final {
    enum_type value_;
    std::string_view name_;
};

#define RUVIA_ENUM_VALUE(enumerator, wire_name)                      \
    ::ruvia::enum_entry<std::remove_cvref_t<decltype(enumerator)>> { \
        enumerator, wire_name                                        \
    }

#define RUVIA_ENUM(enum_type, ...)                                                              \
    [[nodiscard]] constexpr auto ruvia_enum_entries(std::type_identity<enum_type>) noexcept {   \
        constexpr std::array entries{__VA_ARGS__};                                              \
        static_assert(entries.size() > 0, "RUVIA_ENUM requires at least one RUVIA_ENUM_VALUE"); \
        return entries;                                                                         \
    }

namespace detail {

template <typename t_type>
concept has_ruvia_enum_entries = std::is_enum_v<std::remove_cvref_t<t_type>> && requires {
    ruvia_enum_entries(std::type_identity<std::remove_cvref_t<t_type>>{});
};

template <typename t_type>
inline constexpr bool is_ruvia_enum = has_ruvia_enum_entries<t_type>;

}  // namespace detail

template <typename enum_type>
    requires detail::is_ruvia_enum<enum_type>
[[nodiscard]] constexpr std::optional<std::remove_cvref_t<enum_type>> try_parse_enum(std::string_view text) noexcept {
    using clean_type = std::remove_cvref_t<enum_type>;
    constexpr auto entries = ruvia_enum_entries(std::type_identity<clean_type>{});
    for (const auto& entry : entries) {
        if (entry.name_ == text) {
            return entry.value_;
        }
    }
    return std::nullopt;
}

template <typename enum_type>
    requires detail::is_ruvia_enum<enum_type>
[[nodiscard]] constexpr std::optional<std::string_view> try_format_enum(enum_type value) noexcept {
    constexpr auto entries = ruvia_enum_entries(std::type_identity<std::remove_cvref_t<enum_type>>{});
    for (const auto& entry : entries) {
        if (entry.value_ == value) {
            return entry.name_;
        }
    }
    return std::nullopt;
}

template <typename enum_type>
    requires detail::is_ruvia_enum<enum_type>
[[nodiscard]] constexpr std::string_view to_enum_string(enum_type value) {
    const auto formatted = try_format_enum(value);
    if (!formatted) {
        throw std::invalid_argument("unmapped RUVIA_ENUM value");
    }
    return *formatted;
}

namespace detail {

inline void append_model_binary(std::string& output, byte_view value, bytes_encoding encoding = bytes_encoding::base64) {
    output.push_back('"');
    switch (encoding) {
        case bytes_encoding::base64: {
            const auto encoded_size = base64_encoded_size(value.size(), base64_padding::required);
            const auto start = output.size();
            output.resize(start + encoded_size);
            base64_encode(std::span<char>(output.data() + start, encoded_size), value, base64_charset::standard, base64_padding::required);
            break;
        }
        case bytes_encoding::base64url: {
            const auto encoded_size = base64_encoded_size(value.size(), base64_padding::forbidden);
            const auto start = output.size();
            output.resize(start + encoded_size);
            base64_encode(std::span<char>(output.data() + start, encoded_size), value, base64_charset::url_safe, base64_padding::forbidden);
            break;
        }
        case bytes_encoding::hex:
            append_lower_hex(output, value);
            break;
    }
    output.push_back('"');
}

[[nodiscard]] inline std::optional<bytes> decode_model_binary(std::string_view encoded, bytes_encoding encoding = bytes_encoding::base64) {
    if (encoded.empty()) {
        return bytes();
    }
    switch (encoding) {
        case bytes_encoding::base64: {
            if (encoded.size() % 4 != 0) {
                return std::nullopt;
            }
            std::vector<std::byte> decoded(base64_decoded_size_bound(encoded.size()));
            const auto decoded_size = base64_decode(encoded, decoded, base64_charset::standard, base64_padding::required);
            if (!decoded_size) {
                return std::nullopt;
            }
            decoded.resize(*decoded_size);
            return bytes(std::move(decoded));
        }
        case bytes_encoding::base64url: {
            std::vector<std::byte> decoded(base64_decoded_size_bound(encoded.size()));
            const auto decoded_size = base64_decode(encoded, decoded, base64_charset::url_safe, base64_padding::optional);
            if (!decoded_size) {
                return std::nullopt;
            }
            decoded.resize(*decoded_size);
            return bytes(std::move(decoded));
        }
        case bytes_encoding::hex: {
            if (encoded.size() % 2 != 0) {
                return std::nullopt;
            }
            std::vector<std::byte> decoded(encoded.size() / 2);
            if (!decode_hex(encoded, decoded)) {
                return std::nullopt;
            }
            return bytes(std::move(decoded));
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool parse_fixed_digits(std::string_view text, std::size_t offset, std::size_t count, int& out) noexcept {
    if (offset + count > text.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char ch = text[offset + i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    out = value;
    return true;
}

[[nodiscard]] constexpr std::optional<civil_date> try_parse_civil_date(std::string_view text) noexcept {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    int y = 0;
    int m = 0;
    int d = 0;
    if (!parse_fixed_digits(text, 0, 4, y) || !parse_fixed_digits(text, 5, 2, m) || !parse_fixed_digits(text, 8, 2, d)) {
        return std::nullopt;
    }
    const civil_date ymd{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(m)}, std::chrono::day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return ymd;
}

inline void append_padded_uint(std::string& out, unsigned value, std::size_t width) {
    std::array<char, 8> buf{};
    for (std::size_t i = width; i > 0; --i) {
        buf[i - 1] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    }
    out.append(buf.data(), width);
}

inline void append_civil_date(std::string& out, civil_date ymd) {
    const int y = static_cast<int>(ymd.year());
    if (y < 0) {
        out.push_back('-');
        append_padded_uint(out, static_cast<unsigned>(-y), 4);
    } else {
        append_padded_uint(out, static_cast<unsigned>(y), 4);
    }
    out.push_back('-');
    append_padded_uint(out, static_cast<unsigned>(ymd.month()), 2);
    out.push_back('-');
    append_padded_uint(out, static_cast<unsigned>(ymd.day()), 2);
}

template <typename duration_type>
[[nodiscard]] std::optional<std::chrono::sys_time<duration_type>> try_parse_rfc3339(std::string_view text) noexcept {
    // Minimum "YYYY-MM-DDTHH:MM:SSZ" is 20 chars.
    if (text.size() < 20 || (text[10] != 'T' && text[10] != 't' && text[10] != ' ') || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto ymd = try_parse_civil_date(text.substr(0, 10));
    if (!ymd) {
        return std::nullopt;
    }
    int hh = 0;
    int mm = 0;
    int ss = 0;
    if (!parse_fixed_digits(text, 11, 2, hh) || !parse_fixed_digits(text, 14, 2, mm) || !parse_fixed_digits(text, 17, 2, ss)) {
        return std::nullopt;
    }
    if (hh > 23 || mm > 59 || ss > 59) {
        return std::nullopt;
    }
    std::size_t pos = 19;
    std::int64_t subsecond_micros = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        std::size_t frac_digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (frac_digits < 6) {
                subsecond_micros = subsecond_micros * 10 + (text[pos] - '0');
            }
            ++frac_digits;
            ++pos;
        }
        if (frac_digits == 0 || frac_digits > 9) {
            return std::nullopt;
        }
        while (frac_digits++ < 6) {
            subsecond_micros *= 10;
        }
    }
    int tz_offset_seconds = 0;
    if (pos >= text.size()) {
        return std::nullopt;
    }
    if (text[pos] == 'Z' || text[pos] == 'z') {
        ++pos;
    } else if (text[pos] == '+' || text[pos] == '-') {
        const bool neg = text[pos] == '-';
        ++pos;
        int tz_h = 0;
        int tz_m = 0;
        if (pos + 5 != text.size() || text[pos + 2] != ':' || !parse_fixed_digits(text, pos, 2, tz_h) || !parse_fixed_digits(text, pos + 3, 2, tz_m) || tz_h > 23 || tz_m > 59) {
            return std::nullopt;
        }
        pos += 5;
        tz_offset_seconds = (tz_h * 3600 + tz_m * 60) * (neg ? -1 : 1);
    } else {
        return std::nullopt;
    }
    if (pos != text.size()) {
        return std::nullopt;
    }
    const auto base_seconds = std::chrono::sys_days{*ymd} + std::chrono::hours{hh} + std::chrono::minutes{mm} + std::chrono::seconds{ss - tz_offset_seconds};
    const auto total_micros = std::chrono::time_point_cast<std::chrono::microseconds>(base_seconds) + std::chrono::microseconds{subsecond_micros};
    return std::chrono::floor<duration_type>(total_micros);
}

template <typename duration_type>
inline void append_rfc3339(std::string& out, std::chrono::sys_time<duration_type> tp) {
    const auto dp = std::chrono::floor<std::chrono::days>(tp);
    const civil_date ymd{dp};
    append_civil_date(out, ymd);
    out.push_back('T');
    const auto time_of_day = tp - dp;
    const auto hh = std::chrono::duration_cast<std::chrono::hours>(time_of_day);
    const auto mm = std::chrono::duration_cast<std::chrono::minutes>(time_of_day - hh);
    const auto ss = std::chrono::duration_cast<std::chrono::seconds>(time_of_day - hh - mm);
    append_padded_uint(out, static_cast<unsigned>(hh.count()), 2);
    out.push_back(':');
    append_padded_uint(out, static_cast<unsigned>(mm.count()), 2);
    out.push_back(':');
    append_padded_uint(out, static_cast<unsigned>(ss.count()), 2);
    if constexpr (std::same_as<duration_type, std::chrono::milliseconds>) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time_of_day - hh - mm - ss);
        out.push_back('.');
        append_padded_uint(out, static_cast<unsigned>(ms.count()), 3);
    } else if constexpr (std::same_as<duration_type, std::chrono::microseconds>) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(time_of_day - hh - mm - ss);
        out.push_back('.');
        append_padded_uint(out, static_cast<unsigned>(us.count()), 6);
    }
    out.push_back('Z');
}

template <typename t_type>
inline constexpr bool is_chrono_timestamp = std::is_same_v<std::remove_cvref_t<t_type>, utc_seconds> ||
    std::is_same_v<std::remove_cvref_t<t_type>, utc_milliseconds> ||
    std::is_same_v<std::remove_cvref_t<t_type>, utc_microseconds>;

template <typename t_type>
inline constexpr bool is_civil_date = std::is_same_v<std::remove_cvref_t<t_type>, civil_date>;

}  // namespace detail

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

    template <std::same_as<std::vector<t_type>> vector_type>
    explicit array(vector_type&& items) noexcept :
        items_(std::move(items)) {}

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
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }
        auto rebound = std::move(other).rebind_for_model();
        items_ = std::move(rebound.items_);
        return *this;
    }

    ~array() {}

    [[nodiscard]] array clone() const {
        return rebind_for_model();
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

    [[nodiscard]] std::span<const t_type> items() const& noexcept RUVIA_LIFETIMEBOUND {
        return items_;
    }
    std::span<const t_type> items() const&& = delete;

    template <std::same_as<std::vector<t_type>> vector_type>
    void assign(vector_type&& value) & noexcept {
        items_ = std::move(value);
    }
    template <std::same_as<std::vector<t_type>> vector_type>
    void assign(vector_type&&) && = delete;

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

    [[nodiscard]] boxed_array clone() const {
        return rebind_for_model();
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

// Move-only insertion-ordered string-keyed map for dynamic JSON dictionaries
// (`map<string>`, `map<int64_t>`, `map<MyModel>`, etc.).
template <typename v_type>
class map final {
public:
    struct entry_type final {
        string key_;
        v_type value_;

        friend bool operator==(const entry_type& left, const entry_type& right) {
            return left.key_ == right.key_ && left.value_ == right.value_;
        }
    };

    using mapped_type = v_type;
    using value_type = entry_type;
    using size_type = std::size_t;
    using iterator = typename std::vector<entry_type>::iterator;
    using const_iterator = typename std::vector<entry_type>::const_iterator;

    map() noexcept = default;
    map(const map&) = delete;
    map& operator=(const map&) = delete;

    map(map&& other) noexcept :
        entries_(std::move(other.entries_)) {
        other.entries_.clear();
    }

    map& operator=(map&& other) {
        if (this == &other) {
            return *this;
        }
        if (other.owns_for_model(this)) {
            detail::throw_model_move_source_owns_destination();
        }
        auto rebound = std::move(other).rebind_for_model();
        entries_ = std::move(rebound.entries_);
        return *this;
    }

    ~map() {}

    [[nodiscard]] map clone() const {
        return rebind_for_model();
    }

    [[nodiscard]] bool empty() const noexcept {
        return entries_.empty();
    }

    [[nodiscard]] size_type size() const noexcept {
        return entries_.size();
    }

    void reserve(size_type count) & {
        entries_.reserve(count);
    }
    void reserve(size_type) && = delete;

    void clear() & noexcept {
        entries_.clear();
    }
    void clear() && = delete;

    [[nodiscard]] iterator begin() & noexcept RUVIA_LIFETIMEBOUND {
        return entries_.begin();
    }
    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return entries_.begin();
    }
    iterator begin() const&& = delete;

    [[nodiscard]] iterator end() & noexcept RUVIA_LIFETIMEBOUND {
        return entries_.end();
    }
    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return entries_.end();
    }
    iterator end() const&& = delete;

    [[nodiscard]] bool contains(std::string_view key) const noexcept {
        return find(key) != nullptr;
    }

    [[nodiscard]] v_type* find(std::string_view key) & noexcept RUVIA_LIFETIMEBOUND {
        for (auto& entry : entries_) {
            if (entry.key_.view() == key) {
                return std::addressof(entry.value_);
            }
        }
        return nullptr;
    }
    [[nodiscard]] const v_type* find(std::string_view key) const& noexcept RUVIA_LIFETIMEBOUND {
        for (const auto& entry : entries_) {
            if (entry.key_.view() == key) {
                return std::addressof(entry.value_);
            }
        }
        return nullptr;
    }
    const v_type* find(std::string_view) const&& = delete;

    [[nodiscard]] v_type& at(std::string_view key) & RUVIA_LIFETIMEBOUND {
        auto* found = find(key);
        if (found == nullptr) {
            throw std::out_of_range("ruvia::map key not found");
        }
        return *found;
    }
    [[nodiscard]] const v_type& at(std::string_view key) const& RUVIA_LIFETIMEBOUND {
        const auto* found = find(key);
        if (found == nullptr) {
            throw std::out_of_range("ruvia::map key not found");
        }
        return *found;
    }
    const v_type& at(std::string_view) const&& = delete;

    template <typename... args_type>
    v_type& insert_or_assign(std::string_view key, args_type&&... args) & RUVIA_LIFETIMEBOUND {
        v_type prepared = detail::prepare_model_value<v_type>(this, std::forward<args_type>(args)...);
        for (auto& entry : entries_) {
            if (entry.key_.view() == key) {
                entry.value_ = std::move(prepared);
                return entry.value_;
            }
        }
        string owned_key(key);
        return entries_.emplace_back(entry_type{std::move(owned_key), std::move(prepared)}).value_;
    }
    template <typename... args_type>
    v_type& insert_or_assign(std::string_view, args_type&&...) && = delete;

    bool erase(std::string_view key) & noexcept {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->key_.view() == key) {
                entries_.erase(it);
                return true;
            }
        }
        return false;
    }
    bool erase(std::string_view) && = delete;

    friend bool operator==(const map& left, const map& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (const auto& entry : left.entries_) {
            const auto* found = right.find(entry.key_.view());
            if (found == nullptr || !(*found == entry.value_)) {
                return false;
            }
        }
        return true;
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;

    bool emplace_parsed_unique(string&& key, v_type&& value) {
        if (contains(key.view())) {
            return false;
        }
        entries_.push_back(entry_type{std::move(key), std::move(value)});
        return true;
    }

    [[nodiscard]] map rebind_for_model() const& {
        map rebound;
        rebound.reserve(size());
        for (const auto& entry : entries_) {
            rebound.entries_.push_back(entry_type{entry.key_.clone(), detail::rebind_model_value(entry.value_)});
        }
        return rebound;
    }

    void own_for_model() {
        for (auto& entry : entries_) {
            detail::model_value_rebind_access::own_in_place(entry.key_);
            if constexpr (detail::model_value_rebind_access::has_rebind_for_model<v_type>()) {
                detail::model_value_rebind_access::own_in_place(entry.value_);
            }
        }
    }

    [[nodiscard]] map rebind_for_model() && {
        own_for_model();
        map rebound;
        rebound.entries_ = std::move(entries_);
        entries_.clear();
        return rebound;
    }

    [[nodiscard]] bool owns_for_model(const void* address) const noexcept {
        if (detail::model_address_within(address, entries_.data(), entries_.size() * sizeof(entry_type))) {
            return true;
        }
        if constexpr (detail::model_value_rebind_access::can_own<v_type>()) {
            for (const auto& entry : entries_) {
                if (detail::model_value_rebind_access::owns(entry.value_, address)) {
                    return true;
                }
            }
        }
        return false;
    }

    std::vector<entry_type> entries_;
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

    template <typename v_type>
    [[nodiscard]] static bool emplace_parsed_map(map<v_type>& target, string&& key, v_type&& value) {
        return target.emplace_parsed_unique(std::move(key), std::move(value));
    }
};

template <typename>
inline constexpr bool always_false = false;

template <typename t_type>
inline constexpr bool is_ruvia_string = std::is_same_v<std::remove_cvref_t<t_type>, string>;

template <typename t_type>
inline constexpr bool is_ruvia_bytes = std::is_same_v<std::remove_cvref_t<t_type>, bytes>;

template <typename t_type>
inline constexpr bool is_ruvia_uuid = std::is_same_v<std::remove_cvref_t<t_type>, uuid>;

template <typename t_type>
inline constexpr bool is_ruvia_decimal = std::is_same_v<std::remove_cvref_t<t_type>, decimal>;

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
struct ruvia_map_traits : std::false_type {};

template <typename mapped_type>
struct ruvia_map_traits<map<mapped_type>> : std::true_type {
    using value_type = mapped_type;
};
template <typename t_type>
inline constexpr bool is_ruvia_map = ruvia_map_traits<std::remove_cvref_t<t_type>>::value;

}  // namespace detail
}  // namespace ruvia
