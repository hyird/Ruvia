#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"

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

// A move-only entity collection. Elements share this collection's resource;
// the resource owner must outlive the collection and every element.
template <typename entity_type>
class entity_rows final {
public:
    explicit entity_rows(std::pmr::memory_resource* resource = nullptr)
        : rows_(pmr_resource_or_default(resource)) {}
    template <typename owner_type>
        requires requires(owner_type& owner) {
            { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        }
    explicit entity_rows(owner_type& owner)
        : entity_rows(owner.resource()) {}
    entity_rows(const entity_rows&) = delete;
    entity_rows& operator=(const entity_rows&) = delete;
    entity_rows(entity_rows&&) noexcept = default;
    entity_rows& operator=(entity_rows&&) = delete;
    std::size_t size() const noexcept {
        return rows_.size();
    }
    bool empty() const noexcept {
        return rows_.empty();
    }
    const entity_type& operator[](std::size_t i) const& noexcept {
        return rows_[i];
    }
    entity_type& operator[](std::size_t i) & noexcept {
        return rows_[i];
    }
    const entity_type& operator[](std::size_t) const&& = delete;
    const entity_type& front() const& noexcept {
        return rows_.front();
    }
    entity_type& front() & noexcept {
        return rows_.front();
    }
    const entity_type& front() const&& = delete;
    const entity_type& back() const& noexcept {
        return rows_.back();
    }
    entity_type& back() & noexcept {
        return rows_.back();
    }
    const entity_type& back() const&& = delete;
    entity_type* begin() & noexcept {
        return rows_.data();
    }
    const entity_type* begin() const& noexcept {
        return rows_.data();
    }
    entity_type* end() & noexcept {
        return rows_.data() + rows_.size();
    }
    const entity_type* end() const& noexcept {
        return rows_.data() + rows_.size();
    }
    const entity_type* begin() const&& = delete;
    const entity_type* end() const&& = delete;
    void reserve(std::size_t count) {
        rows_.reserve(count);
    }
    void push_back(entity_type&& entity) {
        if (entity.resource() != rows_.get_allocator().resource()) {
            throw std::invalid_argument("entity result elements must use the result's memory resource");
        }
        rows_.push_back(std::move(entity));
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return rows_.get_allocator().resource();
    }

private:
    std::pmr::vector<entity_type> rows_;
};

}  // namespace ruvia
