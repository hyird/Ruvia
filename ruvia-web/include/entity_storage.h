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
#include "fixed_string.h"
#include "integer_parse.h"
#include "model.h"

namespace ruvia::detail {

enum class text_scalar_parse_error {
    invalid_format,
    out_of_range,
};

template <typename t_type>
concept text_scalar = std::is_same_v<t_type, std::remove_cv_t<t_type>> && (std::is_same_v<t_type, bool> || is_model_integer<t_type> || std::is_floating_point_v<t_type> || std::is_same_v<t_type, std::string> || std::is_same_v<t_type, string> || std::is_same_v<t_type, std::string_view>);

// The one text-to-scalar syntax shared by database results and Redis replies:
// boolean tokens 1/t/true/TRUE and 0/f/false/FALSE, strict decimal integers
// (parse_integer), and finite floating-point values. std::string_view borrows
// `text`. A float that is syntactically complete but not representable is
// out_of_range; any other rejection, including a non-finite value, is invalid_format.
template <text_scalar t_type>
[[nodiscard]] std::variant<t_type, text_scalar_parse_error> parse_text_scalar(std::string_view text) {
    using result_type = std::variant<t_type, text_scalar_parse_error>;
    if constexpr (std::is_same_v<t_type, std::string_view>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, std::string>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, string>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, bool>) {
        if (text == "1" || text == "t" || text == "true" || text == "TRUE") {
            return result_type(std::in_place_index<0>, true);
        }
        if (text == "0" || text == "f" || text == "false" || text == "FALSE") {
            return result_type(std::in_place_index<0>, false);
        }
        return text_scalar_parse_error::invalid_format;
    } else if constexpr (std::is_floating_point_v<t_type>) {
        t_type converted{};
        const auto* const last = text.data() + text.size();
        const auto [end, error] = std::from_chars(text.data(), last, converted);
        if (error == std::errc::result_out_of_range && end == last) {
            return text_scalar_parse_error::out_of_range;
        }
        if (error != std::errc{} || end != last || !std::isfinite(converted)) {
            return text_scalar_parse_error::invalid_format;
        }
        return result_type(std::in_place_index<0>, converted);
    } else {
        const auto parsed = parse_integer<t_type>(text);
        if (const auto* error = std::get_if<integer_parse_error>(&parsed)) {
            return *error == integer_parse_error::out_of_range ? text_scalar_parse_error::out_of_range : text_scalar_parse_error::invalid_format;
        }
        return result_type(std::in_place_index<0>, std::get<t_type>(parsed));
    }
}

// Shared owning value storage. Backend descriptors and relations stay with
// their backend.
template <typename t_type>
struct is_std_vector : std::false_type {};
template <typename t_type>
struct is_std_vector<std::vector<t_type>> : std::true_type {
    using value_type = t_type;
};

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

template <typename t_type>
struct is_optional : std::false_type {};
template <typename t_type>
struct is_optional<std::optional<t_type>> : std::true_type {
    using value_type = t_type;
};

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
    constexpr bool movable_rvalue = !std::is_lvalue_reference_v<v_type> && !std::is_const_v<std::remove_reference_t<v_type>>;
    if constexpr (std::is_arithmetic_v<t_type> && is_ruvia_scalar<v_type>) {
        assign_entity_value(out, value.value_);
    } else if constexpr (is_model_integer<t_type> && std::is_arithmetic_v<std::remove_cvref_t<v_type>>) {
        // 整数输入先检查范围；浮点输入须由调用方显式选择整数转换策略。
        out = integer_value<t_type>(value).value_;
    } else if constexpr (std::is_floating_point_v<t_type> && std::is_arithmetic_v<std::remove_cvref_t<v_type>>) {
        if constexpr (std::is_floating_point_v<std::remove_cvref_t<v_type>>) {
            out = checked_model_float<t_type>(value);
        } else {
            out = static_cast<t_type>(value);
        }
    } else if constexpr (std::is_same_v<t_type, string>) {
        if constexpr (movable_rvalue && std::is_same_v<std::remove_cvref_t<v_type>, string>) {
            commit_entity_value(out, rebind_model_value(std::move(value)));
        } else {
            auto owned = string(std::string_view(value));
            commit_entity_value(out, std::move(owned));
        }
    } else if constexpr (std::is_same_v<t_type, std::string>) {
        if constexpr (movable_rvalue && std::is_same_v<std::remove_cvref_t<v_type>, std::string>) {
            std::string owned(std::move(value));
            commit_entity_value(out, std::move(owned));
            return;
        }
        std::string owned{std::string_view(value)};
        commit_entity_value(out, std::move(owned));
    } else if constexpr (is_optional<t_type>::value) {
        using element_type = typename is_optional<t_type>::value_type;
        if constexpr (is_optional<std::remove_cvref_t<v_type>>::value) {
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
        if constexpr (movable_rvalue && std::is_same_v<std::remove_cvref_t<v_type>, t_type>) {
            t_type owned(std::move(value));
            commit_entity_value(out, std::move(owned));
            return;
        }
        if constexpr (std::is_arithmetic_v<element_type> && is_exact_element_range<v_type, element_type>()) {
            t_type owned(value.begin(), value.end());
            commit_entity_value(out, std::move(owned));
        } else if constexpr (std::is_same_v<element_type, std::string>) {
            auto owned = entity_value_slot<t_type>::make_value();
            owned.reserve(value.size());
            if constexpr (movable_rvalue && std::is_same_v<std::remove_cvref_t<decltype(*value.begin())>, std::string>) {
                for (auto& item : value) {
                    owned.emplace_back(std::move(item));
                }
                commit_entity_value(out, std::move(owned));
                return;
            }
            for (const auto& item : value) {
                owned.emplace_back(std::string_view(item));
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
    entity_value_storage& operator=(entity_value_storage&&) = delete;
    template <fixed_string name>
    auto& get() & RUVIA_LIFETIMEBOUND {
        auto& value = slot<name>();
        if (value.state_ != decltype(value.state_)::value) {
            throw std::logic_error("entity value is not set");
        }
        return value.value_;
    }
    template <fixed_string name>
    auto& get() && = delete;
    template <fixed_string name>
    const auto& get() const& RUVIA_LIFETIMEBOUND {
        return const_cast<entity_value_storage*>(this)->template get<name>();
    }
    template <fixed_string name>
    const auto& get() const&& = delete;
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
    bool is_set() const {
        const auto& value = slot<name>();
        return value.state_ != decltype(value.state_)::unset;
    }
    template <fixed_string name>
    bool is_null() const {
        const auto& value = slot<name>();
        return value.state_ == decltype(value.state_)::null;
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
    auto& get() & RUVIA_LIFETIMEBOUND {                             \
        return member.template get<name>();                         \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    auto& get() && = delete;                                        \
    template <::ruvia::fixed_string name>                           \
    const auto& get() const& RUVIA_LIFETIMEBOUND {                  \
        return member.template get<name>();                         \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    const auto& get() const&& = delete;                             \
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
    bool is_set() const {                                           \
        return member.template is_set<name>();                      \
    }                                                               \
    template <::ruvia::fixed_string name>                           \
    bool is_null() const {                                          \
        return member.template is_null<name>();                     \
    }

namespace ruvia {
// Non-negative row count or offset in SQL and RediSearch 64-bit signed range.
// Checked before narrowing so negative integers cannot wrap into huge counts.
class query_row_count final {
public:
    constexpr query_row_count() noexcept = default;

    template <std::integral integer_type>
    constexpr query_row_count(integer_type value) :
        value_(is_valid_value(value) ? static_cast<std::uint64_t>(value) : throw std::invalid_argument("query row count must be in 0..9223372036854775807")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater_equal(+value, 0) && std::in_range<std::int64_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<query_row_count> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return query_row_count(static_cast<std::uint64_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(query_row_count, query_row_count) = default;

private:
    struct validated_tag_type {};

    constexpr query_row_count(std::uint64_t value, validated_tag_type) noexcept :
        value_(value) {}

    std::uint64_t value_{0};
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
    void reserve(std::size_t count) {
        rows_.reserve(count);
    }
    void push_back(entity_type&& entity) {
        rows_.push_back(std::move(entity));
    }

private:
    std::vector<entity_type> rows_;
};

}  // namespace ruvia
