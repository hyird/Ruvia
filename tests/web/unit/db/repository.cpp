#include <memory>
#include <memory_resource>
#include <optional>
#include <string>

#include <asio/io_context.hpp>

#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/db/DbRepository.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;
using Entity = DbEntity<"items", DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"name", std::pmr::string>>;
using ComputedEntity = DbEntity<"computed_items",
    DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"name", std::pmr::string>,
    DbColumn<"name_length", std::int64_t, DbColumnOptions{.generatedType = DbGeneratedType::kStored}>>;

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
    RepositoryRuntime()
        : dispatcher(std::make_shared<detail::WorkerDispatcher>(context, 8)),
          worker(detail::WorkerHandleAccess::make(dispatcher)) {}
    asio::io_context context;
    std::shared_ptr<detail::WorkerDispatcher> dispatcher;
    WorkerHandle worker;
};
DbConfig databaseConfig() {
#ifdef RUVIA_ENABLE_POSTGRESQL
    return {.driver = DbDriver::kPostgreSql};
#else
    return {.driver = DbDriver::kMariaDb};
#endif
}

RUVIA_TEST(db_repository_builder_binds_entity_predicates_to_join_alias) {
    RepositoryRuntime runtime;
    const auto config = databaseConfig();
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, config);
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Entity>();
    auto builder = repository.createQueryBuilder("i");
    auto& query = builder.statement();
    builder.leftJoin("owners", "o", query.binary(builder.column<"id">(), DbBinaryOperator::kEqual, query.column("id", "o")));
    builder.where(Entity::column<"id">() >= 7).andWhere(Entity::column<"name">() == std::string("temporary"));
    builder.orderBy(builder.column<"id">()).take(3);
    const auto statement = builder.getQueryAndParameters();
    if (config.driver == DbDriver::kPostgreSql) {
        RUVIA_CHECK_EQ(statement.sql(), "SELECT \"i\".\"id\", \"i\".\"name\" FROM \"items\" AS \"i\" LEFT JOIN \"owners\" AS \"o\" ON (\"i\".\"id\" = \"o\".\"id\") WHERE ((\"i\".\"id\" >= $1) AND (\"i\".\"name\" = $2)) ORDER BY \"i\".\"id\" ASC LIMIT $3");
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
            auto raw = builder.getRawMany();
            auto count = builder.getCount();
            RUVIA_CHECK(scope.hasPendingOperations());
        }
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
    scope.close();
    RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    RUVIA_CHECK(resource.deallocationCount() > 0);
}

RUVIA_TEST(db_repository_relation_pagination_keeps_cte_single_and_page_alias_safe) {
    RepositoryRuntime runtime;
    detail::DbRegistry registry(runtime.context, runtime.worker, nullptr, databaseConfig());
    detail::ScopedOperationScope scope;
    auto repository = registry.get(scope).getRepository<Parent>();
    auto builder = repository.createQueryBuilder("__ruvia_page");
    DbQuery cte;
    cte.select(cte.column("id", "source")).from("parents", "source");
    builder.addCommonTableExpression("parent_source", cte);
    builder.leftJoinAndSelect("children", "c").take(1);
    const auto statement = builder.getQueryAndParameters();
    const auto sql = statement.sql();
    RUVIA_CHECK_EQ(sql.find("WITH"), sql.rfind("WITH"));
    RUVIA_CHECK(sql.find("parent_source") != std::string_view::npos);
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
            const auto operation = repository.count(where);
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
            const auto raw = builder.getRawMany();
            const auto count = builder.getCount();
        }
        RUVIA_CHECK(!scope.hasPendingOperations());
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
    }
    RUVIA_CHECK(resource.deallocationCount() > 0);
    scope.close();
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

}  // namespace
