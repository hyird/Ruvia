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

#include "ruvia/web/db/DbExpressions.h"
#include "ruvia/web/db/DbFindOptions.h"
#include "ruvia/web/db/DbHandle.h"
#include "ruvia/web/db/DbProjection.h"
#include "ruvia/web/detail/db/DbEntityCodec.h"
#include "ruvia/web/detail/db/DbRelationQuery.h"
#include "ruvia/web/detail/db/DbResultAccess.h"

namespace ruvia {

template <typename Entity, typename Executor>
class DbWriteQueryBuilder;

struct DbSelection final {
    std::string column{};
    DbExpression expression{};
};

struct DbUpsertOptions final {
    std::vector<std::string> conflictPaths{};
    std::vector<std::string> updateColumns{};
    bool doNothing{false};
    bool anyUniqueKey{false};
    bool skipUpdateIfNoValuesChanged{false};
    DbPredicate indexPredicate{};
    std::vector<DbAssignment> updateExpressions{};
    DbExpression updateWhere{};
};

namespace detail {

template <typename T>
concept DbWriteCondition = std::same_as<T, DbPredicate> || std::same_as<T, DbExpression>;

inline DbExpression writeCondition(DbQuery& query, const DbPredicate& predicate, std::string_view table = {}, std::string_view alias = {}) {
    return predicate.expression(query, table, alias);
}
inline DbExpression writeCondition(DbQuery& query, DbExpression predicate, std::string_view = {}, std::string_view = {}) {
    return query.importExpression(predicate);
}

template <typename E, typename Fn>
void forEachEntityColumn(Fn&& fn) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (fn.template operator()<std::tuple_element_t<I, typename E::Columns>>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<typename E::Columns>>{});
}
template <typename E>
constexpr bool hasEntityColumn(std::string_view name) noexcept {
    return [&]<typename... Columns>(std::tuple<Columns...>*) {
        return ((name == Columns::name.view()) || ...);
    }(static_cast<typename E::Columns*>(nullptr));
}
template <typename E>
void requireEntityColumn(std::string_view name) {
    if (!hasEntityColumn<E>(name)) {
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
    query.cache(options.cache);
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
        return dbCountValue(rows);
    }
};
struct DbMapExists final {
    bool operator()(DbRows&& rows, std::pmr::memory_resource*) const {
        if (rows.size() != 1) {
            throw std::runtime_error("database exists did not return exactly one row");
        }
        return rows.front()["exists"].template as<bool>().value();
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

template <typename E>
struct DbMapProjection final {
    static_assert(requires { requires std::derived_from<E, typename E::SqlEntityType>; } || requires { requires std::derived_from<E, typename E::DbProjectionType>; }, "SQL results require a SQL entity or a DbProjection");
    explicit DbMapProjection(std::span<const std::pmr::string> selected) {
        for (const auto& name : selected) {
            requireEntityColumn<E>(name);
        }
        std::size_t index = 0;
        forEachEntityColumn<E>([&]<typename C> {
            selectedColumns_[index++] = selected.empty() || std::ranges::contains(selected, C::name.view());
        });
    }
    E decode(const DbRow& row, std::pmr::memory_resource* resource) const {
        E result(resource);
        std::size_t index = 0;
        forEachEntityColumn<E>([&]<typename C> {
            if (selectedColumns_[index++]) {
                decodeEntityColumn<E, C>(result, row, resource);
            }
        });
        return result;
    }
    DbEntityRows<E> operator()(DbRows&& rows, std::pmr::memory_resource* resource) const {
        auto result = DbResultAccess::makeEntityRows<E>(resource, rows.size());
        DbEntityRowDecoder<E> decoder(selectedColumns_);
        for (const auto& row : rows) {
            result.push_back(decoder.decode(row, resource));
        }
        return result;
    }

private:
    std::array<bool, std::tuple_size_v<typename E::Columns>> selectedColumns_{};
};
template <typename E>
struct DbMapOneProjection final {
    DbMapProjection<E> mapper;
    std::optional<E> operator()(DbRows&& rows, std::pmr::memory_resource* resource) const {
        if (rows.empty()) {
            return std::nullopt;
        }
        return mapper.decode(rows.front(), resource);
    }
};
inline std::pmr::vector<std::pmr::string> applyProjection(DbQuery& query, std::span<const DbSelection> selection,
    std::string_view qualifier, bool returning = false) {
    if (selection.empty()) {
        throw std::invalid_argument("projection requires at least one field");
    }
    std::pmr::vector<std::pmr::string> columns(query.resource());
    std::pmr::vector<DbExpression> values(query.resource());
    for (const auto& field : selection) {
        if (field.column.empty() || std::ranges::find(columns, std::string_view(field.column)) != columns.end()) {
            throw std::invalid_argument("projection requires unique nonempty field names");
        }
        columns.emplace_back(field.column);
        auto expression = field.expression.empty() ? query.column(field.column, qualifier) : query.importExpression(field.expression);
        values.push_back(query.alias(expression, field.column));
    }
    if (returning) {
        query.returning(values);
    } else {
        query.select(values);
    }
    return columns;
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

    DbQueryBuilder& select(std::span<const DbSelection> fields) {
        requirePlainProjection();
        for (const auto& field : fields) {
            if (field.expression.empty()) {
                detail::requireEntityColumn<Entity>(field.column);
            }
        }
        selected_ = detail::applyProjection(query_, fields, alias_);
        return *this;
    }
    DbQueryBuilder& select(std::initializer_list<DbSelection> fields) {
        return select(std::span<const DbSelection>(fields.begin(), fields.size()));
    }
    template <typename Joined>
    DbQueryBuilder& join(DbJoinType type, std::string_view alias, DbExpression on = {}) {
        static_assert(std::derived_from<Joined, typename Joined::SqlEntityType>);
        query_.join(type, Joined::tableName(), (on.empty() ? DbExpression{} : query_.importExpression(on)), alias);
        return *this;
    }
    template <typename Source, typename SourceExecutor>
    DbQueryBuilder& join(DbJoinType type, const DbQueryBuilder<Source, SourceExecutor>& source,
        std::string_view alias, DbExpression on = {}, const DbSourceOptions& options = {}) {
        source.requirePlainProjection();
        query_.join(type, source.query_, (on.empty() ? DbExpression{} : query_.importExpression(on)), alias, options);
        return *this;
    }
    DbQueryBuilder& joinCte(DbJoinType type, std::string_view name, std::string_view alias, DbExpression on = {}) {
        query_.join(type, name, (on.empty() ? DbExpression{} : query_.importExpression(on)), alias);
        return *this;
    }
    DbQueryBuilder& joinFunction(DbJoinType type, DbExpression function, std::string_view alias,
        DbExpression on = {}, const DbSourceOptions& options = {}) {
        query_.joinFunction(type, query_.importExpression(function), (on.empty() ? DbExpression{} : query_.importExpression(on)), alias, options);
        return *this;
    }
    template <typename Source, typename SourceExecutor>
    DbQueryBuilder& with(std::string_view name, const DbQueryBuilder<Source, SourceExecutor>& source, const DbCteOptions& options = {}) {
        source.requirePlainProjection();
        query_.with(name, source.query_, options);
        return *this;
    }
    template <typename Source, typename SourceExecutor>
    DbQueryBuilder& with(std::string_view name, const DbWriteQueryBuilder<Source, SourceExecutor>& source, const DbCteOptions& options = {}) {
        requirePlainProjection();
        source.validate();
        query_.with(name, source.query_, options);
        return *this;
    }
    DbQueryBuilder& fromCte(std::string_view name) {
        requirePlainProjection();
        query_.from(name, alias_);
        return *this;
    }
    DbExpression subquery(DbExpressions& expressions) const {
        requirePlainProjection();
        return expressions.query_.subquery(query_);
    }
    DbExpression exists(DbExpressions& expressions) const {
        requirePlainProjection();
        return expressions.query_.exists(query_);
    }
    template <typename Source, typename SourceExecutor>
    DbQueryBuilder& combine(DbSetOperation operation, const DbQueryBuilder<Source, SourceExecutor>& source) {
        requirePlainProjection();
        source.requirePlainProjection();
        query_.combine(operation, source.query_);
        return *this;
    }
    DbQueryBuilder& groupBy(std::span<const DbExpression> expressions) {
        std::pmr::vector<DbExpression> imported(query_.resource());
        for (auto expression : expressions) {
            imported.push_back(query_.importExpression(expression));
        }
        query_.groupBy(imported);
        return *this;
    }
    DbQueryBuilder& groupBy(std::initializer_list<DbExpression> expressions) {
        return groupBy(std::span<const DbExpression>(expressions.begin(), expressions.size()));
    }
    DbQueryBuilder& having(DbExpression expression) {
        query_.having(query_.importExpression(expression));
        return *this;
    }
    DbQueryBuilder& andHaving(DbExpression expression) {
        query_.andHaving(query_.importExpression(expression));
        return *this;
    }
    DbQueryBuilder& where(DbExpression expression) {
        query_.where(query_.importExpression(expression));
        return *this;
    }
    DbQueryBuilder& andWhere(DbExpression expression) {
        query_.andWhere(query_.importExpression(expression));
        return *this;
    }
    DbQueryBuilder& orWhere(DbExpression expression) {
        query_.orWhere(query_.importExpression(expression));
        return *this;
    }
    DbQueryBuilder& orderBy(DbExpression expression, DbOrderDirection direction = DbOrderDirection::kAsc, DbNullsOrder nulls = DbNullsOrder::kDefault) {
        query_.orderBy(query_.importExpression(expression), direction, nulls);
        return *this;
    }
    DbQueryBuilder& addOrderBy(DbExpression expression, DbOrderDirection direction = DbOrderDirection::kAsc, DbNullsOrder nulls = DbNullsOrder::kDefault) {
        query_.addOrderBy(query_.importExpression(expression), direction, nulls);
        return *this;
    }
    DbQueryBuilder& distinct(bool enabled = true) {
        query_.distinct(enabled);
        return *this;
    }
    DbQueryBuilder& where(const DbPredicate& predicate) {
        query_.where(predicate.expression(query_, Entity::tableName(), alias_));
        return *this;
    }
    DbQueryBuilder& andWhere(const DbPredicate& predicate) {
        query_.andWhere(predicate.expression(query_, Entity::tableName(), alias_));
        return *this;
    }
    DbQueryBuilder& orWhere(const DbPredicate& predicate) {
        query_.orWhere(predicate.expression(query_, Entity::tableName(), alias_));
        return *this;
    }
    DbQueryBuilder& orderBy(std::string_view column, DbOrderDirection direction = DbOrderDirection::kAsc, DbNullsOrder nulls = DbNullsOrder::kDefault) {
        detail::requireEntityColumn<Entity>(column);
        query_.orderBy(query_.column(column, alias_), direction, nulls);
        return *this;
    }
    DbQueryBuilder& addOrderBy(std::string_view column, DbOrderDirection direction = DbOrderDirection::kAsc, DbNullsOrder nulls = DbNullsOrder::kDefault) {
        detail::requireEntityColumn<Entity>(column);
        query_.addOrderBy(query_.column(column, alias_), direction, nulls);
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
    DbQueryBuilder& leftJoinAndSelect(std::string_view relation, std::string_view alias) {
        return joinAndSelect(relation, alias, DbJoinType::kLeft);
    }
    DbQueryBuilder& innerJoinAndSelect(std::string_view relation, std::string_view alias) {
        return joinAndSelect(relation, alias, DbJoinType::kInner);
    }
    DbQueryBuilder& cache(const DbCacheSetting& setting) {
        query_.cache(setting);
        return *this;
    }
    DbQueryBuilder& cache(std::string_view id, std::optional<std::chrono::milliseconds> milliseconds = {}) {
        query_.cache(id, milliseconds);
        return *this;
    }
    DbQueryBuilder& setLock(const DbLockOptions& options) {
        query_.lock(options);
        return *this;
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> getMany() const {
        auto mapper = projectionMapper<Output>();
        if constexpr (std::same_as<Output, Entity>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<Entity>(query_, alias_, executor_.queryDriver());
                return executor_.template queryMapped<DbEntityRows<Entity>>(prepared ? *prepared : query_,
                    detail::DbMapRelatedEntities<Entity>{relations_->clone()});
            }
        }
        return executor_.template queryMapped<DbEntityRows<Output>>(query_, std::move(mapper));
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<std::optional<Output>> getOne() const {
        auto mapper = projectionMapper<Output>();
        auto query = query_.clone(query_.resource());
        query.limit(1);
        if constexpr (std::same_as<Output, Entity>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<Entity>(query, alias_, executor_.queryDriver());
                return executor_.template queryMapped<std::optional<Entity>>(prepared ? *prepared : query,
                    detail::DbMapOneRelatedEntity<Entity>{relations_->clone()});
            }
        }
        return executor_.template queryMapped<std::optional<Output>>(query, detail::DbMapOneProjection<Output>{std::move(mapper)});
    }
    [[nodiscard]] ScopedOperation<std::uint64_t> getCount() const {
        const auto count = countQuery();
        return executor_.template queryMapped<std::uint64_t>(count, detail::DbMapCount{});
    }
    [[nodiscard]] ScopedOperation<bool> getExists() const {
        auto source = query_.clone(query_.resource());
        source.clearOrder().clearLock().limit(std::nullopt).offset(std::nullopt);
        DbQuery query(query_.resource());
        query.select(query.alias(query.exists(source), "exists"));
        query.copyCache(query_);
        return executor_.template queryMapped<bool>(query, detail::DbMapExists{});
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<std::pair<DbEntityRows<Output>, std::uint64_t>> getManyAndCount() const {
        if (query_.hasWrites()) {
            throw std::invalid_argument("getManyAndCount cannot execute a write CTE twice");
        }
        auto mapper = projectionMapper<Output>();
        auto count = countQuery();
        count.copyCache(query_, "-count");
        if constexpr (std::same_as<Output, Entity>) {
            if (relations_ && !relations_->empty()) {
                auto prepared = relations_->template prepare<Entity>(query_, alias_, executor_.queryDriver());
                return executor_.template queryMappedAndCount<DbEntityRows<Entity>>(prepared ? *prepared : query_, count,
                    detail::DbMapRelatedEntities<Entity>{relations_->clone()});
            }
        }
        return executor_.template queryMappedAndCount<DbEntityRows<Output>>(query_, count, std::move(mapper));
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
    template <typename, typename>
    friend class DbQueryBuilder;
    template <typename, typename>
    friend class DbWriteQueryBuilder;
    void requirePlainProjection() const {
        if (relations_ && !relations_->empty()) {
            throw std::invalid_argument("explicit projections and subqueries require joins without relation hydration");
        }
    }
    template <typename Output>
    detail::DbMapProjection<Output> projectionMapper() const {
        if constexpr (!std::same_as<Output, Entity>) {
            requirePlainProjection();
            if (selected_.empty()) {
                throw std::invalid_argument("DTO mapping requires an explicit projection");
            }
        }
        return detail::DbMapProjection<Output>(selected_);
    }
    DbQueryBuilder(detail::DbRepositoryInput<Executor> executor, std::string_view alias)
        : executor_(executor),
          query_(executor.queryResource()),
          alias_(alias.empty() ? detail::entityQueryAlias<Entity>() : alias, query_.resource()),
          selected_(query_.resource()) {
        detail::selectEntity<Entity>(query_, alias_);
    }
    [[nodiscard]] DbQuery countQuery() const {
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
        count.copyCache(query_);
        return count;
    }
    DbQueryBuilder& joinAndSelect(std::string_view relation, std::string_view alias, DbJoinType join) {
        if (query_.hasWrites()) {
            throw std::invalid_argument("write CTEs require flat result mapping");
        }
        if (!selected_.empty()) {
            throw std::invalid_argument("relation hydration requires a full entity projection");
        }
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
    std::pmr::vector<std::pmr::string> selected_;
    std::optional<detail::DbRelationPlan> relations_{};
};

template <typename Entity, typename Executor>
class DbWriteQueryBuilder final {
public:
    DbWriteQueryBuilder(const DbWriteQueryBuilder&) = delete;
    DbWriteQueryBuilder& operator=(const DbWriteQueryBuilder&) = delete;
    DbWriteQueryBuilder(DbWriteQueryBuilder&&) = default;
    DbWriteQueryBuilder& operator=(DbWriteQueryBuilder&&) = delete;

    template <detail::DbWriteCondition Condition = DbPredicate>
    DbWriteQueryBuilder& where(const Condition& predicate) {
        requireCondition(predicate);
        query_.where(detail::writeCondition(query_, predicate, Entity::tableName(), alias_));
        return *this;
    }
    template <detail::DbWriteCondition Condition = DbPredicate>
    DbWriteQueryBuilder& andWhere(const Condition& predicate) {
        requireCondition(predicate);
        query_.andWhere(detail::writeCondition(query_, predicate, Entity::tableName(), alias_));
        return *this;
    }
    template <detail::DbWriteCondition Condition = DbPredicate>
    DbWriteQueryBuilder& orWhere(const Condition& predicate) {
        requireCondition(predicate);
        query_.orWhere(detail::writeCondition(query_, predicate, Entity::tableName(), alias_));
        return *this;
    }
    DbWriteQueryBuilder& set(std::string_view column, DbExpression value) {
        detail::requireEntityColumn<Entity>(column);
        if (detail::isGeneratedEntityColumn<Entity>(column) || value.empty()) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        query_.set(column, query_.importExpression(value));
        return *this;
    }
    DbWriteQueryBuilder& set(const Entity& changes) {
        detail::forEachEntityColumn<Entity>([&]<typename C> {
            if constexpr (C::options.generatedType == DbGeneratedType::kNone) {
                if (changes.template isSet<C::name>()) {
                    query_.set(C::name.view(), detail::entityColumnValue<Entity, C>(query_, changes));
                }
            }
        });
        return *this;
    }
    template <typename Source>
    DbWriteQueryBuilder& updateFrom(std::string_view alias) {
        static_assert(std::derived_from<Source, typename Source::SqlEntityType>);
        query_.updateFrom(Source::tableName(), alias);
        return *this;
    }
    template <typename Source, typename SourceExecutor>
    DbWriteQueryBuilder& updateFrom(const DbQueryBuilder<Source, SourceExecutor>& source, std::string_view alias) {
        source.requirePlainProjection();
        query_.updateFrom(source.query_, alias);
        return *this;
    }
    DbWriteQueryBuilder& updateFromCte(std::string_view name, std::string_view alias) {
        query_.updateFrom(name, alias);
        return *this;
    }
    template <typename Source, typename SourceExecutor>
    DbWriteQueryBuilder& insertFrom(std::span<const std::string_view> columns, const DbQueryBuilder<Source, SourceExecutor>& source) {
        if (!inserting_) {
            throw std::invalid_argument("insertFrom requires an insert builder");
        }
        source.requirePlainProjection();
        source.query_.requireSelectQuery();
        if (columns.empty()) {
            throw std::invalid_argument("insertFrom requires target columns");
        }
        for (auto column : columns) {
            detail::requireEntityColumn<Entity>(column);
            if (detail::isGeneratedEntityColumn<Entity>(column)) {
                throw std::invalid_argument("insertFrom cannot write a generated column");
            }
        }
        const auto count = source.selected_.empty() ? std::tuple_size_v<typename Source::Columns> : source.selected_.size();
        if (count != columns.size()) {
            throw std::invalid_argument("insertFrom projection and target column counts differ");
        }
        query_.insertInto(Entity::tableName(), columns, alias_).insertFrom(source.query_);
        return *this;
    }
    template <typename Source, typename SourceExecutor>
    DbWriteQueryBuilder& insertFrom(std::initializer_list<std::string_view> columns, const DbQueryBuilder<Source, SourceExecutor>& source) {
        return insertFrom(std::span<const std::string_view>(columns.begin(), columns.size()), source);
    }
    template <typename Source, typename SourceExecutor>
    DbWriteQueryBuilder& with(std::string_view name, const DbQueryBuilder<Source, SourceExecutor>& source, const DbCteOptions& options = {}) {
        source.requirePlainProjection();
        query_.with(name, source.query_, options);
        return *this;
    }
    template <typename Source, typename SourceExecutor>
    DbWriteQueryBuilder& with(std::string_view name, const DbWriteQueryBuilder<Source, SourceExecutor>& source, const DbCteOptions& options = {}) {
        source.validate();
        query_.with(name, source.query_, options);
        return *this;
    }
    DbWriteQueryBuilder& returning(std::span<const DbSelection> fields = {}) {
        if (fields.empty()) {
            std::pmr::vector<DbExpression> expressions(query_.resource());
            detail::forEachEntityColumn<Entity>([&]<typename C> { expressions.push_back(query_.column(C::name.view(), qualifier())); });
            query_.returning(expressions);
            selected_.clear();
        } else {
            for (const auto& field : fields) {
                if (field.expression.empty()) {
                    detail::requireEntityColumn<Entity>(field.column);
                }
            }
            selected_ = detail::applyProjection(query_, fields, qualifier(), true);
        }
        return *this;
    }
    DbWriteQueryBuilder& returning(std::initializer_list<DbSelection> fields) {
        return returning(std::span<const DbSelection>(fields.begin(), fields.size()));
    }
    [[nodiscard]] ScopedOperation<DbExecResult> execute() const {
        validate();
        return executor_.execute(query_);
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> getMany() const {
        validate();
        if (!query_.returnsRows()) {
            throw std::invalid_argument("getMany requires RETURNING");
        }
        if constexpr (!std::same_as<Output, Entity>) {
            if (selected_.empty()) {
                throw std::invalid_argument("DTO returning requires an explicit projection");
            }
        }
        return executor_.template queryMapped<DbEntityRows<Output>>(query_, detail::DbMapProjection<Output>(selected_));
    }
    [[nodiscard]] DbStatement getQueryAndParameters() const {
        validate();
        return query_.compile(executor_.queryDriver(), query_.resource());
    }

private:
    friend class DbRepository<Entity, Executor>;
    template <typename, typename>
    friend class DbQueryBuilder;
    template <typename, typename>
    friend class DbWriteQueryBuilder;
    DbWriteQueryBuilder(detail::DbRepositoryInput<Executor> executor, DbQuery query, bool inserting, std::string_view alias = {})
        : executor_(executor),
          query_(std::move(query)),
          inserting_(inserting),
          alias_(alias, query_.resource()),
          selected_(query_.resource()) {}
    std::string_view qualifier() const {
        return alias_.empty() ? Entity::tableName() : std::string_view(alias_);
    }
    template <detail::DbWriteCondition Condition = DbPredicate>
    void requireCondition(const Condition& condition) const {
        if (inserting_ || condition.empty()) {
            throw std::invalid_argument("write WHERE requires an update/delete builder and a nonempty condition");
        }
    }
    void validate() const {
        if (!inserting_ && !query_.hasWhere()) {
            throw std::invalid_argument("repository update/delete requires a condition");
        }
    }
    detail::DbRepositoryExecutor<Executor> executor_;
    DbQuery query_;
    bool inserting_;
    std::pmr::string alias_;
    std::pmr::vector<std::pmr::string> selected_;
};

template <typename Entity, typename Executor>
class DbRepository final {
    static_assert(requires {
        typename Entity::SqlEntityType;
        requires std::derived_from<Entity, typename Entity::SqlEntityType>; }, "SQL repositories require a RUVIA_DB_ENTITY declaration");

public:
    [[nodiscard]] DbQueryBuilder<Entity, Executor> createQueryBuilder(std::string_view alias = {}) const {
        return DbQueryBuilder<Entity, Executor>(executor_, alias);
    }
    [[nodiscard]] DbWriteQueryBuilder<Entity, Executor> createUpdateBuilder(std::string_view alias = {}) const {
        DbQuery query(executor_.queryResource());
        query.update(Entity::tableName(), alias);
        return DbWriteQueryBuilder<Entity, Executor>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] DbWriteQueryBuilder<Entity, Executor> createDeleteBuilder(std::string_view alias = {}) const {
        DbQuery query(executor_.queryResource());
        query.deleteFrom(Entity::tableName(), alias);
        return DbWriteQueryBuilder<Entity, Executor>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] DbWriteQueryBuilder<Entity, Executor> createInsertBuilder() const {
        DbQuery query(executor_.queryResource());
        query.insertInto(Entity::tableName());
        return DbWriteQueryBuilder<Entity, Executor>(executor_, std::move(query), true);
    }
    [[nodiscard]] DbWriteQueryBuilder<Entity, Executor> createInsertBuilder(const Entity& entity) const {
        return createInsertBuilder(std::span<const Entity>(&entity, 1));
    }
    [[nodiscard]] DbWriteQueryBuilder<Entity, Executor> createInsertBuilder(std::span<const Entity> entities) const {
        return DbWriteQueryBuilder<Entity, Executor>(executor_, insertQuery(entities), true);
    }
    [[nodiscard]] ScopedOperation<DbEntityRows<Entity>> find(const DbFindOptions& options = {}) const {
        return findBuilder(options).getMany();
    }
    [[nodiscard]] ScopedOperation<std::optional<Entity>> findOne(const DbFindOptions& options) const {
        return findBuilder(options).getOne();
    }
    [[nodiscard]] ScopedOperation<std::pair<DbEntityRows<Entity>, std::uint64_t>> findAndCount(const DbFindOptions& options = {}) const {
        return findBuilder(options).getManyAndCount();
    }
    [[nodiscard]] ScopedOperation<std::uint64_t> count(const DbFindOptions& options = {}) const {
        return findBuilder(options).getCount();
    }
    [[nodiscard]] ScopedOperation<bool> exists(const DbFindOptions& options = {}) const {
        return findBuilder(options).getExists();
    }
    [[nodiscard]] ScopedOperation<DbExecResult> insert(const Entity& entity) const {
        return insert(std::span<const Entity>(&entity, 1));
    }
    [[nodiscard]] ScopedOperation<DbExecResult> insert(std::span<const Entity> entities) const {
        auto query = insertQuery(entities);
        return executor_.execute(query);
    }
    template <detail::DbWriteCondition Condition = DbPredicate>
    [[nodiscard]] ScopedOperation<DbExecResult> update(const Condition& predicate, const Entity& changes) const {
        auto query = updateQuery(predicate, changes);
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
    template <typename Number>
        requires((std::integral<Number> || std::floating_point<Number>) && !std::same_as<Number, bool>)
    [[nodiscard]] ScopedOperation<DbExecResult> increment(const DbPredicate& predicate, std::string_view propertyPath, Number value) const {
        return adjustNumber(predicate, propertyPath, value, DbBinaryOperator::kAdd);
    }
    template <typename Number>
        requires((std::integral<Number> || std::floating_point<Number>) && !std::same_as<Number, bool>)
    [[nodiscard]] ScopedOperation<DbExecResult> decrement(const DbPredicate& predicate, std::string_view propertyPath, Number value) const {
        return adjustNumber(predicate, propertyPath, value, DbBinaryOperator::kSubtract);
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
        auto query = upsertQuery(entities, options);
        return executor_.execute(query);
    }

    template <detail::DbWriteCondition Condition = DbPredicate>
    [[nodiscard]] ScopedOperation<DbExecResult> update(const Condition& predicate, std::span<const DbAssignment> changes) const {
        auto query = updateQuery(predicate, changes);
        return executor_.execute(query);
    }
    template <detail::DbWriteCondition Condition = DbPredicate>
    [[nodiscard]] ScopedOperation<DbExecResult> update(const Condition& predicate, std::initializer_list<DbAssignment> changes) const {
        return update(predicate, std::span<const DbAssignment>(changes.begin(), changes.size()));
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> insertReturning(const Entity& entity, std::span<const DbSelection> fields = {}) const {
        return insertReturning<Output>(std::span<const Entity>(&entity, 1), fields);
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> insertReturning(std::span<const Entity> entities, std::span<const DbSelection> fields = {}) const {
        auto query = insertQuery(entities);
        return returning<Output>(query, fields);
    }
    template <typename Output = Entity, detail::DbWriteCondition Condition = DbPredicate>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> updateReturning(const Condition& predicate, const Entity& changes, std::span<const DbSelection> fields = {}) const {
        auto query = updateQuery(predicate, changes);
        return returning<Output>(query, fields);
    }
    template <typename Output = Entity, detail::DbWriteCondition Condition = DbPredicate>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> updateReturning(const Condition& predicate, std::span<const DbAssignment> changes, std::span<const DbSelection> fields = {}) const {
        auto query = updateQuery(predicate, changes);
        return returning<Output>(query, fields);
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> deleteReturning(const DbPredicate& predicate, std::span<const DbSelection> fields = {}) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository deleteReturning requires a condition");
        }
        DbQuery query(executor_.queryResource());
        query.deleteFrom(Entity::tableName()).where(predicate.expression(query));
        return returning<Output>(query, fields);
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> upsertReturning(const Entity& entity, const DbUpsertOptions& options, std::span<const DbSelection> fields = {}) const {
        return upsertReturning<Output>(std::span<const Entity>(&entity, 1), options, fields);
    }
    template <typename Output = Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Output>> upsertReturning(std::span<const Entity> entities, const DbUpsertOptions& options, std::span<const DbSelection> fields = {}) const {
        auto query = upsertQuery(entities, options);
        return returning<Output>(query, fields);
    }

private:
    friend class DbHandle;
    friend class DbTransaction;
    explicit DbRepository(detail::DbRepositoryInput<Executor> executor)
        : executor_(executor) {}
    template <typename Number>
    [[nodiscard]] ScopedOperation<DbExecResult> adjustNumber(const DbPredicate& predicate, std::string_view propertyPath, Number value, DbBinaryOperator op) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository increment/decrement requires a condition");
        }
        bool numeric = false;
        detail::forEachEntityColumn<Entity>([&]<typename C> {
            if (C::name.view() == propertyPath && C::options.generatedType == DbGeneratedType::kNone && !detail::IsPmrVector<typename C::value_type>::value) {
                numeric = C::dataType == DbDataType::kSmallInt || C::dataType == DbDataType::kInteger || C::dataType == DbDataType::kBigInt || C::dataType == DbDataType::kNumeric || C::dataType == DbDataType::kReal || C::dataType == DbDataType::kDouble;
            }
        });
        if (!numeric) {
            throw std::invalid_argument("repository increment/decrement requires a writable numeric column");
        }
        DbQuery query(executor_.queryResource());
        query.update(Entity::tableName()).where(detail::writeCondition(query, predicate));
        query.set(propertyPath, query.binary(query.column(propertyPath), op, query.value(value)));
        return executor_.execute(query);
    }
    [[nodiscard]] DbQueryBuilder<Entity, Executor> findBuilder(const DbFindOptions& options) const {
        auto builder = createQueryBuilder();
        detail::applyFindOptions<Entity>(builder.query_, options, builder.alias_);
        if (!options.relations.empty()) {
            builder.relations_.emplace(builder.query_.resource());
            for (const auto& path : options.relations) {
                builder.relations_->template add<Entity>(builder.query_, builder.alias_, path);
            }
        }
        return builder;
    }

    template <typename Output>
    ScopedOperation<DbEntityRows<Output>> returning(DbQuery& query, std::span<const DbSelection> fields) const {
        std::pmr::vector<std::pmr::string> columns(query.resource());
        if (fields.empty()) {
            static_assert(requires { typename Output::Columns; });
            constexpr bool entityColumnsOnly = []<typename... Columns>(std::tuple<Columns...>*) {
                return (detail::hasEntityColumn<Entity>(Columns::name.view()) && ...);
            }(static_cast<typename Output::Columns*>(nullptr));
            if constexpr (entityColumnsOnly) {
                std::pmr::vector<DbExpression> values(query.resource());
                detail::forEachEntityColumn<Output>([&]<typename C> {
                    values.push_back(query.column(C::name.view()));
                });
                query.returning(values);
            } else {
                throw std::invalid_argument("unknown database entity column");
            }
        } else {
            for (const auto& field : fields) {
                if (field.expression.empty()) {
                    detail::requireEntityColumn<Entity>(field.column);
                }
            }
            columns = detail::applyProjection(query, fields, {}, true);
        }
        return executor_.template queryMapped<DbEntityRows<Output>>(query, detail::DbMapProjection<Output>(columns));
    }
    template <detail::DbWriteCondition Condition = DbPredicate>
    DbQuery updateQuery(const Condition& predicate, const Entity& changes) const {
        if (predicate.empty()) {
            throw std::invalid_argument("repository update requires a condition");
        }
        DbQuery query(executor_.queryResource());
        query.update(Entity::tableName()).where(detail::writeCondition(query, predicate));
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
        return query;
    }
    template <detail::DbWriteCondition Condition = DbPredicate>
    DbQuery updateQuery(const Condition& predicate, std::span<const DbAssignment> changes) const {
        if (predicate.empty() || changes.empty()) {
            throw std::invalid_argument("repository update requires a condition and writable columns");
        }
        DbQuery query(executor_.queryResource());
        query.update(Entity::tableName()).where(detail::writeCondition(query, predicate));
        for (std::size_t i = 0; i < changes.size(); ++i) {
            const auto& item = changes[i];
            detail::requireEntityColumn<Entity>(item.column);
            if (detail::isGeneratedEntityColumn<Entity>(item.column) || item.value.empty()) {
                throw std::invalid_argument("update expression requires a writable column");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (changes[j].column == item.column) {
                    throw std::invalid_argument("duplicate update column");
                }
            }
            query.set(item.column, query.importExpression(item.value));
        }
        return query;
    }
    DbQuery upsertQuery(std::span<const Entity> entities, const DbUpsertOptions& options) const {
        auto query = insertQuery(entities);
        if ((options.skipUpdateIfNoValuesChanged || !options.indexPredicate.empty() || !options.updateWhere.empty()) && executor_.queryDriver() != DbDriver::kPostgreSql) {
            throw std::invalid_argument("conditional repository upsert requires PostgreSQL");
        }
        DbConflictOptions conflict{.columns = options.conflictPaths, .doNothing = options.doNothing, .anyUniqueKey = options.anyUniqueKey};
        conflict.targetWhere = options.indexPredicate.expression(query);
        for (const auto& name : options.conflictPaths) {
            detail::requireEntityColumn<Entity>(name);
        }
        for (const auto& name : options.updateColumns) {
            detail::requireEntityColumn<Entity>(name);
            if (detail::isGeneratedEntityColumn<Entity>(name)) {
                throw std::invalid_argument("repository upsert cannot update a generated column");
            }
        }
        for (std::size_t i = 0; i < options.updateExpressions.size(); ++i) {
            const auto& item = options.updateExpressions[i];
            detail::requireEntityColumn<Entity>(item.column);
            if (item.value.empty() || detail::isGeneratedEntityColumn<Entity>(item.column) || options.doNothing) {
                throw std::invalid_argument("upsert expression requires a writable column and update action");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (options.updateExpressions[j].column == item.column) {
                    throw std::invalid_argument("duplicate upsert expression column");
                }
            }
        }
        if (options.doNothing && !options.updateWhere.empty()) {
            throw std::invalid_argument("upsert update condition requires an update action");
        }
        if (!options.doNothing) {
            detail::forEachEntityColumn<Entity>([&]<typename C> {
                bool selected = false;
                const auto custom = std::ranges::find_if(options.updateExpressions, [](const auto& item) { return item.column == C::name.view(); });
                if (custom != options.updateExpressions.end()) {
                    selected = true;
                } else if (options.updateColumns.empty()) {
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
                if (selected || custom != options.updateExpressions.end()) {
                    auto value = custom == options.updateExpressions.end() ? query.excluded(C::name.view()) : query.importExpression(custom->value);
                    conflict.update.push_back({std::string(C::name.view()), value});
                    if (options.skipUpdateIfNoValuesChanged) {
                        auto changed = query.binary(query.column(C::name.view(), Entity::tableName()), DbBinaryOperator::kIsDistinctFrom, value);
                        conflict.updateWhere = conflict.updateWhere.empty() ? changed : query.binary(conflict.updateWhere, DbBinaryOperator::kOr, changed);
                    }
                }
            });
            if (conflict.update.empty() && executor_.queryDriver() == DbDriver::kPostgreSql) {
                conflict.doNothing = true;
            }
        }
        if (!options.updateWhere.empty()) {
            auto condition = query.importExpression(options.updateWhere);
            conflict.updateWhere = conflict.updateWhere.empty() ? condition : query.binary(conflict.updateWhere, DbBinaryOperator::kAnd, condition);
        }
        query.onConflict(conflict);
        return query;
    }
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
DbRepository<Entity, DbTransaction> DbTransaction::getRepository() & {
    (void)queryResource();
    return DbRepository<Entity, DbTransaction>(*this);
}

}  // namespace ruvia
