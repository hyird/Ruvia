#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/model_types.h"

namespace ruvia::detail {

enum class redis_binary_operator : std::uint8_t { equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    logical_and,
    logical_or,
    in,
    not_in };
enum class redis_literal_kind : std::uint8_t { null,
    string,
    signed_integer,
    unsigned_integer,
    real,
    boolean };

// Literals own their text immediately. Only Redis scalar values are accepted;
// SQL expressions, statements and backend parameter types cannot enter this AST.
class redis_literal final {
    using storage_type = std::variant<std::monostate, std::pmr::string, std::int64_t, std::uint64_t, double, bool>;

public:
    explicit redis_literal(std::pmr::memory_resource*) noexcept {}
    redis_literal(std::nullptr_t, std::pmr::memory_resource*) noexcept {}
    redis_literal(std::string_view value, std::pmr::memory_resource* resource)
        : storage_(std::in_place_type<std::pmr::string>, value, resource) {}
    redis_literal(const char* value, std::pmr::memory_resource* resource)
        : redis_literal(value ? std::string_view(value) : throw std::invalid_argument("Redis predicate text must not be null"), resource) {}
    template <typename value_type>
        requires std::is_same_v<value_type, bool>
    redis_literal(value_type value, std::pmr::memory_resource*)
        : storage_(value) {}
    template <typename value_type>
        requires is_model_integer<value_type>
    redis_literal(value_type value, std::pmr::memory_resource*) {
        if constexpr (std::is_signed_v<value_type>) {
            storage_.emplace<std::int64_t>(value);
        } else {
            storage_.emplace<std::uint64_t>(value);
        }
    }
    template <typename value_type>
        requires std::is_floating_point_v<value_type>
    redis_literal(value_type value, std::pmr::memory_resource*)
        : storage_(static_cast<double>(value)) {}
    template <typename value_type>
        requires(is_ruvia_scalar<std::remove_cvref_t<value_type>>)
    redis_literal(const value_type& value, std::pmr::memory_resource* resource)
        : redis_literal(static_cast<model_scalar_value_type<std::remove_cvref_t<value_type>>>(value), resource) {}
    redis_literal(const string& value, std::pmr::memory_resource* resource)
        : redis_literal(value.view(), resource) {}
    redis_literal(redis_literal&&) noexcept = default;
    redis_literal& operator=(redis_literal&&) = delete;
    redis_literal(const redis_literal&) = delete;
    redis_literal& operator=(const redis_literal&) = delete;
    redis_literal(const redis_literal& other, std::pmr::memory_resource* resource)
        : storage_(std::visit([resource](const auto& value) -> storage_type {
              using value_type = std::remove_cvref_t<decltype(value)>;
              if constexpr (std::is_same_v<value_type, std::pmr::string>) {
                  return storage_type(std::in_place_type<std::pmr::string>, value, resource);
              } else {
                  return storage_type(value);
              }
          },
              other.storage_)) {}

    redis_literal_kind kind() const noexcept {
        return static_cast<redis_literal_kind>(storage_.index());
    }
    std::string_view text() const noexcept {
        return std::get<std::pmr::string>(storage_);
    }
    std::int64_t signed_value() const noexcept {
        return std::get<std::int64_t>(storage_);
    }
    std::uint64_t unsigned_value() const noexcept {
        return std::get<std::uint64_t>(storage_);
    }
    double real_value() const noexcept {
        return std::get<double>(storage_);
    }
    bool boolean_value() const noexcept {
        return std::get<bool>(storage_);
    }

private:
    storage_type storage_;
};

template <typename value_type>
concept redis_predicate_argument = requires(value_type&& value, std::pmr::memory_resource* resource) {
    redis_literal(std::forward<value_type>(value), resource);
};

// Only redis_field_reference creates leaves, and only redis_predicate combines
// them, so the tree has exactly these shapes: a comparison or IN is a binary
// node {field, literal} or {field, list}, BETWEEN is {field, lower, upper}, a
// list holds literals, and logical AND/OR is a binary node of two predicates.
struct redis_predicate_node final {
    enum class kind_type : std::uint8_t { field,
        literal,
        binary,
        between,
        list };

    explicit redis_predicate_node(kind_type kind, std::pmr::memory_resource* resource)
        : kind_(kind),
          literal_(resource),
          operands_(resource) {}
    redis_predicate_node(const redis_predicate_node& other, std::pmr::memory_resource* resource, std::size_t offset = 0)
        : kind_(other.kind_),
          binary_(other.binary_),
          field_(other.field_),
          entity_prefix_(other.entity_prefix_),
          literal_(other.literal_, resource),
          operands_(other.operands_, resource) {
        if (offset != 0) {
            for (auto& operand : operands_) {
                operand += offset;
            }
        }
    }
    redis_predicate_node(redis_predicate_node&&) noexcept = default;
    redis_predicate_node& operator=(redis_predicate_node&&) = delete;
    redis_predicate_node(const redis_predicate_node&) = delete;
    redis_predicate_node& operator=(const redis_predicate_node&) = delete;
    kind_type kind_;
    redis_binary_operator binary_{redis_binary_operator::equal};
    std::string_view field_{};
    std::string_view entity_prefix_{};
    redis_literal literal_;
    std::pmr::vector<std::size_t> operands_;
};

struct redis_predicate_storage final {
    explicit redis_predicate_storage(std::pmr::memory_resource* resource, std::size_t initial_capacity = 0)
        : resource_(resource),
          nodes_(resource) {
        if (initial_capacity != 0) {
            nodes_.reserve(initial_capacity);
        }
    }
    redis_predicate_storage(const redis_predicate_storage& other, std::pmr::memory_resource* resource)
        : resource_(resource),
          nodes_(resource),
          root_(other.root_) {
        nodes_.reserve(other.nodes_.size());
        for (const auto& node : other.nodes_) {
            nodes_.emplace_back(node, resource);
        }
    }
    std::pmr::memory_resource* resource_;
    std::pmr::vector<redis_predicate_node> nodes_;
    std::size_t root_{};
    std::size_t add(redis_predicate_node::kind_type kind) {
        nodes_.emplace_back(kind, resource_);
        return nodes_.size() - 1;
    }
    template <redis_predicate_argument value_type>
    std::size_t add_value(value_type&& value) {
        const auto index = add(redis_predicate_node::kind_type::literal);
        // Reconstruct only after the owning value has been successfully created.
        redis_literal literal(std::forward<value_type>(value), resource_);
        std::destroy_at(&nodes_[index].literal_);
        std::construct_at(&nodes_[index].literal_, std::move(literal));
        return index;
    }
};

}  // namespace ruvia::detail
