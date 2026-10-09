#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/db/db_repository.h"

#include "backend_client_fixture.h"
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
};
db_config database_config() {
#ifdef RUVIA_ENABLE_POSTGRESQL
    return {.driver_ = db_driver::postgresql};
#else
    return {.driver_ = db_driver::mariadb};
#endif
}

RUVIA_DB_PROJECTION(item_summary,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(label, std::pmr::string),
    RUVIA_DB_COLUMN(rank, std::int64_t))

RUVIA_TEST(db_repository_projects_computed_dto_and_owns_expression_sources) {
    repository_runtime runtime;
    test::counting_memory_resource source;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();
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
                auto many = builder.get_many<item_summary>();
                auto one = builder.get_one<item_summary>();
                auto page = builder.get_many_and_count<item_summary>();
                RUVIA_CHECK(testing::throws_on([&] { (void)builder.get_many(); }));
            }
        }
        auto partial = repository.create_query_builder();
        partial.select({{"name"}});
        auto result_value = partial.get_many();
        RUVIA_CHECK(testing::throws_on([&] { partial.select({{"missing"}}); }));
        RUVIA_CHECK(testing::throws_on([&] { partial.select({{"id"}, {"id"}}); }));
        co_return;
    });
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_repository_update_builder_owns_complex_conditions_and_cross_table_source) {
    repository_runtime runtime;
    test::counting_memory_resource source;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();
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
        co_return;
    });
}

RUVIA_TEST(db_repository_write_ctes_claim_and_persist_in_one_statement) {
    using archive_type = db_entity<"archive", db_column<"id", std::int64_t>, db_column<"name", std::pmr::string>>;
    repository_runtime runtime;
    test::counting_memory_resource source;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();
        auto archive = client.get_repository<archive_type>();
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
                auto operation = insert.get_many();
            }
        }
        co_return;
    });
}

RUVIA_TEST(db_repository_write_builder_cancelled_operations_release_input_snapshots) {
    repository_runtime runtime;
    test::counting_memory_resource input;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        stop_source stop;
        stop.request_stop();
        auto repository = client.with_options({.stop_token_ = stop.token()}).get_repository<entity_type>();
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
            co_await exercise();
            RUVIA_CHECK(cancelled);
        }
        co_return;
    });
}

RUVIA_TEST(db_repository_composes_entity_joins_lateral_cte_and_grouped_queries) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();
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
        co_return;
    });
}

RUVIA_TEST(db_repository_grouped_projection_reuses_imported_parameters) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto builder = client.get_repository<entity_type>().create_query_builder("i");
        {
            db_expressions x;
            const auto bucket = x.binary(x.column("id", "i"), db_binary_operator::add, x.value(3));
            builder.select({{"id", bucket}}).group_by({bucket});
            builder.having(x.binary(bucket, db_binary_operator::greater, x.value(10)));
        }
        const auto statement = builder.get_query_and_parameters();
        RUVIA_CHECK_EQ(statement.sql(), "SELECT (\"i\".\"id\" + $1) AS \"id\" FROM \"items\" AS \"i\" GROUP BY (\"i\".\"id\" + $1) HAVING ((\"i\".\"id\" + $1) > $2)");
        RUVIA_CHECK_EQ(statement.params().size(), std::size_t{2});
        co_return;
    });
}

RUVIA_TEST(db_repository_expression_writes_own_sources_and_validate_returning) {
    repository_runtime runtime;
    test::counting_memory_resource source;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();
        entity_type entity;
        entity.set<"id">(8);
        entity.set<"name">("initial");
        RUVIA_CHECK(testing::throws_on([&] { (void)repository.insert_returning<item_summary>(entity); }));
        auto where = entity_type::column<"id">() == 8;
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
                RUVIA_CHECK(testing::throws_on([&] { (void)repository.update(where, {{"missing", x.value(1)}}); }));
                RUVIA_CHECK(testing::throws_on([&] { (void)repository.update(where, {{"id", x.value(1)}, {"id", x.value(2)}}); }));
                options.update_expressions_.push_back(options.update_expressions_.front());
                RUVIA_CHECK(testing::throws_on([&] { (void)repository.upsert(entity, options); }));
            }
        }
        co_return;
    });
}
#endif

RUVIA_TEST(db_repository_builder_binds_entity_predicates_to_join_alias) {
    repository_runtime runtime;
    const auto config = database_config();
    test::with_connected_db_client(runtime.attachment_, config, [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();
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
        co_return;
    });
}

RUVIA_TEST(db_repository_rejects_unbounded_writes_and_missing_primary_key) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();
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
        co_return;
    });
}

RUVIA_TEST(db_repository_rejects_writes_that_only_target_computed_columns) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<computed_entity_type>();
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
        }
        co_return;
    });
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
        const db_config config{.driver_ = driver};
        test::with_connected_db_client(runtime.attachment_, config, [&](db_client& client) -> task<void> {
            auto repository = client.get_repository<entity_type>();
            entity_type input;
            input.set<"id">(1);
            input.set<"name">(std::string(400, 'n'));
            for (int i = 0; i < 12; ++i) {
                if (config.driver_ == db_driver::postgresql) {
                    {
                        const auto operation = repository.upsert(input, {.conflict_paths_ = {"id"},
                                                                            .skip_update_if_no_values_changed_ = true,
                                                                            .index_predicate_ = entity_type::column<"name">().is_not_null()});
                    }
                } else {
                    RUVIA_CHECK(testing::throws_on([&] { (void)repository.upsert(input, {.any_unique_key_ = true, .skip_update_if_no_values_changed_ = true}); }));
                    RUVIA_CHECK(testing::throws_on([&] { (void)repository.upsert(input, {.any_unique_key_ = true, .index_predicate_ = entity_type::column<"name">().is_not_null()}); }));
                }
            }
            using key_only_type = db_entity<"keys", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>>;
            auto keys = client.get_repository<key_only_type>();
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
                } else {
                    RUVIA_CHECK(testing::throws_on([&] { (void)keys.upsert(key, options); }));
                }
            }
            co_return;
        });
    }
}

RUVIA_TEST(db_repository_builder_orders_entity_fields_and_owns_predicates) {
    repository_runtime runtime;
    const auto config = database_config();
    test::with_connected_db_client(runtime.attachment_, config, [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();
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
        auto relation = client.get_repository<parent>().create_query_builder("p");
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
        }
        co_return;
    });
}

RUVIA_TEST(db_repository_write_inputs_cover_default_sparse_nullable_and_composite_keys) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.get_repository<entity_type>();

        // A single entity with no explicit values uses DEFAULT VALUES. Bulk
        // default-only input is rejected because there is no row width to encode.
        {
            entity_type defaults;
            auto operation = repository.insert(defaults);
        }
        std::array<entity_type, 2> sparse;
        sparse[0].set<"id">(1);
        sparse[1].set<"name">("only-name");
        auto sparse_insert = repository.insert(std::span<const entity_type>(sparse));
        RUVIA_CHECK(testing::throws_on([&] {
            std::array<entity_type, 2> defaults;
            (void)repository.insert(std::span<const entity_type>(defaults));
        }));

        auto nullable_repository = client.get_repository<nullable_entity_type>();
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

        auto composite = client.get_repository<composite_entity_type>();
        composite_entity_type key;
        key.set<"tenant_id">(10);
        key.set<"item_id">(20);
        auto removed = composite.remove(key);
        composite_entity_type partial;
        partial.set<"tenant_id">(10);
        RUVIA_CHECK(testing::throws_on([&] { (void)composite.remove(partial); }));
        co_return;
    });
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
        test::with_connected_db_client(runtime.attachment_, db_config{.driver_ = driver}, [&](db_client& client) -> task<void> {
            auto repository = client.get_repository<entity_type>();
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
            co_return;
        });
    }
}

}  // namespace
