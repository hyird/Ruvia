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

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "db/db_query_cache.h"
#include "db/db_query_cache_state.h"
#include "db/db_registry.h"
#ifdef RUVIA_ENABLE_REDIS
#include "redis/redis_client_runtime.h"
#include "redis/redis_registry.h"
#endif

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using namespace ruvia;
using entity_type = db_entity<"items", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::pmr::string>>;
using nullable_entity_type = db_entity<"nullable_items",
    db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::pmr::string, db_column_options{.nullable_ = true}>>;
using computed_entity_type = db_entity<"computed_items",
    db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::pmr::string>,
    db_column<"name_length", std::int64_t, db_column_options{.generated_type_ = db_generated_type::stored}>>;
using composite_entity_type = db_entity<"composite_items",
    db_column<"tenant_id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"item_id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::pmr::string>,
    db_column<"revision", std::int64_t>>;

struct child;
struct parent;
RUVIA_DB_ENTITY(parent, "parents", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_one_to_many<"children", child, "parent">)
RUVIA_DB_ENTITY(child, "children", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"parent_id", std::int64_t>,
    db_many_to_one<"parent", parent, db_join_column<"parent_id", "id">>)

struct repository_runtime final {
    asio::io_context& context_ = test::new_test_io_context();
    event_loop_attachment attachment_ = attach_event_loop(context_, {.queue_capacity_ = 8});
    worker_handle worker_ = attachment_.loop().handle();
};
db_config database_config() {
#ifdef RUVIA_ENABLE_POSTGRESQL
    return {.driver_ = db_driver::postgresql};
#else
    return {.driver_ = db_driver::mariadb};
#endif
}
void run_void(task<void> task_value) {
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    std::exception_ptr failure;
    asio::co_spawn(context_value, as_awaitable(std::move(task_value)), [&](std::exception_ptr error) {
        failure = std::move(error);
        context_value.stop();
    });
    context_value.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

RUVIA_DB_PROJECTION(item_summary,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(label, std::pmr::string),
    RUVIA_DB_COLUMN(rank, std::int64_t))

RUVIA_TEST(db_repository_projects_computed_dto_and_owns_expression_sources) {
    repository_runtime runtime;
    test::counting_memory_resource source, target;
    detail::db_registry registry(runtime.context_, runtime.worker_, &target, database_config());
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    const auto baseline = target.live_allocations();
    for (int i = 0; i < 8; ++i) {
        {
            auto builder = repository.create_query_builder("i");
            {
                db_expressions expressions(&source);
                auto label = expressions.call("concat", {expressions.column("name", "i"), expressions.value(std::string(200, 'x'))});
                auto rank = expressions.over(expressions.call("row_number"), {.order_by_ = {{expressions.column("id", "i")}}});
                builder.select({{"id"}, {"label", label}, {"rank", rank}});
            }
            RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
            const auto statement = builder.get_query_and_parameters();
            RUVIA_CHECK(statement.sql().find(database_config().driver_ == db_driver::postgresql ? "\"row_number\"() OVER (ORDER BY" : "row_number() OVER (ORDER BY") != std::string_view::npos);
            RUVIA_CHECK_EQ(statement.params().size(), std::size_t{1});
            RUVIA_CHECK_EQ(detail::db_value_access::text(statement.params()[0]).size(), std::size_t{200});
            auto many = builder.get_many<item_summary>();
            auto one = builder.get_one<item_summary>();
            auto page = builder.get_many_and_count<item_summary>();
            RUVIA_CHECK(testing::throws_on([&] { (void)builder.get_many(); }));
        }
        RUVIA_CHECK(!scope.has_pending_operations());
        RUVIA_CHECK_EQ(target.live_allocations(), baseline);
    }
    auto partial = repository.create_query_builder();
    partial.select({{"name"}});
    auto result_value = partial.get_many();
    RUVIA_CHECK(testing::throws_on([&] { partial.select({{"missing"}}); }));
    RUVIA_CHECK(testing::throws_on([&] { partial.select({{"id"}, {"id"}}); }));
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_repository_update_builder_owns_complex_conditions_and_cross_table_source) {
    repository_runtime runtime;
    test::counting_memory_resource source;
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, {.driver_ = db_driver::postgresql});
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    auto update = repository.create_update_builder("i");
    {
        db_expressions x(&source);
        update.update_from<entity_type>("incoming")
            .set("name", x.column("name", "incoming"))
            .where(x.binary(x.column("id", "i"), db_binary_operator::equal, x.column("id", "incoming")))
            .and_where(x.binary(x.column("name", "i"), db_binary_operator::is_distinct_from, x.column("name", "incoming")))
            .returning({{"id"}, {"label", x.column("name", "i")}});
    }
    RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
    const auto statement = update.get_query_and_parameters();
    RUVIA_CHECK_EQ(statement.sql(), "UPDATE \"items\" AS \"i\" SET \"name\" = \"incoming\".\"name\" FROM \"items\" AS \"incoming\" WHERE ((\"i\".\"id\" = \"incoming\".\"id\") AND (\"i\".\"name\" IS DISTINCT FROM \"incoming\".\"name\")) RETURNING \"i\".\"id\" AS \"id\", \"i\".\"name\" AS \"label\"");
    auto mapped = update.get_many<item_summary>();
    RUVIA_CHECK(testing::throws_on([&] { (void)update.execute(); }));
    db_expressions x;
    entity_type patch;
    patch.set<"name">("next");
    const auto condition = x.binary(x.column("name"), db_binary_operator::is_distinct_from, x.value("next"));
    auto ordinary = repository.update(condition, patch);
    auto returned = repository.update_returning(condition, patch);
    auto expression_update = repository.update(condition, {{"name", x.value("next")}});
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.update(db_expression{}, patch); }));
    auto unbounded = repository.create_update_builder();
    unbounded.set(patch);
    RUVIA_CHECK(testing::throws_on([&] { (void)unbounded.get_query_and_parameters(); }));
    auto predicate = repository.create_update_builder("target");
    predicate.set(patch).where(entity_type::column<"id">() == 7);
    const auto predicate_statement = predicate.get_query_and_parameters();
    RUVIA_CHECK(predicate_statement.sql().find("\"target\".\"id\"") != std::string_view::npos);
    auto derived = repository.create_update_builder("i");
    {
        auto source_query = repository.create_query_builder("s");
        source_query.where(entity_type::column<"id">() > 3);
        derived.update_from(source_query, "incoming").set("name", x.column("name", "incoming")).where(x.binary(x.column("id", "i"), db_binary_operator::equal, x.column("id", "incoming")));
    }
    const auto derived_statement = derived.get_query_and_parameters();
    RUVIA_CHECK(derived_statement.sql().find("FROM (SELECT \"s\".\"id\", \"s\".\"name\" FROM \"items\" AS \"s\" WHERE (\"s\".\"id\" > $1)) AS \"incoming\"") != std::string_view::npos);
    auto inserted = repository.create_insert_builder(patch);
    inserted.returning();
    const auto insert_statement = inserted.get_query_and_parameters();
    RUVIA_CHECK_EQ(insert_statement.sql(), "INSERT INTO \"items\" (\"name\") VALUES ($1) RETURNING \"items\".\"id\", \"items\".\"name\"");
    auto removed = repository.create_delete_builder("old");
    removed.where(entity_type::column<"id">() == 9).returning();
    auto outer = repository.create_query_builder("removed");
    outer.with("removed", removed).from_cte("removed");
    const auto deletion = outer.get_query_and_parameters();
    RUVIA_CHECK(deletion.sql().find("WITH \"removed\" AS (DELETE FROM \"items\" AS \"old\" WHERE (\"old\".\"id\" = $1) RETURNING") == 0);
}

RUVIA_TEST(db_repository_write_ctes_claim_and_persist_in_one_statement) {
    using archive_type = db_entity<"archive", db_column<"id", std::int64_t>, db_column<"name", std::pmr::string>>;
    repository_runtime runtime;
    test::counting_memory_resource resource, source;
    detail::db_registry registry(runtime.context_, runtime.worker_, &resource, {.driver_ = db_driver::postgresql});
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    auto archive = registry.get(scope).get_repository<archive_type>();
    const auto baseline = resource.live_allocations();
    for (int i = 0; i < 8; ++i) {
        {
            auto insert = archive.create_insert_builder();
            {
                db_expressions x(&source);
                auto candidates = repository.create_query_builder("c");
                candidates.where(entity_type::column<"name">() == "pending").order_by("id").take(10).set_lock({.mode_ = db_row_lock::update, .skip_locked_ = true});
                auto claim = repository.create_update_builder("t");
                claim.update_from_cte("candidates", "c")
                    .set("name", x.value(std::string(300, 'x')))
                    .where(x.binary(x.column("id", "t"), db_binary_operator::equal, x.column("id", "c")))
                    .returning();
                auto claimed = repository.create_query_builder("claimed");
                claimed.from_cte("claimed");
                insert.with("candidates", candidates).with("claimed", claim).insert_from({"id", "name"}, claimed).returning();
                auto read = repository.create_query_builder("result");
                read.with("candidates", candidates).with("claimed", claim).from_cte("claimed");
                RUVIA_CHECK(testing::throws_on([&] { (void)read.get_many_and_count(); }));
                RUVIA_CHECK(testing::throws_on([&] { (void)read.get_count(); }));
                RUVIA_CHECK(testing::throws_on([&] { (void)read.subquery(x); }));
                auto result_value = read.get_many();
                auto invalid = archive.create_insert_builder();
                RUVIA_CHECK(testing::throws_on([&] { invalid.insert_from({"id"}, claimed); }));
                RUVIA_CHECK(testing::throws_on([&] { invalid.insert_from({"id", "unknown"}, claimed); }));
            }
            RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
            const auto statement = insert.get_query_and_parameters();
            RUVIA_CHECK(statement.sql().find("WITH \"candidates\" AS (SELECT") == 0);
            RUVIA_CHECK(statement.sql().find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
            RUVIA_CHECK(statement.sql().find("\"claimed\" AS (UPDATE \"items\" AS \"t\"") != std::string_view::npos);
            RUVIA_CHECK(statement.sql().find("INSERT INTO \"archive\" (\"id\", \"name\") SELECT \"claimed\".\"id\", \"claimed\".\"name\" FROM \"claimed\" AS \"claimed\" RETURNING \"archive\".\"id\", \"archive\".\"name\"") != std::string_view::npos);
            RUVIA_CHECK_EQ(statement.params().size(), std::size_t{3});
            RUVIA_CHECK_EQ(detail::db_value_access::text(statement.params()[2]).size(), std::size_t{300});
            auto operation = insert.get_many();
        }
        RUVIA_CHECK(!scope.has_pending_operations());
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    }
}

RUVIA_TEST(db_repository_write_builder_cancelled_operations_release_snapshots) {
    repository_runtime runtime;
    test::counting_memory_resource resource, input;
    detail::db_registry registry(runtime.context_, runtime.worker_, &resource, {.driver_ = db_driver::postgresql});
    ::ruvia::operation_scope scope;
    stop_source stop;
    stop.request_stop();
    auto repository = registry.get(scope).with_options({.stop_token_ = stop.token()}).get_repository<entity_type>();
    const auto baseline = resource.live_allocations();
    for (int i = 0; i < 8; ++i) {
        bool cancelled = false;
        auto exercise = [&]() -> task<void> {
            auto operation = [&] {
                db_expressions x(&input);
                auto update = repository.create_update_builder();
                update.set("name", x.value(std::string(400, 'v')))
                    .where(x.binary(x.column("id"), db_binary_operator::is_distinct_from, x.value(1)))
                    .returning();
                auto outer = repository.create_query_builder("updated");
                outer.with("updated", update).from_cte("updated");
                return outer.get_many();
            }();
            RUVIA_CHECK_EQ(input.live_allocations(), std::size_t{0});
            try {
                auto result_value = co_await std::move(operation);
            } catch (const db_error& error) {
                cancelled = error.code() == db_error::code_type::cancelled;
            }
        };
        std::exception_ptr failure;
        runtime.context_.restart();
        asio::co_spawn(runtime.context_, as_awaitable(exercise()), [&](std::exception_ptr error) {
            failure = std::move(error);
            runtime.context_.stop();
        });
        runtime.context_.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK(!scope.has_pending_operations());
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    }
}

RUVIA_TEST(db_repository_composes_entity_joins_lateral_cte_and_grouped_queries) {
    repository_runtime runtime;
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, {.driver_ = db_driver::postgresql});
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    auto builder = repository.create_query_builder("i");
    {
        db_expressions x;
        auto source_value = repository.create_query_builder("s");
        source_value.where(entity_type::column<"id">() > 4);
        builder.with("recent", source_value, {.materialization_ = db_materialization::materialized});
        builder.join_cte(db_join_type::inner, "recent", "r", x.binary(x.column("id", "i"), db_binary_operator::equal, x.column("id", "r")));
        auto correlated = repository.create_query_builder("c");
        correlated.where(x.binary(x.column("id", "c"), db_binary_operator::equal, x.column("id", "i"))).take(1);
        builder.join(db_join_type::left, correlated, "l", x.value(true), {.lateral_ = true});
        builder.join<entity_type>(db_join_type::cross, "other");
        builder.select({{"id"}, {"rank", x.aggregate("count", {x.star()})}});
        builder.group_by({x.column("id", "i")}).having(x.binary(x.aggregate("count", {x.star()}), db_binary_operator::greater, x.value(1)));
        builder.and_where(source_value.exists(x));
    }
    const auto statement = builder.get_query_and_parameters();
    RUVIA_CHECK(statement.sql().find("WITH \"recent\" AS MATERIALIZED") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("LEFT JOIN LATERAL") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("CROSS JOIN \"items\" AS \"other\"") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("GROUP BY \"i\".\"id\" HAVING") != std::string_view::npos);
    auto operation = builder.get_many<item_summary>();
}

RUVIA_TEST(db_repository_grouped_projection_reuses_imported_parameters) {
    repository_runtime runtime;
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, {.driver_ = db_driver::postgresql});
    ::ruvia::operation_scope scope;
    auto builder = registry.get(scope).get_repository<entity_type>().create_query_builder("i");
    {
        db_expressions x;
        const auto bucket = x.binary(x.column("id", "i"), db_binary_operator::add, x.value(3));
        builder.select({{"id", bucket}}).group_by({bucket});
        builder.having(x.binary(bucket, db_binary_operator::greater, x.value(10)));
    }
    const auto statement = builder.get_query_and_parameters();
    RUVIA_CHECK_EQ(statement.sql(), "SELECT (\"i\".\"id\" + $1) AS \"id\" FROM \"items\" AS \"i\" GROUP BY (\"i\".\"id\" + $1) HAVING ((\"i\".\"id\" + $1) > $2)");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{2});
}

RUVIA_TEST(db_repository_expression_writes_and_returning_release_cold_storage) {
    repository_runtime runtime;
    test::counting_memory_resource resource, source;
    detail::db_registry registry(runtime.context_, runtime.worker_, &resource, {.driver_ = db_driver::postgresql});
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    entity_type entity;
    entity.set<"id">(8);
    entity.set<"name">("initial");
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.insert_returning<item_summary>(entity); }));
    auto where = entity_type::column<"id">() == 8;
    const auto baseline = resource.live_allocations();
    for (int i = 0; i < 12; ++i) {
        {
            auto update = [&] {
                db_expressions x(&source);
                const std::array changes{db_assignment{"name", x.call("concat", {x.column("name"), x.value(std::string(400, 'z'))})}};
                const std::array fields_value{db_selection{"id"}, db_selection{"label", x.call("upper", {x.column("name")})}};
                return repository.update_returning<item_summary>(where, changes, fields_value);
            }();
            RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
            auto insert = repository.insert_returning(entity);
            auto erase = repository.delete_returning(where);
            auto plain_update = repository.update_returning(where, entity);
            db_expressions x;
            db_upsert_options options{.conflict_paths_ = {"id"}, .skip_update_if_no_values_changed_ = true, .update_expressions_ = {{"name", x.call("concat", {x.excluded("name"), x.value("suffix")})}}, .update_where_ = x.binary(x.column("id", "items"), db_binary_operator::greater, x.value(0))};
            auto upsert = repository.upsert_returning(entity, options);
            auto direct_result = repository.update(where, {{"name", x.value("replacement")}});
            RUVIA_CHECK(scope.has_pending_operations());
            RUVIA_CHECK(testing::throws_on([&] { (void)repository.update(where, {{"missing", x.value(1)}}); }));
            RUVIA_CHECK(testing::throws_on([&] { (void)repository.update(where, {{"id", x.value(1)}, {"id", x.value(2)}}); }));
            options.update_expressions_.push_back(options.update_expressions_.front());
            RUVIA_CHECK(testing::throws_on([&] { (void)repository.upsert(entity, options); }));
        }
        RUVIA_CHECK(!scope.has_pending_operations());
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    }
}
#endif

RUVIA_TEST(db_repository_builder_binds_entity_predicates_to_join_alias) {
    repository_runtime runtime;
    const auto config = database_config();
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, config);
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    auto builder = repository.create_query_builder("i");
    builder.where(entity_type::column<"id">() >= 7).and_where(entity_type::column<"name">() == std::string("temporary"));
    builder.order_by("id").take(3);
    const auto statement = builder.get_query_and_parameters();
    if (config.driver_ == db_driver::postgresql) {
        RUVIA_CHECK_EQ(statement.sql(), "SELECT \"i\".\"id\", \"i\".\"name\" FROM \"items\" AS \"i\" WHERE ((\"i\".\"id\" >= $1) AND (\"i\".\"name\" = $2)) ORDER BY \"i\".\"id\" ASC LIMIT $3");
    } else {
        RUVIA_CHECK(statement.sql().find("WHERE ((`i`.`id` >= ?) AND (`i`.`name` = ?))") != std::string_view::npos);
    }
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{3});
    RUVIA_CHECK_EQ(detail::db_value_access::text(statement.params()[1]), "temporary");
}

RUVIA_TEST(db_repository_relation_cold_operations_release_owned_plans_and_arguments) {
    repository_runtime runtime;
    test::counting_memory_resource resource;
    detail::db_registry registry(runtime.context_, runtime.worker_, &resource, database_config());
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<parent>();
    const auto baseline = resource.live_allocations();
    for (int i = 0; i < 12; ++i) {
        {
            auto many = repository.find({.relations_ = {"children"}, .order_ = {{"id"}}, .take_ = 1});
            auto one = repository.find_one({.relations_ = {"children"}});
            auto builder = repository.create_query_builder("p");
            builder.left_join_and_select("children", "c").take(1);
            auto get_many = builder.get_many();
            auto get_one = builder.get_one();
            auto count = builder.get_count();
            auto page = builder.get_many_and_count();
            auto exists = builder.get_exists();
            RUVIA_CHECK(scope.has_pending_operations());
        }
        RUVIA_CHECK(!scope.has_pending_operations());
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    }
    scope.close();
    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    RUVIA_CHECK(resource.deallocation_count() > 0);
}

RUVIA_TEST(db_repository_relation_pagination_keeps_page_alias_safe) {
    repository_runtime runtime;
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, database_config());
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<parent>();
    auto builder = repository.create_query_builder("__ruvia_page");
    builder.left_join_and_select("children", "c").take(1);
    const auto statement = builder.get_query_and_parameters();
    const auto sql = statement.sql();
    RUVIA_CHECK(sql.find("__ruvia_page") != std::string_view::npos);
    RUVIA_CHECK(sql.find("__ruvia_page_1") != std::string_view::npos);
}

RUVIA_TEST(db_repository_cold_operations_release_all_operation_allocations) {
    repository_runtime runtime;
    test::counting_memory_resource resource;
    detail::db_registry registry(runtime.context_, runtime.worker_, &resource, database_config());
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    entity_type input;
    input.set<"id">(8);
    input.set<"name">(std::string(400, 'x'));
    auto where = entity_type::column<"id">() == 8;
    db_upsert_options upsert;
#ifdef RUVIA_ENABLE_POSTGRESQL
    upsert.conflict_paths_ = {"id"};
#else
    upsert.any_unique_key_ = true;
#endif
    const auto baseline = resource.live_allocations();
    for (int i = 0; i < 8; ++i) {
        {
            const auto operation = repository.find({.where_ = entity_type::column<"name">() == std::string(400, 'x')});
            RUVIA_CHECK(scope.has_pending_operations());
        }
        {
            const auto operation = repository.find_one({.where_ = entity_type::column<"id">() == 8});
        }
        {
            const auto operation = repository.count({.where_ = entity_type::column<"id">() == 8});
        }
        {
            const auto operation = repository.exists({.where_ = entity_type::column<"id">() == 8});
        }
        {
            const auto operation = repository.find_and_count({.where_ = entity_type::column<"name">() == std::string(400, 'x'), .take_ = 1});
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
            const auto operation = repository.delete_by(where);
        }
        {
            const auto operation = repository.remove(input);
        }
        {
            auto builder = repository.create_query_builder();
            const auto many = builder.get_many();
            const auto one = builder.get_one();
            const auto count = builder.get_count();
        }
        RUVIA_CHECK(!scope.has_pending_operations());
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    }
    RUVIA_CHECK(resource.deallocation_count() > 0);
    auto pending_page = repository.find_and_count();
    scope.close();
    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.find(); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.create_query_builder(); }));
}

RUVIA_TEST(db_repository_rejects_unbounded_writes_and_missing_primary_key) {
    repository_runtime runtime;
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, database_config());
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    entity_type changes;
    changes.set<"name">("new");
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.update({}, changes); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.delete_by({}); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.remove(changes); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.find({.order_ = {{"missing"}}}); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.increment({}, "id", 1); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.increment(entity_type::column<"id">() == 1, "name", 1); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.decrement(entity_type::column<"id">() == 1, "missing", 1); }));
    std::array<entity_type, 2> batch;
    batch[0].set<"id">(1);
    batch[0].set<"name">("new");
    batch[1].set<"id">(2);
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.upsert(batch, {.conflict_paths_ = {"id"}}); }));
    RUVIA_CHECK(!scope.has_pending_operations());
}

RUVIA_TEST(db_repository_rejects_writes_that_only_target_computed_columns) {
    repository_runtime runtime;
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, database_config());
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<computed_entity_type>();
    computed_entity_type changes;
    changes.set<"name_length">(3);
    auto predicate = computed_entity_type::column<"id">() == 1;
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.update(predicate, changes); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.increment(predicate, "name_length", 1); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)repository.upsert(changes, {.update_columns_ = {"name_length"}}); }));

    computed_entity_type input;
    input.set<"id">(1);
    input.set<"name">("Ada");
    input.set<"name_length">(999);
    {
        const auto operation = repository.insert(input);
        RUVIA_CHECK(scope.has_pending_operations());
    }
    scope.close();
}

RUVIA_TEST(db_repository_conditional_upsert_owns_options_and_releases_cold_storage) {
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        repository_runtime runtime;
        test::counting_memory_resource resource;
        const db_config config{.driver_ = driver};
        detail::db_registry registry(runtime.context_, runtime.worker_, &resource, config);
        ::ruvia::operation_scope scope;
        auto repository = registry.get(scope).get_repository<entity_type>();
        entity_type input;
        input.set<"id">(1);
        input.set<"name">(std::string(400, 'n'));
        const auto baseline = resource.live_allocations();
        for (int i = 0; i < 12; ++i) {
            if (config.driver_ == db_driver::postgresql) {
                {
                    const auto operation = repository.upsert(input, {.conflict_paths_ = {"id"},
                                                                        .skip_update_if_no_values_changed_ = true,
                                                                        .index_predicate_ = entity_type::column<"name">().is_not_null()});
                    RUVIA_CHECK(resource.live_allocations() > baseline);
                }
            } else {
                RUVIA_CHECK(testing::throws_on([&] { (void)repository.upsert(input, {.any_unique_key_ = true, .skip_update_if_no_values_changed_ = true}); }));
                RUVIA_CHECK(testing::throws_on([&] { (void)repository.upsert(input, {.any_unique_key_ = true, .index_predicate_ = entity_type::column<"name">().is_not_null()}); }));
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            RUVIA_CHECK(!scope.has_pending_operations());
        }
        using key_only_type = db_entity<"keys", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>>;
        auto keys = registry.get(scope).get_repository<key_only_type>();
        key_only_type key;
        key.set<"id">(1);
        {
            db_upsert_options options;
            if (config.driver_ == db_driver::postgresql) {
                options.conflict_paths_ = {"id"};
            } else {
                options.any_unique_key_ = true;
            }
            if (config.driver_ == db_driver::postgresql) {
                const auto operation = keys.upsert(key, options);
                RUVIA_CHECK(scope.has_pending_operations());
            } else {
                RUVIA_CHECK(testing::throws_on([&] { (void)keys.upsert(key, options); }));
            }
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    }
}

RUVIA_TEST(db_repository_builder_orders_entity_fields_and_owns_predicates) {
    repository_runtime runtime;
    const auto config = database_config();
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, config);
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();
    auto builder = repository.create_query_builder("i");
    builder.where(entity_type::column<"id">() >= 1)
        .and_where(entity_type::column<"name">().is_not_null())
        .or_where(entity_type::column<"id">() == 2)
        .order_by("name", db_order_direction::desc, db_nulls_order::last)
        .add_order_by("id", db_order_direction::asc, db_nulls_order::first)
        .skip(2)
        .take(3);
    builder.cache("builder-page", std::chrono::seconds(5));
    const auto statement = builder.get_query_and_parameters();
    const auto sql = statement.sql();
    if (config.driver_ == db_driver::postgresql) {
        RUVIA_CHECK(sql.find("ORDER BY \"i\".\"name\" DESC NULLS LAST, \"i\".\"id\" ASC NULLS FIRST") != std::string_view::npos);
    } else {
        RUVIA_CHECK(sql.find("ORDER BY") != std::string_view::npos);
        RUVIA_CHECK(sql.find("LIMIT ? OFFSET ?") != std::string_view::npos);
    }
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{4});
    RUVIA_CHECK(testing::throws_on([&] { builder.order_by("unknown"); }));
    RUVIA_CHECK(testing::throws_on([&] { builder.add_order_by("unknown"); }));
    // Validation precedes mutation: rejected fields leave the valid plan intact.
    const auto after_rejected_order = builder.get_query_and_parameters();
    RUVIA_CHECK_EQ(after_rejected_order.sql(), sql);

    auto locked = repository.create_query_builder("i");
    locked.set_lock({.mode_ = db_row_lock::share, .nowait_ = true, .tables_ = {"i"}});
    const auto lock_statement = locked.get_query_and_parameters();
    if (config.driver_ == db_driver::postgresql) {
        RUVIA_CHECK(lock_statement.sql().find("FOR SHARE OF \"i\" NOWAIT") != std::string_view::npos);
    } else {
        RUVIA_CHECK(lock_statement.sql().find("LOCK IN SHARE MODE") != std::string_view::npos);
    }
    auto relation = registry.get(scope).get_repository<parent>().create_query_builder("p");
    relation.inner_join_and_select("children", "c");
    const auto relation_statement = relation.get_query_and_parameters();
    RUVIA_CHECK(relation_statement.sql().find("INNER JOIN") != std::string_view::npos);
    {
        auto many = builder.get_many();
        auto one = builder.get_one();
        auto count = builder.get_count();
        auto exists = builder.get_exists();
        auto page = builder.get_many_and_count();
        entity_type patch;
        patch.set<"name">("cold");
        auto write = repository.update(entity_type::column<"id">() == 1, patch);
        RUVIA_CHECK(scope.has_pending_operations());
    }
    RUVIA_CHECK(!scope.has_pending_operations());
}

RUVIA_TEST(db_repository_write_inputs_cover_default_sparse_nullable_and_composite_keys) {
    repository_runtime runtime;
    detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, database_config());
    ::ruvia::operation_scope scope;
    auto repository = registry.get(scope).get_repository<entity_type>();

    // A single entity with no explicit values uses DEFAULT VALUES. Bulk
    // default-only input is rejected because there is no row width to encode.
    {
        entity_type defaults;
        auto operation = repository.insert(defaults);
        RUVIA_CHECK(scope.has_pending_operations());
    }
    std::array<entity_type, 2> sparse;
    sparse[0].set<"id">(1);
    sparse[1].set<"name">("only-name");
    auto sparse_insert = repository.insert(std::span<const entity_type>(sparse));
    RUVIA_CHECK(scope.has_pending_operations());
    RUVIA_CHECK(testing::throws_on([&] {
        std::array<entity_type, 2> defaults;
        (void)repository.insert(std::span<const entity_type>(defaults));
    }));

    auto nullable_repository = registry.get(scope).get_repository<nullable_entity_type>();
    nullable_entity_type nullable;
    nullable.set<"id">(7);
    nullable.set_null<"name">();
    auto update = nullable_repository.update(nullable_entity_type::column<"id">() == 7, nullable);
    auto increment = nullable_repository.increment(nullable_entity_type::column<"id">() == 7, "id", 1.5);
    auto decrement = nullable_repository.decrement(nullable_entity_type::column<"id">() == 7, "id", 1);
    auto deleted = nullable_repository.delete_by(nullable_entity_type::column<"id">() == 7);
    (void)sparse_insert;
    (void)update;
    (void)increment;
    (void)decrement;
    (void)deleted;

    auto composite = registry.get(scope).get_repository<composite_entity_type>();
    composite_entity_type key;
    key.set<"tenant_id">(10);
    key.set<"item_id">(20);
    auto removed = composite.remove(key);
    RUVIA_CHECK(scope.has_pending_operations());
    composite_entity_type partial;
    partial.set<"tenant_id">(10);
    RUVIA_CHECK(testing::throws_on([&] { (void)composite.remove(partial); }));
}

RUVIA_TEST(db_repository_upsert_option_families_validate_paths_and_driver_rules) {
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        repository_runtime runtime;
        detail::db_registry registry(runtime.context_, runtime.worker_, nullptr, db_config{.driver_ = driver});
        ::ruvia::operation_scope scope;
        auto repository = registry.get(scope).get_repository<entity_type>();
        entity_type input;
        input.set<"id">(1);
        input.set<"name">("Ada");

        db_upsert_options options;
        if (driver == db_driver::postgresql) {
            options.conflict_paths_ = {"id"};
            options.update_columns_ = {"name"};
            options.skip_update_if_no_values_changed_ = true;
            options.index_predicate_ = entity_type::column<"name">().is_not_null();
            auto conditional = repository.upsert(input, options);
            RUVIA_CHECK(scope.has_pending_operations());
            (void)conditional;

            options = {};
            options.conflict_paths_ = {"id"};
            options.do_nothing_ = true;
            auto nothing = repository.upsert(input, options);
            (void)nothing;
        } else {
            options.any_unique_key_ = true;
            auto any_key = repository.upsert(input, options);
            (void)any_key;
            RUVIA_CHECK(testing::throws_on([&] {
                (void)repository.upsert(input, db_upsert_options{.do_nothing_ = true, .any_unique_key_ = true});
            }));
            RUVIA_CHECK(testing::throws_on([&] {
                (void)repository.upsert(input, db_upsert_options{.conflict_paths_ = {"id"}});
            }));
            RUVIA_CHECK(testing::throws_on([&] {
                (void)repository.upsert(input, db_upsert_options{.any_unique_key_ = true, .skip_update_if_no_values_changed_ = true});
            }));
        }
        RUVIA_CHECK(testing::throws_on([&] {
            (void)repository.upsert(input, db_upsert_options{.conflict_paths_ = {"missing"}});
        }));
        RUVIA_CHECK(testing::throws_on([&] {
            (void)repository.upsert(input, db_upsert_options{.update_columns_ = {"missing"}});
        }));
        RUVIA_CHECK(!scope.has_pending_operations());
    }
}

#ifdef RUVIA_ENABLE_REDIS
RUVIA_TEST(db_repository_cached_operations_own_inputs_and_release_cold_storage) {
    repository_runtime runtime;
    test::counting_memory_resource resource;
    {
        auto config = database_config();
        detail::redis_client_runtime redis(runtime.context_, runtime.worker_,
            detail::redis_config_storage(redis_config{}, &resource), &resource);
        ::ruvia::operation_scope redis_scope;
        auto store_value = redis.handle(redis_scope);
        detail::db_registry registry(runtime.context_, runtime.worker_, &resource, config, store_value, db_cache_config{});
        ::ruvia::operation_scope scope;
        auto handle = registry.get(scope);
        auto cache = handle.query_result_cache();
        auto repository = handle.get_repository<entity_type>();
        const auto baseline = resource.live_allocations();
        for (int index = 0; index < 12; ++index) {
            {
                db_find_options options{.where_ = entity_type::column<"name">() == std::string(500, 'x'),
                    .cache_ = db_cache_options{.id_ = std::string(500, 'k'), .milliseconds_ = std::chrono::seconds(30)}};
                auto page = repository.find_and_count(options);
                auto count = repository.count(options);
                auto one = repository.find_one(options);
                auto builder = repository.create_query_builder();
                builder.cache("users", std::chrono::seconds(30));
                auto cached = builder.get_many();
                builder.cache(false);
                auto bypass = builder.get_many();
                const std::array<std::string_view, 2> ids{"users", "users-count"};
                auto remove = cache.remove(ids);
                auto clear = cache.clear();
                options.cache_ = false;
                RUVIA_CHECK(scope.has_pending_operations());
            }
            RUVIA_CHECK(!scope.has_pending_operations());
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
        }
        auto pending = repository.find({.cache_ = true});
        auto clear = cache.clear();
        scope.close();
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
        RUVIA_CHECK(testing::throws_on([&] { (void)cache.clear(); }));
        RUVIA_CHECK(testing::throws_on([&] { (void)handle.query_result_cache(); }));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}
RUVIA_TEST(db_query_result_cache_cold_drop_and_shutdown_report_typed_failures) {
    repository_runtime runtime;
    test::counting_memory_resource resource;
    auto config = database_config();
    detail::redis_client_runtime redis(runtime.context_, runtime.worker_,
        detail::redis_config_storage(redis_config{}, &resource), &resource);
    ::ruvia::operation_scope redis_scope;
    auto store_value = redis.handle(redis_scope);
    detail::db_registry registry(runtime.context_, runtime.worker_, &resource, config, store_value, db_cache_config{});
    ::ruvia::operation_scope scope;
    auto handle = registry.get(scope);
    auto cache = handle.query_result_cache();
    const auto baseline = resource.live_allocations();

    {
        auto cold = cache.clear();
        RUVIA_CHECK(scope.has_pending_operations());
    }
    RUVIA_CHECK(!scope.has_pending_operations());
    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);

    const std::array<std::string_view, 1> empty_id{""};
    bool invalid_identifier = false;
    try {
        run_void([&]() -> task<void> {
            auto operation = cache.remove(empty_id);
            co_await std::move(operation);
        }());
    } catch (const std::invalid_argument&) {
        invalid_identifier = true;
    }
    RUVIA_CHECK(invalid_identifier);
    RUVIA_CHECK(!scope.has_pending_operations());
    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);

    registry.close_now();
    bool closing = false;
    try {
        run_void([&]() -> task<void> {
            auto operation = cache.clear();
            co_await std::move(operation);
        }());
    } catch (const db_error& error) {
        closing = error.code() == db_error::code_type::closing;
    }
    RUVIA_CHECK(closing);
    RUVIA_CHECK(!scope.has_pending_operations());
    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
}
RUVIA_TEST(db_cache_policy_snapshots_settings_and_bypasses_writes_and_locks) {
    repository_runtime runtime;
    auto* resource = std::pmr::get_default_resource();
    db_cache_config config{.always_enabled_ = true};
    detail::redis_client_runtime redis(runtime.context_, runtime.worker_,
        detail::redis_config_storage(redis_config{}, resource), resource);
    ::ruvia::operation_scope redis_scope;
    auto store_value = redis.handle(redis_scope);
    detail::db_query_cache_state cache(store_value, detail::db_cache_config_storage(config, resource), config.name_space_, resource);
    db_query query;
    query.select(query.column("id")).from("items");
    const auto driver = database_config().driver_;
    auto statement = query.compile(driver, resource);
    RUVIA_CHECK(cache.key(query, statement, driver).has_value());
    query.cache(false);
    RUVIA_CHECK(!cache.key(query, statement, driver));
    std::string id(500, 'x');
    query.cache(std::string_view(id), std::chrono::seconds(30));
    auto copied = query.clone(resource);
    query.cache(false);
    id.assign(500, 'y');
    RUVIA_CHECK_EQ(*cache.key(copied, statement, driver), detail::db_cache_key(config.name_space_, std::string(500, 'x'), {}, {}, driver, resource));
    copied.lock({});
    RUVIA_CHECK(!cache.key(copied, statement, driver));
    db_query write;
    write.update("items").set("name", write.value("changed"));
    write.returning({write.column("id")});
    RUVIA_CHECK(!cache.key(write, statement, driver));
    db_query with_write;
    with_write.with("changed", write).select(with_write.column("id")).from("changed");
    RUVIA_CHECK(!cache.key(with_write, statement, driver));
    RUVIA_CHECK(testing::throws_on([&] { query.cache(std::chrono::milliseconds(0)); }));
    RUVIA_CHECK(testing::throws_on([&] { query.cache("id", std::chrono::milliseconds(-1)); }));
    config.duration_ = std::chrono::milliseconds(0);
    RUVIA_CHECK(testing::throws_on([&] { detail::db_cache_config_storage stored(config, resource); }));
    config.duration_ = std::chrono::seconds(1);
    config.name_space_.clear();
    RUVIA_CHECK(testing::throws_on([&] { detail::db_cache_config_storage stored(config, resource); }));
}

RUVIA_TEST(db_query_cache_binding_preserves_store_and_expires_with_its_scope) {
    repository_runtime runtime;
    test::counting_memory_resource resource;
    detail::redis_client_runtime redis(runtime.context_, runtime.worker_,
        detail::redis_config_storage(redis_config{}, &resource), &resource);
    ::ruvia::operation_scope redis_scope;
    auto store_value = redis.handle(redis_scope);
    ::ruvia::operation_scope db_scope;
    detail::db_registry registry(runtime.context_, runtime.worker_, &resource,
        database_config(), store_value, db_cache_config{});
    auto cache = registry.get(db_scope).query_result_cache();
    auto pending = cache.clear();
    redis_scope.close();
    RUVIA_CHECK(testing::throws_on([&] {
        run_void([&]() -> task<void> { co_await std::move(pending); }());
    }));
    RUVIA_CHECK(!db_scope.has_pending_operations());

    ::ruvia::operation_scope second_scope;
    auto second_store = redis.handle(second_scope);
    detail::db_registry second(runtime.context_, runtime.worker_, &resource,
        database_config(), second_store, db_cache_config{});
    second.close_now();
    {
        auto ping = second_store.ping();
    }
    RUVIA_CHECK(!second_scope.has_pending_operations());
}

RUVIA_TEST(db_query_cache_binding_rejects_another_worker) {
    repository_runtime database_runtime;
    repository_runtime redis_runtime;
    auto* resource = std::pmr::get_default_resource();
    detail::redis_client_runtime redis(redis_runtime.context_, redis_runtime.worker_,
        detail::redis_config_storage(redis_config{}, resource), resource);
    ::ruvia::operation_scope scope;
    auto store_value = redis.handle(scope);
    RUVIA_CHECK(testing::throws_on([&] {
        detail::db_registry registry(database_runtime.context_, database_runtime.worker_,
            resource, database_config(), store_value, db_cache_config{});
    }));
}

RUVIA_TEST(db_query_cache_registration_owns_policy_and_resolves_registered_alias) {
    repository_runtime runtime;
    test::counting_memory_resource resource;
    {
        const std::array redis_definitions{detail::redis_definition_type{
            std::pmr::string("cache", &resource), detail::redis_config_storage(redis_config{}, &resource)}};
        detail::redis_registry redis(runtime.context_, &resource, redis_definitions, runtime.worker_);
        db_query_cache_registration registration{
            .redis_alias_ = "cache", .policy_ = {.name_space_ = std::string(200, 'n')}};
        std::array databases{detail::db_definition{
            std::pmr::string("default", &resource), detail::db_config_storage(database_config(), &resource)}};
        databases[0].query_cache_.emplace(registration, &resource);
        registration.redis_alias_ = "changed";
        registration.policy_.name_space_.assign(200, 'x');
        RUVIA_CHECK_EQ(databases[0].query_cache_->redis_alias_, std::string_view("cache"));
        RUVIA_CHECK_EQ(databases[0].query_cache_->policy_.name_space_, std::string_view(std::string(200, 'n')));
        {
            detail::db_registry registry(runtime.context_, runtime.worker_, &resource, databases, &redis);
            ::ruvia::operation_scope scope;
            auto cache = registry.get(scope).query_result_cache();
            const auto baseline = resource.live_allocations();
            {
                auto cold = cache.clear();
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
        }
        databases[0].query_cache_->redis_alias_ = "missing";
        bool missing = false;
        try {
            detail::db_registry registry(runtime.context_, runtime.worker_, &resource, databases, &redis);
        } catch (const redis_error& error) {
            missing = error.code() == redis_error::code_type::not_configured;
        }
        RUVIA_CHECK(missing);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}
#endif

}  // namespace
