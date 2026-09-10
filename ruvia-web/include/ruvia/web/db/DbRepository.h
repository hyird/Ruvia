#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/db/DbHandle.h"
#include "ruvia/web/db/DbPredicate.h"
#include "ruvia/web/detail/db/DbEntityCodec.h"
#include "ruvia/web/detail/db/DbRelationQuery.h"

namespace ruvia {

struct DbFindOrder final {
    std::string column{};
    DbOrderDirection direction{DbOrderDirection::kAsc};
    DbNullsOrder nulls{DbNullsOrder::kDefault};
};
struct DbFindOptions final {
    DbPredicate where{};
    std::vector<std::string> relations{};
    std::vector<DbFindOrder> order{};
    std::optional<std::uint64_t> skip{};
    std::optional<std::uint64_t> take{};
    std::optional<DbLockOptions> lock{};
};
struct DbUpsertOptions final {
    std::vector<std::string> conflictPaths{};
    std::vector<std::string> updateColumns{};
    bool doNothing{false};
    bool anyUniqueKey{false};
};

namespace detail {

template <typename E, typename Fn>
void forEachEntityColumn(Fn&& fn) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (fn.template operator()<std::tuple_element_t<I, typename E::Columns>>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<typename E::Columns>>{});
}
template <typename E>
void requireEntityColumn(std::string_view name) {
    bool found = false;
    forEachEntityColumn<E>([&]<typename C> { found |= name == C::name.view(); });
    if (!found) {
        throw std::invalid_argument("unknown database entity column");
    }
}
template <typename E>
bool isGeneratedEntityColumn(std::string_view name) {
    bool generated = false;
    forEachEntityColumn<E>([&]<typename C> {
        if constexpr (C::options.generatedType != DbGeneratedType::kNone) {
            generated |= name == C::name.view();
        }
    });
    return generated;
}
template <typename E>
void selectEntity(DbQuery& query, std::string_view alias) {
    const auto qualifier = alias.empty() ? E::tableName() : alias;
    forEachEntityColumn<E>([&]<typename C> { query.addSelect(query.column(C::name.view(), qualifier)); });
    query.from(E::tableName(), alias);
}
template <typename E>
constexpr std::string_view entityQueryAlias() noexcept {
    const auto table = E::tableName();
    const auto dot = table.rfind('.');
    return table.substr(dot == std::string_view::npos ? 0 : dot + 1);
}
template <typename E>
void applyFindOptions(DbQuery& query, const DbFindOptions& options, std::string_view alias = {}) {
    if (!options.where.empty()) {
        query.where(options.where.expression(query, E::tableName(), alias));
    }
    for (const auto& order : options.order) {
        requireEntityColumn<E>(order.column);
        query.addOrderBy(query.column(order.column, alias.empty() ? E::tableName() : alias), order.direction, order.nulls);
    }
    query.offset(options.skip).limit(options.take);
    if (options.lock) {
        query.lock(*options.lock);
    }
}
template <typename E>
struct DbMapEntityRows final {
    DbEntityRows<E> operator()(DbRows&& rows, std::pmr::memory_resource* resource) const {
        return mapDbEntityRows<E>(std::move(rows), resource);
    }
};
template <typename E>
struct DbMapOneEntity final {
    std::optional<E> operator()(DbRows&& rows, std::pmr::memory_resource* resource) const {
        if (rows.empty()) {
            return std::nullopt;
        }
        E entity(resource);
        decodeEntity(entity, rows.front(), resource, static_cast<typename E::Columns*>(nullptr),
            std::make_index_sequence<std::tuple_size_v<typename E::Columns>>{});
        return std::optional<E>(std::in_place, std::move(entity));
    }
};
struct DbMapCount final {
    std::uint64_t operator()(DbRows&& rows, std::pmr::memory_resource*) const {
        if (rows.size() != 1) {
            throw std::runtime_error("database count did not return exactly one row");
        }
        return rows.front()["count"].template as<std::uint64_t>().value();
    }
};
template <typename E, typename C>
DbExpression entityColumnValue(DbQuery& query, const E& entity) {
    if (!entity.template isSet<C::name>()) {
        return query.defaultValue();
    }
    if (entity.template isNull<C::name>()) {
        return query.nullValue();
    }
    return query.value(entityDbValue(entity.template get<C::name>(), query.resource()));
}
template <typename Executor>
using DbRepositoryExecutor = std::conditional_t<std::same_as<Executor, DbHandle>, DbHandle, Executor&>;
template <typename Executor>
using DbRepositoryInput = std::conditional_t<std::same_as<Executor, DbHandle>, const DbHandle&, Executor&>;

}  // namespace detail

template <typename Entity, typename Executor>
class DbQueryBuilder final {
public:
    DbQueryBuilder(const DbQueryBuilder&) = delete;
    DbQueryBuilder& operator=(const DbQueryBuilder&) = delete;
    DbQueryBuilder(DbQueryBuilder&&) = default;
    DbQueryBuilder& operator=(DbQueryBuilder&&) = delete;

    [[nodiscard]] DbQuery& statement() & noexcept {
        return query_;
    }
    DbQuery& statement() && = delete;
    template <FixedString Name>
    [[nodiscard]] DbExpression column() {
        return query_.column(Entity::template columnName<Name>(), alias_);
    }
    DbQueryBuilder& where(const DbPredicate& predicate) {
        return where(predicate.expression(query_, Entity::tableName(), alias_));
    }
    DbQueryBuilder& where(DbExpression predicate) {
        query_.where(predicate);
        return *this;
    }
    DbQueryBuilder& andWhere(const DbPredicate& predicate) {
        return andWhere(predicate.expression(query_, Entity::tableName(), alias_));
    }
    DbQueryBuilder& andWhere(DbExpression predicate) {
        query_.andWhere(predicate);
        return *this;
    }
    DbQueryBuilder& orWhere(const DbPredicate& predicate) {
        query_.orWhere(predicate.expression(query_, Entity::tableName(), alias_));
        return *this;
    }
    DbQueryBuilder& select(std::span<const DbExpression> projections) {
        query_.select(projections);
        return *this;
    }
    DbQueryBuilder& addSelect(DbExpression projection) {
        query_.addSelect(projection);
        return *this;
    }
    DbQueryBuilder& orderBy(DbExpression expression, DbOrderDirection direction = DbOrderDirection::kAsc, DbNullsOrder nulls = DbNullsOrder::kDefault) {
        query_.orderBy(expression, direction, nulls);
        return *this;
    }
    DbQueryBuilder& addOrderBy(DbExpression expression, DbOrderDirection direction = DbOrderDirection::kAsc, DbNullsOrder nulls = DbNullsOrder::kDefault) {
        query_.addOrderBy(expression, direction, nulls);
        return *this;
    }
    DbQueryBuilder& skip(std::uint64_t count) {
        query_.offset(count);
        return *this;
    }
    DbQueryBuilder& take(std::uint64_t count) {
        query_.limit(count);
        return *this;
    }
    DbQueryBuilder& leftJoin(std::string_view table, std::string_view alias, DbExpression on) {
        query_.join(DbJoinType::kLeft, table, on, alias);
        return *this;
    }
    DbQueryBuilder& innerJoin(std::string_view table, std::string_view alias, DbExpression on) {
        query_.join(DbJoinType::kInner, table, on, alias);
        return *this;
    }
    DbQueryBuilder& leftJoinAndSelect(std::string_view relation, std::string_view alias) {
        return joinAndSelect(relation, alias, DbJoinType::kLeft);
    }
    DbQueryBuilder& innerJoinAndSelect(std::string_view relation, std::string_view alias) {
        return joinAndSelect(relation, alias, DbJoinType::kInner);
    }
    DbQueryBuilder& groupBy(std::span<const DbExpression> expressions) {
        query_.groupBy(expressions);
        return *this;
    }
    DbQueryBuilder& having(DbExpression predicate) {
        query_.having(predicate);
        return *this;
    }
    DbQueryBuilder& setLock(const DbLockOptions& options) {
        query_.lock(options);
        return *this;
    }
    DbQueryBuilder& addCommonTableExpression(std::string_view name, const DbQuery& query, const DbCteOptions& options = {}) {
        query_.with(name, query, options);
        return *this;
    }
    [[nodiscard]] ScopedOperation<DbEntityRows<Entity>> getMany() const {
        if (relations_ && !relations_->empty()) {
            auto prepared = relations_->template prepare<Entity>(query_, alias_, executor_.queryDriver());
            return executor_.template queryMapped<DbEntityRows<Entity>>(prepared ? *prepared : query_,
                detail::DbMapRelatedEntities<Entity>{relations_->clone()});
        }
        return executor_.template query<Entity>(query_);
    }
    [[nodiscard]] ScopedOperation<std::optional<Entity>> getOne() const {
        auto query = query_.clone(query_.resource());
        query.limit(1);
        if (relations_ && !relations_->empty()) {
            auto prepared = relations_->template prepare<Entity>(query, alias_, executor_.queryDriver());
            return executor_.template queryMapped<std::optional<Entity>>(prepared ? *prepared : query,
                detail::DbMapOneRelatedEntity<Entity>{relations_->clone()});
        }
        return executor_.template queryMapped<std::optional<Entity>>(query, detail::DbMapOneEntity<Entity>{});
    }
    [[nodiscard]] ScopedOperation<DbRows> getRawMany() const {
        if (relations_ && !relations_->empty()) {
            auto prepared = relations_->template prepare<Entity>(query_, alias_, executor_.queryDriver());
            return executor_.query(prepared ? *prepared : query_);
        }
        return executor_.query(query_);
    }
    [[nodiscard]] ScopedOperation<std::uint64_t> getCount() const {
        auto source = query_.clone(query_.resource());
        source.clearOrder().clearLock().limit(std::nullopt).offset(std::nullopt);
        if (relations_ && !relations_->empty()) {
            DbQuery roots(query_.resource());
            constexpr auto keys = detail::dbPrimaryKeyColumns<Entity>();
            for (const auto key : keys) {
                roots.addSelect(roots.column(key, "__ruvia_count_roots"));
            }
            roots.from(source, "__ruvia_count_roots").distinct();
            source = std::move(roots);
        }
        DbQuery count(query_.resource());
        const std::array args{count.star()};
        count.select(count.alias(count.aggregate("count", args), "count")).from(source, "count_source");
        return executor_.template queryMapped<std::uint64_t>(count, detail::DbMapCount{});
    }
    [[nodiscard]] ScopedOperation<DbExecResult> execute() const {
        return executor_.execute(query_);
    }
    [[nodiscard]] DbStatement getQueryAndParameters() const {
        if (relations_ && !relations_->empty()) {
            auto prepared = relations_->template prepare<Entity>(query_, alias_, executor_.queryDriver());
            return (prepared ? *prepared : query_).compile(executor_.queryDriver(), query_.resource());
        }
        return query_.compile(executor_.queryDriver(), query_.resource());
    }

private:
    friend class DbRepository<Entity, Executor>;
    DbQueryBuilder(detail::DbRepositoryInput<Executor> executor, std::string_view alias)
        : executor_(executor),
          query_(executor.queryResource()),
          alias_(alias.empty() ? detail::entityQueryAlias<Entity>() : alias, query_.resource()) {
        detail::selectEntity<Entity>(query_, alias_);
    }
    DbQueryBuilder& joinAndSelect(std::string_view relation, std::string_view alias, DbJoinType join) {
        if (alias.empty()) {
            throw std::invalid_argument("relation joins require an alias");
        }
        if (!relations_) {
            relations_.emplace(query_.resource());
        }
        relations_->template add<Entity>(query_, alias_, relation, alias, join);
        return *this;
    }
    detail::DbRepositoryExecutor<Executor> executor_;
    DbQuery query_;
    std::pmr::string alias_;
    std::optional<detail::DbRelationPlan> relations_{};
};

template <typename Entity, typename Executor>
class DbRepository final {
public:
    [[nodiscard]] DbQueryBuilder<Entity, Executor> createQueryBuilder(std::string_view alias = {}) const {
        return DbQueryBuilder<Entity, Executor>(executor_, alias);
    }
    [[nodiscard]] ScopedOperation<DbEntityRows<Entity>> find(const DbFindOptions& options = {}) const {
        DbQuery query(executor_.queryResource());
        const auto alias = options.relations.empty() ? std::string_view{} : detail::entityQueryAlias<Entity>();
        detail::selectEntity<Entity>(query, alias);
        detail::applyFindOptions<Entity>(query, options, alias);
        if (!options.relations.empty()) {
            detail::DbRelationPlan relations(query.resource());
            for (const auto& path : options.relations) {
                relations.template add<Entity>(query, alias, path);
            }
            auto prepared = relations.template prepare<Entity>(query, alias, executor_.queryDriver());
            return executor_.template queryMapped<DbEntityRows<Entity>>(prepared ? *prepared : query,
                detail::DbMapRelatedEntities<Entity>{std::move(relations)});
        }
        return executor_.template query<Entity>(query);
    }
    [[nodiscard]] ScopedOperation<std::optional<Entity>> findOne(const DbFindOptions& options) const {
        DbQuery query(executor_.queryResource());
        const auto alias = options.relations.empty() ? std::string_view{} : detail::entityQueryAlias<Entity>();
        detail::selectEntity<Entity>(query, alias);
        detail::applyFindOptions<Entity>(query, options, alias);
        query.limit(1);
        if (!options.relations.empty()) {
            detail::DbRelationPlan relations(query.resource());
            for (const auto& path : options.relations) {
                relations.template add<Entity>(query, alias, path);
            }
            auto prepared = relations.template prepare<Entity>(query, alias, executor_.queryDriver());
            return executor_.template queryMapped<std::optional<Entity>>(prepared ? *prepared : query,
                detail::DbMapOneRelatedEntity<Entity>{std::move(relations)});
        }
        return executor_.template queryMapped<std::optional<Entity>>(query, detail::DbMapOneEntity<Entity>{});
    }
    [[nodiscard]] ScopedOperation<std::uint64_t> count(const DbPredicate& predicate = {}) const {
        DbQuery query(executor_.queryResource());
        const std::array args{query.star()};
        query.select(query.alias(query.aggregate("count", args), "count")).from(Entity::tableName());
        if (!predicate.empty()) {
            query.where(predicate.expression(query));
        }
        return executor_.template queryMapped<std::uint64_t>(query, detail::DbMapCount{});
    }
    [[nodiscard]] ScopedOperation<DbExecResult> insert(const Entity& entity) const {
        return insert(std::span<const Entity>(&entity, 1));
    }
    [[nodiscard]] ScopedOperation<DbExecResult> insert(std::span<const Entity> entities) const {
        auto query = insertQuery(entities);
        return executor_.execute(query);
    }
    [[nodiscard]] ScopedOperation<DbExecResult> update(const DbPredicate& predicate, const Entity& changes) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository update requires a condition");
        }
        DbQuery query(executor_.queryResource());
        query.update(Entity::tableName()).where(predicate.expression(query));
        bool selected = false;
        detail::forEachEntityColumn<Entity>([&]<typename C> {
            if constexpr (C::options.generatedType == DbGeneratedType::kNone) {
                if (changes.template isSet<C::name>()) {
                    query.set(C::name.view(), detail::entityColumnValue<Entity, C>(query, changes));
                    selected = true;
                }
            }
        });
        if (!selected) {
            throw std::invalid_argument("repository update requires a writable column");
        }
        return executor_.execute(query);
    }
    [[nodiscard]] ScopedOperation<DbExecResult> deleteBy(const DbPredicate& predicate) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository deleteBy requires a condition");
        }
        DbQuery query(executor_.queryResource());
        query.deleteFrom(Entity::tableName()).where(predicate.expression(query));
        return executor_.execute(query);
    }
    [[nodiscard]] ScopedOperation<DbExecResult> remove(const Entity& entity) const {
        DbQuery query(executor_.queryResource());
        query.deleteFrom(Entity::tableName());
        detail::forEachEntityColumn<Entity>([&]<typename C> {
            if constexpr (C::options.primaryKey) {
                if (!entity.template isSet<C::name>() || entity.template isNull<C::name>()) {
                    throw std::invalid_argument("remove requires all primary key values");
                }
                query.andWhere(query.binary(query.column(C::name.view()), DbBinaryOperator::kEqual, detail::entityColumnValue<Entity, C>(query, entity)));
            }
        });
        if (!query.hasWhere()) {
            throw std::invalid_argument("remove requires an entity primary key");
        }
        return executor_.execute(query);
    }
    [[nodiscard]] ScopedOperation<DbExecResult> upsert(const Entity& entity, const DbUpsertOptions& options) const {
        return upsert(std::span<const Entity>(&entity, 1), options);
    }
    [[nodiscard]] ScopedOperation<DbExecResult> upsert(std::span<const Entity> entities, const DbUpsertOptions& options) const {
        auto query = insertQuery(entities);
        DbConflictOptions conflict{.columns = options.conflictPaths, .doNothing = options.doNothing, .anyUniqueKey = options.anyUniqueKey};
        for (const auto& name : options.conflictPaths) {
            detail::requireEntityColumn<Entity>(name);
        }
        for (const auto& name : options.updateColumns) {
            detail::requireEntityColumn<Entity>(name);
            if (detail::isGeneratedEntityColumn<Entity>(name)) {
                throw std::invalid_argument("repository upsert cannot update a generated column");
            }
        }
        if (!options.doNothing) {
            detail::forEachEntityColumn<Entity>([&]<typename C> {
                bool selected = false;
                if (options.updateColumns.empty()) {
                    if constexpr (!C::options.primaryKey && !C::options.generated && C::options.generatedType == DbGeneratedType::kNone) {
                        selected = std::ranges::all_of(entities, [](const Entity& entity) { return entity.template isSet<C::name>(); });
                        if (!selected && std::ranges::any_of(entities, [](const Entity& entity) { return entity.template isSet<C::name>(); })) {
                            throw std::invalid_argument("bulk upsert requires identical set columns or explicit updateColumns");
                        }
                        selected &= std::ranges::none_of(options.conflictPaths, [](const auto& name) { return name == C::name.view(); });
                    }
                } else {
                    if constexpr (C::options.generatedType == DbGeneratedType::kNone) {
                        selected = std::ranges::any_of(options.updateColumns, [](const auto& name) { return name == C::name.view(); });
                    }
                }
                if (selected) {
                    auto value = query.excluded(C::name.view());
                    conflict.update.push_back({std::string(C::name.view()), value});
                }
            });
        }
        query.onConflict(conflict);
        return executor_.execute(query);
    }

private:
    friend class DbHandle;
    friend class DbTransaction;
    explicit DbRepository(detail::DbRepositoryInput<Executor> executor)
        : executor_(executor) {}
    DbQuery insertQuery(std::span<const Entity> entities) const {
        if (entities.empty()) {
            throw std::invalid_argument("repository insert requires an entity");
        }
        DbQuery query(executor_.queryResource());
        std::pmr::vector<std::string_view> columns(query.resource());
        detail::forEachEntityColumn<Entity>([&]<typename C> {
            if constexpr (C::options.generatedType == DbGeneratedType::kNone) {
                if (std::ranges::any_of(entities, [](const Entity& entity) { return entity.template isSet<C::name>(); })) {
                    columns.push_back(C::name.view());
                }
            }
        });
        query.insertInto(Entity::tableName(), columns);
        if (columns.empty()) {
            if (entities.size() != 1) {
                throw std::invalid_argument("bulk default-only inserts require at least one explicit column");
            }
            return query;
        }
        for (const auto& entity : entities) {
            std::pmr::vector<DbExpression> row(query.resource());
            detail::forEachEntityColumn<Entity>([&]<typename C> {
                if constexpr (C::options.generatedType == DbGeneratedType::kNone) {
                    if (std::ranges::find(columns, C::name.view()) != columns.end()) {
                        row.push_back(detail::entityColumnValue<Entity, C>(query, entity));
                    }
                }
            });
            query.values(row);
        }
        return query;
    }
    detail::DbRepositoryExecutor<Executor> executor_;
};

template <typename Entity>
DbRepository<Entity, DbHandle> DbHandle::getRepository() const {
    requireActive();
    return DbRepository<Entity, DbHandle>(*this);
}
template <typename Entity>
ScopedOperation<DbEntityRows<Entity>> DbHandle::query(const DbQuery& query) const {
    return queryMapped<DbEntityRows<Entity>>(query, detail::DbMapEntityRows<Entity>{});
}
template <typename Entity>
DbRepository<Entity, DbTransaction> DbTransaction::getRepository() & {
    (void)queryResource();
    return DbRepository<Entity, DbTransaction>(*this);
}
template <typename Entity>
ScopedOperation<DbEntityRows<Entity>> DbTransaction::query(const DbQuery& query) & {
    return queryMapped<DbEntityRows<Entity>>(query, detail::DbMapEntityRows<Entity>{});
}

}  // namespace ruvia
