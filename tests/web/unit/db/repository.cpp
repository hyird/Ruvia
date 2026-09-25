#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/web/db/DbRepository.h"
#include "ruvia/web/detail/db/DbQueryCache.h"
#include "ruvia/web/detail/db/DbQueryCacheState.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using namespace ruvia;
using Entity = DbEntity<"items", DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"name", std::pmr::string>>;
using NullableEntity = DbEntity<"nullable_items",
    DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"name", std::pmr::string, DbColumnOptions{.nullable = true}>>;
using ComputedEntity = DbEntity<"computed_items",
    DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"name", std::pmr::string>,
    DbColumn<"name_length", std::int64_t, DbColumnOptions{.generatedType = DbGeneratedType::kStored}>>;
using CompositeEntity = DbEntity<"composite_items",
    DbColumn<"tenant_id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"item_id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"name", std::pmr::string>,
    DbColumn<"revision", std::int64_t>>;

struct Child;
using ParentBase = DbEntity<"parents", DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbOneToMany<"children", Child, "parent">>;
using ChildBase = DbEntity<"children", DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"parent_id", std::int64_t>,
    DbManyToOne<"parent", ParentBase, DbJoinColumn<"parent_id", "id">>>;
struct Child final : ChildBase {
    using ChildBase::ChildBase;
};
using Parent = ParentBase;

struct RepositoryRuntime final {
    asio::io_context& context = test::newTestIoContext();
    EventLoopAttachment attachment = attachEventLoop(context, {.mailboxCapacity = 8});
    WorkerHandle worker = attachment.loop().handle();
};
DbConfig databaseConfig() {
#ifdef RUVIA_ENABLE_POSTGRESQL
    return {.driver = DbDriver::kPostgreSql};
#else
    return {.driver = DbDriver::kMariaDb};
#endif
}
void runVoid(Task<void> task) {
    auto& context = test::newTestIoContext();
    auto attachment = attachEventLoop(context, {.mailboxCapacity = 8});
    std::exception_ptr failure;
    asio::co_spawn(context, asAwaitable(std::move(task)), [&](std::exception_ptr error) {
        failure = std::move(error);
        context.stop();
    });
    context.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

RUVIA_DB_PROJECTION(ItemSummary,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(label, std::pmr::string),
    RUVIA_DB_COLUMN(rank, std::int64_t))

RUVIA_TEST(db_repository_projects_computed_dto_and_owns_expression_sources) {
    RepositoryRuntime runtime;
    test::CountingMemoryResource source, target;
    detail::DbRegistry registry(runtime.context, runtime.worker, &target, databaseConfig());
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    const auto baseline = target.liveAllocations();
    for (int i = 0; i < 8; ++i) {
        {
            auto builder = repository.createQueryBuilder("i");
            {
                DbExpressions expressions(&source);
                auto label = expressions.call("concat", {expressions.column("name", "i"), expressions.value(std::string(200, 'x'))});
                auto rank = expressions.over(expressions.call("row_number"), {.orderBy = {{expressions.column("id", "i")}}});
                builder.select({{"id"}, {"label", label}, {"rank", rank}});
            }
            RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
            const auto statement = builder.getQueryAndParameters();
            RUVIA_CHECK(statement.sql().find(databaseConfig().driver == DbDriver::kPostgreSql ? "\"row_number\"() OVER (ORDER BY" : "row_number() OVER (ORDER BY") != std::string_view::npos);
            RUVIA_CHECK_EQ(statement.params().size(), std::size_t{1});
            RUVIA_CHECK_EQ(detail::DbValueAccess::text(statement.params()[0]).size(), std::size_t{200});
            auto many = builder.getMany<ItemSummary>();
            auto one = builder.getOne<ItemSummary>();
            auto page = builder.getManyAndCount<ItemSummary>();
            RUVIA_CHECK(testing::throwsOn([&] { (void)builder.getMany(); }));
        }
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(target.liveAllocations(), baseline);
    }
    auto partial = repository.createQueryBuilder();
    partial.select({{"name"}});
    auto result = partial.getMany();
    RUVIA_CHECK(testing::throwsOn([&] { partial.select({{"missing"}}); }));
    RUVIA_CHECK(testing::throwsOn([&] { partial.select({{"id"}, {"id"}}); }));
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_repository_update_builder_owns_complex_conditions_and_cross_table_source) {
    RepositoryRuntime runtime;
    test::CountingMemoryResource source;
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, {.driver = DbDriver::kPostgreSql});
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    auto update = repository.createUpdateBuilder("i");
    {
        DbExpressions x(&source);
        update.updateFrom<Entity>("incoming")
            .set("name", x.column("name", "incoming"))
            .where(x.binary(x.column("id", "i"), DbBinaryOperator::kEqual, x.column("id", "incoming")))
            .andWhere(x.binary(x.column("name", "i"), DbBinaryOperator::kIsDistinctFrom, x.column("name", "incoming")))
            .returning({{"id"}, {"label", x.column("name", "i")}});
    }
    RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
    const auto statement = update.getQueryAndParameters();
    RUVIA_CHECK_EQ(statement.sql(), "UPDATE \"items\" AS \"i\" SET \"name\" = \"incoming\".\"name\" FROM \"items\" AS \"incoming\" WHERE ((\"i\".\"id\" = \"incoming\".\"id\") AND (\"i\".\"name\" IS DISTINCT FROM \"incoming\".\"name\")) RETURNING \"i\".\"id\" AS \"id\", \"i\".\"name\" AS \"label\"");
    auto mapped = update.getMany<ItemSummary>();
    RUVIA_CHECK(testing::throwsOn([&] { (void)update.execute(); }));
    DbExpressions x;
    Entity patch;
    patch.set<"name">("next");
    const auto condition = x.binary(x.column("name"), DbBinaryOperator::kIsDistinctFrom, x.value("next"));
    auto ordinary = repository.update(condition, patch);
    auto returned = repository.updateReturning(condition, patch);
    auto expressionUpdate = repository.update(condition, {{"name", x.value("next")}});
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.update(DbExpression{}, patch); }));
    auto unbounded = repository.createUpdateBuilder();
    unbounded.set(patch);
    RUVIA_CHECK(testing::throwsOn([&] { (void)unbounded.getQueryAndParameters(); }));
    auto predicate = repository.createUpdateBuilder("target");
    predicate.set(patch).where(Entity::column<"id">() == 7);
    const auto predicateStatement = predicate.getQueryAndParameters();
    RUVIA_CHECK(predicateStatement.sql().find("\"target\".\"id\"") != std::string_view::npos);
    auto derived = repository.createUpdateBuilder("i");
    {
        auto sourceQuery = repository.createQueryBuilder("s");
        sourceQuery.where(Entity::column<"id">() > 3);
        derived.updateFrom(sourceQuery, "incoming").set("name", x.column("name", "incoming")).where(x.binary(x.column("id", "i"), DbBinaryOperator::kEqual, x.column("id", "incoming")));
    }
    const auto derivedStatement = derived.getQueryAndParameters();
    RUVIA_CHECK(derivedStatement.sql().find("FROM (SELECT \"s\".\"id\", \"s\".\"name\" FROM \"items\" AS \"s\" WHERE (\"s\".\"id\" > $1)) AS \"incoming\"") != std::string_view::npos);
    auto inserted = repository.createInsertBuilder(patch);
    inserted.returning();
    const auto insertStatement = inserted.getQueryAndParameters();
    RUVIA_CHECK_EQ(insertStatement.sql(), "INSERT INTO \"items\" (\"name\") VALUES ($1) RETURNING \"items\".\"id\", \"items\".\"name\"");
    auto removed = repository.createDeleteBuilder("old");
    removed.where(Entity::column<"id">() == 9).returning();
    auto outer = repository.createQueryBuilder("removed");
    outer.with("removed", removed).fromCte("removed");
    const auto deletion = outer.getQueryAndParameters();
    RUVIA_CHECK(deletion.sql().find("WITH \"removed\" AS (DELETE FROM \"items\" AS \"old\" WHERE (\"old\".\"id\" = $1) RETURNING") == 0);
}

RUVIA_TEST(db_repository_write_ctes_claim_and_persist_in_one_statement) {
    using Archive = DbEntity<"archive", DbColumn<"id", std::int64_t>, DbColumn<"name", std::pmr::string>>;
    RepositoryRuntime runtime;
    test::CountingMemoryResource resource, source;
    detail::DbRegistry registry(runtime.context, runtime.worker, &resource, {.driver = DbDriver::kPostgreSql});
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    auto archive = registry.get(scope).getRepository<Archive>();
    const auto baseline = resource.liveAllocations();
    for (int i = 0; i < 8; ++i) {
        {
            auto insert = archive.createInsertBuilder();
            {
                DbExpressions x(&source);
                auto candidates = repository.createQueryBuilder("c");
                candidates.where(Entity::column<"name">() == "pending").orderBy("id").take(10).setLock({.mode = DbRowLock::kUpdate, .skipLocked = true});
                auto claim = repository.createUpdateBuilder("t");
                claim.updateFromCte("candidates", "c")
                    .set("name", x.value(std::string(300, 'x')))
                    .where(x.binary(x.column("id", "t"), DbBinaryOperator::kEqual, x.column("id", "c")))
                    .returning();
                auto claimed = repository.createQueryBuilder("claimed");
                claimed.fromCte("claimed");
                insert.with("candidates", candidates).with("claimed", claim).insertFrom({"id", "name"}, claimed).returning();
                auto read = repository.createQueryBuilder("result");
                read.with("candidates", candidates).with("claimed", claim).fromCte("claimed");
                RUVIA_CHECK(testing::throwsOn([&] { (void)read.getManyAndCount(); }));
                RUVIA_CHECK(testing::throwsOn([&] { (void)read.getCount(); }));
                RUVIA_CHECK(testing::throwsOn([&] { (void)read.subquery(x); }));
                auto result = read.getMany();
                auto invalid = archive.createInsertBuilder();
                RUVIA_CHECK(testing::throwsOn([&] { invalid.insertFrom({"id"}, claimed); }));
                RUVIA_CHECK(testing::throwsOn([&] { invalid.insertFrom({"id", "unknown"}, claimed); }));
            }
            RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
            const auto statement = insert.getQueryAndParameters();
            RUVIA_CHECK(statement.sql().find("WITH \"candidates\" AS (SELECT") == 0);
            RUVIA_CHECK(statement.sql().find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
            RUVIA_CHECK(statement.sql().find("\"claimed\" AS (UPDATE \"items\" AS \"t\"") != std::string_view::npos);
            RUVIA_CHECK(statement.sql().find("INSERT INTO \"archive\" (\"id\", \"name\") SELECT \"claimed\".\"id\", \"claimed\".\"name\" FROM \"claimed\" AS \"claimed\" RETURNING \"archive\".\"id\", \"archive\".\"name\"") != std::string_view::npos);
            RUVIA_CHECK_EQ(statement.params().size(), std::size_t{3});
            RUVIA_CHECK_EQ(detail::DbValueAccess::text(statement.params()[2]).size(), std::size_t{300});
            auto operation = insert.getMany();
        }
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
}

RUVIA_TEST(db_repository_write_builder_cancelled_operations_release_snapshots) {
    RepositoryRuntime runtime;
    test::CountingMemoryResource resource, input;
    detail::DbRegistry registry(runtime.context, runtime.worker, &resource, {.driver = DbDriver::kPostgreSql});
    detail::ScopedOperationScope scope;
    StopSource stop;
    stop.requestStop();
    auto repository = registry.get(scope).withOptions({.stopToken = stop.token()}).getRepository<Entity>();
    const auto baseline = resource.liveAllocations();
    for (int i = 0; i < 8; ++i) {
        bool cancelled = false;
        auto exercise = [&]() -> Task<void> {
            auto operation = [&] {
                DbExpressions x(&input);
                auto update = repository.createUpdateBuilder();
                update.set("name", x.value(std::string(400, 'v')))
                    .where(x.binary(x.column("id"), DbBinaryOperator::kIsDistinctFrom, x.value(1)))
                    .returning();
                auto outer = repository.createQueryBuilder("updated");
                outer.with("updated", update).fromCte("updated");
                return outer.getMany();
            }();
            RUVIA_CHECK_EQ(input.liveAllocations(), std::size_t{0});
            try {
                auto result = co_await std::move(operation);
            } catch (const DbError& error) {
                cancelled = error.code() == DbError::Code::kCancelled;
            }
        };
        std::exception_ptr failure;
        runtime.context.restart();
        asio::co_spawn(runtime.context, asAwaitable(exercise()), [&](std::exception_ptr error) {
            failure = std::move(error);
            runtime.context.stop();
        });
        runtime.context.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
}

RUVIA_TEST(db_repository_composes_entity_joins_lateral_cte_and_grouped_queries) {
    RepositoryRuntime runtime;
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, {.driver = DbDriver::kPostgreSql});
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    auto builder = repository.createQueryBuilder("i");
    {
        DbExpressions x;
        auto source = repository.createQueryBuilder("s");
        source.where(Entity::column<"id">() > 4);
        builder.with("recent", source, {.materialization = DbMaterialization::kMaterialized});
        builder.joinCte(DbJoinType::kInner, "recent", "r", x.binary(x.column("id", "i"), DbBinaryOperator::kEqual, x.column("id", "r")));
        auto correlated = repository.createQueryBuilder("c");
        correlated.where(x.binary(x.column("id", "c"), DbBinaryOperator::kEqual, x.column("id", "i"))).take(1);
        builder.join(DbJoinType::kLeft, correlated, "l", x.value(true), {.lateral = true});
        builder.join<Entity>(DbJoinType::kCross, "other");
        builder.select({{"id"}, {"rank", x.aggregate("count", {x.star()})}});
        builder.groupBy({x.column("id", "i")}).having(x.binary(x.aggregate("count", {x.star()}), DbBinaryOperator::kGreater, x.value(1)));
        builder.andWhere(source.exists(x));
    }
    const auto statement = builder.getQueryAndParameters();
    RUVIA_CHECK(statement.sql().find("WITH \"recent\" AS MATERIALIZED") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("LEFT JOIN LATERAL") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("CROSS JOIN \"items\" AS \"other\"") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("GROUP BY \"i\".\"id\" HAVING") != std::string_view::npos);
    auto operation = builder.getMany<ItemSummary>();
}

RUVIA_TEST(db_repository_grouped_projection_reuses_imported_parameters) {
    RepositoryRuntime runtime;
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, {.driver = DbDriver::kPostgreSql});
    detail::ScopedOperationScope scope;
    auto builder = registry.get(scope).getRepository<Entity>().createQueryBuilder("i");
    {
        DbExpressions x;
        const auto bucket = x.binary(x.column("id", "i"), DbBinaryOperator::kAdd, x.value(3));
        builder.select({{"id", bucket}}).groupBy({bucket});
        builder.having(x.binary(bucket, DbBinaryOperator::kGreater, x.value(10)));
    }
    const auto statement = builder.getQueryAndParameters();
    RUVIA_CHECK_EQ(statement.sql(), "SELECT (\"i\".\"id\" + $1) AS \"id\" FROM \"items\" AS \"i\" GROUP BY (\"i\".\"id\" + $1) HAVING ((\"i\".\"id\" + $1) > $2)");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{2});
}

RUVIA_TEST(db_repository_expression_writes_and_returning_release_cold_storage) {
    RepositoryRuntime runtime;
    test::CountingMemoryResource resource, source;
    detail::DbRegistry registry(runtime.context, runtime.worker, &resource, {.driver = DbDriver::kPostgreSql});
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    Entity entity;
    entity.set<"id">(8);
    entity.set<"name">("initial");
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.insertReturning<ItemSummary>(entity); }));
    auto where = Entity::column<"id">() == 8;
    const auto baseline = resource.liveAllocations();
    for (int i = 0; i < 12; ++i) {
        {
            auto update = [&] {
                DbExpressions x(&source);
                const std::array changes{DbAssignment{"name", x.call("concat", {x.column("name"), x.value(std::string(400, 'z'))})}};
                const std::array fields{DbSelection{"id"}, DbSelection{"label", x.call("upper", {x.column("name")})}};
                return repository.updateReturning<ItemSummary>(where, changes, fields);
            }();
            RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
            auto insert = repository.insertReturning(entity);
            auto erase = repository.deleteReturning(where);
            auto plainUpdate = repository.updateReturning(where, entity);
            DbExpressions x;
            DbUpsertOptions options{.conflictPaths = {"id"}, .skipUpdateIfNoValuesChanged = true, .updateExpressions = {{"name", x.call("concat", {x.excluded("name"), x.value("suffix")})}}, .updateWhere = x.binary(x.column("id", "items"), DbBinaryOperator::kGreater, x.value(0))};
            auto upsert = repository.upsertReturning(entity, options);
            auto directResult = repository.update(where, {{"name", x.value("replacement")}});
            RUVIA_CHECK(scope.hasPendingOperations());
            RUVIA_CHECK(testing::throwsOn([&] { (void)repository.update(where, {{"missing", x.value(1)}}); }));
            RUVIA_CHECK(testing::throwsOn([&] { (void)repository.update(where, {{"id", x.value(1)}, {"id", x.value(2)}}); }));
            options.updateExpressions.push_back(options.updateExpressions.front());
            RUVIA_CHECK(testing::throwsOn([&] { (void)repository.upsert(entity, options); }));
        }
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
}
#endif

RUVIA_TEST(db_repository_builder_binds_entity_predicates_to_join_alias) {
    RepositoryRuntime runtime;
    const auto config = databaseConfig();
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, config);
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    auto builder = repository.createQueryBuilder("i");
    builder.where(Entity::column<"id">() >= 7).andWhere(Entity::column<"name">() == std::string("temporary"));
    builder.orderBy("id").take(3);
    const auto statement = builder.getQueryAndParameters();
    if (config.driver == DbDriver::kPostgreSql) {
        RUVIA_CHECK_EQ(statement.sql(), "SELECT \"i\".\"id\", \"i\".\"name\" FROM \"items\" AS \"i\" WHERE ((\"i\".\"id\" >= $1) AND (\"i\".\"name\" = $2)) ORDER BY \"i\".\"id\" ASC LIMIT $3");
    } else {
        RUVIA_CHECK(statement.sql().find("WHERE ((`i`.`id` >= ?) AND (`i`.`name` = ?))") != std::string_view::npos);
    }
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{3});
    RUVIA_CHECK_EQ(detail::DbValueAccess::text(statement.params()[1]), "temporary");
}

RUVIA_TEST(db_repository_relation_cold_operations_release_owned_plans_and_arguments) {
    RepositoryRuntime runtime;
    test::CountingMemoryResource resource;
    detail::DbRegistry registry(runtime.context, runtime.worker, &resource, databaseConfig());
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Parent>();
    const auto baseline = resource.liveAllocations();
    for (int i = 0; i < 12; ++i) {
        {
            auto many = repository.find({.relations = {"children"}, .order = {{"id"}}, .take = 1});
            auto one = repository.findOne({.relations = {"children"}});
            auto builder = repository.createQueryBuilder("p");
            builder.leftJoinAndSelect("children", "c").take(1);
            auto getMany = builder.getMany();
            auto getOne = builder.getOne();
            auto count = builder.getCount();
            auto page = builder.getManyAndCount();
            auto exists = builder.getExists();
            RUVIA_CHECK(scope.hasPendingOperations());
        }
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
    scope.close();
    RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    RUVIA_CHECK(resource.deallocationCount() > 0);
}

RUVIA_TEST(db_repository_relation_pagination_keeps_page_alias_safe) {
    RepositoryRuntime runtime;
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, databaseConfig());
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Parent>();
    auto builder = repository.createQueryBuilder("__ruvia_page");
    builder.leftJoinAndSelect("children", "c").take(1);
    const auto statement = builder.getQueryAndParameters();
    const auto sql = statement.sql();
    RUVIA_CHECK(sql.find("__ruvia_page") != std::string_view::npos);
    RUVIA_CHECK(sql.find("__ruvia_page_1") != std::string_view::npos);
}

RUVIA_TEST(db_repository_cold_operations_release_all_operation_allocations) {
    RepositoryRuntime runtime;
    test::CountingMemoryResource resource;
    detail::DbRegistry registry(runtime.context, runtime.worker, &resource, databaseConfig());
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    Entity input;
    input.set<"id">(8);
    input.set<"name">(std::string(400, 'x'));
    auto where = Entity::column<"id">() == 8;
    DbUpsertOptions upsert;
#ifdef RUVIA_ENABLE_POSTGRESQL
    upsert.conflictPaths = {"id"};
#else
    upsert.anyUniqueKey = true;
#endif
    const auto baseline = resource.liveAllocations();
    for (int i = 0; i < 8; ++i) {
        {
            const auto operation = repository.find({.where = Entity::column<"name">() == std::string(400, 'x')});
            RUVIA_CHECK(scope.hasPendingOperations());
        }
        {
            const auto operation = repository.findOne({.where = Entity::column<"id">() == 8});
        }
        {
            const auto operation = repository.count({.where = Entity::column<"id">() == 8});
        }
        {
            const auto operation = repository.exists({.where = Entity::column<"id">() == 8});
        }
        {
            const auto operation = repository.findAndCount({.where = Entity::column<"name">() == std::string(400, 'x'), .take = 1});
        }
        {
            const auto operation = repository.increment(where, "id", 1);
        }
        {
            const auto operation = repository.decrement(where, "id", 1);
        }
        {
            const auto operation = repository.insert(input);
        }
        {
            const auto operation = repository.update(where, input);
        }
        {
            const auto operation = repository.upsert(input, upsert);
        }
        {
            const auto operation = repository.deleteBy(where);
        }
        {
            const auto operation = repository.remove(input);
        }
        {
            auto builder = repository.createQueryBuilder();
            const auto many = builder.getMany();
            const auto one = builder.getOne();
            const auto count = builder.getCount();
        }
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
    RUVIA_CHECK(resource.deallocationCount() > 0);
    auto pendingPage = repository.findAndCount();
    scope.close();
    RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.find(); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.createQueryBuilder(); }));
}

RUVIA_TEST(db_repository_rejects_unbounded_writes_and_missing_primary_key) {
    RepositoryRuntime runtime;
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, databaseConfig());
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    Entity changes;
    changes.set<"name">("new");
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.update({}, changes); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.deleteBy({}); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.remove(changes); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.find({.order = {{"missing"}}}); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.increment({}, "id", 1); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.increment(Entity::column<"id">() == 1, "name", 1); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.decrement(Entity::column<"id">() == 1, "missing", 1); }));
    std::array<Entity, 2> batch;
    batch[0].set<"id">(1);
    batch[0].set<"name">("new");
    batch[1].set<"id">(2);
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.upsert(batch, {.conflictPaths = {"id"}}); }));
    RUVIA_CHECK(!scope.hasPendingOperations());
}

RUVIA_TEST(db_repository_rejects_writes_that_only_target_computed_columns) {
    RepositoryRuntime runtime;
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, databaseConfig());
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<ComputedEntity>();
    ComputedEntity changes;
    changes.set<"name_length">(3);
    auto predicate = ComputedEntity::column<"id">() == 1;
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.update(predicate, changes); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.increment(predicate, "name_length", 1); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)repository.upsert(changes, {.updateColumns = {"name_length"}}); }));

    ComputedEntity input;
    input.set<"id">(1);
    input.set<"name">("Ada");
    input.set<"name_length">(999);
    {
        const auto operation = repository.insert(input);
        RUVIA_CHECK(scope.hasPendingOperations());
    }
    scope.close();
}

RUVIA_TEST(db_repository_conditional_upsert_owns_options_and_releases_cold_storage) {
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        DbDriver::kPostgreSql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        DbDriver::kMariaDb,
#endif
    };
    for (const auto driver : drivers) {
        RepositoryRuntime runtime;
        test::CountingMemoryResource resource;
        const DbConfig config{.driver = driver};
        detail::DbRegistry registry(runtime.context, runtime.worker, &resource, config);
        detail::ScopedOperationScope scope;
        auto repository = registry.get(scope).getRepository<Entity>();
        Entity input;
        input.set<"id">(1);
        input.set<"name">(std::string(400, 'n'));
        const auto baseline = resource.liveAllocations();
        for (int i = 0; i < 12; ++i) {
            if (config.driver == DbDriver::kPostgreSql) {
                {
                    const auto operation = repository.upsert(input, {.conflictPaths = {"id"},
                                                                        .skipUpdateIfNoValuesChanged = true,
                                                                        .indexPredicate = Entity::column<"name">().isNotNull()});
                    RUVIA_CHECK(resource.liveAllocations() > baseline);
                }
            } else {
                RUVIA_CHECK(testing::throwsOn([&] { (void)repository.upsert(input, {.anyUniqueKey = true, .skipUpdateIfNoValuesChanged = true}); }));
                RUVIA_CHECK(testing::throwsOn([&] { (void)repository.upsert(input, {.anyUniqueKey = true, .indexPredicate = Entity::column<"name">().isNotNull()}); }));
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            RUVIA_CHECK(!scope.hasPendingOperations());
        }
        using KeyOnly = DbEntity<"keys", DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>>;
        auto keys = registry.get(scope).getRepository<KeyOnly>();
        KeyOnly key;
        key.set<"id">(1);
        {
            DbUpsertOptions options;
            if (config.driver == DbDriver::kPostgreSql) {
                options.conflictPaths = {"id"};
            } else {
                options.anyUniqueKey = true;
            }
            if (config.driver == DbDriver::kPostgreSql) {
                const auto operation = keys.upsert(key, options);
                RUVIA_CHECK(scope.hasPendingOperations());
            } else {
                RUVIA_CHECK(testing::throwsOn([&] { (void)keys.upsert(key, options); }));
            }
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
}

RUVIA_TEST(db_repository_builder_orders_entity_fields_and_owns_predicates) {
    RepositoryRuntime runtime;
    const auto config = databaseConfig();
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, config);
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    auto builder = repository.createQueryBuilder("i");
    builder.where(Entity::column<"id">() >= 1)
        .andWhere(Entity::column<"name">().isNotNull())
        .orWhere(Entity::column<"id">() == 2)
        .orderBy("name", DbOrderDirection::kDesc, DbNullsOrder::kLast)
        .addOrderBy("id", DbOrderDirection::kAsc, DbNullsOrder::kFirst)
        .skip(2)
        .take(3);
    builder.cache("builder-page", std::chrono::seconds(5));
    const auto statement = builder.getQueryAndParameters();
    const auto sql = statement.sql();
    if (config.driver == DbDriver::kPostgreSql) {
        RUVIA_CHECK(sql.find("ORDER BY \"i\".\"name\" DESC NULLS LAST, \"i\".\"id\" ASC NULLS FIRST") != std::string_view::npos);
    } else {
        RUVIA_CHECK(sql.find("ORDER BY") != std::string_view::npos);
        RUVIA_CHECK(sql.find("LIMIT ? OFFSET ?") != std::string_view::npos);
    }
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{4});
    RUVIA_CHECK(testing::throwsOn([&] { builder.orderBy("unknown"); }));
    RUVIA_CHECK(testing::throwsOn([&] { builder.addOrderBy("unknown"); }));
    // Validation precedes mutation: rejected fields leave the valid plan intact.
    const auto afterRejectedOrder = builder.getQueryAndParameters();
    RUVIA_CHECK_EQ(afterRejectedOrder.sql(), sql);

    auto locked = repository.createQueryBuilder("i");
    locked.setLock({.mode = DbRowLock::kShare, .nowait = true, .tables = {"i"}});
    const auto lockStatement = locked.getQueryAndParameters();
    if (config.driver == DbDriver::kPostgreSql) {
        RUVIA_CHECK(lockStatement.sql().find("FOR SHARE OF \"i\" NOWAIT") != std::string_view::npos);
    } else {
        RUVIA_CHECK(lockStatement.sql().find("LOCK IN SHARE MODE") != std::string_view::npos);
    }
    auto relation = registry.get(scope).getRepository<Parent>().createQueryBuilder("p");
    relation.innerJoinAndSelect("children", "c");
    const auto relationStatement = relation.getQueryAndParameters();
    RUVIA_CHECK(relationStatement.sql().find("INNER JOIN") != std::string_view::npos);
    {
        auto many = builder.getMany();
        auto one = builder.getOne();
        auto count = builder.getCount();
        auto exists = builder.getExists();
        auto page = builder.getManyAndCount();
        Entity patch;
        patch.set<"name">("cold");
        auto write = repository.update(Entity::column<"id">() == 1, patch);
        RUVIA_CHECK(scope.hasPendingOperations());
    }
    RUVIA_CHECK(!scope.hasPendingOperations());
}

RUVIA_TEST(db_repository_write_inputs_cover_default_sparse_nullable_and_composite_keys) {
    RepositoryRuntime runtime;
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, databaseConfig());
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();

    // A single entity with no explicit values uses DEFAULT VALUES. Bulk
    // default-only input is rejected because there is no row width to encode.
    {
        Entity defaults;
        auto operation = repository.insert(defaults);
        RUVIA_CHECK(scope.hasPendingOperations());
    }
    std::array<Entity, 2> sparse;
    sparse[0].set<"id">(1);
    sparse[1].set<"name">("only-name");
    auto sparseInsert = repository.insert(std::span<const Entity>(sparse));
    RUVIA_CHECK(scope.hasPendingOperations());
    RUVIA_CHECK(testing::throwsOn([&] {
        std::array<Entity, 2> defaults;
        (void)repository.insert(std::span<const Entity>(defaults));
    }));

    auto nullableRepository = registry.get(scope).getRepository<NullableEntity>();
    NullableEntity nullable;
    nullable.set<"id">(7);
    nullable.setNull<"name">();
    auto update = nullableRepository.update(NullableEntity::column<"id">() == 7, nullable);
    auto increment = nullableRepository.increment(NullableEntity::column<"id">() == 7, "id", 1.5);
    auto decrement = nullableRepository.decrement(NullableEntity::column<"id">() == 7, "id", 1);
    auto deleted = nullableRepository.deleteBy(NullableEntity::column<"id">() == 7);
    (void)sparseInsert;
    (void)update;
    (void)increment;
    (void)decrement;
    (void)deleted;

    auto composite = registry.get(scope).getRepository<CompositeEntity>();
    CompositeEntity key;
    key.set<"tenant_id">(10);
    key.set<"item_id">(20);
    auto removed = composite.remove(key);
    RUVIA_CHECK(scope.hasPendingOperations());
    CompositeEntity partial;
    partial.set<"tenant_id">(10);
    RUVIA_CHECK(testing::throwsOn([&] { (void)composite.remove(partial); }));
}

RUVIA_TEST(db_repository_upsert_option_families_validate_paths_and_driver_rules) {
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        DbDriver::kPostgreSql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        DbDriver::kMariaDb,
#endif
    };
    for (const auto driver : drivers) {
        RepositoryRuntime runtime;
        detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, DbConfig{.driver = driver});
        detail::ScopedOperationScope scope;
        auto repository = registry.get(scope).getRepository<Entity>();
        Entity input;
        input.set<"id">(1);
        input.set<"name">("Ada");

        DbUpsertOptions options;
        if (driver == DbDriver::kPostgreSql) {
            options.conflictPaths = {"id"};
            options.updateColumns = {"name"};
            options.skipUpdateIfNoValuesChanged = true;
            options.indexPredicate = Entity::column<"name">().isNotNull();
            auto conditional = repository.upsert(input, options);
            RUVIA_CHECK(scope.hasPendingOperations());
            (void)conditional;

            options = {};
            options.conflictPaths = {"id"};
            options.doNothing = true;
            auto nothing = repository.upsert(input, options);
            (void)nothing;
        } else {
            options.anyUniqueKey = true;
            auto anyKey = repository.upsert(input, options);
            (void)anyKey;
            RUVIA_CHECK(testing::throwsOn([&] {
                (void)repository.upsert(input, DbUpsertOptions{.doNothing = true, .anyUniqueKey = true});
            }));
            RUVIA_CHECK(testing::throwsOn([&] {
                (void)repository.upsert(input, DbUpsertOptions{.conflictPaths = {"id"}});
            }));
            RUVIA_CHECK(testing::throwsOn([&] {
                (void)repository.upsert(input, DbUpsertOptions{.anyUniqueKey = true, .skipUpdateIfNoValuesChanged = true});
            }));
        }
        RUVIA_CHECK(testing::throwsOn([&] {
            (void)repository.upsert(input, DbUpsertOptions{.conflictPaths = {"missing"}});
        }));
        RUVIA_CHECK(testing::throwsOn([&] {
            (void)repository.upsert(input, DbUpsertOptions{.updateColumns = {"missing"}});
        }));
        RUVIA_CHECK(!scope.hasPendingOperations());
    }
}

#ifdef RUVIA_ENABLE_REDIS
RUVIA_TEST(db_repository_cached_operations_own_inputs_and_release_cold_storage) {
    RepositoryRuntime runtime;
    test::CountingMemoryResource resource;
    {
        auto config = databaseConfig();
        config.cache.emplace();
        detail::DbRegistry registry(runtime.context, runtime.worker, &resource, config);
        detail::ScopedOperationScope scope;
        auto handle = registry.get(scope);
        auto cache = handle.queryResultCache();
        auto repository = handle.getRepository<Entity>();
        const auto baseline = resource.liveAllocations();
        for (int index = 0; index < 12; ++index) {
            {
                DbFindOptions options{.where = Entity::column<"name">() == std::string(500, 'x'),
                    .cache = DbCacheOptions{.id = std::string(500, 'k'), .milliseconds = std::chrono::seconds(30)}};
                auto page = repository.findAndCount(options);
                auto count = repository.count(options);
                auto one = repository.findOne(options);
                auto builder = repository.createQueryBuilder();
                builder.cache("users", std::chrono::seconds(30));
                auto cached = builder.getMany();
                builder.cache(false);
                auto bypass = builder.getMany();
                const std::array<std::string_view, 2> ids{"users", "users-count"};
                auto remove = cache.remove(ids);
                auto clear = cache.clear();
                options.cache = false;
                RUVIA_CHECK(scope.hasPendingOperations());
            }
            RUVIA_CHECK(!scope.hasPendingOperations());
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
        }
        auto pending = repository.find({.cache = true});
        auto clear = cache.clear();
        scope.close();
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
        RUVIA_CHECK(testing::throwsOn([&] { (void)cache.clear(); }));
        RUVIA_CHECK(testing::throwsOn([&] { (void)handle.queryResultCache(); }));
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}
RUVIA_TEST(db_query_result_cache_cold_drop_and_shutdown_report_typed_failures) {
    RepositoryRuntime runtime;
    test::CountingMemoryResource resource;
    auto config = databaseConfig();
    config.cache.emplace();
    detail::DbRegistry registry(runtime.context, runtime.worker, &resource, config);
    detail::ScopedOperationScope scope;
    auto handle = registry.get(scope);
    auto cache = handle.queryResultCache();
    const auto baseline = resource.liveAllocations();

    {
        auto cold = cache.clear();
        RUVIA_CHECK(scope.hasPendingOperations());
    }
    RUVIA_CHECK(!scope.hasPendingOperations());
    RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);

    const std::array<std::string_view, 1> emptyId{""};
    bool invalidIdentifier = false;
    try {
        runVoid([&]() -> Task<void> {
            auto operation = cache.remove(emptyId);
            co_await std::move(operation);
        }());
    } catch (const std::invalid_argument&) {
        invalidIdentifier = true;
    }
    RUVIA_CHECK(invalidIdentifier);
    RUVIA_CHECK(!scope.hasPendingOperations());
    RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);

    registry.closeNow();
    bool closing = false;
    try {
        runVoid([&]() -> Task<void> {
            auto operation = cache.clear();
            co_await std::move(operation);
        }());
    } catch (const RedisError& error) {
        closing = error.code() == RedisError::Code::kClosing;
    }
    RUVIA_CHECK(closing);
    RUVIA_CHECK(!scope.hasPendingOperations());
    RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
}
RUVIA_TEST(db_cache_policy_snapshots_settings_and_bypasses_writes_and_locks) {
    RepositoryRuntime runtime;
    auto* resource = std::pmr::get_default_resource();
    DbCacheConfig config{.alwaysEnabled = true};
    detail::DbQueryCacheState cache(runtime.context, runtime.worker, detail::DbCacheConfigStorage(config, resource), resource);
    DbQuery query;
    query.select(query.column("id")).from("items");
    const auto driver = databaseConfig().driver;
    auto statement = query.compile(driver, resource);
    RUVIA_CHECK(cache.key(query, statement, driver).has_value());
    query.cache(false);
    RUVIA_CHECK(!cache.key(query, statement, driver));
    std::string id(500, 'x');
    query.cache(std::string_view(id), std::chrono::seconds(30));
    auto copied = query.clone(resource);
    query.cache(false);
    id.assign(500, 'y');
    RUVIA_CHECK_EQ(*cache.key(copied, statement, driver), detail::dbCacheKey(config.nameSpace, std::string(500, 'x'), {}, {}, driver, resource));
    copied.lock({});
    RUVIA_CHECK(!cache.key(copied, statement, driver));
    DbQuery write;
    write.update("items").set("name", write.value("changed"));
    write.returning({write.column("id")});
    RUVIA_CHECK(!cache.key(write, statement, driver));
    DbQuery withWrite;
    withWrite.with("changed", write).select(withWrite.column("id")).from("changed");
    RUVIA_CHECK(!cache.key(withWrite, statement, driver));
    RUVIA_CHECK(testing::throwsOn([&] { query.cache(std::chrono::milliseconds(0)); }));
    RUVIA_CHECK(testing::throwsOn([&] { query.cache("id", std::chrono::milliseconds(-1)); }));
    auto settings = databaseConfig();
    settings.cache.emplace();
    settings.cache->duration = std::chrono::milliseconds(0);
    RUVIA_CHECK(testing::throwsOn([&] { detail::DbConfigStorage stored(settings, resource); }));
    settings.cache->duration = std::chrono::seconds(1);
    settings.cache->nameSpace.clear();
    RUVIA_CHECK(testing::throwsOn([&] { detail::DbConfigStorage stored(settings, resource); }));
}
#endif

}  // namespace
