#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/web/detail/redis/redis_entity_codec.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/detail/redis/redis_query_compile.h"
#include "ruvia/web/detail/redis/redis_repository_commands.h"
#include "ruvia/web/detail/redis/redis_repository_config.h"
#include "ruvia/web/detail/redis/redis_repository_mapping.h"
#include "ruvia/web/entity_rows.h"
#include "ruvia/web/redis/redis_entity.h"
#include "ruvia/web/redis/redis_find_options.h"
#include "ruvia/web/redis/redis_handle.h"
#include "ruvia/web/redis/redis_repository_types.h"
#include "ruvia/web/redis/redis_write_result.h"

namespace ruvia::detail {

template <typename entity_type, typename fn_type>
void for_each_redis_repository_column(fn_type&& function) {
    [&]<std::size_t... i>(std::index_sequence<i...>) {
        (function.template operator()<std::tuple_element_t<i, typename entity_type::columns_type>>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<typename entity_type::columns_type>>{});
}

template <typename entity_type>
struct redis_repository_map_exists final {
    bool operator()(redis_value&& reply, std::pmr::memory_resource*) const {
        const auto values = redis_orm_array(reply);
        if (values.size() != 1) {
            throw redis_error(redis_error::code_type::protocol_error,
                "invalid entity exists reply");
        }
        return redis_orm_count(values.front()) != 0;
    }
};

}  // namespace ruvia::detail

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
        return write("update", *id, changes, options);
    }

    [[nodiscard]] scoped_operation<entity_rows<entity_type>> find(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        auto args = detail::compile_redis_find<entity_type>(options, mapping(), memory);
        const auto views = detail::redis_orm_argument_views(args);
        detail::redis_map_many<entity_type> mapper{std::pmr::string(mapping().prefix_, memory)};
        return handle_.template command_mapped<entity_rows<entity_type>>(views, std::move(mapper));
    }

    [[nodiscard]] scoped_operation<std::optional<entity_type>> find_one(
        const redis_find_options& options) const {
        const auto memory = resource();
        const bool plain = options.order_.empty() && !options.skip_ && !options.take_;
        if (plain) {
            if (const auto id = detail::redis_primary_key<entity_type>(options.where_, memory)) {
                const auto key = detail::redis_entity_key<entity_type>(*id, memory, mapping().prefix_);
                const std::array<std::string_view, 2> args{"HGETALL", key};
                detail::redis_map_one<entity_type> mapper{
                    std::pmr::string(key, memory), std::pmr::string(mapping().prefix_, memory)};
                return handle_.template command_mapped<std::optional<entity_type>>(args, std::move(mapper));
            }
        }
        auto args = detail::compile_redis_find<entity_type>(options, mapping(), memory, false, 1);
        const auto views = detail::redis_orm_argument_views(args);
        detail::redis_map_first<entity_type> mapper{std::pmr::string(mapping().prefix_, memory)};
        return handle_.template command_mapped<std::optional<entity_type>>(views, std::move(mapper));
    }

    [[nodiscard]] scoped_operation<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        auto args = detail::compile_redis_find<entity_type>(options, mapping(), memory);
        const auto views = detail::redis_orm_argument_views(args);
        detail::redis_map_search<entity_type> mapper{std::pmr::string(mapping().prefix_, memory)};
        return handle_.template command_mapped<std::pair<entity_rows<entity_type>, std::uint64_t>>(
            views, std::move(mapper));
    }

    [[nodiscard]] scoped_operation<std::uint64_t> count(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        auto args = detail::compile_redis_find<entity_type>(options, mapping(), memory, true);
        const auto views = detail::redis_orm_argument_views(args);
        return handle_.template command_mapped<std::uint64_t>(views, detail::redis_map_count{});
    }

    [[nodiscard]] scoped_operation<bool> exists(
        const redis_find_options& options = {}) const {
        const auto memory = resource();
        if (options.order_.empty() && !options.skip_ && !options.take_) {
            if (const auto id = detail::redis_primary_key<entity_type>(options.where_, memory)) {
                const auto key = detail::redis_entity_key<entity_type>(*id, memory, mapping().prefix_);
                return handle_.exists(key);
            }
        }
        auto args = detail::compile_redis_find<entity_type>(options, mapping(), memory, true);
        const auto views = detail::redis_orm_argument_views(args);
        return handle_.template command_mapped<bool>(
            views, detail::redis_repository_map_exists<entity_type>{});
    }

    [[nodiscard]] scoped_operation<redis_write_result> delete_by(
        const redis_predicate& predicate) const {
        auto* memory = resource();
        const auto id = require_primary_key(predicate, memory);
        return delete_by_id(*id, memory);
    }

    [[nodiscard]] scoped_operation<redis_write_result> remove(const entity_type& entity) const {
        auto* memory = resource();
        return delete_by_id(detail::redis_entity_id(entity, memory), memory);
    }

    [[nodiscard]] scoped_operation<bool> expire(
        const redis_predicate& predicate, std::chrono::seconds ttl) const {
        auto* memory = resource();
        const auto id = require_primary_key(predicate, memory);
        const auto key = detail::redis_entity_key<entity_type>(*id, memory, mapping().prefix_);
        return handle_.expire(key, ttl);
    }

    [[nodiscard]] scoped_operation<redis_ttl> ttl(const redis_predicate& predicate) const {
        auto* memory = resource();
        const auto id = require_primary_key(predicate, memory);
        const auto key = detail::redis_entity_key<entity_type>(*id, memory, mapping().prefix_);
        return handle_.pttl(key);
    }

    // Explicit schema management. Repository acquisition never creates an
    // index, and an existing index is reported by Redis.
    [[nodiscard]] scoped_operation<void> create_index() const {
        const auto memory = resource();
        auto args = detail::compile_redis_index<entity_type>(mapping(), memory);
        const auto views = detail::redis_orm_argument_views(args);
        return handle_.template command_mapped<void>(views, detail::redis_orm_status_result);
    }

    [[nodiscard]] scoped_operation<void> drop_index() const {
        auto* memory = resource();
        std::pmr::string name(mapping().prefix_, memory);
        name += ":idx";
        const std::array<std::string_view, 2> args{"FT.DROPINDEX", name};
        return handle_.template command_mapped<void>(args, detail::redis_orm_status_result);
    }

private:
    friend class redis_handle;

    explicit redis_repository(const redis_handle& handle, const redis_repository_config& config)
        : handle_(handle),
          mapping_(detail::normalize_redis_mapping<entity_type>(config, handle.resource_)),
          registration_(handle.registration_.scope(), this, &redis_repository::expire_capability) {}

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

    [[nodiscard]] static std::optional<std::pmr::string> require_primary_key(
        const redis_predicate& predicate, std::pmr::memory_resource* memory) {
        auto result_value = detail::redis_primary_key<entity_type>(predicate, memory);
        if (!result_value) {
            throw std::invalid_argument(
                "Redis repository operation requires an exact primary-key predicate");
        }
        return result_value;
    }

    [[nodiscard]] scoped_operation<redis_write_result> delete_by_id(
        std::string_view id, std::pmr::memory_resource* memory) const {
        const auto key = detail::redis_entity_key<entity_type>(id, memory, mapping().prefix_);
        auto args = detail::redis_orm_delete_arguments(key, memory);
        const auto views = detail::redis_orm_argument_views(args);
        return handle_.template command_mapped<redis_write_result>(
            views, detail::redis_orm_delete_result);
    }

    [[nodiscard]] scoped_operation<redis_write_result> write(
        std::string_view mode, std::string_view id, const entity_type& entity,
        redis_write_options options) const {
        auto* memory = resource();
        const auto key = detail::redis_entity_key<entity_type>(id, memory, mapping().prefix_);
        auto args = detail::redis_orm_write_arguments(key, mode, options, memory);

        std::size_t required = 0;
        detail::for_each_redis_repository_column<entity_type>([&]<typename column_type> {
            if constexpr (!column_type::options.nullable_) {
                ++required;
            }
        });
        args.emplace_back(detail::encode_redis_scalar(required, memory));
        detail::for_each_redis_repository_column<entity_type>([&]<typename column_type> {
            if constexpr (!column_type::options.nullable_) {
                args.emplace_back(column_type::name.view());
            }
        });

        detail::for_each_redis_repository_column<entity_type>([&]<typename column_type> {
            using value_type = typename column_type::value_type;
            constexpr bool is_id = column_type::options.primary_key_;
            const auto field = column_type::name.view();
            if constexpr (is_id) {
                if (entity.template is_set<column_type::name>() &&
                    detail::encode_redis_scalar(entity.template get<column_type::name>(), memory) != id) {
                    throw std::invalid_argument("an entity update cannot change its ID");
                }
                if (mapping().index_kind(field) == redis_index_kind::numeric) {
                    validate_numeric_search_value(id);
                }
                append_set(args, field, id, memory);
                append_presence(args, field, true, memory);
                if constexpr (is_redis_tag_value<value_type>) {
                    append_set(args, redis_tag_shadow(field, memory),
                        detail::encode_redis_tag(id, memory), memory);
                }
            } else if (entity.template is_set<column_type::name>()) {
                const bool is_null = entity.template is_null<column_type::name>();
                if (is_null) {
                    append_delete(args, field, memory);
                    append_presence(args, field, false, memory);
                    if constexpr (is_redis_tag_value<value_type>) {
                        append_delete(args, redis_tag_shadow(field, memory), memory);
                    }
                } else {
                    auto value = detail::encode_redis_scalar(
                        entity.template get<column_type::name>(), memory);
                    if (mapping().index_kind(field) == redis_index_kind::numeric) {
                        validate_numeric_search_value(value);
                    }
                    append_set(args, field, value, memory);
                    append_presence(args, field, true, memory);
                    if constexpr (is_redis_tag_value<value_type>) {
                        append_set(args, redis_tag_shadow(field, memory),
                            detail::encode_redis_tag(value, memory), memory);
                    }
                }
            }
        });

        const auto views = detail::redis_orm_argument_views(args);
        return handle_.template command_mapped<redis_write_result>(
            views, detail::redis_orm_exec_result);
    }

    template <typename t_type>
    static constexpr bool is_redis_tag_value =
        detail::is_redis_entity_string<t_type> || std::is_same_v<std::remove_cvref_t<t_type>, bool> ||
        std::is_same_v<std::remove_cvref_t<t_type>, bool_value>;

    static void append_set(detail::redis_orm_arguments_type& args, std::string_view field,
        std::string_view value, std::pmr::memory_resource*) {
        args.emplace_back("s");
        args.emplace_back(field);
        args.emplace_back(value);
    }
    static void append_delete(detail::redis_orm_arguments_type& args, std::string_view field,
        std::pmr::memory_resource*) {
        args.emplace_back("d");
        args.emplace_back(field);
        args.emplace_back();
    }
    static void append_presence(detail::redis_orm_arguments_type& args, std::string_view field,
        bool present, std::pmr::memory_resource* memory) {
        auto shadow = redis_present_shadow(field, memory);
        if (present) {
            append_set(args, shadow, "1", memory);
        } else {
            append_delete(args, shadow, memory);
        }
    }
    static std::pmr::string redis_present_shadow(
        std::string_view field, std::pmr::memory_resource* memory) {
        std::pmr::string result_value("__ruvia_present_", memory);
        result_value.append(field.data(), field.size());
        return result_value;
    }
    static std::pmr::string redis_tag_shadow(
        std::string_view field, std::pmr::memory_resource* memory) {
        std::pmr::string result_value("__ruvia_tag_", memory);
        result_value.append(field.data(), field.size());
        return result_value;
    }
    static void validate_numeric_search_value(std::string_view value) {
        double number = 0.0;
        const auto parsed_value = std::from_chars(
            value.data(), value.data() + value.size(), number,
            std::chars_format::general);
        if (parsed_value.ec != std::errc{} || parsed_value.ptr != value.data() + value.size() ||
            !std::isfinite(number) || std::abs(number) > 9007199254740991.0) {
            throw std::invalid_argument(
                "numeric search values must be finite and within the exact integer range of Redis Search");
        }
    }

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
