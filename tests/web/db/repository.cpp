#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_repository.h"

#include "backend_client_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace db_repository_test {
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
        auto repository = client.handle().get_repository<entity_type>();
        for (int i = 0; i < 8; ++i) {
            {
                auto builder = repository.create_query_builder("i");
                {
                    db_query expressions(&source);
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
RUVIA_TEST(db_repository_consumes_selected_projection_rows_and_owns_selection) {
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.column_ = "label", .rows_ = {"Ada"}},
            {.column_ = "label", .rows_ = {"Grace", "Linus"}},
            {.column_ = "label", .rows_ = {}}});
    repository_runtime runtime;
    db_client client(runtime.attachment_.loop(), peer.config());
    test::drive_backend_client(runtime.attachment_, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.handle().get_repository<entity_type>();
        auto one_operation = [&] {
            auto builder = repository.create_query_builder("i");
            db_query expressions;
            builder.select({{"label", expressions.column("name", "i")}});
            return builder.get_one<item_summary>();
        }();
        const auto one = co_await std::move(one_operation);
        RUVIA_CHECK(one.has_value());
        if (one) {
            RUVIA_CHECK_EQ(one->get<"label">(), "Ada");
            RUVIA_CHECK(!one->is_set<"id">());
            RUVIA_CHECK(!one->is_set<"rank">());
        }
        auto many_operation = [&] {
            auto builder = repository.create_query_builder("i");
            db_query expressions;
            builder.select({{"label", expressions.column("name", "i")}});
            return builder.get_many<item_summary>();
        }();
        const auto many = co_await std::move(many_operation);
        RUVIA_CHECK_EQ(many.size(), std::size_t{2});
        if (many.size() == 2) {
            RUVIA_CHECK_EQ(many[0].get<"label">(), "Grace");
            RUVIA_CHECK_EQ(many[1].get<"label">(), "Linus");
            for (const auto& row : many) {
                RUVIA_CHECK(!row.is_set<"id">());
                RUVIA_CHECK(!row.is_set<"rank">());
            }
        }
        auto builder = repository.create_query_builder("i");
        db_query expressions;
        builder.select({{"label", expressions.column("name", "i")}});
        const auto empty = co_await builder.get_one<item_summary>();
        RUVIA_CHECK(!empty.has_value());
    });
}

RUVIA_TEST(db_repository_update_builder_owns_complex_conditions_and_cross_table_source) {
    repository_runtime runtime;
    test::counting_memory_resource source;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.handle().get_repository<entity_type>();
        auto update = repository.create_update_builder("i");
        {
            db_query x(&source);
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
        db_query x;
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
        auto repository = client.handle().get_repository<entity_type>();
        auto archive = client.handle().get_repository<archive_type>();
        for (int i = 0; i < 8; ++i) {
            {
                auto insert = archive.create_insert_builder();
                {
                    db_query x(&source);
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
                    db_query x(&input);
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
        auto repository = client.handle().get_repository<entity_type>();
        auto builder = repository.create_query_builder("i");
        {
            db_query x;
            auto source_value = repository.create_query_builder("s");
            source_value.where(entity_type::column<"id">() > 4);
            source_value.select({{"id"}});
            builder.with("recent", source_value, {.materialization_ = db_materialization::materialized});
            builder.join_cte(db_join_type::inner, "recent", "r", x.binary(x.column("id", "i"), db_binary_operator::equal, x.column("id", "r")));
            auto correlated = repository.create_query_builder("c");
            correlated.where(x.binary(x.column("id", "c"), db_binary_operator::equal, x.column("id", "i"))).take(1);
            builder.join(db_join_type::left, correlated, "l", x.value(true), {.lateral_ = true});
            builder.join<entity_type>(db_join_type::cross, "other");
            builder.select({{"id"}, {"rank", x.aggregate("count", {x.star()})}});
            builder.group_by({x.column("id", "i")}).having(x.binary(x.aggregate("count", {x.star()}), db_binary_operator::greater, x.value(1)));
            builder.and_where(source_value.exists(x));
            builder.and_where(x.binary(x.column("id", "i"), db_binary_operator::in, source_value.subquery(x)));
        }
        const auto statement = builder.get_query_and_parameters();
        RUVIA_CHECK(statement.sql().find("WITH \"recent\" AS MATERIALIZED") != std::string_view::npos);
        RUVIA_CHECK(statement.sql().find("LEFT JOIN LATERAL") != std::string_view::npos);
        RUVIA_CHECK(statement.sql().find("CROSS JOIN \"items\" AS \"other\"") != std::string_view::npos);
        RUVIA_CHECK(statement.sql().find("GROUP BY \"i\".\"id\" HAVING") != std::string_view::npos);
        RUVIA_CHECK(statement.sql().find("EXISTS (SELECT") != std::string_view::npos);
        RUVIA_CHECK(statement.sql().find(" IN (SELECT") != std::string_view::npos);
        auto operation = builder.get_many<item_summary>();
        co_return;
    });
}

RUVIA_TEST(db_repository_grouped_projection_reuses_imported_parameters) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto builder = client.handle().get_repository<entity_type>().create_query_builder("i");
        {
            db_query x;
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
        auto repository = client.handle().get_repository<entity_type>();
        entity_type entity;
        entity.set<"id">(8);
        entity.set<"name">("initial");
        RUVIA_CHECK(testing::throws_on([&] { (void)repository.insert_returning<item_summary>(entity); }));
        auto where = entity_type::column<"id">() == 8;
        for (int i = 0; i < 12; ++i) {
            {
                auto update = [&] {
                    db_query x(&source);
                    const std::array changes{db_assignment{"name", x.call("concat", {x.column("name"), x.value(std::string(400, 'z'))})}};
                    const std::array fields_value{db_selection{"id"}, db_selection{"label", x.call("upper", {x.column("name")})}};
                    return repository.update_returning<item_summary>(where, changes, fields_value);
                }();
                RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
                auto insert = repository.insert_returning(entity);
                auto erase = repository.delete_returning(where);
                auto plain_update = repository.update_returning(where, entity);
                db_query x;
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
        auto repository = client.handle().get_repository<entity_type>();
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
        auto repository = client.handle().get_repository<entity_type>();
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
        auto repository = client.handle().get_repository<computed_entity_type>();
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
            auto repository = client.handle().get_repository<entity_type>();
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
            auto keys = client.handle().get_repository<key_only_type>();
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
        auto repository = client.handle().get_repository<entity_type>();
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
        auto relation = client.handle().get_repository<parent>().create_query_builder("p");
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
        auto repository = client.handle().get_repository<entity_type>();

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

        auto nullable_repository = client.handle().get_repository<nullable_entity_type>();
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

        auto composite = client.handle().get_repository<composite_entity_type>();
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
            auto repository = client.handle().get_repository<entity_type>();
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

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_repository_builders_support_typed_joins_assignments_upserts_delete_using_and_read_only_columns) {
    using role_entity = db_entity<"roles",
        db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
        db_column<"name", std::pmr::string>>;
    using user_role_entity = db_entity<"user_roles",
        db_column<"user_id", std::int64_t, db_column_options{.primary_key_ = true}>,
        db_column<"role_id", std::int64_t, db_column_options{.primary_key_ = true}>>;
    using trigger_populated_entity = db_entity<"devices",
        db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
        db_column<"name", std::pmr::string, db_column_options{.nullable_ = true}>,
        db_column<"revision", std::int64_t>,
        db_column<"protocol_address", std::pmr::string, db_column_options{.read_only_ = true}>,
        db_column<"updated_at", std::pmr::string>>;

    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto roles = client.handle().get_repository<role_entity>();
        auto query = roles.create_query_builder("r");
        query.join<user_role_entity>(db_join_type::inner, "ur", user_role_entity::column<"role_id">() == role_entity::column<"id">())
            .where(user_role_entity::column<"user_id">("ur") == 42)
            .distinct_on({"id"})
            .order_by(role_entity::column<"id">())
            .add_order_by(user_role_entity::column<"role_id">("ur"), db_order_direction::desc);
        const auto role_sql = query.get_query_and_parameters();
        RUVIA_CHECK_EQ(role_sql.sql(),
            "SELECT DISTINCT ON (\"r\".\"id\") \"r\".\"id\", \"r\".\"name\" FROM \"roles\" AS \"r\" "
            "INNER JOIN \"user_roles\" AS \"ur\" ON (\"ur\".\"role_id\" = \"r\".\"id\") "
            "WHERE (\"ur\".\"user_id\" = $1) ORDER BY \"r\".\"id\" ASC, \"ur\".\"role_id\" DESC");

        auto clean_grouped = roles.create_query_builder("r");
        clean_grouped.select({{"id"}, role_entity::column<"name">().coalesce("fallback", "label")})
            .group_by({"id"})
            .add_group_by(role_entity::column<"name">())
            .having(role_entity::column<"id">() > 0)
            .and_having(query.exists());
        const auto grouped_sql = clean_grouped.get_query_and_parameters();
        RUVIA_CHECK(grouped_sql.sql().find("GROUP BY \"r\".\"id\", \"r\".\"name\" HAVING ((\"r\".\"id\" > $2) AND EXISTS (SELECT") != std::string_view::npos);

        auto devices = client.handle().get_repository<trigger_populated_entity>();
        trigger_populated_entity dev;
        dev.set<"id">(1);
        dev.set<"name">("sensor-1");
        dev.set<"revision">(1);
        dev.set<"protocol_address">("ignored-by-orm-writes");
        dev.set<"updated_at">("2026-01-01T00:00:00Z");

        auto insert_dev = devices.create_insert_builder(dev);
        insert_dev.on_conflict({.conflict_paths_ = {"id"}, .skip_update_if_no_values_changed_ = true}).returning();
        const auto insert_dev_sql = insert_dev.get_query_and_parameters();
        RUVIA_CHECK(insert_dev_sql.sql().find("protocol_address\") VALUES") == std::string_view::npos);
        RUVIA_CHECK(insert_dev_sql.sql().find("DO UPDATE SET \"name\" = excluded.\"name\"") != std::string_view::npos);
        RUVIA_CHECK(insert_dev_sql.sql().find("RETURNING \"devices\".\"id\", \"devices\".\"name\", \"devices\".\"revision\", \"devices\".\"protocol_address\", \"devices\".\"updated_at\"") != std::string_view::npos);
        auto one_inserted = insert_dev.get_one();
        (void)one_inserted;

        RUVIA_CHECK(testing::throws_on([&] {
            auto bad = devices.create_update_builder();
            bad.set("protocol_address", "x");
        }));

        db_query incoming;
        incoming.values({incoming.value(10), incoming.value("edge-a"), incoming.value(1), incoming.value("now")});
        auto from_cte = devices.create_query_builder("inc");
        from_cte.from_cte("incoming")
            .select({
                {"id", incoming.column("id", "inc")},
                {"name", incoming.column("name", "inc")},
                {"revision", incoming.column("revision", "inc")},
                {"updated_at", incoming.column("updated_at", "inc")},
            });
        auto pipeline = devices.create_insert_builder();
        pipeline.with("incoming", incoming, {.columns_ = {"id", "name", "revision", "updated_at"}})
            .insert_from({"id", "name", "revision", "updated_at"}, from_cte)
            .on_conflict({
                .conflict_paths_ = {"id"},
                .skip_update_if_no_values_changed_ = true,
                .update_expressions_ = {trigger_populated_entity::column<"revision">().increment(1)},
                .update_predicate_ = trigger_populated_entity::column<"revision">() < 100,
            })
            .returning({{"id"}});
        const auto pipeline_sql = pipeline.get_query_and_parameters();
        RUVIA_CHECK(pipeline_sql.sql().find("WITH \"incoming\" (\"id\", \"name\", \"revision\", \"updated_at\") AS (VALUES ($1, $2, $3, $4))") == 0);
        RUVIA_CHECK(pipeline_sql.sql().find("ON CONFLICT (\"id\") DO UPDATE SET") != std::string_view::npos);
        RUVIA_CHECK(pipeline_sql.sql().find("\"revision\" = (\"devices\".\"revision\" + $5)") != std::string_view::npos);

        auto update_builder = devices.create_update_builder("d");
        update_builder.set("name", "renamed")
            .set_null("name")
            .increment("revision", 2)
            .set_now("updated_at")
            .update_from<role_entity>("r")
            .join<user_role_entity>(db_join_type::inner, "ur", user_role_entity::column<"role_id">("ur") == role_entity::column<"id">("r"))
            .where(trigger_populated_entity::column<"id">() == user_role_entity::column<"user_id">("ur"))
            .returning({{"id"}});
        const auto update_sql = update_builder.get_query_and_parameters();
        RUVIA_CHECK_EQ(update_sql.sql(),
            "UPDATE \"devices\" AS \"d\" SET \"name\" = NULL, \"revision\" = (\"d\".\"revision\" + $1), \"updated_at\" = \"now\"() "
            "FROM \"roles\" AS \"r\" INNER JOIN \"user_roles\" AS \"ur\" ON (\"ur\".\"role_id\" = \"r\".\"id\") "
            "WHERE (\"d\".\"id\" = \"ur\".\"user_id\") RETURNING \"d\".\"id\" AS \"id\"");

        auto delete_builder = devices.create_delete_builder("d");
        delete_builder.delete_using<role_entity>("r")
            .join_cte(db_join_type::inner, "expired", "e", role_entity::column<"id">("r") == role_entity::column<"id">("e"))
            .where(trigger_populated_entity::column<"id">() == role_entity::column<"id">("r"))
            .returning({{"id"}});
        const auto delete_sql = delete_builder.get_query_and_parameters();
        RUVIA_CHECK_EQ(delete_sql.sql(),
            "DELETE FROM \"devices\" AS \"d\" USING \"roles\" AS \"r\" INNER JOIN \"expired\" AS \"e\" ON (\"r\".\"id\" = \"e\".\"id\") "
            "WHERE (\"d\".\"id\" = \"r\".\"id\") RETURNING \"d\".\"id\" AS \"id\"");

        auto ret_one = devices.update_returning_one<item_summary>(
            trigger_populated_entity::column<"id">() == 1,
            {
                trigger_populated_entity::column<"name">() = "next",
                trigger_populated_entity::column<"revision">() += 1,
                trigger_populated_entity::column<"updated_at">().set_now(),
            },
            {{"id"}, trigger_populated_entity::column<"name">().coalesce("", "label"), {"rank", trigger_populated_entity::column<"revision">().select().expression()}});
        (void)ret_one;
        co_return;
    });
}

RUVIA_TEST(db_repository_typeorm_style_api_covers_soft_delete_save_aggregates_joins_and_raw_queries) {
    using soft_entity = db_entity<"articles",
        db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
        db_column<"title", std::pmr::string>,
        db_column<"views", std::int64_t>,
        db_column<"revision", std::int64_t, db_column_options{.version_ = true}>,
        db_column<"updated_at", std::pmr::string, db_column_options{.update_date_ = true}>,
        db_column<"deleted_at", std::pmr::string, db_column_options{.delete_date_ = true}>>;

    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repo = client.handle().get_repository<soft_entity>();

        auto active_qb = repo.create_query_builder("a");
        active_qb.select({"id", "title"})
            .add_select("views")
            .add_select(db_expr::count(), "total_count")
            .where(soft_entity::column<"views">() >= 10)
            .and_where([](db_where_builder& sub) {
                sub.where(soft_entity::column<"title">().ilike("%ruvia%"))
                    .or_where(soft_entity::column<"id">() == 1);
            })
            .group_by({"id", "title", "views"})
            .having(soft_entity::column<"views">() > 0)
            .or_having(soft_entity::column<"id">() == 1)
            .limit(20)
            .offset(5);
        const auto active_sql = active_qb.get_query_and_parameters();
        RUVIA_CHECK(active_sql.sql().find("SELECT \"a\".\"id\" AS \"id\", \"a\".\"title\" AS \"title\", \"a\".\"views\" AS \"views\", \"count\"(*) AS \"total_count\" FROM \"articles\" AS \"a\"") == 0);
        RUVIA_CHECK(active_sql.sql().find("(\"a\".\"deleted_at\" IS NULL)") != std::string_view::npos);
        RUVIA_CHECK(active_sql.sql().find("HAVING ((\"a\".\"views\" > $4) OR (\"a\".\"id\" = $5))") != std::string_view::npos);
        RUVIA_CHECK(active_sql.sql().find("LIMIT $6 OFFSET $7") != std::string_view::npos);

        active_qb.with_deleted();
        const auto with_deleted_sql = active_qb.get_query_and_parameters();
        RUVIA_CHECK(with_deleted_sql.sql().find("deleted_at") == std::string_view::npos);

        auto parents = client.handle().get_repository<parent>();
        auto rel_join = parents.create_query_builder("p");
        rel_join.left_join("children", "c", child::column<"id">("c") > 0)
            .add_select(db_expr::count(child::column<"id">("c")), "child_count")
            .group_by({"id"});
        const auto rel_join_sql = rel_join.get_query_and_parameters();
        RUVIA_CHECK_EQ(rel_join_sql.sql(),
            "SELECT \"p\".\"id\", \"count\"(\"c\".\"id\") AS \"child_count\" FROM \"parents\" AS \"p\" "
            "LEFT JOIN \"children\" AS \"c\" ON ((\"p\".\"id\" = \"c\".\"parent_id\") AND (\"c\".\"id\" > $1)) "
            "GROUP BY \"p\".\"id\"");

        db_values_builder incoming({"id", "title", "views"});
        incoming.row({db_expr::value(1), db_expr::value("Hello"), db_expr::value(10)});
        auto values_select = repo.create_query_builder("v");
        values_select.with_deleted()
            .with("incoming", incoming, db_materialization::materialized)
            .from_cte("incoming")
            .select({"id", "title", "views"});
        const auto values_sql = values_select.get_query_and_parameters();
        RUVIA_CHECK(values_sql.sql().find("WITH \"incoming\" (\"id\", \"title\", \"views\") AS MATERIALIZED (VALUES ($1, $2, $3))") == 0);

        soft_entity item;
        item.set<"id">(1);
        item.set<"title">("Ruvia ORM");
        item.set<"views">(100);
        auto ins_ignore = repo.create_query_builder().insert();
        ins_ignore.values(item).or_ignore().returning({"id"});
        const auto ins_ignore_sql = ins_ignore.get_query_and_parameters();
        RUVIA_CHECK_EQ(ins_ignore_sql.sql(),
            "INSERT INTO \"articles\" (\"id\", \"title\", \"views\") VALUES ($1, $2, $3) ON CONFLICT DO NOTHING RETURNING \"articles\".\"id\" AS \"id\"");

        auto ins_update = repo.create_query_builder().insert();
        ins_update.values(item).or_update({"title", "views"}, {"id"}).returning({"id"});
        const auto ins_update_sql = ins_update.get_query_and_parameters();
        RUVIA_CHECK_EQ(ins_update_sql.sql(),
            "INSERT INTO \"articles\" (\"id\", \"title\", \"views\") VALUES ($1, $2, $3) ON CONFLICT (\"id\") DO UPDATE SET \"title\" = excluded.\"title\", \"views\" = excluded.\"views\" RETURNING \"articles\".\"id\" AS \"id\"");

        auto soft_del_qb = repo.create_query_builder("a").soft_delete();
        soft_del_qb.where(soft_entity::column<"id">() == 1);
        const auto soft_del_sql = soft_del_qb.get_query_and_parameters();
        RUVIA_CHECK_EQ(soft_del_sql.sql(),
            "UPDATE \"articles\" AS \"a\" SET \"deleted_at\" = \"now\"(), \"updated_at\" = \"now\"(), \"revision\" = (\"a\".\"revision\" + $1) WHERE (\"a\".\"id\" = $2)");

        auto restore_qb = repo.create_query_builder("a").restore();
        restore_qb.where(soft_entity::column<"id">() == 1);
        const auto restore_sql = restore_qb.get_query_and_parameters();
        RUVIA_CHECK_EQ(restore_sql.sql(),
            "UPDATE \"articles\" AS \"a\" SET \"deleted_at\" = NULL, \"updated_at\" = \"now\"() WHERE (\"a\".\"id\" = $1)");
        {
            auto op1 = repo.find_by(soft_entity::column<"id">() == 1);
            auto op2 = repo.find_one_by(soft_entity::column<"id">() == 1);
            auto op3 = repo.find_and_count_by(soft_entity::column<"id">() == 1);
            auto op4 = repo.count_by(soft_entity::column<"id">() == 1);
            auto op5 = repo.exists_by(soft_entity::column<"id">() == 1);
            auto op6 = repo.save(item);
            auto op7 = repo.soft_delete(soft_entity::column<"id">() == 1);
            auto op8 = repo.restore(soft_entity::column<"id">() == 1);
            auto op9 = repo.sum("views", soft_entity::column<"id">() == 1);
            auto op10 = repo.average("views");
            auto op11 = repo.minimum("views");
            auto op12 = repo.maximum("views");
            auto op13 = repo.clear();
            auto op14 = active_qb.get_raw_one();
            auto op15 = active_qb.get_raw_many();
            auto op16 = rel_join.get_raw_and_entities();
            auto op17 = active_qb.get_scalar<std::int64_t>();
            auto op18 = active_qb.get_scalars<std::int64_t>();
            auto op19 = ins_update.get_scalar<std::int64_t>();
            auto op20 = repo.insert_returning_scalar<std::int64_t>(item, "id");
            auto op21 = repo.update_returning_scalar<std::int64_t>(soft_entity::column<"id">() == 1, item, "revision");
            (void)op1;
            (void)op2;
            (void)op3;
            (void)op4;
            (void)op5;
            (void)op6;
            (void)op7;
            (void)op8;
            (void)op9;
            (void)op10;
            (void)op11;
            (void)op12;
            (void)op13;
            (void)op14;
            (void)op15;
            (void)op16;
            (void)op17;
            (void)op18;
            (void)op19;
            (void)op20;
            (void)op21;
        }
        co_return;
    });
}
#endif
}  // namespace db_repository_test

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_postgresql_declares_float8_for_doubles_and_leaves_other_parameters_untyped) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle, {{.rows_ = {"raw"}}, {.rows_ = {"compiled"}}});
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
        // An integral double must still reach the server as float8.
        const std::array<db_value, 4> params{db_value{5}, db_value{5.0}, db_value{true}, db_value{"text"}};
        (void)co_await handle.query("SELECT $1, $2, $3, $4", std::span<const db_value>(params));
        // Untyped, json_build_object's variadic "any" arguments cannot be resolved.
        db_query query;
        query.select(query.call("json_build_object", {query.value("k"), query.value(5)})).from("records");
        (void)co_await handle.query(query);
    });
    const auto statements = peer.postgres_parses();
    RUVIA_CHECK_EQ(statements.size(), std::size_t{2});
    if (statements.size() == 2) {
        // The SQL decides every type except a double's, which is declared
        // float8 regardless of its value; compiled SQL casts from the value category.
        RUVIA_CHECK(statements[0].sql_ == "SELECT $1, $2, $3, $4");
        RUVIA_CHECK((statements[0].parameter_types_ == std::vector<std::uint32_t>{0, 701, 0, 0}));
        RUVIA_CHECK(statements[1].sql_ == "SELECT \"json_build_object\"($1::text, $2::int8) FROM \"records\"");
        RUVIA_CHECK((statements[1].parameter_types_ == std::vector<std::uint32_t>{0, 0}));
        RUVIA_CHECK((statements[0].parameter_values_ ==
                     std::vector<std::optional<std::string>>{"5", "5", "true", "text"}));
        RUVIA_CHECK((statements[1].parameter_values_ ==
                     std::vector<std::optional<std::string>>{"k", "5"}));
    }
}

RUVIA_TEST(db_postgresql_owns_lazy_query_inputs_and_preserves_text_parameter_values) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"first"}}, {.rows_ = {"second"}}});
    auto& io = test::new_test_io_context();
    auto attachment = attach_event_loop(io);
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
        auto operation = [&] {
            const std::array<char, 4> unterminated{'a', 'b', 'c', '!'};
            const std::string long_value(4096, 'x');
            const std::array params{db_value{std::string_view(unterminated.data(), 3)}, db_value{long_value},
                db_value{std::string_view{}}, db_value{nullptr}, db_value{false}, db_value{-7},
                db_value{std::uint64_t{18446744073709551615ULL}}, db_value{1.5}};
            return handle.query("SELECT $1, $2, $3, $4, $5, $6, $7, $8", params);
        }();
        (void)co_await std::move(operation);
        // A compiled value is checked by the driver before it reaches the wire.
        const std::string invalid_text("a\0b", 3);
        db_query invalid;
        invalid.select(invalid.value(std::string_view(invalid_text))).from("records");
        bool rejected = false;
        try {
            (void)co_await handle.query(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        // Validation failure keeps the connection reusable, and this query
        // must not borrow the previous operation's parameter storage.
        const std::array params{db_value{"next"}, db_value{9}};
        (void)co_await handle.query("SELECT $1, $2", params);
    });
    const auto statements = peer.postgres_parses();
    RUVIA_CHECK_EQ(statements.size(), std::size_t{2});
    if (statements.size() == 2) {
        RUVIA_CHECK((statements[0].parameter_values_ ==
                     std::vector<std::optional<std::string>>{"abc", std::string(4096, 'x'), "", std::nullopt,
                         "false", "-7", "18446744073709551615", "1.5"}));
        RUVIA_CHECK((statements[1].parameter_values_ == std::vector<std::optional<std::string>>{"next", "9"}));
    }
}

namespace db_repository_array_test {

using array_entity = ruvia::db_entity<"array_records", ruvia::db_column<"values", std::pmr::vector<int>>>;

struct array_case final {
    std::string_view text_;
    std::vector<int> values_;
};

RUVIA_TEST(db_repository_decodes_postgresql_array_bounds_in_element_order) {
    using namespace ruvia;
    const std::array valid{
        array_case{"{7,8,9}", {7, 8, 9}},
        array_case{"[0:2]={7,8,9}", {7, 8, 9}},
        array_case{"[0:0]={7}", {7}},
        array_case{"[-3:-1]={9,7,8}", {9, 7, 8}},
        array_case{"[2:4]={8,9,7}", {8, 9, 7}},
        array_case{"[-2147483648:-2147483647]={7,8}", {7, 8}},
        array_case{"[2147483645:2147483646]={8,9}", {8, 9}},
        array_case{"{}", {}},
    };
    const std::array invalid{
        "[0:2]={7,8}",
        "[-3:-1]={7,8,9,10}",
        "[2:1]={7}",
        "[0:1]={}",
        "[-2147483649:-2147483648]={7,8}",
        "[2147483647:2147483648]={7,8}",
        "[2147483647:2147483647]={7}",
        "[-2147483648:2147483647]={7}",
        "[-2147483648:-1]={7}",
        "[:1]={7}",
        "[0:]={7}",
        "[0:0]{7}",
        "[0:0]x={7}",
        "[0:1][0:1]={{7,8},{9,10}}",
        "{{7,8},{9,10}}",
    };
    std::vector<test::backend_startup_peer::postgres_reply> replies;
    for (const auto& value : valid) {
        replies.push_back({.column_ = "values", .type_ = 1007, .rows_ = {std::string(value.text_)}});
    }
    for (const auto value : invalid) {
        replies.push_back({.column_ = "values", .type_ = 1007, .rows_ = {value}});
    }
    replies.push_back({.column_ = "values", .type_ = 1007, .rows_ = {"{21,22}"}});
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.handle().get_repository<array_entity>();
        for (const auto& value : valid) {
            bool decoded = false;
            try {
                const auto rows = co_await repository.find();
                RUVIA_CHECK_EQ(rows.size(), std::size_t{1});
                if (!rows.empty()) {
                    RUVIA_CHECK(std::ranges::equal(rows[0].get<"values">(), value.values_));
                }
                decoded = true;
            } catch (const db_conversion_error&) {
                decoded = false;
            }
            RUVIA_CHECK(decoded);
        }
        for ([[maybe_unused]] const auto value : invalid) {
            bool rejected = false;
            try {
                (void)co_await repository.find();
            } catch (const db_conversion_error& error) {
                rejected = error.code() == db_conversion_error::code_type::invalid_format;
            }
            RUVIA_CHECK(rejected);
        }
        const auto rows = co_await repository.find();
        RUVIA_CHECK_EQ(rows.size(), std::size_t{1});
        if (!rows.empty()) {
            RUVIA_CHECK(std::ranges::equal(rows[0].get<"values">(), std::array{21, 22}));
        }
    });
}

}  // namespace db_repository_array_test
#endif
