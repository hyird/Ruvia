#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/detail/redis/redis_entity_codec.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/detail/redis/redis_entity_traits.h"
#include "ruvia/web/detail/redis/redis_predicate_storage.h"
#include "ruvia/web/detail/redis/redis_query_compile.h"
#include "ruvia/web/detail/redis/redis_repository_commands.h"
#include "ruvia/web/detail/redis/redis_repository_mapping.h"
#include "ruvia/web/entity_rows.h"
#include "ruvia/web/fixed_string.h"
#include "ruvia/web/redis/redis_handle.h"
namespace ruvia {

template <typename entity_type, fixed_string name>
class redis_field_reference;

namespace detail {
struct redis_predicate_access;
}

// An owning Redis filter. Literal text is copied into the process resource when
// the filter is built, so the filter may outlive the strings used to build it.
// Compilers normalize their output into the destination worker pool before
// returning an operation.
class redis_predicate final {
public:
    redis_predicate() noexcept = default;
    redis_predicate(const redis_predicate& other) {
        if (other.storage_) {
            auto* resource = process_resource();
            storage_ = make_pmr_object<detail::redis_predicate_storage>(resource, *other.storage_, resource);
        }
    }
    redis_predicate& operator=(const redis_predicate& other) {
        if (this != &other) {
            *this = redis_predicate(other);
        }
        return *this;
    }
    redis_predicate(redis_predicate&&) noexcept = default;
    redis_predicate& operator=(redis_predicate&&) noexcept = default;
    bool empty() const noexcept {
        return storage_ == nullptr;
    }
    redis_predicate& and_where(const redis_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty Redis predicates");
        }
        if (empty()) {
            *this = other;
        } else {
            append(detail::redis_binary_operator::logical_and, other);
        }
        return *this;
    }
    redis_predicate& or_where(const redis_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty Redis predicates");
        }
        if (empty()) {
            *this = other;
        } else {
            append(detail::redis_binary_operator::logical_or, other);
        }
        return *this;
    }
    friend redis_predicate operator&&(redis_predicate left, const redis_predicate& right) {
        left.append(detail::redis_binary_operator::logical_and, right);
        return left;
    }
    friend redis_predicate operator||(redis_predicate left, const redis_predicate& right) {
        left.append(detail::redis_binary_operator::logical_or, right);
        return left;
    }

private:
    template <typename, fixed_string>
    friend class redis_field_reference;
    friend struct detail::redis_predicate_access;
    static redis_predicate create(std::size_t initial_capacity = 0) {
        redis_predicate result;
        auto* resource = process_resource();
        result.storage_ = make_pmr_object<detail::redis_predicate_storage>(resource, resource, initial_capacity);
        return result;
    }
    void append(detail::redis_binary_operator operation, const redis_predicate& right) {
        if (empty() || right.empty()) {
            throw std::invalid_argument("cannot combine empty Redis predicates");
        }
        auto& storage = *storage_;
        const auto left_root = storage.root_;
        const auto offset = storage.nodes_.size();
        const auto right_root = right.storage_->root_ + offset;
        const auto count = right.storage_->nodes_.size();
        storage.nodes_.reserve(offset + count + 1);
        for (std::size_t index = 0; index < count; ++index) {
            storage.nodes_.emplace_back(right.storage_->nodes_[index], storage.resource_, offset);
        }
        storage.root_ = storage.add(detail::redis_predicate_node::kind_type::binary);
        auto& root = storage.nodes_[storage.root_];
        root.binary_ = operation;
        root.operands_ = {left_root, right_root};
    }
    std::unique_ptr<detail::redis_predicate_storage, pmr_object_deleter<detail::redis_predicate_storage>> storage_;
};

namespace detail {
struct redis_predicate_access final {
    // Null for an empty predicate.
    static const redis_predicate_storage* storage(const redis_predicate& predicate) noexcept {
        return predicate.storage_.get();
    }
};
}  // namespace detail

template <typename entity_type, fixed_string name>
class redis_field_reference final {
    static_assert(detail::redis_entity_schema<entity_type>, "Redis field references require a Redis entity");
    static_assert(entity_type::template column_index<name>() < std::tuple_size_v<typename entity_type::columns_type>);
    using operation = detail::redis_binary_operator;
    using kind = detail::redis_predicate_node::kind_type;

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
        auto result_value = create_field(4);
        auto& storage = *result_value.storage_;
        const auto lower_node = storage.add_value(std::forward<lower_type>(lower));
        const auto upper_node = storage.add_value(std::forward<upper_type>(upper));
        storage.root_ = storage.add(kind::between);
        storage.nodes_[storage.root_].operands_ = {0, lower_node, upper_node};
        return result_value;
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
    static redis_predicate create_field(std::size_t initial_capacity) {
        auto result_value = redis_predicate::create(initial_capacity);
        auto& storage = *result_value.storage_;
        const auto index = storage.add(kind::field);
        storage.nodes_[index].field_ = name.view();
        storage.nodes_[index].entity_prefix_ = entity_type::prefix();
        return result_value;
    }
    template <detail::redis_predicate_argument value_type>
    static redis_predicate compare(operation op, value_type&& value) {
        auto result_value = create_field(3);
        auto& storage = *result_value.storage_;
        const auto literal = storage.add_value(std::forward<value_type>(value));
        storage.root_ = storage.add(kind::binary);
        storage.nodes_[storage.root_].binary_ = op;
        storage.nodes_[storage.root_].operands_ = {0, literal};
        return result_value;
    }
    template <typename value_type>
    static redis_predicate list(std::span<const value_type> values, operation op) {
        auto result_value = create_field(values.size() + 3);
        auto& storage = *result_value.storage_;
        std::pmr::vector<std::size_t> operands(storage.resource_);
        operands.reserve(values.size());
        for (const auto& value : values) {
            operands.push_back(storage.add_value(value));
        }
        const auto list_node = storage.add(kind::list);
        storage.nodes_[list_node].operands_ = std::move(operands);
        storage.root_ = storage.add(kind::binary);
        storage.nodes_[storage.root_].binary_ = op;
        storage.nodes_[storage.root_].operands_ = {0, list_node};
        return result_value;
    }
};

}  // namespace ruvia

namespace ruvia {

struct redis_column_options final {
    bool primary_key_{false};
    bool nullable_{false};
};

template <fixed_string field_name, typename t_type, redis_column_options options_type = {}>
struct redis_column final {
    static_assert(detail::is_redis_entity_scalar<t_type>, "Redis columns require owning scalar values");
    static_assert(!options_type.primary_key_ || (detail::is_redis_entity_primary_key_scalar<t_type> && !options_type.nullable_),
        "Redis primary keys must be non-nullable strings or integers");
    static constexpr auto name = field_name;
    using value_type = t_type;
    static constexpr auto options = options_type;
};

namespace detail {
template <typename t_type>
struct is_redis_column : std::false_type {};
template <fixed_string name, typename t_type, redis_column_options options_type>
struct is_redis_column<redis_column<name, t_type, options_type>> : std::true_type {};
template <fixed_string key_prefix, typename... columns>
struct redis_entity_storage final {
    static_assert((is_redis_column<columns>::value && ...),
        "Redis entities require RUVIA_REDIS_COLUMN descriptors");
    static_assert((std::size_t{columns::options.primary_key_} + ... + std::size_t{0}) == 1,
        "Redis entities require exactly one primary key");
    static_assert(unique_entity_columns<columns...>(), "duplicate Redis entity column name");
    static constexpr auto prefix_value = key_prefix;
    using type = entity_value_storage<columns...>;
};
}  // namespace detail

#define RUVIA_REDIS_COLUMN(name, value_type, ...) ::ruvia::redis_column<::ruvia::fixed_string{#name}, value_type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_REDIS_ENTITY(name, key_prefix, ...)                                                                      \
    struct name final {                                                                                                \
    private:                                                                                                           \
        using descriptor_type = ::ruvia::detail::redis_entity_storage<::ruvia::fixed_string{key_prefix}, __VA_ARGS__>; \
        using storage_type = typename descriptor_type::type;                                                           \
        storage_type entity_;                                                                                          \
                                                                                                                       \
    public:                                                                                                            \
        using redis_entity_type = name;                                                                                \
        using columns_type = typename storage_type::columns_type;                                                      \
        explicit name(std::pmr::memory_resource* resource = nullptr)                                                   \
            : entity_(resource) {}                                                                                     \
        template <typename owner_type>                                                                                 \
            requires requires(owner_type& owner) {                                                                     \
                { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;                               \
            }                                                                                                          \
        explicit name(owner_type& owner)                                                                               \
            : entity_(owner.resource()) {}                                                                             \
        static constexpr std::string_view prefix() noexcept {                                                          \
            return descriptor_type::prefix_value.view();                                                               \
        }                                                                                                              \
        template <::ruvia::fixed_string field_name>                                                                    \
        static consteval std::size_t column_index() {                                                                  \
            return storage_type::template index<field_name>();                                                         \
        }                                                                                                              \
        template <::ruvia::fixed_string field_name>                                                                    \
        static constexpr ::ruvia::redis_field_reference<name, field_name> column() noexcept {                          \
            return {};                                                                                                 \
        }                                                                                                              \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_)                                                                         \
    };

}  // namespace ruvia

namespace ruvia {

enum class redis_order_direction : std::uint8_t {
    asc,
    desc,
};

struct redis_find_order final {
    borrowed_text field_{};
    redis_order_direction direction_{redis_order_direction::asc};
};

// RediSearch always pages results; this page size applies when `take_` is unset.
inline constexpr std::uint64_t redis_find_default_take = 100;

// Redis filters and pagination have no SQL relation, lock or cache options.
// With `take_` unset, find() returns every match or throws redis_error
// (command_error) when more than redis_find_default_take entities match after
// `skip_`; it never truncates silently. find_and_count() returns one page of
// that size together with the total count.
struct redis_find_options final {
    redis_predicate where_{};
    std::optional<redis_find_order> order_{};
    std::optional<std::uint64_t> skip_{};
    std::optional<std::uint64_t> take_{};
};

enum class redis_index_kind : std::uint8_t {
    none,
    tag,
    text,
    numeric,
};

struct redis_index_config final {
    borrowed_text field_{};
    redis_index_kind kind_{redis_index_kind::tag};
    bool sortable_{false};
};

struct redis_repository_config final {
    borrowed_text prefix_{};
    std::vector<redis_index_config> indexes_{};
};

// Omitted expiration preserves an existing TTL; new entities are persistent.
struct redis_write_options final {
    std::optional<std::chrono::milliseconds> ttl_{};
    bool persist_{false};
};

class redis_write_result final {
public:
    explicit constexpr redis_write_result(std::uint64_t affected_entities) noexcept
        : affected_entities_(affected_entities) {}
    [[nodiscard]] constexpr std::uint64_t affected_entities() const noexcept {
        return affected_entities_;
    }

private:
    std::uint64_t affected_entities_{};
};

template <typename entity_type>
class redis_repository final {
    static_assert(detail::redis_entity_schema<entity_type>, "Redis repositories require a RUVIA_REDIS_ENTITY declaration");

public:
    redis_repository(const redis_repository&) = delete;
    redis_repository& operator=(const redis_repository&) = delete;

    // A repository is tied to its worker operation scope. Moving it transfers
    // its value registration and PMR mapping; copying would create a second
    // owner with ambiguous mapping lifetime.
    redis_repository(redis_repository&& other) noexcept
        : handle_(other.handle_),
          mapping_(std::move(other.mapping_)),
          registration_(std::move(other.registration_), this) {
        // The source no longer has a registration. Destroy moved-from
        // PMR containers now: implementations may retain allocator-owned state.
        other.mapping_.reset();
    }
    redis_repository& operator=(redis_repository&&) = delete;

    [[nodiscard]] task<redis_write_result> insert(
        const entity_type& entity, redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch{};
        return write("insert", detail::redis_entity_id(entity, id_scratch), entity, options);
    }

    [[nodiscard]] task<redis_write_result> upsert(
        const entity_type& entity, redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch{};
        return write("upsert", detail::redis_entity_id(entity, id_scratch), entity, options);
    }

    [[nodiscard]] task<redis_write_result> update(
        const redis_predicate& predicate, const entity_type& changes,
        redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch{};
        const auto id = require_primary_key(predicate, id_scratch);
        return write("update", id, changes, options);
    }

    [[nodiscard]] task<entity_rows<entity_type>> find(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        auto args = detail::compile_redis_find(mapping(), options, memory);
        detail::redis_map_many<entity_type> mapper{{std::pmr::string(mapping().prefix_, memory)},
            options.take_.has_value() ? std::nullopt : std::optional<std::uint64_t>(options.skip_.value_or(0U))};
        return handle_.template command_mapped<entity_rows<entity_type>>(std::move(args), std::move(mapper));
    }

    [[nodiscard]] task<std::optional<entity_type>> find_one(
        const redis_find_options& options) const {
        const auto memory = resource();
        const bool plain = !options.order_.has_value() && !options.skip_ && !options.take_;
        if (plain) {
            detail::redis_scalar_scratch_type id_scratch{};
            if (const auto id = detail::redis_primary_key(mapping(), options.where_, id_scratch)) {
                auto key = detail::redis_entity_key(mapping().prefix_, *id, memory);
                detail::redis_orm_arguments_type args(memory);
                args.reserve(2);
                args.emplace_back("HGETALL");
                args.emplace_back(key);
                detail::redis_map_one<entity_type> mapper{
                    std::move(key), std::pmr::string(mapping().prefix_, memory)};
                return handle_.template command_mapped<std::optional<entity_type>>(std::move(args), std::move(mapper));
            }
        }
        const auto take = options.take_.has_value() ? (std::min)(*options.take_, std::uint64_t{1}) : std::uint64_t{1};
        auto args = detail::compile_redis_find(mapping(), options, memory, false, take);
        detail::redis_map_first<entity_type> mapper{{std::pmr::string(mapping().prefix_, memory)}};
        return handle_.template command_mapped<std::optional<entity_type>>(std::move(args), std::move(mapper));
    }

    [[nodiscard]] task<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        auto args = detail::compile_redis_find(mapping(), options, memory);
        detail::redis_map_search<entity_type> mapper{std::pmr::string(mapping().prefix_, memory)};
        return handle_.template command_mapped<std::pair<entity_rows<entity_type>, std::uint64_t>>(
            std::move(args), std::move(mapper));
    }

    [[nodiscard]] task<std::uint64_t> count(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        return handle_.template command_mapped<std::uint64_t>(
            detail::compile_redis_find(mapping(), options, memory, true), detail::redis_map_count{});
    }

    [[nodiscard]] task<bool> exists(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        if (!options.order_.has_value() && !options.skip_ && !options.take_) {
            detail::redis_scalar_scratch_type id_scratch{};
            if (const auto id = detail::redis_primary_key(mapping(), options.where_, id_scratch)) {
                const auto key = detail::redis_entity_key(mapping().prefix_, *id, memory);
                return handle_.exists(key);
            }
        }
        return handle_.template command_mapped<bool>(
            detail::compile_redis_find(mapping(), options, memory, true),
            detail::redis_map_exists{});
    }

    [[nodiscard]] task<redis_write_result> delete_by(
        const redis_predicate& predicate) const {
        auto* memory = resource();
        detail::redis_scalar_scratch_type id_scratch{};
        const auto id = require_primary_key(predicate, id_scratch);
        return delete_by_id(id, memory);
    }

    [[nodiscard]] task<redis_write_result> remove(const entity_type& entity) const {
        auto* memory = resource();
        detail::redis_scalar_scratch_type id_scratch{};
        return delete_by_id(detail::redis_entity_id(entity, id_scratch), memory);
    }

    [[nodiscard]] task<bool> expire(
        const redis_predicate& predicate, std::chrono::seconds ttl) const {
        auto* memory = resource();
        detail::redis_scalar_scratch_type id_scratch{};
        const auto id = require_primary_key(predicate, id_scratch);
        const auto key = detail::redis_entity_key(mapping().prefix_, id, memory);
        return handle_.expire(key, ttl);
    }

    [[nodiscard]] task<redis_ttl> ttl(const redis_predicate& predicate) const {
        auto* memory = resource();
        detail::redis_scalar_scratch_type id_scratch{};
        const auto id = require_primary_key(predicate, id_scratch);
        const auto key = detail::redis_entity_key(mapping().prefix_, id, memory);
        return handle_.pttl(key);
    }

    // Explicit schema management. Repository acquisition never creates an
    // index, and an existing index is reported by Redis.
    [[nodiscard]] task<void> create_index() const {
        const auto memory = resource();
        return handle_.template command_mapped<void>(
            detail::compile_redis_index(mapping(), memory), detail::redis_orm_status_result);
    }

    [[nodiscard]] task<void> drop_index() const {
        auto* memory = resource();
        detail::redis_orm_arguments_type args(memory);
        args.reserve(2);
        args.emplace_back("FT.DROPINDEX");
        auto& name = args.emplace_back(mapping().prefix_);
        name += ":idx";
        return handle_.template command_mapped<void>(std::move(args), detail::redis_orm_status_result);
    }

private:
    friend class redis_handle;

    explicit redis_repository(const redis_handle& handle, const redis_repository_config& config)
        : handle_(handle),
          mapping_(detail::normalize_redis_mapping(columns, entity_type::prefix(), config, handle.resource_)),
          registration_(handle.registration_.scope(), this, &redis_repository::expire_capability) {
        detail::validate_redis_entity<entity_type>();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const {
        registration_.require_active();
        handle_.registration_.require_active();
        if (!mapping_) {
            throw std::logic_error("Redis repository mapping has expired");
        }
        return handle_.resource_;
    }

    [[nodiscard]] const detail::redis_mapping& mapping() const {
        if (!mapping_) {
            throw std::logic_error("Redis repository mapping has expired");
        }
        return *mapping_;
    }

    static void expire_capability(void* target) noexcept {
        auto& repository = *static_cast<redis_repository*>(target);
        repository.mapping_.reset();
    }

    [[nodiscard]] std::string_view require_primary_key(
        const redis_predicate& predicate, std::span<char, detail::redis_scalar_scratch_size> scratch) const {
        const auto result_value = detail::redis_primary_key(mapping(), predicate, scratch);
        if (!result_value) {
            throw std::invalid_argument(
                "Redis repository operation requires an exact primary-key predicate");
        }
        return *result_value;
    }

    [[nodiscard]] task<redis_write_result> delete_by_id(
        std::string_view id, std::pmr::memory_resource* memory) const {
        return handle_.template script_mapped<redis_write_result>(detail::redis_orm_delete_script(),
            detail::redis_orm_delete_arguments(mapping().prefix_, id, memory), detail::redis_orm_delete_result);
    }

    [[nodiscard]] task<redis_write_result> write(
        std::string_view mode, std::string_view id, const entity_type& entity,
        redis_write_options options) const {
        auto* memory = resource();
        static constexpr std::size_t required = [] {
            std::size_t count = 0;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
                if constexpr (!column_type::options.nullable_) {
                    ++count;
                }
            });
            return count;
        }();
        static constexpr std::size_t max_mutation_arguments = [] {
            std::size_t count = 0;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
                constexpr auto value_kind = detail::redis_column_value_kind<typename column_type::value_type>();
                constexpr bool tag_shadow =
                    value_kind == detail::redis_column_kind::text || value_kind == detail::redis_column_kind::bool_value;
                count += tag_shadow ? 9U : 6U;
            });
            return count;
        }();
        auto args = detail::redis_orm_write_arguments(
            mapping().prefix_, id, mode, options, 1 + required + max_mutation_arguments, memory);
        detail::redis_scalar_scratch_type scalar_scratch{};
        args.emplace_back(detail::encode_redis_scalar(required, scalar_scratch));
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
            if constexpr (!column_type::options.nullable_) {
                args.emplace_back(column_type::name.view());
            }
        });

        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
            constexpr auto value_kind = detail::redis_column_value_kind<typename column_type::value_type>();
            constexpr bool tag_shadow =
                value_kind == detail::redis_column_kind::text || value_kind == detail::redis_column_kind::bool_value;
            const auto field = column_type::name.view();
            if constexpr (column_type::options.primary_key_) {
                if (entity.template is_set<column_type::name>() &&
                    detail::encode_redis_scalar(entity.template get<column_type::name>(), scalar_scratch) != id) {
                    throw std::invalid_argument("an entity update cannot change its ID");
                }
                detail::append_redis_entity_field(args, field, id,
                    mapping().index_kind(field) == redis_index_kind::numeric, tag_shadow, memory);
            } else if (entity.template is_set<column_type::name>()) {
                if (entity.template is_null<column_type::name>()) {
                    detail::append_redis_entity_field(args, field, std::nullopt, false, tag_shadow, memory);
                } else {
                    const auto value = detail::encode_redis_scalar(entity.template get<column_type::name>(), scalar_scratch);
                    detail::append_redis_entity_field(args, field, value,
                        mapping().index_kind(field) == redis_index_kind::numeric, tag_shadow, memory);
                }
            }
        });

        return handle_.template script_mapped<redis_write_result>(
            detail::redis_orm_write_script(), std::move(args), detail::redis_orm_exec_result);
    }

    static constexpr const auto& columns = detail::redis_query_columns<entity_type>;

    redis_handle handle_;
    std::optional<detail::redis_mapping> mapping_;
    scoped_capability_registration registration_;
};

template <typename entity_type>
redis_repository<entity_type> redis_handle::repository() const {
    return repository<entity_type>(redis_repository_config{});
}

template <typename entity_type>
redis_repository<entity_type> redis_handle::repository(const redis_repository_config& config) const {
    registration_.require_active();
    return redis_repository<entity_type>(*this, config);
}

}  // namespace ruvia
