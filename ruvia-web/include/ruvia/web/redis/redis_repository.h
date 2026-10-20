#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/web/detail/redis/redis_entity_codec.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/detail/redis/redis_query_compile.h"
#include "ruvia/web/detail/redis/redis_repository_commands.h"
#include "ruvia/web/detail/redis/redis_repository_mapping.h"
#include "ruvia/web/entity_rows.h"
#include "ruvia/web/redis/redis_entity.h"
#include "ruvia/web/redis/redis_find_options.h"
#include "ruvia/web/redis/redis_handle.h"
#include "ruvia/web/redis/redis_repository_types.h"
#include "ruvia/web/redis/redis_write_result.h"

namespace ruvia {

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

    [[nodiscard]] scoped_operation<redis_write_result> insert(
        const entity_type& entity, redis_write_options options = {}) const {
        return write("insert", detail::redis_entity_id(entity, resource()), entity, options);
    }

    [[nodiscard]] scoped_operation<redis_write_result> upsert(
        const entity_type& entity, redis_write_options options = {}) const {
        return write("upsert", detail::redis_entity_id(entity, resource()), entity, options);
    }

    [[nodiscard]] scoped_operation<redis_write_result> update(
        const redis_predicate& predicate, const entity_type& changes,
        redis_write_options options = {}) const {
        auto* memory = resource();
        const auto id = require_primary_key(predicate, memory);
        return write("update", id, changes, options);
    }

    [[nodiscard]] scoped_operation<entity_rows<entity_type>> find(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        auto args = detail::compile_redis_find(mapping(), options, memory);
        detail::redis_map_many<entity_type> mapper{{std::pmr::string(mapping().prefix_, memory)},
            options.take_.has_value() ? std::nullopt : std::optional<std::uint64_t>(options.skip_.value_or(0U))};
        return handle_.template command_mapped<entity_rows<entity_type>>(std::move(args), std::move(mapper));
    }

    [[nodiscard]] scoped_operation<std::optional<entity_type>> find_one(
        const redis_find_options& options) const {
        const auto memory = resource();
        const bool plain = options.order_.empty() && !options.skip_ && !options.take_;
        if (plain) {
            if (const auto id = detail::redis_primary_key(mapping(), options.where_, memory)) {
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

    [[nodiscard]] scoped_operation<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        auto args = detail::compile_redis_find(mapping(), options, memory);
        detail::redis_map_search<entity_type> mapper{std::pmr::string(mapping().prefix_, memory)};
        return handle_.template command_mapped<std::pair<entity_rows<entity_type>, std::uint64_t>>(
            std::move(args), std::move(mapper));
    }

    [[nodiscard]] scoped_operation<std::uint64_t> count(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        return handle_.template command_mapped<std::uint64_t>(
            detail::compile_redis_find(mapping(), options, memory, true), detail::redis_map_count{});
    }

    [[nodiscard]] scoped_operation<bool> exists(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        if (options.order_.empty() && !options.skip_ && !options.take_) {
            if (const auto id = detail::redis_primary_key(mapping(), options.where_, memory)) {
                const auto key = detail::redis_entity_key(mapping().prefix_, *id, memory);
                return handle_.exists(key);
            }
        }
        return handle_.template command_mapped<bool>(
            detail::compile_redis_find(mapping(), options, memory, true),
            detail::redis_map_exists{});
    }

    [[nodiscard]] scoped_operation<redis_write_result> delete_by(
        const redis_predicate& predicate) const {
        auto* memory = resource();
        const auto id = require_primary_key(predicate, memory);
        return delete_by_id(id, memory);
    }

    [[nodiscard]] scoped_operation<redis_write_result> remove(const entity_type& entity) const {
        auto* memory = resource();
        return delete_by_id(detail::redis_entity_id(entity, memory), memory);
    }

    [[nodiscard]] scoped_operation<bool> expire(
        const redis_predicate& predicate, std::chrono::seconds ttl) const {
        auto* memory = resource();
        const auto id = require_primary_key(predicate, memory);
        const auto key = detail::redis_entity_key(mapping().prefix_, id, memory);
        return handle_.expire(key, ttl);
    }

    [[nodiscard]] scoped_operation<redis_ttl> ttl(const redis_predicate& predicate) const {
        auto* memory = resource();
        const auto id = require_primary_key(predicate, memory);
        const auto key = detail::redis_entity_key(mapping().prefix_, id, memory);
        return handle_.pttl(key);
    }

    // Explicit schema management. Repository acquisition never creates an
    // index, and an existing index is reported by Redis.
    [[nodiscard]] scoped_operation<void> create_index() const {
        const auto memory = resource();
        return handle_.template command_mapped<void>(
            detail::compile_redis_index(mapping(), memory), detail::redis_orm_status_result);
    }

    [[nodiscard]] scoped_operation<void> drop_index() const {
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

    [[nodiscard]] std::pmr::string require_primary_key(
        const redis_predicate& predicate, std::pmr::memory_resource* memory) const {
        auto result_value = detail::redis_primary_key(mapping(), predicate, memory);
        if (!result_value) {
            throw std::invalid_argument(
                "Redis repository operation requires an exact primary-key predicate");
        }
        return std::move(*result_value);
    }

    [[nodiscard]] scoped_operation<redis_write_result> delete_by_id(
        std::string_view id, std::pmr::memory_resource* memory) const {
        const auto key = detail::redis_entity_key(mapping().prefix_, id, memory);
        return handle_.template script_mapped<redis_write_result>(detail::redis_orm_delete_script(),
            detail::redis_orm_delete_arguments(key, memory), detail::redis_orm_delete_result);
    }

    [[nodiscard]] scoped_operation<redis_write_result> write(
        std::string_view mode, std::string_view id, const entity_type& entity,
        redis_write_options options) const {
        auto* memory = resource();
        const auto key = detail::redis_entity_key(mapping().prefix_, id, memory);
        auto args = detail::redis_orm_write_arguments(key, mode, options, memory);

        static constexpr std::size_t required = [] {
            std::size_t count = 0;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
                if constexpr (!column_type::options.nullable_) {
                    ++count;
                }
            });
            return count;
        }();
        args.emplace_back(detail::encode_redis_scalar(required, memory));
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
                    detail::encode_redis_scalar(entity.template get<column_type::name>(), memory) != id) {
                    throw std::invalid_argument("an entity update cannot change its ID");
                }
                detail::append_redis_entity_field(args, field, id,
                    mapping().index_kind(field) == redis_index_kind::numeric, tag_shadow, memory);
            } else if (entity.template is_set<column_type::name>()) {
                if (entity.template is_null<column_type::name>()) {
                    detail::append_redis_entity_field(args, field, std::nullopt, false, tag_shadow, memory);
                } else {
                    const auto value = detail::encode_redis_scalar(entity.template get<column_type::name>(), memory);
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
redis_repository<entity_type> redis_handle::get_repository(const redis_repository_config& config) const {
    registration_.require_active();
    return redis_repository<entity_type>(*this, config);
}

}  // namespace ruvia
