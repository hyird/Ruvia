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

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/web/db/DbExecResult.h"
#include "ruvia/web/db/DbFindOptions.h"
#include "ruvia/web/detail/redis/RedisEntityCodec.h"
#include "ruvia/web/detail/redis/RedisEntityKey.h"
#include "ruvia/web/detail/redis/RedisQueryCompile.h"
#include "ruvia/web/detail/redis/RedisRepositoryCommands.h"
#include "ruvia/web/detail/redis/RedisRepositoryConfig.h"
#include "ruvia/web/detail/redis/RedisRepositoryMapping.h"
#include "ruvia/web/redis/RedisHandle.h"
#include "ruvia/web/redis/RedisRepositoryTypes.h"

namespace ruvia::detail {

template <typename Entity, typename Fn>
void forEachRedisRepositoryColumn(Fn&& function) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (function.template operator()<std::tuple_element_t<I, typename Entity::Columns>>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<typename Entity::Columns>>{});
}

template <typename Entity>
struct RedisRepositoryMapExists final {
    bool operator()(RedisValue&& reply, std::pmr::memory_resource*) const {
        const auto values = redisOrmArray(reply);
        if (values.size() != 1) {
            throw RedisError(RedisError::Code::kProtocolError,
                "invalid entity exists reply");
        }
        return redisOrmCount(values.front()) != 0;
    }
};

}  // namespace ruvia::detail

namespace ruvia {

template <typename Entity>
class RedisRepository final : private detail::ScopedCapabilityNode {
public:
    RedisRepository(const RedisRepository&) = delete;
    RedisRepository& operator=(const RedisRepository&) = delete;

    // A repository is tied to its worker operation scope. Moving it transfers
    // the capability node and its PMR mapping; copying would create a second
    // owner with ambiguous mapping lifetime.
    RedisRepository(RedisRepository&& other) noexcept
        : detail::ScopedCapabilityNode(std::move(other)),
          handle_(other.handle_),
          mapping_(std::move(other.mapping_)) {
        // The source no longer has a scope node. Destroy even empty moved-from
        // PMR containers now: implementations may retain allocator-owned state.
        other.mapping_.reset();
    }
    RedisRepository& operator=(RedisRepository&&) = delete;

    [[nodiscard]] ScopedOperation<DbExecResult> insert(
        const Entity& entity, RedisWriteOptions options = {}) const {
        return write("insert", detail::redisEntityId(entity, resource()), entity, options);
    }

    [[nodiscard]] ScopedOperation<DbExecResult> upsert(
        const Entity& entity, RedisWriteOptions options = {}) const {
        return write("upsert", detail::redisEntityId(entity, resource()), entity, options);
    }

    [[nodiscard]] ScopedOperation<DbExecResult> update(
        const DbPredicate& predicate, const Entity& changes,
        RedisWriteOptions options = {}) const {
        auto* memory = resource();
        const auto id = requirePrimaryKey(predicate, memory);
        return write("update", *id, changes, options);
    }

    [[nodiscard]] ScopedOperation<DbEntityRows<Entity>> find(
        const DbFindOptions& options = {}) const {
        const auto memory = resource();
        validateFindOptions(options);
        auto args = detail::compileRedisFind<Entity>(options, mapping(), memory);
        const auto views = detail::redisOrmArgumentViews(args);
        detail::RedisMapMany<Entity> mapper{std::pmr::string(mapping().prefix, memory)};
        return handle_.template commandMapped<DbEntityRows<Entity>>(views, std::move(mapper));
    }

    [[nodiscard]] ScopedOperation<std::optional<Entity>> findOne(
        const DbFindOptions& options) const {
        const auto memory = resource();
        validateFindOptions(options);
        const bool plain = options.order.empty() && !options.skip && !options.take;
        if (plain) {
            if (const auto id = detail::redisPrimaryKey<Entity>(options.where, memory)) {
                const auto key = detail::redisEntityKey<Entity>(*id, memory, mapping().prefix);
                const std::array<std::string_view, 2> args{"HGETALL", key};
                detail::RedisMapOne<Entity> mapper{
                    std::pmr::string(key, memory), std::pmr::string(mapping().prefix, memory)};
                return handle_.template commandMapped<std::optional<Entity>>(args, std::move(mapper));
            }
        }
        auto args = detail::compileRedisFind<Entity>(options, mapping(), memory, false, 1);
        const auto views = detail::redisOrmArgumentViews(args);
        detail::RedisMapFirst<Entity> mapper{std::pmr::string(mapping().prefix, memory)};
        return handle_.template commandMapped<std::optional<Entity>>(views, std::move(mapper));
    }

    [[nodiscard]] ScopedOperation<std::pair<DbEntityRows<Entity>, std::uint64_t>> findAndCount(
        const DbFindOptions& options = {}) const {
        const auto memory = resource();
        validateFindOptions(options);
        auto args = detail::compileRedisFind<Entity>(options, mapping(), memory);
        const auto views = detail::redisOrmArgumentViews(args);
        detail::RedisMapSearch<Entity> mapper{std::pmr::string(mapping().prefix, memory)};
        return handle_.template commandMapped<std::pair<DbEntityRows<Entity>, std::uint64_t>>(
            views, std::move(mapper));
    }

    [[nodiscard]] ScopedOperation<std::uint64_t> count(
        const DbFindOptions& options = {}) const {
        const auto memory = resource();
        validateFindOptions(options);
        auto args = detail::compileRedisFind<Entity>(options, mapping(), memory, true);
        const auto views = detail::redisOrmArgumentViews(args);
        return handle_.template commandMapped<std::uint64_t>(views, detail::RedisMapCount{});
    }

    [[nodiscard]] ScopedOperation<bool> exists(
        const DbFindOptions& options = {}) const {
        const auto memory = resource();
        validateFindOptions(options);
        if (options.order.empty() && !options.skip && !options.take) {
            if (const auto id = detail::redisPrimaryKey<Entity>(options.where, memory)) {
                const auto key = detail::redisEntityKey<Entity>(*id, memory, mapping().prefix);
                return handle_.exists(key);
            }
        }
        auto args = detail::compileRedisFind<Entity>(options, mapping(), memory, true);
        const auto views = detail::redisOrmArgumentViews(args);
        return handle_.template commandMapped<bool>(
            views, detail::RedisRepositoryMapExists<Entity>{});
    }

    [[nodiscard]] ScopedOperation<DbExecResult> deleteBy(
        const DbPredicate& predicate) const {
        auto* memory = resource();
        const auto id = requirePrimaryKey(predicate, memory);
        return deleteById(*id, memory);
    }

    [[nodiscard]] ScopedOperation<DbExecResult> remove(const Entity& entity) const {
        auto* memory = resource();
        return deleteById(detail::redisEntityId(entity, memory), memory);
    }

    [[nodiscard]] ScopedOperation<bool> expire(
        const DbPredicate& predicate, std::chrono::seconds ttl) const {
        auto* memory = resource();
        const auto id = requirePrimaryKey(predicate, memory);
        const auto key = detail::redisEntityKey<Entity>(*id, memory, mapping().prefix);
        return handle_.expire(key, ttl);
    }

    [[nodiscard]] ScopedOperation<RedisTtl> ttl(const DbPredicate& predicate) const {
        auto* memory = resource();
        const auto id = requirePrimaryKey(predicate, memory);
        const auto key = detail::redisEntityKey<Entity>(*id, memory, mapping().prefix);
        return handle_.pttl(key);
    }

    // Explicit schema management. Repository acquisition never creates an
    // index, and an existing index is reported by Redis.
    [[nodiscard]] ScopedOperation<void> createIndex() const {
        const auto memory = resource();
        auto args = detail::compileRedisIndex<Entity>(mapping(), memory);
        const auto views = detail::redisOrmArgumentViews(args);
        return handle_.template commandMapped<void>(views, detail::redisOrmStatusResult);
    }

    [[nodiscard]] ScopedOperation<void> dropIndex() const {
        auto* memory = resource();
        std::pmr::string name(mapping().prefix, memory);
        name += ":idx";
        const std::array<std::string_view, 2> args{"FT.DROPINDEX", name};
        return handle_.template commandMapped<void>(args, detail::redisOrmStatusResult);
    }

private:
    friend class RedisHandle;

    explicit RedisRepository(const RedisHandle& handle, const RedisRepositoryConfig& config)
        : detail::ScopedCapabilityNode(handle.operationScope(), &RedisRepository::expireCapability),
          handle_(handle),
          mapping_(detail::normalizeRedisMapping<Entity>(config, handle.resource_)) {}

    [[nodiscard]] std::pmr::memory_resource* resource() const {
        detail::ScopedCapabilityNode::requireActive();
        handle_.requireActive();
        if (!mapping_) {
            throw std::logic_error("Redis repository mapping has expired");
        }
        return handle_.resource_;
    }

    [[nodiscard]] const detail::RedisMapping& mapping() const {
        if (!mapping_) {
            throw std::logic_error("Redis repository mapping has expired");
        }
        return *mapping_;
    }

    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
        auto& repository = static_cast<RedisRepository&>(capability);
        repository.mapping_.reset();
    }

    static void validateFindOptions(const DbFindOptions& options) {
        if (!options.relations.empty()) {
            throw std::invalid_argument("Redis repository does not support relations");
        }
        if (options.lock) {
            throw std::invalid_argument("Redis repository does not support row locks");
        }
        const bool cacheDisabled = std::holds_alternative<std::monostate>(options.cache) ||
                                   (std::holds_alternative<bool>(options.cache) &&
                                       !std::get<bool>(options.cache));
        if (!cacheDisabled) {
            throw std::invalid_argument("Redis repository does not support query cache options");
        }
    }

    [[nodiscard]] static std::optional<std::pmr::string> requirePrimaryKey(
        const DbPredicate& predicate, std::pmr::memory_resource* memory) {
        auto result = detail::redisPrimaryKey<Entity>(predicate, memory);
        if (!result) {
            throw std::invalid_argument(
                "Redis repository operation requires an exact primary-key predicate");
        }
        return result;
    }

    [[nodiscard]] ScopedOperation<DbExecResult> deleteById(
        std::string_view id, std::pmr::memory_resource* memory) const {
        const auto key = detail::redisEntityKey<Entity>(id, memory, mapping().prefix);
        auto args = detail::redisOrmDeleteArguments(key, memory);
        const auto views = detail::redisOrmArgumentViews(args);
        return handle_.template commandMapped<DbExecResult>(
            views, detail::redisOrmDeleteResult);
    }

    [[nodiscard]] ScopedOperation<DbExecResult> write(
        std::string_view mode, std::string_view id, const Entity& entity,
        RedisWriteOptions options) const {
        auto* memory = resource();
        const auto key = detail::redisEntityKey<Entity>(id, memory, mapping().prefix);
        auto args = detail::redisOrmWriteArguments(key, mode, options, memory);

        std::size_t required = 0;
        detail::forEachRedisRepositoryColumn<Entity>([&]<typename Column> {
            if constexpr (!Column::options.nullable) {
                ++required;
            }
        });
        args.emplace_back(detail::encodeRedisScalar(required, memory));
        detail::forEachRedisRepositoryColumn<Entity>([&]<typename Column> {
            if constexpr (!Column::options.nullable) {
                args.emplace_back(Column::name.view());
            }
        });

        detail::forEachRedisRepositoryColumn<Entity>([&]<typename Column> {
            using Value = typename Column::value_type;
            constexpr bool isId = Column::options.primaryKey;
            const auto field = Column::name.view();
            if constexpr (isId) {
                if (entity.template isSet<Column::name>() &&
                    detail::encodeRedisScalar(entity.template get<Column::name>(), memory) != id) {
                    throw std::invalid_argument("an entity update cannot change its ID");
                }
                if (mapping().indexKind(field) == RedisIndexKind::kNumeric) {
                    validateNumericSearchValue(id);
                }
                appendSet(args, field, id, memory);
                appendPresence(args, field, true, memory);
                if constexpr (isRedisTagValue<Value>) {
                    appendSet(args, redisTagShadow(field, memory),
                        detail::encodeRedisTag(id, memory), memory);
                }
            } else if (entity.template isSet<Column::name>()) {
                const bool isNull = entity.template isNull<Column::name>();
                if (isNull) {
                    appendDelete(args, field, memory);
                    appendPresence(args, field, false, memory);
                    if constexpr (isRedisTagValue<Value>) {
                        appendDelete(args, redisTagShadow(field, memory), memory);
                    }
                } else {
                    auto value = detail::encodeRedisScalar(
                        entity.template get<Column::name>(), memory);
                    if (mapping().indexKind(field) == RedisIndexKind::kNumeric) {
                        validateNumericSearchValue(value);
                    }
                    appendSet(args, field, value, memory);
                    appendPresence(args, field, true, memory);
                    if constexpr (isRedisTagValue<Value>) {
                        appendSet(args, redisTagShadow(field, memory),
                            detail::encodeRedisTag(value, memory), memory);
                    }
                }
            }
        });

        const auto views = detail::redisOrmArgumentViews(args);
        return handle_.template commandMapped<DbExecResult>(
            views, detail::redisOrmExecResult);
    }

    template <typename T>
    static constexpr bool isRedisTagValue =
        detail::isRedisEntityString<T> || std::is_same_v<std::remove_cvref_t<T>, bool> ||
        std::is_same_v<std::remove_cvref_t<T>, Bool>;

    static void appendSet(detail::RedisOrmArguments& args, std::string_view field,
        std::string_view value, std::pmr::memory_resource*) {
        args.emplace_back("s");
        args.emplace_back(field);
        args.emplace_back(value);
    }
    static void appendDelete(detail::RedisOrmArguments& args, std::string_view field,
        std::pmr::memory_resource*) {
        args.emplace_back("d");
        args.emplace_back(field);
        args.emplace_back();
    }
    static void appendPresence(detail::RedisOrmArguments& args, std::string_view field,
        bool present, std::pmr::memory_resource* memory) {
        auto shadow = redisPresentShadow(field, memory);
        if (present) {
            appendSet(args, shadow, "1", memory);
        } else {
            appendDelete(args, shadow, memory);
        }
    }
    static std::pmr::string redisPresentShadow(
        std::string_view field, std::pmr::memory_resource* memory) {
        std::pmr::string result("__ruvia_present_", memory);
        result.append(field.data(), field.size());
        return result;
    }
    static std::pmr::string redisTagShadow(
        std::string_view field, std::pmr::memory_resource* memory) {
        std::pmr::string result("__ruvia_tag_", memory);
        result.append(field.data(), field.size());
        return result;
    }
    static void validateNumericSearchValue(std::string_view value) {
        double number = 0.0;
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), number,
            std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            !std::isfinite(number) || std::abs(number) > 9007199254740991.0) {
            throw std::invalid_argument(
                "numeric search values must be finite and within the exact integer range of Redis Search");
        }
    }

    RedisHandle handle_;
    std::optional<detail::RedisMapping> mapping_;
};

template <typename Entity>
RedisRepository<Entity> RedisHandle::getRepository(const RedisRepositoryConfig& config) const {
    requireActive();
    return RedisRepository<Entity>(*this, config);
}

}  // namespace ruvia
