#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {
// Non-negative row count or offset in SQL and RediSearch 64-bit signed range.
// Checked before narrowing so negative integers cannot wrap into huge counts.
class query_row_count final {
public:
    constexpr query_row_count() noexcept = default;

    template <std::integral integer_type>
    constexpr query_row_count(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint64_t>(value)
                     : throw std::invalid_argument("query row count must be in 0..9223372036854775807")) {}

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

    constexpr query_row_count(std::uint64_t value, validated_tag_type) noexcept
        : value_(value) {}

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
