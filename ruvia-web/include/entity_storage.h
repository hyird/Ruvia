#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "byte_view.h"
#include "checked_integer.h"
#include "fixed_string.h"
#include "json_codec.h"
#include "model.h"
#include "model_value.h"

namespace ruvia {

// Failure category shared by db_error and redis_error.
enum class backend_error_code : std::uint8_t {
    not_configured = 1,
    // DNS resolution, TCP/TLS establishment, or the startup handshake
    // (authentication, database selection) failed.
    connect_failed,
    io_error,
    protocol_error,
    // The server rejected one statement or command.
    command_failed,
    timeout,
    cancelled,
    closing,
};

// Interruptions use generic codes so every asynchronous API shares the same
// standard conditions; backend-specific failures retain their own category.
[[nodiscard]] inline std::error_code make_error_code(backend_error_code code) noexcept {
    if (code == backend_error_code::timeout) {
        return std::make_error_code(std::errc::timed_out);
    }
    if (code == backend_error_code::cancelled) {
        return std::make_error_code(std::errc::operation_canceled);
    }
    class backend_category final : public std::error_category {
    public:
        [[nodiscard]] const char* name() const noexcept override {
            return "ruvia.backend";
        }

        [[nodiscard]] std::string message(int value) const override {
            switch (static_cast<backend_error_code>(value)) {
                case backend_error_code::not_configured:
                    return "backend is not configured";
                case backend_error_code::connect_failed:
                    return "backend connection failed";
                case backend_error_code::io_error:
                    return "backend I/O failed";
                case backend_error_code::protocol_error:
                    return "backend protocol error";
                case backend_error_code::command_failed:
                    return "backend command failed";
                case backend_error_code::timeout:
                    return "backend operation timed out";
                case backend_error_code::cancelled:
                    return "backend operation cancelled";
                case backend_error_code::closing:
                    return "backend is closing";
            }
            return "unknown backend error";
        }
    };
    static const backend_category category;
    return {static_cast<int>(code), category};
}

// Sort direction of one ordering key, shared by the SQL and Redis query layers.
enum class order_direction : std::uint8_t {
    asc,
    desc,
};

// A database field or Redis reply that cannot become the requested type.
class conversion_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        invalid_format,
        out_of_range,
        unexpected_null,
        missing_column,
        unexpected_row_count,
    };

    conversion_error(code_type code, std::string_view message) :
        std::runtime_error(std::string(message)),
        code_(code) {}

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }

private:
    code_type code_;
};

}  // namespace ruvia

template <>
struct std::is_error_code_enum<ruvia::backend_error_code> : std::true_type {};

namespace ruvia::detail {

enum class text_scalar_parse_error {
    invalid_format,
    out_of_range,
};

template <typename duration_type>
[[nodiscard]] std::optional<std::chrono::sys_time<duration_type>> try_parse_db_timestamp(std::string_view text) noexcept {
    if (text.size() < 19 || (text[10] != 'T' && text[10] != 't' && text[10] != ' ') || text[13] != ':' || text[16] != ':') {
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
    if (pos < text.size()) {
        if (text[pos] == 'Z' || text[pos] == 'z') {
            ++pos;
        } else if (text[pos] == '+' || text[pos] == '-') {
            const bool neg = text[pos] == '-';
            ++pos;
            int tz_h = 0;
            int tz_m = 0;
            const auto rem = text.size() - pos;
            if (rem == 2) {
                if (!parse_fixed_digits(text, pos, 2, tz_h) || tz_h > 23) {
                    return std::nullopt;
                }
                pos += 2;
            } else if (rem == 4) {
                if (!parse_fixed_digits(text, pos, 2, tz_h) || !parse_fixed_digits(text, pos + 2, 2, tz_m) || tz_h > 23 || tz_m > 59) {
                    return std::nullopt;
                }
                pos += 4;
            } else if (rem == 5 && text[pos + 2] == ':') {
                if (!parse_fixed_digits(text, pos, 2, tz_h) || !parse_fixed_digits(text, pos + 3, 2, tz_m) || tz_h > 23 || tz_m > 59) {
                    return std::nullopt;
                }
                pos += 5;
            } else {
                return std::nullopt;
            }
            tz_offset_seconds = (tz_h * 3600 + tz_m * 60) * (neg ? -1 : 1);
        } else {
            return std::nullopt;
        }
    }
    if (pos != text.size()) {
        return std::nullopt;
    }
    const auto base_seconds = std::chrono::sys_days{*ymd} + std::chrono::hours{hh} + std::chrono::minutes{mm} + std::chrono::seconds{ss - tz_offset_seconds};
    const auto total_micros = std::chrono::time_point_cast<std::chrono::microseconds>(base_seconds) + std::chrono::microseconds{subsecond_micros};
    return std::chrono::floor<duration_type>(total_micros);
}

template <typename t_type>
concept text_scalar = std::is_same_v<t_type, std::remove_cv_t<t_type>> &&
    (std::is_same_v<t_type, bool> ||
        is_model_integer<t_type> ||
        std::is_floating_point_v<t_type> ||
        std::is_same_v<t_type, std::string> ||
        std::is_same_v<t_type, string> ||
        std::is_same_v<t_type, std::string_view> ||
        std::is_same_v<t_type, bytes> ||
        std::is_same_v<t_type, uuid> ||
        std::is_same_v<t_type, decimal> ||
        is_civil_date<t_type> ||
        is_chrono_timestamp<t_type> ||
        is_ruvia_enum<t_type>);

// The one text-to-scalar syntax shared by database results and Redis replies:
// boolean tokens 1/t/true/TRUE and 0/f/false/FALSE, strict decimal integers
// (std::from_chars: no whitespace or '+'), finite floating-point values,
// RFC 9562 UUIDs, exact decimals, ISO 8601 / SQL dates & timestamps, and
// RUVIA_ENUM wire names. std::string_view borrows `text`. A number that is
// syntactically complete but not representable is out_of_range; any other
// rejection, including a non-finite value, is invalid_format.
template <text_scalar t_type>
[[nodiscard]] std::variant<t_type, text_scalar_parse_error> parse_text_scalar(std::string_view text) {
    using result_type = std::variant<t_type, text_scalar_parse_error>;
    if constexpr (std::is_same_v<t_type, std::string_view>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, std::string>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, string>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, bytes>) {
        return result_type(std::in_place_index<0>, bytes(as_bytes(text)));
    } else if constexpr (std::is_same_v<t_type, uuid>) {
        if (const auto parsed = uuid::try_parse(text)) {
            return result_type(std::in_place_index<0>, *parsed);
        }
        return text_scalar_parse_error::invalid_format;
    } else if constexpr (std::is_same_v<t_type, decimal>) {
        if (const auto parsed = decimal::try_parse(text)) {
            return result_type(std::in_place_index<0>, *parsed);
        }
        return text_scalar_parse_error::invalid_format;
    } else if constexpr (is_civil_date<t_type>) {
        if (const auto parsed = try_parse_civil_date(text)) {
            return result_type(std::in_place_index<0>, *parsed);
        }
        return text_scalar_parse_error::invalid_format;
    } else if constexpr (is_chrono_timestamp<t_type>) {
        if (const auto parsed = try_parse_db_timestamp<typename t_type::duration>(text)) {
            return result_type(std::in_place_index<0>, *parsed);
        }
        return text_scalar_parse_error::invalid_format;
    } else if constexpr (is_ruvia_enum<t_type>) {
        if (const auto parsed = try_parse_enum<t_type>(text)) {
            return result_type(std::in_place_index<0>, *parsed);
        }
        return text_scalar_parse_error::invalid_format;
    } else if constexpr (std::is_same_v<t_type, bool>) {
        if (text == "1" || text == "t" || text == "true" || text == "TRUE") {
            return result_type(std::in_place_index<0>, true);
        }
        if (text == "0" || text == "f" || text == "false" || text == "FALSE") {
            return result_type(std::in_place_index<0>, false);
        }
        return text_scalar_parse_error::invalid_format;
    } else {
        t_type converted{};
        const auto* const last = text.data() + text.size();
        const auto [end, error] = std::from_chars(text.data(), last, converted);
        if (error == std::errc::result_out_of_range && end == last) {
            return text_scalar_parse_error::out_of_range;
        }
        if (error != std::errc{} || end != last) {
            return text_scalar_parse_error::invalid_format;
        }
        if constexpr (std::is_floating_point_v<t_type>) {
            if (!std::isfinite(converted)) {
                return text_scalar_parse_error::invalid_format;
            }
        }
        return result_type(std::in_place_index<0>, converted);
    }
}

// The one text-to-scalar conversion for database fields and Redis replies.
template <text_scalar t_type>
[[nodiscard]] t_type convert_text(std::string_view text) {
    auto parsed = parse_text_scalar<t_type>(text);
    if (const auto* error = std::get_if<text_scalar_parse_error>(&parsed)) {
        if constexpr (std::is_same_v<t_type, bool>) {
            throw conversion_error(conversion_error::code_type::invalid_format, "text is not a boolean");
        } else if constexpr (std::is_same_v<t_type, uuid>) {
            throw conversion_error(conversion_error::code_type::invalid_format, "text is not a valid UUID");
        } else if constexpr (std::is_same_v<t_type, decimal>) {
            throw conversion_error(conversion_error::code_type::invalid_format, "text is not a valid decimal");
        } else if constexpr (is_civil_date<t_type> || is_chrono_timestamp<t_type>) {
            throw conversion_error(conversion_error::code_type::invalid_format, "text is not a valid date or timestamp");
        } else if constexpr (is_ruvia_enum<t_type>) {
            throw conversion_error(conversion_error::code_type::invalid_format, "text is not a valid enum value");
        } else {
            throw conversion_error(*error == text_scalar_parse_error::out_of_range ? conversion_error::code_type::out_of_range : conversion_error::code_type::invalid_format, "text is not a valid number");
        }
    }
    return std::get<t_type>(std::move(parsed));
}

// Shared owning value storage. Backend descriptors and relations stay with
// their backend.
template <typename t_type>
struct is_std_vector : std::false_type {};
template <typename t_type>
struct is_std_vector<std::vector<t_type>> : std::true_type {
    using value_type = t_type;
};

template <typename t_type>
struct is_optional : std::false_type {};
template <typename t_type>
struct is_optional<std::optional<t_type>> : std::true_type {
    using value_type = t_type;
};

template <typename value_type>
[[nodiscard]] value_type clone_entity_value(const value_type& source) {
    if constexpr (std::is_same_v<value_type, string> || std::is_same_v<value_type, bytes>) {
        return source.clone();
    } else if constexpr (is_optional<value_type>::value) {
        using inner_type = typename is_optional<value_type>::value_type;
        if (!source) {
            return std::nullopt;
        }
        return std::optional<inner_type>(clone_entity_value(*source));
    } else if constexpr (is_std_vector<value_type>::value) {
        value_type cloned;
        cloned.reserve(source.size());
        for (const auto& item : source) {
            cloned.push_back(clone_entity_value(item));
        }
        return cloned;
    } else {
        return value_type(source);
    }
}

template <typename value_type>
void commit_entity_value(value_type& target, value_type&& owned) noexcept {
    if constexpr (std::is_same_v<value_type, std::string> || is_std_vector<value_type>::value) {
        target.swap(owned);
    } else {
        static_assert(std::is_nothrow_move_constructible_v<value_type>);
        target.~value_type();
        std::construct_at(&target, std::move(owned));
    }
}
template <typename t_type>
struct entity_value_slot {
    enum class state_type : unsigned char { unset,
        null,
        value };
    entity_value_slot() = default;
    entity_value_slot(const entity_value_slot&) = delete;
    entity_value_slot& operator=(const entity_value_slot&) = delete;
    entity_value_slot(entity_value_slot&& other) noexcept :
        state_(std::exchange(other.state_, state_type::unset)),
        value_(std::move(other.value_)) {}
    entity_value_slot& operator=(entity_value_slot&& other) noexcept {
        if (this != &other) {
            commit_entity_value(value_, std::move(other.value_));
            state_ = std::exchange(other.state_, state_type::unset);
        }
        return *this;
    }
    [[nodiscard]] entity_value_slot clone() const {
        entity_value_slot copy;
        if (state_ == state_type::value) {
            copy.value_ = clone_entity_value(value_);
        }
        copy.state_ = state_;
        return copy;
    }
    static t_type make_value() {
        return t_type{};
    }
    void clear() {
        auto empty = make_value();
        commit_entity_value(value_, std::move(empty));
    }
    state_type state_{state_type::unset};
    t_type value_{};
};

// Position of the descriptor named `name` in a descriptor tuple (entity
// columns or relations), or the tuple size when it is absent.
template <typename tuple_type, fixed_string name>
consteval std::size_t descriptor_index() {
    return []<std::size_t... i>(std::index_sequence<i...>) consteval {
        const std::array<std::string_view, sizeof...(i)> names{std::tuple_element_t<i, tuple_type>::name.view()...};
        return fixed_name_index(names, name.view());
    }(std::make_index_sequence<std::tuple_size_v<tuple_type>>{});
}
template <typename... column_types>
consteval bool unique_entity_columns() {
    const std::array<std::string_view, sizeof...(column_types)> names{column_types::name.view()...};
    return fixed_names_distinct(names);
}

// Visits every element of a descriptor tuple (entity columns, relations, join
// columns) in declaration order as function.template operator()<element, index>().
template <typename tuple_type, typename function_type>
constexpr void for_each_descriptor(function_type&& function) {
    [&]<std::size_t... i>(std::index_sequence<i...>) {
        (function.template operator()<std::tuple_element_t<i, tuple_type>, i>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<tuple_type>>{});
}

template <typename range_type, typename element_type>
consteval bool is_exact_element_range() noexcept {
    using decayed_type = std::remove_cvref_t<range_type>;
    if constexpr (requires { typename decayed_type::value_type; }) {
        return std::is_same_v<typename decayed_type::value_type, element_type>;
    } else {
        return std::is_same_v<std::remove_cvref_t<decltype(*std::declval<range_type&>().begin())>, element_type>;
    }
}

template <typename t_type, typename v_type>
void assign_entity_value(t_type& out, v_type&& value) {
    using raw_v_type = std::remove_cvref_t<v_type>;
    constexpr bool movable_rvalue = !std::is_lvalue_reference_v<v_type> && !std::is_const_v<std::remove_reference_t<v_type>>;
    if constexpr (std::is_same_v<t_type, bool>) {
        static_assert(std::is_same_v<raw_v_type, bool>, "bool entity columns require an exact bool value");
        out = value;
    } else if constexpr (is_model_integer<t_type>) {
        static_assert(is_model_integer<raw_v_type>, "integer entity columns require an integer value");
        out = checked_model_integer<t_type>(value);
    } else if constexpr (std::is_floating_point_v<t_type> && (is_model_integer<raw_v_type> || std::is_floating_point_v<raw_v_type>)) {
        if constexpr (std::is_floating_point_v<raw_v_type>) {
            out = checked_model_float<t_type>(value);
        } else {
            out = static_cast<t_type>(value);
        }
    } else if constexpr (std::is_same_v<t_type, string>) {
        if constexpr (movable_rvalue && std::is_same_v<raw_v_type, string>) {
            commit_entity_value(out, rebind_model_value(std::move(value)));
        } else if constexpr (movable_rvalue && std::is_same_v<raw_v_type, std::string>) {
            commit_entity_value(out, string(std::move(value)));
        } else {
            auto owned = string(std::string_view(value));
            commit_entity_value(out, std::move(owned));
        }
    } else if constexpr (std::is_same_v<t_type, std::string>) {
        if constexpr (movable_rvalue && std::is_same_v<raw_v_type, std::string>) {
            std::string owned(std::move(value));
            commit_entity_value(out, std::move(owned));
        } else if constexpr (movable_rvalue && std::is_same_v<raw_v_type, string>) {
            commit_entity_value(out, std::move(value).take_string());
        } else {
            std::string owned{std::string_view(value)};
            commit_entity_value(out, std::move(owned));
        }
    } else if constexpr (std::is_same_v<t_type, bytes>) {
        if constexpr (movable_rvalue && std::is_same_v<raw_v_type, bytes>) {
            commit_entity_value(out, std::move(value));
        } else if constexpr (std::is_same_v<raw_v_type, bytes>) {
            commit_entity_value(out, value.clone());
        } else if constexpr (movable_rvalue && std::is_same_v<raw_v_type, std::vector<std::byte>>) {
            commit_entity_value(out, bytes(std::move(value)));
        } else {
            commit_entity_value(out, bytes(byte_view(value)));
        }
    } else if constexpr (std::is_same_v<t_type, uuid> && std::is_convertible_v<v_type&&, std::string_view> && !std::is_same_v<raw_v_type, uuid>) {
        out = uuid(std::string_view(std::forward<v_type>(value)));
    } else if constexpr (std::is_same_v<t_type, decimal> && std::is_convertible_v<v_type&&, std::string_view> && !std::is_same_v<raw_v_type, decimal>) {
        out = decimal(std::string_view(std::forward<v_type>(value)));
    } else if constexpr (std::is_same_v<t_type, decimal> && is_model_integer<raw_v_type>) {
        out = decimal(checked_model_integer<std::int64_t>(value));
    } else if constexpr (is_optional<t_type>::value) {
        using element_type = typename is_optional<t_type>::value_type;
        if constexpr (is_optional<raw_v_type>::value) {
            if (!value) {
                out.reset();
            } else {
                auto owned = entity_value_slot<element_type>::make_value();
                if constexpr (movable_rvalue) {
                    assign_entity_value(owned, std::move(*value));
                } else {
                    assign_entity_value(owned, *value);
                }
                out.emplace(std::move(owned));
            }
        } else {
            auto owned = entity_value_slot<element_type>::make_value();
            assign_entity_value(owned, std::forward<v_type>(value));
            out.emplace(std::move(owned));
        }
    } else if constexpr (is_std_vector<t_type>::value) {
        using element_type = typename is_std_vector<t_type>::value_type;
        if constexpr (movable_rvalue && std::is_same_v<raw_v_type, t_type>) {
            t_type owned(std::move(value));
            commit_entity_value(out, std::move(owned));
        } else if constexpr (std::is_arithmetic_v<element_type> && is_exact_element_range<v_type, element_type>()) {
            t_type owned(value.begin(), value.end());
            commit_entity_value(out, std::move(owned));
        } else if constexpr (std::is_same_v<element_type, std::string>) {
            auto owned = entity_value_slot<t_type>::make_value();
            owned.reserve(value.size());
            if constexpr (movable_rvalue && std::is_same_v<std::remove_cvref_t<decltype(*value.begin())>, std::string>) {
                for (auto& item : value) {
                    owned.emplace_back(std::move(item));
                }
            } else {
                for (const auto& item : value) {
                    owned.emplace_back(std::string_view(item));
                }
            }
            commit_entity_value(out, std::move(owned));
        } else {
            auto owned = entity_value_slot<t_type>::make_value();
            owned.reserve(value.size());
            for (auto& item : value) {
                auto element = entity_value_slot<element_type>::make_value();
                if constexpr (movable_rvalue) {
                    assign_entity_value(element, std::move(item));
                } else {
                    assign_entity_value(element, item);
                }
                owned.push_back(std::move(element));
            }
            commit_entity_value(out, std::move(owned));
        }
    } else {
        if constexpr (std::is_nothrow_assignable_v<t_type&, v_type&&>) {
            out = std::forward<v_type>(value);
        } else {
            auto owned = entity_value_slot<t_type>::make_value();
            owned = std::forward<v_type>(value);
            commit_entity_value(out, std::move(owned));
        }
    }
}

template <typename... columns>
class entity_value_storage final {
    static_assert(unique_entity_columns<columns...>(), "duplicate entity column name");
    using slots_type = std::tuple<entity_value_slot<typename columns::value_type>...>;
    template <fixed_string name>
    auto& slot() {
        return std::get<column_index<name>()>(slots_);
    }
    template <fixed_string name>
    const auto& slot() const {
        return std::get<column_index<name>()>(slots_);
    }

public:
    using columns_type = std::tuple<columns...>;
    template <fixed_string name>
    static consteval std::size_t column_index() {
        constexpr auto result_value = descriptor_index<columns_type, name>();
        static_assert(result_value < sizeof...(columns), "unknown entity column");
        return result_value;
    }
    template <fixed_string name>
    static consteval std::string_view column_name() noexcept {
        return std::tuple_element_t<column_index<name>(), columns_type>::name.view();
    }
    entity_value_storage() = default;
    entity_value_storage(const entity_value_storage&) = delete;
    entity_value_storage& operator=(const entity_value_storage&) = delete;
    entity_value_storage(entity_value_storage&&) noexcept = default;
    entity_value_storage& operator=(entity_value_storage&&) noexcept = default;
    [[nodiscard]] entity_value_storage clone() const {
        entity_value_storage copy;
        [&]<std::size_t... i>(std::index_sequence<i...>) {
            ((std::get<i>(copy.slots_) = std::get<i>(slots_).clone()), ...);
        }(std::make_index_sequence<sizeof...(columns)>{});
        return copy;
    }
    template <fixed_string name>
    [[nodiscard]] const auto& get() const& RUVIA_LIFETIMEBOUND {
        const auto& value = slot<name>();
        if (value.state_ != decltype(value.state_)::value) {
            throw std::logic_error("entity value is not set");
        }
        return value.value_;
    }
    template <fixed_string name>
    const auto& get() const&& = delete;
    template <fixed_string name>
    [[nodiscard]] auto take() && {
        auto& value = slot<name>();
        if (value.state_ != decltype(value.state_)::value) {
            throw std::logic_error("entity value is not set");
        }
        auto taken = std::move(value.value_);
        value.clear();
        value.state_ = decltype(value.state_)::unset;
        return taken;
    }
    template <fixed_string name, typename value_type>
    void set(value_type&& value) {
        auto& target = slot<name>();
        assign_entity_value(target.value_, std::forward<value_type>(value));
        target.state_ = decltype(target.state_)::value;
    }
    template <fixed_string name>
    void set_null()
        requires(std::tuple_element_t<column_index<name>(), columns_type>::options.nullable_)
    {
        auto& value = slot<name>();
        value.clear();
        value.state_ = decltype(value.state_)::null;
    }
    template <fixed_string name>
    void reset() {
        auto& value = slot<name>();
        value.clear();
        value.state_ = decltype(value.state_)::unset;
    }
    template <fixed_string name>
    [[nodiscard]] bool is_set() const noexcept {
        const auto& value = slot<name>();
        return value.state_ != decltype(value.state_)::unset;
    }
    template <fixed_string name>
    [[nodiscard]] bool is_null() const noexcept {
        const auto& value = slot<name>();
        return value.state_ == decltype(value.state_)::null;
    }
    template <fixed_string name>
    [[nodiscard]] bool has_value() const noexcept {
        const auto& value = slot<name>();
        return value.state_ == decltype(value.state_)::value;
    }
    slots_type slots_;
};

template <typename tuple>
struct entity_storage_from_tuple;
template <typename... columns>
struct entity_storage_from_tuple<std::tuple<columns...>> {
    using type = entity_value_storage<columns...>;
};

}  // namespace ruvia::detail

// Construction, row rebinding and column lookup shared by every entity owner
// macro (SQL entity, SQL projection, Redis entity). Leaves access public.
#define RUVIA_DETAIL_ENTITY_OWNER_API(owner, storage_type, member) \
private:                                                           \
    template <typename>                                            \
    friend class ::ruvia::entity_rows;                             \
    explicit owner(storage_type&& storage) noexcept :              \
        member(std::move(storage)) {}                              \
                                                                   \
public:                                                            \
    owner() = default;                                             \
    owner(const owner&) = delete;                                  \
    owner& operator=(const owner&) = delete;                       \
    owner(owner&&) noexcept = default;                             \
    owner& operator=(owner&&) noexcept = default;                  \
    [[nodiscard]] owner clone() const {                            \
        return owner(member.clone());                              \
    }                                                              \
    template <::ruvia::fixed_string field_name>                    \
    static consteval std::size_t column_index() {                  \
        return storage_type::template column_index<field_name>();  \
    }                                                              \
    template <::ruvia::fixed_string field_name>                    \
    static consteval std::string_view column_name() noexcept {     \
        return storage_type::template column_name<field_name>();   \
    }

// Public value facades only forward; state transitions belong to the owner.
#define RUVIA_DETAIL_ENTITY_VALUE_API(member)                       \
    template <::ruvia::fixed_string name>                           \
    [[nodiscard]] const auto& get() const& RUVIA_LIFETIMEBOUND {    \
        return member.template get<name>();                         \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    const auto& get() const&& = delete;                             \
    template <::ruvia::fixed_string name>                           \
    [[nodiscard]] auto take() && {                                  \
        return std::move(member).template take<name>();             \
    }                                                               \
    template <::ruvia::fixed_string name, typename value_type>      \
    auto& set(value_type&& value) & RUVIA_LIFETIMEBOUND {           \
        member.template set<name>(std::forward<value_type>(value)); \
        return *this;                                               \
    }                                                               \
    template <::ruvia::fixed_string name, typename value_type>      \
    auto& set(value_type&&) && = delete;                            \
    template <::ruvia::fixed_string name>                           \
    auto& set_null() & RUVIA_LIFETIMEBOUND                          \
        requires requires { member.template set_null<name>(); }     \
    {                                                               \
        member.template set_null<name>();                           \
        return *this;                                               \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    auto& set_null() &&                                             \
        requires requires { member.template set_null<name>(); }     \
    = delete;                                                       \
    template <::ruvia::fixed_string name>                           \
    auto& reset() & RUVIA_LIFETIMEBOUND {                           \
        member.template reset<name>();                              \
        return *this;                                               \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    auto& reset() && = delete;                                      \
    template <::ruvia::fixed_string name>                           \
    [[nodiscard]] bool is_set() const noexcept {                    \
        return member.template is_set<name>();                      \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    [[nodiscard]] bool is_null() const noexcept {                   \
        return member.template is_null<name>();                     \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    [[nodiscard]] bool has_value() const noexcept {                 \
        return member.template has_value<name>();                   \
    }

namespace ruvia {
namespace detail {
struct query_row_count_policy final {
    static constexpr std::uint64_t default_value = 0;
    static constexpr bool implicit_conversion = true;
    static constexpr const char* invalid_value_message = "query row count must be in 0..9223372036854775807";

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater_equal(+value, 0) && std::in_range<std::int64_t>(+value);
    }
};
}  // namespace detail

// Non-negative row count or offset in SQL and RediSearch 64-bit signed range.
// Checked before narrowing so negative integers cannot wrap into huge counts.
class query_row_count final : public checked_integer<query_row_count, std::uint64_t, detail::query_row_count_policy> {
public:
    using checked_integer::checked_integer;
};

static_assert(std::is_trivially_copyable_v<query_row_count>);
static_assert(sizeof(query_row_count) == sizeof(std::uint64_t));

// A move-only entity collection.
template <typename entity_type>
class entity_rows final {
public:
    using value_type = entity_type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = entity_type&;
    using const_reference = const entity_type&;
    using pointer = entity_type*;
    using const_pointer = const entity_type*;
    using iterator = entity_type*;
    using const_iterator = const entity_type*;

    entity_rows() noexcept = default;
    entity_rows(const entity_rows&) = delete;
    entity_rows& operator=(const entity_rows&) = delete;
    entity_rows(entity_rows&&) noexcept = default;
    entity_rows& operator=(entity_rows&& other) noexcept {
        if (this != &other) {
            rows_ = std::move(other.rows_);
            other.rows_.clear();
        }
        return *this;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return rows_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return rows_.empty();
    }
    [[nodiscard]] const entity_type& operator[](std::size_t i) const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_[i];
    }
    [[nodiscard]] entity_type& operator[](std::size_t i) & noexcept RUVIA_LIFETIMEBOUND {
        return rows_[i];
    }
    const entity_type& operator[](std::size_t) const&& = delete;
    [[nodiscard]] const entity_type& front() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.front();
    }
    [[nodiscard]] entity_type& front() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.front();
    }
    const entity_type& front() const&& = delete;
    [[nodiscard]] const entity_type& back() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.back();
    }
    [[nodiscard]] entity_type& back() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.back();
    }
    const entity_type& back() const&& = delete;
    [[nodiscard]] entity_type* data() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data();
    }
    [[nodiscard]] const entity_type* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data();
    }
    const entity_type* data() const&& = delete;
    [[nodiscard]] entity_type* begin() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data();
    }
    [[nodiscard]] const entity_type* begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data();
    }
    [[nodiscard]] entity_type* end() & noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data() + rows_.size();
    }
    [[nodiscard]] const entity_type* end() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_.data() + rows_.size();
    }
    const entity_type* begin() const&& = delete;
    const entity_type* end() const&& = delete;
    void reserve(std::size_t count) & {
        rows_.reserve(count);
    }
    void reserve(std::size_t) && = delete;
    void push_back(entity_type&& entity) & {
        rows_.push_back(std::move(entity));
    }
    void push_back(entity_type&&) && = delete;
    [[nodiscard]] entity_rows clone() const {
        entity_rows copy;
        copy.rows_.reserve(rows_.size());
        for (const auto& row : rows_) {
            copy.rows_.push_back(row.clone());
        }
        return copy;
    }
    [[nodiscard]] std::span<const entity_type> rows() const& noexcept RUVIA_LIFETIMEBOUND {
        return rows_;
    }
    std::span<const entity_type> rows() const&& = delete;
    [[nodiscard]] std::vector<entity_type> take_rows() && noexcept {
        return std::exchange(rows_, {});
    }

private:
    std::vector<entity_type> rows_;
};

template <typename t_type>
concept any_entity = std::same_as<t_type, std::remove_cvref_t<t_type>> &&
    requires { typename t_type::columns_type; } &&
    ((requires { requires std::same_as<t_type, typename t_type::db_entity_type>; }) ||
        (requires { requires std::same_as<t_type, typename t_type::db_projection_type>; }) ||
        (requires { requires std::same_as<t_type, typename t_type::redis_entity_type>; }));

template <typename t_type>
concept writable_entity = any_entity<t_type> && !requires { typename t_type::db_projection_type; };

namespace detail {

template <typename column_type>
consteval bool is_raw_json_entity_column() noexcept {
    if constexpr (requires { column_type::data_type; }) {
        using data_enum = std::remove_cvref_t<decltype(column_type::data_type)>;
        if constexpr (requires { data_enum::json; data_enum::jsonb; }) {
            return column_type::data_type == data_enum::json || column_type::data_type == data_enum::jsonb;
        }
    }
    return false;
}

template <typename column_type>
consteval bool is_patchable_entity_column() noexcept {
    if constexpr (requires { column_type::options.read_only_; column_type::options.generated_type_; }) {
        using generated_enum = std::remove_cvref_t<decltype(column_type::options.generated_type_)>;
        return !column_type::options.read_only_ && column_type::options.generated_type_ == generated_enum::none;
    }
    return true;
}

inline void append_raw_entity_json(std::string& output, std::string_view raw) {
    auto cursor = raw;
    skip_json_whitespace(cursor);
    const auto start = cursor;
    if (!skip_json_value(cursor, 0)) {
        throw std::invalid_argument("entity JSON column does not contain valid JSON");
    }
    const auto token = start.substr(0, start.size() - cursor.size());
    skip_json_whitespace(cursor);
    if (!cursor.empty()) {
        throw std::invalid_argument("entity JSON column contains trailing JSON tokens");
    }
    output.append(token);
}

template <typename value_type>
void append_entity_json_scalar_or_array(std::string& output, const value_type& value, bytes_encoding binary_encoding) {
    if constexpr (std::is_same_v<value_type, std::string>) {
        append_json_string(output, value);
    } else if constexpr (std::is_same_v<value_type, bytes>) {
        append_model_binary(output, value.view(), binary_encoding);
    } else if constexpr (is_optional<value_type>::value) {
        if (!value) {
            output.append("null");
        } else {
            append_entity_json_scalar_or_array(output, *value, binary_encoding);
        }
    } else if constexpr (is_std_vector<value_type>::value) {
        output.push_back('[');
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                output.push_back(',');
            }
            first = false;
            if constexpr (std::is_same_v<typename is_std_vector<value_type>::value_type, bool>) {
                append_json_value(output, static_cast<bool>(item));
            } else {
                append_entity_json_scalar_or_array(output, item, binary_encoding);
            }
        }
        output.push_back(']');
    } else {
        append_json_value(output, value);
    }
}

}  // namespace detail

template <any_entity entity_type>
void append_json(std::string& output, const entity_type& entity, bytes_encoding binary_encoding = bytes_encoding::base64);

template <any_entity entity_type>
void append_json(std::string& output, const entity_rows<entity_type>& rows, bytes_encoding binary_encoding = bytes_encoding::base64) {
    output.push_back('[');
    bool first = true;
    for (const auto& row : rows) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        append_json(output, row, binary_encoding);
    }
    output.push_back(']');
}

template <any_entity entity_type>
void append_json(std::string& output, const entity_type& entity, bytes_encoding binary_encoding) {
    output.push_back('{');
    bool first = true;
    detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if (!entity.template is_set<column_type::name>()) {
            return;
        }
        if (!first) {
            output.push_back(',');
        }
        first = false;
        append_json_string(output, column_type::name.view());
        output.push_back(':');
        if (entity.template is_null<column_type::name>()) {
            output.append("null");
            return;
        }
        const auto& value = entity.template get<column_type::name>();
        if constexpr (detail::is_raw_json_entity_column<column_type>() && (std::is_same_v<typename column_type::value_type, std::string> || std::is_same_v<typename column_type::value_type, string>)) {
            detail::append_raw_entity_json(output, std::string_view(value));
        } else {
            detail::append_entity_json_scalar_or_array(output, value, binary_encoding);
        }
    });
    if constexpr (requires { typename entity_type::relations_type; }) {
        detail::for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t> {
            if (!entity.template is_set<relation_type::name>()) {
                return;
            }
            if (!first) {
                output.push_back(',');
            }
            first = false;
            append_json_string(output, relation_type::name.view());
            output.push_back(':');
            if (entity.template is_null<relation_type::name>()) {
                output.append("null");
                return;
            }
            append_json(output, entity.template get<relation_type::name>(), binary_encoding);
        });
    }
    output.push_back('}');
}

template <any_entity entity_type>
[[nodiscard]] std::string to_json(const entity_type& entity, bytes_encoding binary_encoding = bytes_encoding::base64) {
    std::string output;
    append_json(output, entity, binary_encoding);
    return output;
}

template <any_entity entity_type>
[[nodiscard]] std::string to_json(const entity_rows<entity_type>& rows, bytes_encoding binary_encoding = bytes_encoding::base64) {
    std::string output;
    append_json(output, rows, binary_encoding);
    return output;
}

template <typename model_type, any_entity entity_type>
    requires detail::is_model<model_type>
[[nodiscard]] model_type from_entity(const entity_type& source);

namespace detail {

template <typename target_type, typename source_type>
void assign_patch_value(target_type& out, const source_type& value) {
    if constexpr (is_std_vector<target_type>::value && is_ruvia_array<source_type>) {
        using element_type = typename is_std_vector<target_type>::value_type;
        target_type converted;
        converted.reserve(value.size());
        for (const auto& item : value) {
            auto elem = entity_value_slot<element_type>::make_value();
            assign_patch_value(elem, item);
            converted.push_back(std::move(elem));
        }
        commit_entity_value(out, std::move(converted));
    } else if constexpr (std::is_same_v<target_type, std::string> && (is_ruvia_json_value<source_type> || is_ruvia_json_object<source_type>)) {
        std::string owned(value.view());
        commit_entity_value(out, std::move(owned));
    } else if constexpr (std::is_same_v<target_type, string> && (is_ruvia_json_value<source_type> || is_ruvia_json_object<source_type>)) {
        commit_entity_value(out, string(value.view()));
    } else {
        assign_entity_value(out, value);
    }
}

template <typename field_type, typename source_type>
[[nodiscard]] field_type convert_entity_to_model_value(const source_type& value) {
    if constexpr (is_model<field_type> && any_entity<source_type>) {
        return ::ruvia::from_entity<field_type>(value);
    } else if constexpr (is_ruvia_array<field_type>) {
        using element_type = typename field_type::value_type;
        field_type result;
        result.reserve(value.size());
        for (const auto& item : value) {
            if constexpr (std::is_same_v<source_type, std::vector<bool>>) {
                result.push_back(convert_entity_to_model_value<element_type>(static_cast<bool>(item)));
            } else {
                result.push_back(convert_entity_to_model_value<element_type>(item));
            }
        }
        return result;
    } else if constexpr (std::is_same_v<field_type, json_value> && (std::is_same_v<source_type, std::string> || std::is_same_v<source_type, string>)) {
        auto parsed = json_value::parse(std::string(std::string_view(value)));
        if (!parsed.has_value()) {
            throw conversion_error(conversion_error::code_type::invalid_format, "entity JSON field is not valid JSON");
        }
        return std::move(parsed).take_value();
    } else if constexpr (std::is_same_v<field_type, json_object> && (std::is_same_v<source_type, std::string> || std::is_same_v<source_type, string>)) {
        auto parsed = json_object::parse(std::string(std::string_view(value)));
        if (!parsed.has_value()) {
            throw conversion_error(conversion_error::code_type::invalid_format, "entity JSON field is not a valid JSON object");
        }
        return std::move(parsed).take_value();
    } else if constexpr (std::is_same_v<field_type, string>) {
        return string(std::string_view(value));
    } else if constexpr (std::is_same_v<field_type, bytes>) {
        return value.clone();
    } else if constexpr (is_model_integer<field_type> && is_model_integer<source_type>) {
        if (!std::in_range<field_type>(value)) {
            throw conversion_error(conversion_error::code_type::out_of_range, "entity integer value is out of range for model field");
        }
        return static_cast<field_type>(value);
    } else if constexpr (is_model_floating<field_type> && (is_model_integer<source_type> || std::is_floating_point_v<source_type>)) {
        if constexpr (std::is_floating_point_v<source_type>) {
            return checked_model_float<field_type>(value);
        } else {
            return static_cast<field_type>(value);
        }
    } else {
        return field_type(value);
    }
}

}  // namespace detail

// Two-phase transactional patch from a RUVIA_MODEL onto an entity:
// - Missing model fields leave the entity column untouched.
// - Explicit `null` model fields call `set_null()` (or throw std::invalid_argument if non-nullable).
// - Present values convert and assign onto matching writable entity columns.
// On any exception, `target` remains completely unchanged.
template <writable_entity entity_type, typename model_type>
    requires detail::is_model<model_type>
void apply_model_patch(entity_type& target, const model_type& patch) {
    auto staged = target.clone();
    detail::model::visit_model_fields(patch, detail::model::model_access::schema<model_type>(), [&](const auto& descriptor, const auto& slot) {
        using descriptor_type = std::remove_cvref_t<decltype(descriptor)>;
        constexpr auto field_name = descriptor_type::source_name;
        constexpr auto col_index = detail::descriptor_index<typename entity_type::columns_type, field_name>();
        if constexpr (col_index < std::tuple_size_v<typename entity_type::columns_type>) {
            using column_type = std::tuple_element_t<col_index, typename entity_type::columns_type>;
            if constexpr (detail::is_patchable_entity_column<column_type>()) {
                if (!slot.is_present()) {
                    return;
                }
                if (slot.is_null()) {
                    if constexpr (column_type::options.nullable_) {
                        staged.template set_null<field_name>();
                    } else {
                        throw std::invalid_argument("cannot patch non-nullable entity column with null");
                    }
                    return;
                }
                if (slot.has_value()) {
                    auto converted = detail::entity_value_slot<typename column_type::value_type>::make_value();
                    detail::assign_patch_value(converted, *slot.value());
                    staged.template set<field_name>(std::move(converted));
                }
            }
        }
    });
    target = std::move(staged);
}

// Maps set columns (and loaded relations on db_entity) of `source` into a RUVIA_MODEL.
// Unset columns/unloaded relations leave the corresponding model fields unset.
template <typename model_type, any_entity entity_type>
    requires detail::is_model<model_type>
[[nodiscard]] model_type from_entity(const entity_type& source) {
    model_type result{};
    detail::model::visit_model_fields(result, detail::model::model_access::schema<model_type>(), [&](const auto& descriptor, auto& slot) {
        using descriptor_type = std::remove_cvref_t<decltype(descriptor)>;
        using slot_type = std::remove_cvref_t<decltype(slot)>;
        using field_value_type = typename slot_type::value_type;
        constexpr auto field_name = descriptor_type::source_name;
        constexpr auto col_index = detail::descriptor_index<typename entity_type::columns_type, field_name>();
        if constexpr (col_index < std::tuple_size_v<typename entity_type::columns_type>) {
            if (!source.template is_set<field_name>()) {
                return;
            }
            if (source.template is_null<field_name>()) {
                if constexpr (slot_type::nullable) {
                    slot.assign(nullptr);
                } else {
                    throw conversion_error(conversion_error::code_type::unexpected_null, "NULL entity column cannot map to non-nullable model field");
                }
                return;
            }
            slot.assign(detail::convert_entity_to_model_value<field_value_type>(source.template get<field_name>()));
        } else if constexpr (requires { typename entity_type::relations_type; }) {
            constexpr auto rel_index = detail::descriptor_index<typename entity_type::relations_type, field_name>();
            if constexpr (rel_index < std::tuple_size_v<typename entity_type::relations_type>) {
                if (!source.template is_set<field_name>()) {
                    return;
                }
                if (source.template is_null<field_name>()) {
                    if constexpr (slot_type::nullable) {
                        slot.assign(nullptr);
                    } else {
                        throw conversion_error(conversion_error::code_type::unexpected_null, "NULL entity relation cannot map to non-nullable model field");
                    }
                    return;
                }
                slot.assign(detail::convert_entity_to_model_value<field_value_type>(source.template get<field_name>()));
            }
        }
    });
    return result;
}

}  // namespace ruvia
