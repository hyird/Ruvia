#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/web/db/db_schema.h"

#include "test_harness.h"

using ruvia::db_data_type;
using ruvia::db_driver;
using ruvia::db_generated_type;
using ruvia::db_procedure;
using ruvia::db_schema;
using ruvia::db_schema_column;
using ruvia::db_schema_constraint;
using ruvia::db_table_definition;
using ruvia::db_table_option;
using ruvia::testing::throws_on;

using enum_entity_type = ruvia::db_entity<"enum_events",
    ruvia::db_column<"state", std::pmr::string,
        ruvia::db_column_options{.enum_name_ = ruvia::fixed_string{"app.event_state"}, .default_expression_ = ruvia::fixed_string{"'pending'::app.event_state"}}>,
    ruvia::db_column<"created_at", std::pmr::string,
        ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .default_expression_ = ruvia::fixed_string{"now()"}}>>;

RUVIA_TEST(db_schema_entity_named_enum_and_default_expressions) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<enum_entity_type>();
    const auto migrations = schema.compile("enum_defaults");
    RUVIA_CHECK(migrations[0].sql().find("\"state\" \"app\".\"event_state\" NOT NULL DEFAULT ('pending'::app.event_state)") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("DEFAULT (now())") != std::string_view::npos);
    db_schema maria({.driver_ = db_driver::mariadb});
    RUVIA_CHECK(throws_on([&] { maria.create_table<enum_entity_type>(); }));
    ruvia::db_query defaults;
    RUVIA_CHECK(throws_on([&] { schema.create_table<enum_entity_type>({.defaults_ = {{"state", defaults.value("ready")}}}); }));
}

using schema_target_type = ruvia::db_entity<"schema_targets", ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>>;
using schema_owner_type = ruvia::db_entity<"schema_owners",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"parent_id", std::int64_t>, ruvia::db_column<"parent_tenant_id", std::int64_t>,
    ruvia::db_column<"peer_id", std::int64_t>,
    ruvia::db_many_to_one<"parent", schema_target_type, ruvia::db_join_column<"parent_id", "id">,
        ruvia::db_join_column<"parent_tenant_id", "tenant_id">>,
    ruvia::db_one_to_one<"peer", schema_target_type, ruvia::db_join_column<"peer_id", "id">>,
    ruvia::db_one_to_many<"children", schema_target_type, "parent">,
    ruvia::db_many_to_many<"labels", schema_target_type, ruvia::db_join_table<"owner_labels", ruvia::db_join_columns<ruvia::db_join_column<"owner_id", "id">>, ruvia::db_join_columns<ruvia::db_join_column<"label_id", "id">, ruvia::db_join_column<"label_tenant_id", "tenant_id">>>>>;

RUVIA_TEST(db_schema_create_table_composite_foreign_key_and_nullable_default) {
    db_schema schema({.driver_ = db_driver::postgresql});
    db_table_definition table_value{.name_ = "tenant_users", .columns_ = {db_schema_column{.name_ = "tenant_id", .type_ = {.data_type_ = db_data_type::big_int}}, db_schema_column{.name_ = "user_id", .type_ = {.data_type_ = db_data_type::big_int}, .nullable_ = true}}, .constraints_ = {db_schema_constraint{.name_ = "tenant_users_pk", .kind_ = ruvia::db_constraint_kind::primary_key, .columns_ = {"tenant_id", "user_id"}}, db_schema_constraint{.name_ = "tenant_fk", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"tenant_id", "user_id"}, .referenced_table_ = "tenants", .referenced_columns_ = {"id", "id"}, .on_delete_ = ruvia::db_referential_action::cascade}}};
    schema.create_table(table_value);
    const auto migrations = schema.compile("001_create");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    RUVIA_CHECK(migrations[0].sql().find("PRIMARY KEY") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("FOREIGN KEY") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("ON DELETE CASCADE") != std::string_view::npos);
}

RUVIA_TEST(db_schema_partial_gin_index_quotes_identifiers_and_literals) {
    db_schema schema({.driver_ = db_driver::postgresql});
    ruvia::db_query query;
    const auto value = query.value(ruvia::db_value(std::string_view("x' OR 1=1")));
    const auto predicate = query.binary(query.column("state"), ruvia::db_binary_operator::equal, value);
    schema.create_index({.name_ = "events_idx", .table_ = "events", .keys_ = {{.column_ = "payload"}}, .method_ = ruvia::db_index_method::gin, .where_ = predicate});
    const auto migrations = schema.compile("002_index");
    RUVIA_CHECK(migrations[0].sql().find("USING gin") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("x'' OR 1=1") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("DROP TABLE") == std::string_view::npos);
}

RUVIA_TEST(db_schema_postgresql_batch_is_atomic_and_unwrapped_isolated) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_schema("one");
    schema.create_schema("two");
    auto migrations = schema.compile("003_batch");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    RUVIA_CHECK(migrations[0].sql().find("DO $ruvia$ BEGIN") == 0);

    db_schema invalid({.driver_ = db_driver::postgresql});
    invalid.create_schema("one");
    invalid.create_index({.name_ = "idx", .table_ = "t", .keys_ = {{.column_ = "x"}}, .concurrently_ = true});
    RUVIA_CHECK(throws_on([&] { (void)invalid.compile("004_invalid"); }));
}

RUVIA_TEST(db_schema_enum_and_postgresql_only_operations_reject_mariadb) {
    db_schema schema({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"open", "closed"};
    schema.create_enum("status", values);
    auto migrations = schema.compile("005_enum");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    db_schema addition({.driver_ = db_driver::postgresql});
    addition.add_enum_value("status", "paused");
    const auto added = addition.compile("006_enum_value");
    RUVIA_CHECK(added[0].atomicity() == ruvia::db_migration_atomicity::unwrapped);

    db_schema maria({.driver_ = db_driver::mariadb});
    RUVIA_CHECK(throws_on([&] { maria.create_enum("status", values); }));
}

RUVIA_TEST(db_schema_entity_metadata_preserves_explicit_array_element_types) {
    using entity_type = ruvia::db_entity<"app.events",
        ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true, .generated_ = true}>,
        ruvia::db_column<"payloads", std::pmr::vector<std::pmr::string>, ruvia::db_column_options{.data_type_ = db_data_type::jsonb}>>;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<entity_type>();
    const auto migrations = schema.compile("entity");
    RUVIA_CHECK_EQ(migrations[0].sql(), "CREATE TABLE \"app\".\"events\" (\"id\" BIGINT GENERATED BY DEFAULT AS IDENTITY NOT NULL, \"payloads\" JSONB[] NOT NULL, CONSTRAINT \"events_pkey\" PRIMARY KEY (\"id\"))");
}

RUVIA_TEST(db_schema_char_entity_and_column_operations_render_with_dialect_limits) {
    using entity_type = ruvia::db_entity<"char_records",
        ruvia::db_column<"code", std::pmr::string, ruvia::db_column_options{.data_type_ = db_data_type::char_value, .length_ = 64}>,
        ruvia::db_column<"codes", std::pmr::vector<std::pmr::string>, ruvia::db_column_options{.data_type_ = db_data_type::char_value, .length_ = 8}>>;
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_table<entity_type>();
    pg.alter_column_type("char_records", "code", {.data_type_ = db_data_type::char_value, .length_ = 32});
    const auto pg_migrations = pg.compile("char_pg");
    RUVIA_CHECK(pg_migrations[0].sql().find("\"code\" CHAR(64) NOT NULL") != std::string_view::npos);
    RUVIA_CHECK(pg_migrations[0].sql().find("\"codes\" CHAR(8)[] NOT NULL") != std::string_view::npos);
    RUVIA_CHECK(pg_migrations[0].sql().find("ALTER TABLE \"char_records\" ALTER COLUMN \"code\" TYPE CHAR(32)") != std::string_view::npos);

    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table({.name_ = "char_records", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 255}}}});
    RUVIA_CHECK(maria.compile("char_maria")[0].sql().find("`code` CHAR(255) NOT NULL") != std::string_view::npos);

    db_schema pg_limit({.driver_ = db_driver::postgresql});
    pg_limit.create_table({.name_ = "char_limit", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 10485760}}}});
    RUVIA_CHECK(pg_limit.compile("char_pg_limit")[0].sql().find("\"code\" CHAR(10485760) NOT NULL") != std::string_view::npos);

    RUVIA_CHECK(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value}}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 10485761}}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::mariadb});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 256}}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 8, .precision_ = 2}}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::text, .length_ = 8}}}});
    }));
}

RUVIA_TEST(db_schema_entity_computed_columns_render_and_validate) {
    using entity_type = ruvia::db_entity<"computed_events",
        ruvia::db_column<"first_name", ruvia::string>,
        ruvia::db_column<"last_name", ruvia::string>,
        ruvia::db_column<"display_name", ruvia::string,
            ruvia::db_column_options{.generated_type_ = db_generated_type::stored}>>;
    ruvia::db_query expression;
    auto generated = expression.binary(expression.column("first_name"), ruvia::db_binary_operator::concat,
        expression.column("last_name"));
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<entity_type>({.generated_columns_ = {{.column_ = "display_name", .expression_ = generated}}});
    const auto migrations = schema.compile("computed");
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("\"display_name\" TEXT GENERATED ALWAYS AS") != std::string_view::npos);
    RUVIA_CHECK(sql.find(" STORED") != std::string_view::npos);
    RUVIA_CHECK(throws_on([&] { schema.create_table<entity_type>(); }));

    using maria_entity_type = ruvia::db_entity<"maria_computed",
        ruvia::db_column<"value", std::int64_t>,
        ruvia::db_column<"double_value", std::int64_t,
            ruvia::db_column_options{.generated_type_ = db_generated_type::virtual_value, .nullable_ = true}>>;
    ruvia::db_query maria_expression;
    auto doubled = maria_expression.binary(maria_expression.column("value"), ruvia::db_binary_operator::multiply,
        maria_expression.value(2));
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table<maria_entity_type>({.generated_columns_ = {{.column_ = "double_value", .expression_ = doubled}}});
    RUVIA_CHECK(maria.compile("maria_computed")[0].sql().find(" VIRTUAL") != std::string_view::npos);
    RUVIA_CHECK(throws_on([&] {
        maria.add_column("maria_computed", {.name_ = "invalid", .type_ = {.data_type_ = db_data_type::big_int}, .generated_type_ = db_generated_type::stored, .as_expression_ = doubled});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema pg({.driver_ = db_driver::postgresql});
        pg.create_table<maria_entity_type>({.generated_columns_ = {{.column_ = "double_value", .expression_ = doubled}}});
    }));
}

RUVIA_TEST(db_schema_computed_columns_reject_invalid_metadata_and_conflicts) {
    using computed_type = ruvia::db_entity<"invalid_computed",
        ruvia::db_column<"value", std::int64_t>,
        ruvia::db_column<"total", std::int64_t,
            ruvia::db_column_options{.generated_type_ = db_generated_type::stored}>>;
    ruvia::db_query query;
    const auto expression = query.binary(query.column("value"), ruvia::db_binary_operator::add, query.value(1));
    RUVIA_CHECK(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{.column_ = "missing", .expression_ = expression}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{"total", expression}, {"total", expression}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{"value", expression}, {"total", expression}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{.column_ = "total", .expression_ = {}}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.defaults_ = {{.column_ = "total", .value_ = expression}},
            .generated_columns_ = {{.column_ = "total", .expression_ = expression}}});
    }));
    using identity_computed_type = ruvia::db_entity<"identity_computed",
        ruvia::db_column<"id", std::int64_t,
            ruvia::db_column_options{.generated_ = true, .generated_type_ = db_generated_type::stored}>>;
    RUVIA_CHECK(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<identity_computed_type>({.generated_columns_ = {{.column_ = "id", .expression_ = expression}}});
    }));
}

RUVIA_TEST(db_schema_mariadb_computed_columns_reject_table_primary_keys) {
    using entity_type = ruvia::db_entity<"computed_keys",
        ruvia::db_column<"source", std::int64_t>,
        ruvia::db_column<"computed", std::int64_t,
            ruvia::db_column_options{.primary_key_ = true, .generated_type_ = db_generated_type::stored, .nullable_ = true}>>;
    ruvia::db_query query;
    auto expression = query.column("source");
    db_schema schema({.driver_ = db_driver::mariadb});
    RUVIA_CHECK(throws_on([&] {
        schema.create_table<entity_type>({.generated_columns_ = {{"computed", expression}}});
    }));
    for (const bool composite : {false, true}) {
        db_table_definition table_value{.name_ = "computed_keys", .columns_ = {{.name_ = "source", .type_ = {.data_type_ = db_data_type::big_int}}, {.name_ = "computed", .type_ = {.data_type_ = db_data_type::big_int}, .nullable_ = true, .generated_type_ = db_generated_type::stored, .as_expression_ = expression}}, .constraints_ = {{.name_ = "computed_key", .kind_ = ruvia::db_constraint_kind::primary_key, .columns_ = {"computed"}}}};
        if (composite) {
            table_value.constraints_[0].columns_.push_back("source");
        }
        RUVIA_CHECK(throws_on([&] { schema.create_table(table_value); }));
    }
}

RUVIA_TEST(db_schema_entity_relations_emit_owning_foreign_keys_and_one_to_one_unique) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<schema_owner_type>();
    const auto migrations = schema.compile("relations");
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("FOREIGN KEY (\"parent_id\",\"parent_tenant_id\") REFERENCES \"schema_targets\" (\"id\",\"tenant_id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("FOREIGN KEY (\"peer_id\") REFERENCES \"schema_targets\" (\"id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("CONSTRAINT \"schema_owners_peer_key\" UNIQUE (\"peer_id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("children") == std::string_view::npos);
}

RUVIA_TEST(db_schema_many_to_many_relation_table_has_composite_primary_key_and_two_foreign_keys) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_relation_tables<schema_owner_type>();
    const auto migrations = schema.compile("relation_tables");
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("CREATE TABLE \"owner_labels\"") != std::string_view::npos);
    RUVIA_CHECK(sql.find("PRIMARY KEY (\"owner_id\",\"label_id\",\"label_tenant_id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("FOREIGN KEY (\"owner_id\") REFERENCES \"schema_owners\" (\"id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("FOREIGN KEY (\"label_id\",\"label_tenant_id\") REFERENCES \"schema_targets\" (\"id\",\"tenant_id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"label_id\" BIGINT NOT NULL") != std::string_view::npos);
}

RUVIA_TEST(db_schema_qualified_types_and_replace_view_use_valid_ddl_grammar) {
    db_schema schema({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"on", "off"};
    schema.create_enum("app.state", values);
    schema.create_table({.name_ = "app.devices", .columns_ = {{.name_ = "state", .type_ = {.custom_name_ = "app.state"}}}});
    ruvia::db_query query;
    query.select(query.column("state")).from("app.devices");
    schema.create_view("app.device_state", query, true);
    const auto migrations = schema.compile("types");
    RUVIA_CHECK(migrations[0].sql().find("CREATE TYPE \"app\".\"state\" AS ENUM") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("\"state\" \"app\".\"state\" NOT NULL") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("CREATE OR REPLACE VIEW \"app\".\"device_state\" AS SELECT") != std::string_view::npos);
}

RUVIA_TEST(db_schema_procedure_builds_channel_guard_and_escapes_body_delimiters) {
    ruvia::db_query query;
    ruvia::db_procedure body;
    body.declare_row("channel", "link");
    ruvia::db_query channel;
    channel.from("link").where(channel.binary(channel.column("id"), ruvia::db_binary_operator::equal, channel.column("link_id", "new"))).lock({.mode_ = ruvia::db_row_lock::share});
    const std::array<std::string_view, 1> targets{"channel"};
    body.select_into(channel, targets, true);
    body.begin_if(query.binary(query.column("protocol", "channel"), ruvia::db_binary_operator::not_equal, query.column("protocol", "new")));
    body.raise_exception("channel mismatch $ruvia$ 'quoted'", "23514");
    body.end_if();
    body.assign("new.revision", query.binary(query.column("revision", "old"), ruvia::db_binary_operator::add, query.value(1)));
    body.return_value(query.column("new"));
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_trigger_function("app.bind_channel", body);
    schema.create_trigger({.name_ = "bind_channel", .table_ = "app.device", .function_ = "app.bind_channel", .events_ = {ruvia::db_trigger_event::insert, ruvia::db_trigger_event::update}});
    const auto migrations = schema.compile("guard");
    RUVIA_CHECK(migrations[0].sql().find("\"channel\" \"link\"%ROWTYPE") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("FOR SHARE INTO STRICT \"channel\"") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("AS $ruvia1$") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("DO $ruvia2$") == 0);
    RUVIA_CHECK(migrations[0].sql().find("BEFORE INSERT OR UPDATE ON \"app\".\"device\" FOR EACH ROW EXECUTE FUNCTION \"app\".\"bind_channel\"()") != std::string_view::npos);
}

RUVIA_TEST(db_schema_timescale_policies_are_typed_function_calls_and_table_options) {
    ruvia::db_query query;
    auto interval = query.cast(query.value("7 days"), db_data_type::interval);
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_hypertable({.table_ = "telemetry.events", .time_column_ = "ts", .chunk_interval_ = interval, .if_not_exists_ = true});
    schema.set_compression("telemetry.events", {.segment_by_ = {"device_id"}, .order_by_ = {{"ts", ruvia::db_order_direction::desc}}});
    schema.add_compression_policy("telemetry.events", interval, true);
    schema.add_retention_policy("telemetry.events", query.cast(query.value("90 days"), db_data_type::interval));
    const auto migrations = schema.compile("storage");
    RUVIA_CHECK(migrations[0].sql().find("\"create_hypertable\"(E'\"telemetry\".\"events\"', \"by_range\"(E'ts', CAST(E'7 days' AS INTERVAL))") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("\"timescaledb\".\"compress\" = true") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("\"compress_after\" => CAST(E'7 days' AS INTERVAL)") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("\"drop_after\" => CAST(E'90 days' AS INTERVAL)") != std::string_view::npos);
}

RUVIA_TEST(db_schema_single_perform_and_control_flow_have_complete_blocks) {
    ruvia::db_query query;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.perform(query.call("pg_notify", {query.value("channel"), query.value("body $ruvia$")}));
    const auto migrations = schema.compile("notify");
    RUVIA_CHECK(migrations[0].sql().find("DO $ruvia1$ BEGIN PERFORM") == 0);
    ruvia::db_procedure body;
    body.begin_if(query.value(true));
    RUVIA_CHECK(throws_on([&] { (void)body.body(); }));
    body.end_if();
    RUVIA_CHECK(throws_on([&] { body.otherwise(); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    RUVIA_CHECK(throws_on([&] { maria.validate_constraint("t", "fk"); }));
    ruvia::db_query removed;
    removed.delete_from("t").returning({removed.column("id")});
    RUVIA_CHECK(throws_on([&] { schema.create_view("v", removed); }));
    ruvia::db_query read;
    read.with("removed", removed).from("removed");
    RUVIA_CHECK(throws_on([&] { schema.create_view("v", read); }));
}

RUVIA_TEST(db_schema_ddl_operations_and_table_options_render_for_both_drivers) {
    ruvia::db_query pg_expressions;
    const auto pg_default = pg_expressions.value("pending");
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_schema("cs_pg", true);
    pg.create_table({.name_ = "cs_pg.records",
        .columns_ = {{.name_ = "id",
                         .type_ = {.data_type_ = db_data_type::big_int},
                         .identity_ = ruvia::db_identity::always},
            {.name_ = "name",
                .type_ = {.data_type_ = db_data_type::varchar, .length_ = 32},
                .nullable_ = true,
                .unique_ = true},
            {.name_ = "state", .type_ = {.data_type_ = db_data_type::text}, .default_value_ = pg_default}},
        .if_not_exists_ = true,
        .unlogged_ = true});
    pg.rename_table("cs_pg.records", "records_renamed");
    pg.add_column("cs_pg.records_renamed",
        {.name_ = "created_at", .type_ = {.data_type_ = db_data_type::timestamp_tz}, .nullable_ = true}, true);
    pg.rename_column("cs_pg.records_renamed", "created_at", "created_on");
    pg.alter_column_type("cs_pg.records_renamed", "state", {.data_type_ = db_data_type::varchar, .length_ = 64},
        pg_expressions.column("state"));
    pg.set_column_nullable("cs_pg.records_renamed", "state", true);
    pg.set_column_nullable("cs_pg.records_renamed", "state", false);
    pg.set_column_default("cs_pg.records_renamed", "state", pg_expressions.value("ready"));
    pg.drop_column_default("cs_pg.records_renamed", "state");
    pg.drop_column("cs_pg.records_renamed", "created_on", ruvia::db_drop_behavior::cascade, true);
    pg.drop_table("cs_pg.records_renamed", ruvia::db_drop_behavior::cascade, true);
    pg.drop_schema("cs_pg", ruvia::db_drop_behavior::cascade, true);
    const auto pg_migrations = pg.compile("cs_pg_ops");
    RUVIA_CHECK_EQ(pg_migrations.size(), std::size_t{1});
    const auto pg_sql = pg_migrations[0].sql();
    RUVIA_CHECK(pg_sql.find("CREATE SCHEMA IF NOT EXISTS \"cs_pg\"") != std::string_view::npos);
    RUVIA_CHECK(pg_sql.find("CREATE UNLOGGED TABLE IF NOT EXISTS \"cs_pg\".\"records\"") != std::string_view::npos);
    RUVIA_CHECK(pg_sql.find("GENERATED ALWAYS AS IDENTITY") != std::string_view::npos);
    RUVIA_CHECK(pg_sql.find("VARCHAR(32) UNIQUE") != std::string_view::npos);
    RUVIA_CHECK(pg_sql.find("ALTER COLUMN \"state\" TYPE VARCHAR(64) USING \"state\"") != std::string_view::npos);
    RUVIA_CHECK(pg_sql.find("DROP COLUMN IF EXISTS \"created_on\" CASCADE") != std::string_view::npos);
    RUVIA_CHECK(pg_sql.find("DROP SCHEMA IF EXISTS \"cs_pg\" CASCADE") != std::string_view::npos);

    ruvia::db_query maria_expressions;
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_schema("cs_maria", true);
    maria.create_table({.name_ = "cs_maria.records",
        .columns_ = {{.name_ = "id",
                         .type_ = {.data_type_ = db_data_type::big_int},
                         .identity_ = ruvia::db_identity::by_default},
            {.name_ = "state",
                .type_ = {.data_type_ = db_data_type::varchar, .length_ = 32},
                .nullable_ = true,
                .default_value_ = maria_expressions.value("ready")}},
        .if_not_exists_ = true});
    maria.add_column("cs_maria.records", {.name_ = "note", .type_ = {.data_type_ = db_data_type::text}, .nullable_ = true}, true);
    maria.rename_column("cs_maria.records", "note", "memo");
    maria.set_column_default("cs_maria.records", "memo", maria_expressions.value("n/a"));
    maria.drop_column_default("cs_maria.records", "memo");
    maria.drop_column("cs_maria.records", "memo", ruvia::db_drop_behavior::cascade, true);
    maria.add_constraint("cs_maria.records", {.name_ = "records_state_unique", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"state"}});
    maria.drop_constraint("cs_maria.records", "records_state_unique", ruvia::db_drop_behavior::cascade, true);
    maria.rename_table("cs_maria.records", "records_renamed");
    maria.drop_table("cs_maria.records_renamed", ruvia::db_drop_behavior::cascade, true);
    RUVIA_CHECK(throws_on([&] { maria.drop_schema("cs_maria", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_schema("cs_maria", ruvia::db_drop_behavior::restrict, true);
    const auto maria_migrations = maria.compile("cs_maria_ops");
    RUVIA_CHECK_EQ(maria_migrations.size(), std::size_t{12});
    RUVIA_CHECK_EQ(maria_migrations[0].id(), "cs_maria_ops_1");
    RUVIA_CHECK(maria_migrations[1].sql().find("CREATE TABLE IF NOT EXISTS `cs_maria`.`records`") != std::string_view::npos);
    RUVIA_CHECK(maria_migrations[1].sql().find("AUTO_INCREMENT") != std::string_view::npos);
    RUVIA_CHECK(maria_migrations[2].sql().find("ADD COLUMN IF NOT EXISTS `note` TEXT") != std::string_view::npos);
    RUVIA_CHECK(maria_migrations[9].sql().find("ALTER TABLE `cs_maria`.`records` RENAME TO `cs_maria`.`records_renamed`") != std::string_view::npos);
    RUVIA_CHECK(maria_migrations[10].sql().find("DROP TABLE IF EXISTS `cs_maria`.`records_renamed` CASCADE") != std::string_view::npos);
    RUVIA_CHECK(throws_on([&] {
        maria.alter_column_type("cs_maria.records", "state", {.data_type_ = db_data_type::text});
    }));
    RUVIA_CHECK(throws_on([&] { maria.set_column_nullable("cs_maria.records", "state", true); }));
}

RUVIA_TEST(db_schema_constraints_cover_actions_deferred_not_valid_and_rejections) {
    ruvia::db_query expressions;
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.add_constraint("cs_child",
        {.name_ = "child_fk",
            .kind_ = ruvia::db_constraint_kind::foreign_key,
            .columns_ = {"parent_id"},
            .referenced_table_ = "cs_parent",
            .referenced_columns_ = {"id"},
            .on_delete_ = ruvia::db_referential_action::set_null,
            .on_update_ = ruvia::db_referential_action::set_default,
            .deferrable_ = true,
            .initially_deferred_ = true,
            .not_valid_ = true});
    pg.add_constraint("cs_child",
        {.name_ = "child_unique", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"parent_id"}, .deferrable_ = true});
    pg.add_constraint("cs_child",
        {.name_ = "child_check",
            .kind_ = ruvia::db_constraint_kind::check,
            .not_valid_ = true,
            .check_ = expressions.binary(expressions.column("amount"), ruvia::db_binary_operator::greater_equal,
                expressions.value(0))});
    pg.validate_constraint("cs_child", "child_fk");
    const auto migrations = pg.compile("cs_constraints");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("ON DELETE SET NULL ON UPDATE SET DEFAULT DEFERRABLE INITIALLY DEFERRED NOT VALID") != std::string_view::npos);
    RUVIA_CHECK(sql.find("CONSTRAINT \"child_unique\" UNIQUE (\"parent_id\") DEFERRABLE") != std::string_view::npos);
    RUVIA_CHECK(sql.find("CONSTRAINT \"child_check\" CHECK ((\"amount\" >= 0)) NOT VALID") != std::string_view::npos);
    RUVIA_CHECK(sql.find("VALIDATE CONSTRAINT \"child_fk\"") != std::string_view::npos);

    db_table_definition invalid_table{.name_ = "cs_invalid",
        .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::big_int}}},
        .constraints_ = {{.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .not_valid_ = true}}};
    RUVIA_CHECK(throws_on([&] { pg.create_table(invalid_table); }));
    RUVIA_CHECK(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .initially_deferred_ = true});
    }));
    RUVIA_CHECK(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::check, .deferrable_ = true, .check_ = expressions.value(true)});
    }));
    RUVIA_CHECK(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .not_valid_ = true});
    }));
    RUVIA_CHECK(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"id"}, .referenced_table_ = "cs_parent", .referenced_columns_ = {}});
    }));
    RUVIA_CHECK(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::check});
    }));

    db_schema maria({.driver_ = db_driver::mariadb});
    RUVIA_CHECK(throws_on([&] {
        maria.add_constraint("cs_child", {.name_ = "fk", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"id"}, .referenced_table_ = "cs_parent", .referenced_columns_ = {"id"}, .deferrable_ = true});
    }));
    RUVIA_CHECK(throws_on([&] {
        maria.add_constraint("cs_child", {.name_ = "check", .kind_ = ruvia::db_constraint_kind::check, .not_valid_ = true, .check_ = expressions.value(true)});
    }));
}

RUVIA_TEST(db_schema_constructor_table_flags_and_compile_boundaries_are_checked) {
    RUVIA_CHECK(throws_on([&] { db_schema invalid({}); }));
    db_schema empty({.driver_ = db_driver::postgresql});
    RUVIA_CHECK(empty.compile("cs_empty").empty());
    RUVIA_CHECK(throws_on([&] { (void)empty.compile(""); }));

    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_table({.name_ = "cs_temp_pg", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true});
    RUVIA_CHECK(pg.compile("cs_temp_pg")[0].sql().find("CREATE TEMPORARY TABLE") != std::string_view::npos);
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table({.name_ = "cs_temp_maria", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true});
    RUVIA_CHECK(maria.compile("cs_temp_maria")[0].sql().find("CREATE TEMPORARY TABLE") != std::string_view::npos);
    RUVIA_CHECK(throws_on([&] {
        maria.create_table({.name_ = "cs_invalid_flags", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true, .unlogged_ = true});
    }));
    RUVIA_CHECK(throws_on([&] {
        maria.create_table({.name_ = "cs_unlogged", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .unlogged_ = true});
    }));
    RUVIA_CHECK(throws_on([&] {
        maria.create_table({.name_ = "cs_always", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}, .identity_ = ruvia::db_identity::always}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        pg.create_table({.name_ = "cs_duplicate", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}, {.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}});
    }));
}

RUVIA_TEST(db_schema_index_variants_and_dialect_rejections) {
    ruvia::db_query expressions;
    const auto predicate = expressions.binary(expressions.column("active"), ruvia::db_binary_operator::equal, expressions.value(true));
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_index({.name_ = "cs_idx", .table_ = "cs_records", .keys_ = {{.column_ = "name", .order_ = ruvia::db_order_direction::desc, .nulls_ = ruvia::db_nulls_order::last, .operator_class_ = "text_ops"}, {.expression_ = expressions.column("lower_name")}}, .unique_ = true, .if_not_exists_ = true, .method_ = ruvia::db_index_method::btree, .include_ = {"id"}, .where_ = predicate});
    const auto pg_migrations = pg.compile("cs_index");
    RUVIA_CHECK_EQ(pg_migrations.size(), std::size_t{1});
    RUVIA_CHECK(pg_migrations[0].sql().find("CREATE UNIQUE INDEX IF NOT EXISTS \"cs_idx\" ON \"cs_records\" USING btree") != std::string_view::npos);
    RUVIA_CHECK(pg_migrations[0].sql().find("\"name\" \"text_ops\" DESC NULLS LAST, (\"lower_name\")") != std::string_view::npos);
    RUVIA_CHECK(pg_migrations[0].sql().find("INCLUDE (\"id\") WHERE (\"active\" = TRUE)") != std::string_view::npos);

    for (const auto method : {ruvia::db_index_method::hash, ruvia::db_index_method::gin, ruvia::db_index_method::gist,
             ruvia::db_index_method::sp_gist, ruvia::db_index_method::brin}) {
        db_schema method_schema({.driver_ = db_driver::postgresql});
        method_schema.create_index({.name_ = "cs_method", .table_ = "cs_records", .keys_ = {{.column_ = "name"}}, .method_ = method});
        RUVIA_CHECK(method_schema.compile("cs_method")[0].sql().find("USING ") != std::string_view::npos);
    }
    db_schema concurrent({.driver_ = db_driver::postgresql});
    concurrent.create_index({.name_ = "cs_concurrent", .table_ = "cs_records", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    const auto concurrent_migrations = concurrent.compile("cs_concurrent");
    RUVIA_CHECK_EQ(concurrent_migrations[0].atomicity(), ruvia::db_migration_atomicity::unwrapped);
    db_schema drop({.driver_ = db_driver::postgresql});
    drop.drop_index("cs_records.cs_idx", true, true);
    RUVIA_CHECK(drop.compile("cs_drop_index")[0].sql().find("DROP INDEX CONCURRENTLY IF EXISTS \"cs_records\".\"cs_idx\"") != std::string_view::npos);

    RUVIA_CHECK(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id", .expression_ = expressions.column("id")}}});
    }));
    RUVIA_CHECK(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}, {.column_ = ""}}});
    }));
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_index({.name_ = "cs_maria_idx", .table_ = "cs_records", .keys_ = {{.column_ = "name", .order_ = ruvia::db_order_direction::desc}}, .unique_ = true, .if_not_exists_ = true});
    RUVIA_CHECK(maria.compile("cs_maria_idx")[0].sql().find("CREATE UNIQUE INDEX IF NOT EXISTS `cs_maria_idx` USING btree ON `cs_records`") != std::string_view::npos);
    RUVIA_CHECK(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    }));
    RUVIA_CHECK(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .method_ = ruvia::db_index_method::hash});
    }));
    RUVIA_CHECK(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .include_ = {"extra"}});
    }));
    RUVIA_CHECK(throws_on([&] { maria.drop_index("idx"); }));
}

RUVIA_TEST(db_schema_views_enums_extensions_and_commands_cover_dialect_boundaries) {
    ruvia::db_query query;
    query.select(query.column("id")).from("cs_records");
    db_schema pg({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"new", "done"};
    pg.create_enum("cs_status", values);
    pg.drop_enum("cs_status", ruvia::db_drop_behavior::cascade, true);
    pg.create_extension("hstore", true);
    pg.create_view("cs_view", query, true);
    pg.drop_view("cs_view", false, ruvia::db_drop_behavior::cascade, true);
    pg.create_view("cs_mat", query, false, true);
    pg.drop_view("cs_mat", true, ruvia::db_drop_behavior::restrict, true);
    ruvia::db_query command;
    command.update("cs_records").set("state", command.value("done"));
    pg.execute(command);
    const auto migrations = pg.compile("cs_objects");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("CREATE TYPE \"cs_status\" AS ENUM (E'new',E'done')") != std::string_view::npos);
    RUVIA_CHECK(sql.find("DROP TYPE IF EXISTS \"cs_status\" CASCADE") != std::string_view::npos);
    RUVIA_CHECK(sql.find("CREATE EXTENSION IF NOT EXISTS \"hstore\"") != std::string_view::npos);
    RUVIA_CHECK(sql.find("CREATE OR REPLACE VIEW \"cs_view\" AS SELECT") != std::string_view::npos);
    RUVIA_CHECK(sql.find("DROP MATERIALIZED VIEW IF EXISTS \"cs_mat\"") != std::string_view::npos);
    RUVIA_CHECK(sql.find("UPDATE \"cs_records\" SET \"state\" = E'done'") != std::string_view::npos);
    db_schema enum_addition({.driver_ = db_driver::postgresql});
    enum_addition.add_enum_value("cs_status", "archived", true);
    const auto enum_migrations = enum_addition.compile("cs_enum_addition");
    RUVIA_CHECK_EQ(enum_migrations.size(), std::size_t{1});
    RUVIA_CHECK(enum_migrations[0].sql().find("ALTER TYPE \"cs_status\" ADD VALUE IF NOT EXISTS E'archived'") != std::string_view::npos);

    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_view("cs_view", query);
    maria.drop_view("cs_view", false, ruvia::db_drop_behavior::cascade, true);
    maria.execute(command);
    RUVIA_CHECK(maria.compile("cs_objects").size() == std::size_t{3});
    RUVIA_CHECK(throws_on([&] { maria.create_enum("status", values); }));
    RUVIA_CHECK(throws_on([&] { maria.add_enum_value("status", "x"); }));
    RUVIA_CHECK(throws_on([&] { maria.drop_enum("status"); }));
    RUVIA_CHECK(throws_on([&] { maria.create_extension("hstore"); }));
    RUVIA_CHECK(throws_on([&] { maria.create_view("mat", query, false, true); }));
    RUVIA_CHECK(throws_on([&] { maria.drop_view("mat", true); }));
    RUVIA_CHECK(throws_on([&] { pg.create_view("bad", command); }));
    RUVIA_CHECK(throws_on([&] { pg.create_view("bad", query, true, true); }));
    RUVIA_CHECK(throws_on([&] { pg.create_enum("empty", {}); }));
    ruvia::db_query returned;
    returned.delete_from("cs_records").returning({returned.column("id")});
    RUVIA_CHECK(throws_on([&] { pg.execute(returned); }));
}

RUVIA_TEST(db_schema_trigger_variants_cover_events_timing_when_arguments_and_rejections) {
    ruvia::db_query expressions;
    const auto when = expressions.binary(expressions.column("enabled"), ruvia::db_binary_operator::equal, expressions.value(true));
    db_procedure procedure;
    procedure.return_value();
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_trigger_function("cs_trigger_fn", procedure, true);
    pg.create_trigger({.name_ = "cs_before_insert", .table_ = "cs_records", .function_ = "cs_trigger_fn", .events_ = {ruvia::db_trigger_event::insert}});
    pg.create_trigger({.name_ = "cs_after_update", .table_ = "cs_records", .function_ = "cs_trigger_fn", .timing_ = ruvia::db_trigger_timing::after, .events_ = {ruvia::db_trigger_event::update}, .update_columns_ = {"state", "enabled"}, .when_ = when, .arguments_ = {"arg 'one'", "two"}});
    pg.create_trigger({.name_ = "cs_statement", .table_ = "cs_records", .function_ = "cs_trigger_fn", .events_ = {ruvia::db_trigger_event::delete_value, ruvia::db_trigger_event::truncate}, .for_each_row_ = false});
    pg.create_trigger({.name_ = "cs_instead", .table_ = "cs_records_view", .function_ = "cs_trigger_fn", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}});
    pg.drop_trigger("cs_records", "cs_after_update", true);
    pg.drop_trigger_function("cs_trigger_fn", ruvia::db_drop_behavior::cascade, true);
    const auto migrations = pg.compile("cs_triggers");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("CREATE OR REPLACE FUNCTION \"cs_trigger_fn\"() RETURNS trigger LANGUAGE plpgsql") != std::string_view::npos);
    RUVIA_CHECK(sql.find("AFTER UPDATE OF \"state\", \"enabled\" ON \"cs_records\" FOR EACH ROW WHEN ((\"enabled\" = TRUE)) EXECUTE FUNCTION \"cs_trigger_fn\"(E'arg ''one''', E'two')") != std::string_view::npos);
    RUVIA_CHECK(sql.find("AFTER UPDATE OF") != std::string_view::npos);
    RUVIA_CHECK(sql.find("DELETE OR TRUNCATE ON \"cs_records\" FOR EACH STATEMENT") != std::string_view::npos);
    RUVIA_CHECK(sql.find("INSTEAD OF INSERT ON \"cs_records_view\" FOR EACH ROW") != std::string_view::npos);
    RUVIA_CHECK(sql.find("DROP TRIGGER IF EXISTS \"cs_after_update\" ON \"cs_records\"") != std::string_view::npos);
    RUVIA_CHECK(sql.find("DROP FUNCTION IF EXISTS \"cs_trigger_fn\"() CASCADE") != std::string_view::npos);

    RUVIA_CHECK(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f"}); }));
    RUVIA_CHECK(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert, ruvia::db_trigger_event::insert}}); }));
    RUVIA_CHECK(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert}, .update_columns_ = {"x"}}); }));
    RUVIA_CHECK(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::truncate}}); }));
    RUVIA_CHECK(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}, .for_each_row_ = false}); }));
    RUVIA_CHECK(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}, .when_ = when}); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    RUVIA_CHECK(throws_on([&] { maria.create_trigger_function("f", procedure); }));
    RUVIA_CHECK(throws_on([&] { maria.create_trigger({.name_ = "f", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert}}); }));
    RUVIA_CHECK(throws_on([&] { maria.drop_trigger("t", "f"); }));
    RUVIA_CHECK(throws_on([&] { maria.perform(expressions.value(true)); }));
    RUVIA_CHECK(throws_on([&] { maria.run(procedure); }));
}

RUVIA_TEST(db_schema_timescale_options_cover_direct_settings_and_rejections) {
    ruvia::db_query expressions;
    const auto interval = expressions.cast(expressions.value("1 day"), db_data_type::interval);
    db_schema pg({.driver_ = db_driver::postgresql});
    const std::array table_options{db_table_option{"autovacuum_enabled", true}, db_table_option{"fillfactor", std::int64_t{80}}, db_table_option{"toast_tuple_target", "128"}};
    pg.set_table_options("cs_events", table_options);
    pg.set_database_option("cs_db", "timescaledb.max_background_workers", "8");
    pg.set_chunk_time_interval("cs_events", interval);
    pg.set_compression("cs_events", {.enabled_ = false});
    pg.remove_compression_policy("cs_events", true);
    pg.remove_retention_policy("cs_events", true);
    const auto migrations = pg.compile("cs_timescale_options");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("ALTER TABLE \"cs_events\" SET (\"autovacuum_enabled\" = true, \"fillfactor\" = 80, \"toast_tuple_target\" = E'128')") != std::string_view::npos);
    RUVIA_CHECK(sql.find("ALTER DATABASE \"cs_db\" SET \"timescaledb\".\"max_background_workers\" TO E'8'") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"set_chunk_time_interval\"(E'\"cs_events\"', CAST(E'1 day' AS INTERVAL))") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"timescaledb\".\"compress\" = false") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"remove_compression_policy\"(E'\"cs_events\"', \"if_exists\" => TRUE)") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"remove_retention_policy\"(E'\"cs_events\"', \"if_exists\" => TRUE)") != std::string_view::npos);
    RUVIA_CHECK(throws_on([&] { pg.set_table_options("cs_events", std::span<const db_table_option>()); }));
    const std::array duplicate_options{db_table_option{"fillfactor", std::int64_t{80}}, db_table_option{"fillfactor", std::int64_t{90}}};
    RUVIA_CHECK(throws_on([&] {
        pg.set_table_options("cs_events", duplicate_options);
    }));
    RUVIA_CHECK(throws_on([&] { pg.set_compression("cs_events", {.enabled_ = false, .segment_by_ = {"device_id"}}); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    const std::array maria_options{db_table_option{"x", true}};
    RUVIA_CHECK(throws_on([&] { maria.set_table_options("t", maria_options); }));
    RUVIA_CHECK(throws_on([&] { maria.set_database_option("d", "x", "y"); }));
    RUVIA_CHECK(throws_on([&] { maria.create_hypertable({.table_ = "t", .time_column_ = "ts"}); }));
    RUVIA_CHECK(throws_on([&] { maria.set_chunk_time_interval("t", interval); }));
    RUVIA_CHECK(throws_on([&] { maria.set_compression("t", {}); }));
    RUVIA_CHECK(throws_on([&] { maria.add_compression_policy("t", interval); }));
    RUVIA_CHECK(throws_on([&] { maria.remove_compression_policy("t"); }));
    RUVIA_CHECK(throws_on([&] { maria.add_retention_policy("t", interval); }));
    RUVIA_CHECK(throws_on([&] { maria.remove_retention_policy("t"); }));
}

RUVIA_TEST(db_procedure_public_helpers_render_and_validate) {
    ruvia::db_query expressions;
    ruvia::db_procedure procedure;
    procedure.declare_variable({.name_ = "counter", .type_ = {.data_type_ = db_data_type::integer}, .default_value_ = expressions.value(0)});
    procedure.declare_row("row_value", "cs_records");
    procedure.declare_record("record_value");
    procedure.assign("counter", expressions.binary(expressions.column("counter"), ruvia::db_binary_operator::add, expressions.value(1)));
    ruvia::db_query select;
    select.select(select.column("id")).from("cs_records");
    const std::array<std::string_view, 1> targets{"row_value"};
    procedure.select_into(select, targets, false);
    procedure.select_into(select, targets, true);
    ruvia::db_query command;
    command.update("cs_records").set("state", command.value("done"));
    procedure.execute(command);
    procedure.perform(expressions.call("pg_notify", {expressions.value("cs"), expressions.value("body")}));
    procedure.begin_if(expressions.column("enabled"));
    procedure.else_if(expressions.column("fallback"));
    procedure.otherwise();
    procedure.end_if();
    procedure.begin_block();
    procedure.catch_sql_state("23505");
    procedure.end_block();
    procedure.return_value();
    procedure.raise_exception("failed", "P0001");
    const auto body = procedure.body();
    RUVIA_CHECK(body.find("DECLARE\n") == 0);
    RUVIA_CHECK(body.find("\"counter\" INTEGER := 0;") != std::string_view::npos);
    RUVIA_CHECK(body.find("\"row_value\" \"cs_records\"%ROWTYPE;") != std::string_view::npos);
    RUVIA_CHECK(body.find("SELECT \"id\" FROM \"cs_records\" INTO \"row_value\";") != std::string_view::npos);
    RUVIA_CHECK(body.find("INTO STRICT \"row_value\";") != std::string_view::npos);
    RUVIA_CHECK(body.find("ELSIF \"fallback\" THEN") != std::string_view::npos);
    RUVIA_CHECK(body.find("EXCEPTION\nWHEN SQLSTATE E'23505'") != std::string_view::npos);
    RUVIA_CHECK(body.find("RETURN;\nRAISE EXCEPTION USING MESSAGE = E'failed'") != std::string_view::npos);

    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_schema("cs_nested");
    ruvia::db_procedure applied;
    applied.apply(pg);
    RUVIA_CHECK(applied.body().find("CREATE SCHEMA \"cs_nested\";") != std::string_view::npos);
    db_schema non_transactional({.driver_ = db_driver::postgresql});
    non_transactional.create_index({.name_ = "cs_idx", .table_ = "cs_records", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    RUVIA_CHECK(throws_on([&] { applied.apply(non_transactional); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    RUVIA_CHECK(throws_on([&] { applied.apply(maria); }));

    RUVIA_CHECK(throws_on([&] { procedure.declare_record("counter"); }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.execute(select);
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.select_into(command, targets);
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.select_into(select, {});
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.else_if(expressions.value(true));
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.otherwise();
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.end_if();
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.catch_sql_state("23505");
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.begin_block();
        invalid.catch_sql_state("bad");
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.end_block();
    }));
    RUVIA_CHECK(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.raise_exception("bad", "00000");
    }));
}
