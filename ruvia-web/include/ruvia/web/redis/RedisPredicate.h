#pragma once

#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/FixedString.h"
#include "ruvia/web/detail/redis/RedisPredicateStorage.h"

namespace ruvia {

template <typename entity_type, FixedString name>
class redis_field_reference;

namespace detail {
struct redis_predicate_access;
}

// An owning Redis filter. Literal text is copied when the filter is built;
// the default PMR resource must outlive the filter. Compilers normalize their
// output into the destination worker pool before returning an operation.
class redis_predicate final {
public:
    redis_predicate() noexcept = default;
    redis_predicate(const redis_predicate&) = delete;
    redis_predicate& operator=(const redis_predicate&) = delete;
    redis_predicate(redis_predicate&&) noexcept = default;
    redis_predicate& operator=(redis_predicate&&) noexcept = default;
    bool empty() const noexcept {
        return storage_ == nullptr;
    }
    friend redis_predicate operator&&(redis_predicate left, const redis_predicate& right) {
        return combine(std::move(left), detail::redis_binary_operator::logical_and, right);
    }
    friend redis_predicate operator||(redis_predicate left, const redis_predicate& right) {
        return combine(std::move(left), detail::redis_binary_operator::logical_or, right);
    }

private:
    template <typename, FixedString>
    friend class redis_field_reference;
    friend struct detail::redis_predicate_access;
    static redis_predicate create() {
        redis_predicate result;
        auto* resource = std::pmr::get_default_resource();
        result.storage_ = detail::makePmrObject<detail::redis_predicate_storage>(resource, resource);
        return result;
    }
    static redis_predicate combine(redis_predicate left, detail::redis_binary_operator operation, const redis_predicate& right) {
        if (left.empty() || right.empty()) {
            throw std::invalid_argument("cannot combine empty Redis predicates");
        }
        auto& storage = *left.storage_;
        const auto left_root = storage.root;
        const auto offset = storage.nodes.size();
        const auto right_root = right.storage_->root + offset;
        const auto count = right.storage_->nodes.size();
        storage.nodes.reserve(offset + count + 1);
        for (std::size_t index = 0; index < count; ++index) {
            storage.nodes.emplace_back(right.storage_->nodes[index], storage.resource, offset);
        }
        storage.root = storage.add(detail::redis_expression_inspection::kind_type::binary);
        auto& root = storage.nodes[storage.root];
        root.binary = operation;
        root.operands = {left_root, right_root};
        return left;
    }
    std::unique_ptr<detail::redis_predicate_storage, detail::PmrObjectDeleter<detail::redis_predicate_storage>> storage_;
};

namespace detail {
struct redis_predicate_access final {
    static redis_expression root(const redis_predicate& predicate) noexcept {
        return predicate.empty() ? redis_expression{} : redis_expression(*predicate.storage_, predicate.storage_->root);
    }
};
}  // namespace detail

template <typename entity_type, FixedString name>
class redis_field_reference final {
    static_assert(entity_type::template columnIndex<name>() < std::tuple_size_v<typename entity_type::Columns>);
    using operation = detail::redis_binary_operator;
    using kind = detail::redis_expression_inspection::kind_type;

public:
    template <detail::redis_predicate_argument value_type>
    redis_predicate operator==(value_type&& value) const {
        return compare(operation::equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    redis_predicate operator!=(value_type&& value) const {
        return compare(operation::not_equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    redis_predicate operator<(value_type&& value) const {
        return compare(operation::less, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    redis_predicate operator<=(value_type&& value) const {
        return compare(operation::less_equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    redis_predicate operator>(value_type&& value) const {
        return compare(operation::greater, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    redis_predicate operator>=(value_type&& value) const {
        return compare(operation::greater_equal, std::forward<value_type>(value));
    }
    redis_predicate is_null() const {
        return compare(operation::equal, nullptr);
    }
    redis_predicate is_not_null() const {
        return compare(operation::not_equal, nullptr);
    }
    template <detail::redis_predicate_argument lower_type, detail::redis_predicate_argument upper_type>
    redis_predicate between(lower_type&& lower, upper_type&& upper) const {
        auto result = create_field();
        auto& storage = *result.storage_;
        const auto lower_node = storage.add_value(std::forward<lower_type>(lower));
        const auto upper_node = storage.add_value(std::forward<upper_type>(upper));
        storage.root = storage.add(kind::between);
        storage.nodes[storage.root].operands = {0, lower_node, upper_node};
        return result;
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    redis_predicate in(std::span<const value_type> values) const {
        return list(values, operation::in);
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    redis_predicate in(std::initializer_list<value_type> values) const {
        return in(std::span<const value_type>(values.begin(), values.size()));
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    redis_predicate not_in(std::span<const value_type> values) const {
        return list(values, operation::not_in);
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    redis_predicate not_in(std::initializer_list<value_type> values) const {
        return not_in(std::span<const value_type>(values.begin(), values.size()));
    }

private:
    static redis_predicate create_field() {
        auto result = redis_predicate::create();
        auto& storage = *result.storage_;
        const auto index = storage.add(kind::field);
        storage.nodes[index].field = name.view();
        storage.nodes[index].entity_prefix = entity_type::prefix();
        return result;
    }
    template <detail::redis_predicate_argument value_type>
    static redis_predicate compare(operation op, value_type&& value) {
        auto result = create_field();
        auto& storage = *result.storage_;
        const auto literal = storage.add_value(std::forward<value_type>(value));
        storage.root = storage.add(kind::binary);
        storage.nodes[storage.root].binary = op;
        storage.nodes[storage.root].operands = {0, literal};
        return result;
    }
    template <typename value_type>
    static redis_predicate list(std::span<const value_type> values, operation op) {
        auto result = create_field();
        auto& storage = *result.storage_;
        std::pmr::vector<std::size_t> operands(storage.resource);
        operands.reserve(values.size());
        for (const auto& value : values) {
            operands.push_back(storage.add_value(value));
        }
        const auto list_node = storage.add(kind::list);
        storage.nodes[list_node].operands = std::move(operands);
        storage.root = storage.add(kind::binary);
        storage.nodes[storage.root].binary = op;
        storage.nodes[storage.root].operands = {0, list_node};
        return result;
    }
};

}  // namespace ruvia
