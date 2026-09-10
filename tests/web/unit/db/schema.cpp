#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/web/db/DbSchema.h"

#include "test_harness.h"

using ruvia::DbDataType;
using ruvia::DbDriver;
using ruvia::DbGeneratedType;
using ruvia::DbSchema;
using ruvia::DbSchemaColumn;
using ruvia::DbSchemaConstraint;
using ruvia::DbTableDefinition;
using ruvia::testing::throwsOn;

using SchemaTarget = ruvia::DbEntity<"schema_targets", ruvia::DbColumn<"id", std::int64_t, ruvia::DbColumnOptions{.primaryKey = true}>,
    ruvia::DbColumn<"tenant_id", std::int64_t, ruvia::DbColumnOptions{.primaryKey = true}>>;
using SchemaOwner = ruvia::DbEntity<"schema_owners",
    ruvia::DbColumn<"id", std::int64_t, ruvia::DbColumnOptions{.primaryKey = true}>,
    ruvia::DbColumn<"parent_id", std::int64_t>, ruvia::DbColumn<"parent_tenant_id", std::int64_t>,
    ruvia::DbColumn<"peer_id", std::int64_t>,
    ruvia::DbManyToOne<"parent", SchemaTarget, ruvia::DbJoinColumn<"parent_id", "id">,
        ruvia::DbJoinColumn<"parent_tenant_id", "tenant_id">>,
    ruvia::DbOneToOne<"peer", SchemaTarget, ruvia::DbJoinColumn<"peer_id", "id">>,
    ruvia::DbOneToMany<"children", SchemaTarget, "parent">,
    ruvia::DbManyToMany<"labels", SchemaTarget, ruvia::DbJoinTable<"owner_labels", ruvia::DbJoinColumns<ruvia::DbJoinColumn<"owner_id", "id">>, ruvia::DbJoinColumns<ruvia::DbJoinColumn<"label_id", "id">, ruvia::DbJoinColumn<"label_tenant_id", "tenant_id">>>>>;

RUVIA_TEST(db_schema_create_table_composite_foreign_key_and_nullable_default) {
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    DbTableDefinition table{.name = "tenant_users", .columns = {DbSchemaColumn{.name = "tenant_id", .type = {.dataType = DbDataType::kBigInt}}, DbSchemaColumn{.name = "user_id", .type = {.dataType = DbDataType::kBigInt}, .nullable = true}}, .constraints = {DbSchemaConstraint{.name = "tenant_users_pk", .kind = ruvia::DbConstraintKind::kPrimaryKey, .columns = {"tenant_id", "user_id"}}, DbSchemaConstraint{.name = "tenant_fk", .kind = ruvia::DbConstraintKind::kForeignKey, .columns = {"tenant_id", "user_id"}, .referencedTable = "tenants", .referencedColumns = {"id", "id"}, .onDelete = ruvia::DbReferentialAction::kCascade}}};
    schema.createTable(table);
    const auto migrations = schema.compile("001_create");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    RUVIA_CHECK(migrations[0].sql().find("PRIMARY KEY") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("FOREIGN KEY") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("ON DELETE CASCADE") != std::string_view::npos);
}

RUVIA_TEST(db_schema_partial_gin_index_quotes_identifiers_and_literals) {
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    ruvia::DbQuery query;
    const auto value = query.value(ruvia::DbValue(std::string_view("x' OR 1=1")));
    const auto predicate = query.binary(query.column("state"), ruvia::DbBinaryOperator::kEqual, value);
    schema.createIndex({.name = "events_idx", .table = "events", .keys = {{.column = "payload"}}, .method = ruvia::DbIndexMethod::kGin, .where = predicate});
    const auto migrations = schema.compile("002_index");
    RUVIA_CHECK(migrations[0].sql().find("USING gin") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("x'' OR 1=1") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("DROP TABLE") == std::string_view::npos);
}

RUVIA_TEST(db_schema_postgresql_batch_is_atomic_and_unwrapped_isolated) {
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createSchema("one");
    schema.createSchema("two");
    auto migrations = schema.compile("003_batch");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    RUVIA_CHECK(migrations[0].sql().find("DO $ruvia$ BEGIN") == 0);

    DbSchema invalid({.driver = DbDriver::kPostgreSql});
    invalid.createSchema("one");
    invalid.createIndex({.name = "idx", .table = "t", .keys = {{.column = "x"}}, .concurrently = true});
    RUVIA_CHECK(throwsOn([&] { (void)invalid.compile("004_invalid"); }));
}

RUVIA_TEST(db_schema_enum_and_postgresql_only_operations_reject_mariadb) {
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    const std::array<std::string_view, 2> values{"open", "closed"};
    schema.createEnum("status", values);
    auto migrations = schema.compile("005_enum");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    DbSchema addition({.driver = DbDriver::kPostgreSql});
    addition.addEnumValue("status", "paused");
    const auto added = addition.compile("006_enum_value");
    RUVIA_CHECK(added[0].atomicity() == ruvia::DbMigrationAtomicity::kUnwrapped);

    DbSchema maria({.driver = DbDriver::kMariaDb});
    RUVIA_CHECK(throwsOn([&] { maria.createEnum("status", values); }));
}

RUVIA_TEST(db_schema_entity_metadata_preserves_explicit_array_element_types) {
    using Entity = ruvia::DbEntity<"app.events",
        ruvia::DbColumn<"id", std::int64_t, ruvia::DbColumnOptions{.primaryKey = true, .generated = true}>,
        ruvia::DbColumn<"payloads", std::pmr::vector<std::pmr::string>, ruvia::DbColumnOptions{.dataType = DbDataType::kJsonb}>>;
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createTable<Entity>();
    const auto migrations = schema.compile("entity");
    RUVIA_CHECK_EQ(migrations[0].sql(), "CREATE TABLE \"app\".\"events\" (\"id\" BIGINT GENERATED BY DEFAULT AS IDENTITY NOT NULL, \"payloads\" JSONB[] NOT NULL, CONSTRAINT \"events_pkey\" PRIMARY KEY (\"id\"))");
}

RUVIA_TEST(db_schema_entity_computed_columns_render_and_validate) {
    using Entity = ruvia::DbEntity<"computed_events",
        ruvia::DbColumn<"first_name", ruvia::String>,
        ruvia::DbColumn<"last_name", ruvia::String>,
        ruvia::DbColumn<"display_name", ruvia::String,
            ruvia::DbColumnOptions{.generatedType = DbGeneratedType::kStored}>>;
    ruvia::DbQuery expression;
    auto generated = expression.binary(expression.column("first_name"), ruvia::DbBinaryOperator::kConcat,
        expression.column("last_name"));
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createTable<Entity>({.generatedColumns = {{.column = "display_name", .expression = generated}}});
    const auto migrations = schema.compile("computed");
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("\"display_name\" TEXT GENERATED ALWAYS AS") != std::string_view::npos);
    RUVIA_CHECK(sql.find(" STORED") != std::string_view::npos);
    RUVIA_CHECK(throwsOn([&] { schema.createTable<Entity>(); }));

    using MariaEntity = ruvia::DbEntity<"maria_computed",
        ruvia::DbColumn<"value", std::int64_t>,
        ruvia::DbColumn<"double_value", std::int64_t,
            ruvia::DbColumnOptions{.generatedType = DbGeneratedType::kVirtual, .nullable = true}>>;
    ruvia::DbQuery mariaExpression;
    auto doubled = mariaExpression.binary(mariaExpression.column("value"), ruvia::DbBinaryOperator::kMultiply,
        mariaExpression.value(2));
    DbSchema maria({.driver = DbDriver::kMariaDb});
    maria.createTable<MariaEntity>({.generatedColumns = {{.column = "double_value", .expression = doubled}}});
    RUVIA_CHECK(maria.compile("maria_computed")[0].sql().find(" VIRTUAL") != std::string_view::npos);
    RUVIA_CHECK(throwsOn([&] {
        maria.addColumn("maria_computed", {.name = "invalid", .type = {.dataType = DbDataType::kBigInt}, .generatedType = DbGeneratedType::kStored, .asExpression = doubled});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema pg({.driver = DbDriver::kPostgreSql});
        pg.createTable<MariaEntity>({.generatedColumns = {{.column = "double_value", .expression = doubled}}});
    }));
}

RUVIA_TEST(db_schema_computed_columns_reject_invalid_metadata_and_conflicts) {
    using Computed = ruvia::DbEntity<"invalid_computed",
        ruvia::DbColumn<"value", std::int64_t>,
        ruvia::DbColumn<"total", std::int64_t,
            ruvia::DbColumnOptions{.generatedType = DbGeneratedType::kStored}>>;
    ruvia::DbQuery query;
    const auto expression = query.binary(query.column("value"), ruvia::DbBinaryOperator::kAdd, query.value(1));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema schema({.driver = DbDriver::kPostgreSql});
        schema.createTable<Computed>({.generatedColumns = {{.column = "missing", .expression = expression}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema schema({.driver = DbDriver::kPostgreSql});
        schema.createTable<Computed>({.generatedColumns = {{"total", expression}, {"total", expression}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema schema({.driver = DbDriver::kPostgreSql});
        schema.createTable<Computed>({.generatedColumns = {{"value", expression}, {"total", expression}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema schema({.driver = DbDriver::kPostgreSql});
        schema.createTable<Computed>({.generatedColumns = {{.column = "total", .expression = {}}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema schema({.driver = DbDriver::kPostgreSql});
        schema.createTable<Computed>({.defaults = {{.column = "total", .value = expression}},
            .generatedColumns = {{.column = "total", .expression = expression}}});
    }));
    using IdentityComputed = ruvia::DbEntity<"identity_computed",
        ruvia::DbColumn<"id", std::int64_t,
            ruvia::DbColumnOptions{.generated = true, .generatedType = DbGeneratedType::kStored}>>;
    RUVIA_CHECK(throwsOn([&] {
        DbSchema schema({.driver = DbDriver::kPostgreSql});
        schema.createTable<IdentityComputed>({.generatedColumns = {{.column = "id", .expression = expression}}});
    }));
}

RUVIA_TEST(db_schema_mariadb_computed_columns_reject_table_primary_keys) {
    using Entity = ruvia::DbEntity<"computed_keys",
        ruvia::DbColumn<"source", std::int64_t>,
        ruvia::DbColumn<"computed", std::int64_t,
            ruvia::DbColumnOptions{.primaryKey = true, .generatedType = DbGeneratedType::kStored, .nullable = true}>>;
    ruvia::DbQuery query;
    auto expression = query.column("source");
    DbSchema schema({.driver = DbDriver::kMariaDb});
    RUVIA_CHECK(throwsOn([&] {
        schema.createTable<Entity>({.generatedColumns = {{"computed", expression}}});
    }));
    for (const bool composite : {false, true}) {
        DbTableDefinition table{.name = "computed_keys", .columns = {{.name = "source", .type = {.dataType = DbDataType::kBigInt}}, {.name = "computed", .type = {.dataType = DbDataType::kBigInt}, .nullable = true, .generatedType = DbGeneratedType::kStored, .asExpression = expression}}, .constraints = {{.name = "computed_key", .kind = ruvia::DbConstraintKind::kPrimaryKey, .columns = {"computed"}}}};
        if (composite) {
            table.constraints[0].columns.push_back("source");
        }
        RUVIA_CHECK(throwsOn([&] { schema.createTable(table); }));
    }
}

RUVIA_TEST(db_schema_entity_relations_emit_owning_foreign_keys_and_one_to_one_unique) {
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createTable<SchemaOwner>();
    const auto migrations = schema.compile("relations");
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("FOREIGN KEY (\"parent_id\",\"parent_tenant_id\") REFERENCES \"schema_targets\" (\"id\",\"tenant_id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("FOREIGN KEY (\"peer_id\") REFERENCES \"schema_targets\" (\"id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("CONSTRAINT \"schema_owners_peer_key\" UNIQUE (\"peer_id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("children") == std::string_view::npos);
}

RUVIA_TEST(db_schema_many_to_many_relation_table_has_composite_primary_key_and_two_foreign_keys) {
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createRelationTables<SchemaOwner>();
    const auto migrations = schema.compile("relation_tables");
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("CREATE TABLE \"owner_labels\"") != std::string_view::npos);
    RUVIA_CHECK(sql.find("PRIMARY KEY (\"owner_id\",\"label_id\",\"label_tenant_id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("FOREIGN KEY (\"owner_id\") REFERENCES \"schema_owners\" (\"id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("FOREIGN KEY (\"label_id\",\"label_tenant_id\") REFERENCES \"schema_targets\" (\"id\",\"tenant_id\")") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"label_id\" BIGINT NOT NULL") != std::string_view::npos);
}

RUVIA_TEST(db_schema_qualified_types_and_replace_view_use_valid_ddl_grammar) {
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    const std::array<std::string_view, 2> values{"on", "off"};
    schema.createEnum("app.state", values);
    schema.createTable({.name = "app.devices", .columns = {{.name = "state", .type = {.customName = "app.state"}}}});
    ruvia::DbQuery query;
    query.select(query.column("state")).from("app.devices");
    schema.createView("app.device_state", query, true);
    const auto migrations = schema.compile("types");
    RUVIA_CHECK(migrations[0].sql().find("CREATE TYPE \"app\".\"state\" AS ENUM") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("\"state\" \"app\".\"state\" NOT NULL") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("CREATE OR REPLACE VIEW \"app\".\"device_state\" AS SELECT") != std::string_view::npos);
}

RUVIA_TEST(db_schema_procedure_builds_channel_guard_and_escapes_body_delimiters) {
    ruvia::DbQuery query;
    ruvia::DbProcedure body;
    body.declareRow("channel", "link");
    ruvia::DbQuery channel;
    channel.from("link").where(channel.binary(channel.column("id"), ruvia::DbBinaryOperator::kEqual, channel.column("link_id", "new"))).lock({.mode = ruvia::DbRowLock::kShare});
    const std::array<std::string_view, 1> targets{"channel"};
    body.selectInto(channel, targets, true);
    body.beginIf(query.binary(query.column("protocol", "channel"), ruvia::DbBinaryOperator::kNotEqual, query.column("protocol", "new")));
    body.raiseException("channel mismatch $ruvia$ 'quoted'", "23514");
    body.endIf();
    body.assign("new.revision", query.binary(query.column("revision", "old"), ruvia::DbBinaryOperator::kAdd, query.value(1)));
    body.returnValue(query.column("new"));
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createTriggerFunction("app.bind_channel", body);
    schema.createTrigger({.name = "bind_channel", .table = "app.device", .function = "app.bind_channel", .events = {ruvia::DbTriggerEvent::kInsert, ruvia::DbTriggerEvent::kUpdate}});
    const auto migrations = schema.compile("guard");
    RUVIA_CHECK(migrations[0].sql().find("\"channel\" \"link\"%ROWTYPE") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("FOR SHARE INTO STRICT \"channel\"") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("AS $ruvia1$") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("DO $ruvia2$") == 0);
    RUVIA_CHECK(migrations[0].sql().find("BEFORE INSERT OR UPDATE ON \"app\".\"device\" FOR EACH ROW EXECUTE FUNCTION \"app\".\"bind_channel\"()") != std::string_view::npos);
}

RUVIA_TEST(db_schema_timescale_policies_are_typed_function_calls_and_table_options) {
    ruvia::DbQuery query;
    auto interval = query.cast(query.value("7 days"), DbDataType::kInterval);
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createHypertable({.table = "telemetry.events", .timeColumn = "ts", .chunkInterval = interval, .ifNotExists = true});
    schema.setCompression("telemetry.events", {.segmentBy = {"device_id"}, .orderBy = {{"ts", ruvia::DbOrderDirection::kDesc}}});
    schema.addCompressionPolicy("telemetry.events", interval, true);
    schema.addRetentionPolicy("telemetry.events", query.cast(query.value("90 days"), DbDataType::kInterval));
    const auto migrations = schema.compile("storage");
    RUVIA_CHECK(migrations[0].sql().find("\"create_hypertable\"(E'\"telemetry\".\"events\"', \"by_range\"(E'ts', CAST(E'7 days' AS INTERVAL))") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("\"timescaledb\".\"compress\" = true") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("\"compress_after\" => CAST(E'7 days' AS INTERVAL)") != std::string_view::npos);
    RUVIA_CHECK(migrations[0].sql().find("\"drop_after\" => CAST(E'90 days' AS INTERVAL)") != std::string_view::npos);
}

RUVIA_TEST(db_schema_single_perform_and_control_flow_have_complete_blocks) {
    ruvia::DbQuery query;
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.perform(query.call("pg_notify", {query.value("channel"), query.value("body $ruvia$")}));
    const auto migrations = schema.compile("notify");
    RUVIA_CHECK(migrations[0].sql().find("DO $ruvia1$ BEGIN PERFORM") == 0);
    ruvia::DbProcedure body;
    body.beginIf(query.value(true));
    RUVIA_CHECK(throwsOn([&] { (void)body.body(); }));
    body.endIf();
    RUVIA_CHECK(throwsOn([&] { body.otherwise(); }));
    DbSchema maria({.driver = DbDriver::kMariaDb});
    RUVIA_CHECK(throwsOn([&] { maria.validateConstraint("t", "fk"); }));
    ruvia::DbQuery removed;
    removed.deleteFrom("t").returning({removed.column("id")});
    RUVIA_CHECK(throwsOn([&] { schema.createView("v", removed); }));
    ruvia::DbQuery read;
    read.with("removed", removed).from("removed");
    RUVIA_CHECK(throwsOn([&] { schema.createView("v", read); }));
}
