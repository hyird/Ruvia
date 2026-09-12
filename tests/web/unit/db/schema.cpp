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
using ruvia::DbProcedure;
using ruvia::DbSchema;
using ruvia::DbSchemaColumn;
using ruvia::DbSchemaConstraint;
using ruvia::DbTableDefinition;
using ruvia::DbTableOption;
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

RUVIA_TEST(db_schema_char_entity_and_column_operations_render_with_dialect_limits) {
    using Entity = ruvia::DbEntity<"char_records",
        ruvia::DbColumn<"code", std::pmr::string, ruvia::DbColumnOptions{.dataType = DbDataType::kChar, .length = 64}>,
        ruvia::DbColumn<"codes", std::pmr::vector<std::pmr::string>, ruvia::DbColumnOptions{.dataType = DbDataType::kChar, .length = 8}>>;
    DbSchema pg({.driver = DbDriver::kPostgreSql});
    pg.createTable<Entity>();
    pg.alterColumnType("char_records", "code", {.dataType = DbDataType::kChar, .length = 32});
    const auto pgMigrations = pg.compile("char_pg");
    RUVIA_CHECK(pgMigrations[0].sql().find("\"code\" CHAR(64) NOT NULL") != std::string_view::npos);
    RUVIA_CHECK(pgMigrations[0].sql().find("\"codes\" CHAR(8)[] NOT NULL") != std::string_view::npos);
    RUVIA_CHECK(pgMigrations[0].sql().find("ALTER TABLE \"char_records\" ALTER COLUMN \"code\" TYPE CHAR(32)") != std::string_view::npos);

    DbSchema maria({.driver = DbDriver::kMariaDb});
    maria.createTable({.name = "char_records", .columns = {{.name = "code", .type = {.dataType = DbDataType::kChar, .length = 255}}}});
    RUVIA_CHECK(maria.compile("char_maria")[0].sql().find("`code` CHAR(255) NOT NULL") != std::string_view::npos);

    DbSchema pgLimit({.driver = DbDriver::kPostgreSql});
    pgLimit.createTable({.name = "char_limit", .columns = {{.name = "code", .type = {.dataType = DbDataType::kChar, .length = 10485760}}}});
    RUVIA_CHECK(pgLimit.compile("char_pg_limit")[0].sql().find("\"code\" CHAR(10485760) NOT NULL") != std::string_view::npos);

    RUVIA_CHECK(throwsOn([&] {
        DbSchema invalid({.driver = DbDriver::kPostgreSql});
        invalid.createTable({.name = "invalid_char", .columns = {{.name = "code", .type = {.dataType = DbDataType::kChar}}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema invalid({.driver = DbDriver::kPostgreSql});
        invalid.createTable({.name = "invalid_char", .columns = {{.name = "code", .type = {.dataType = DbDataType::kChar, .length = 10485761}}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema invalid({.driver = DbDriver::kMariaDb});
        invalid.createTable({.name = "invalid_char", .columns = {{.name = "code", .type = {.dataType = DbDataType::kChar, .length = 256}}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema invalid({.driver = DbDriver::kPostgreSql});
        invalid.createTable({.name = "invalid_char", .columns = {{.name = "code", .type = {.dataType = DbDataType::kChar, .length = 8, .precision = 2}}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema invalid({.driver = DbDriver::kPostgreSql});
        invalid.createTable({.name = "invalid_char", .columns = {{.name = "code", .type = {.dataType = DbDataType::kText, .length = 8}}}});
    }));
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

RUVIA_TEST(db_schema_ddl_operations_and_table_options_render_for_both_drivers) {
    ruvia::DbQuery pgExpressions;
    const auto pgDefault = pgExpressions.value("pending");
    DbSchema pg({.driver = DbDriver::kPostgreSql});
    pg.createSchema("cs_pg", true);
    pg.createTable({.name = "cs_pg.records",
        .columns = {{.name = "id",
                        .type = {.dataType = DbDataType::kBigInt},
                        .identity = ruvia::DbIdentity::kAlways},
            {.name = "name",
                .type = {.dataType = DbDataType::kVarchar, .length = 32},
                .nullable = true,
                .unique = true},
            {.name = "state", .type = {.dataType = DbDataType::kText}, .defaultValue = pgDefault}},
        .ifNotExists = true,
        .unlogged = true});
    pg.renameTable("cs_pg.records", "records_renamed");
    pg.addColumn("cs_pg.records_renamed",
        {.name = "created_at", .type = {.dataType = DbDataType::kTimestampTz}, .nullable = true}, true);
    pg.renameColumn("cs_pg.records_renamed", "created_at", "created_on");
    pg.alterColumnType("cs_pg.records_renamed", "state", {.dataType = DbDataType::kVarchar, .length = 64},
        pgExpressions.column("state"));
    pg.setColumnNullable("cs_pg.records_renamed", "state", true);
    pg.setColumnNullable("cs_pg.records_renamed", "state", false);
    pg.setColumnDefault("cs_pg.records_renamed", "state", pgExpressions.value("ready"));
    pg.dropColumnDefault("cs_pg.records_renamed", "state");
    pg.dropColumn("cs_pg.records_renamed", "created_on", ruvia::DbDropBehavior::kCascade, true);
    pg.dropTable("cs_pg.records_renamed", ruvia::DbDropBehavior::kCascade, true);
    pg.dropSchema("cs_pg", ruvia::DbDropBehavior::kCascade, true);
    const auto pgMigrations = pg.compile("cs_pg_ops");
    RUVIA_CHECK_EQ(pgMigrations.size(), std::size_t{1});
    const auto pgSql = pgMigrations[0].sql();
    RUVIA_CHECK(pgSql.find("CREATE SCHEMA IF NOT EXISTS \"cs_pg\"") != std::string_view::npos);
    RUVIA_CHECK(pgSql.find("CREATE UNLOGGED TABLE IF NOT EXISTS \"cs_pg\".\"records\"") != std::string_view::npos);
    RUVIA_CHECK(pgSql.find("GENERATED ALWAYS AS IDENTITY") != std::string_view::npos);
    RUVIA_CHECK(pgSql.find("VARCHAR(32) UNIQUE") != std::string_view::npos);
    RUVIA_CHECK(pgSql.find("ALTER COLUMN \"state\" TYPE VARCHAR(64) USING \"state\"") != std::string_view::npos);
    RUVIA_CHECK(pgSql.find("DROP COLUMN IF EXISTS \"created_on\" CASCADE") != std::string_view::npos);
    RUVIA_CHECK(pgSql.find("DROP SCHEMA IF EXISTS \"cs_pg\" CASCADE") != std::string_view::npos);

    ruvia::DbQuery mariaExpressions;
    DbSchema maria({.driver = DbDriver::kMariaDb});
    maria.createSchema("cs_maria", true);
    maria.createTable({.name = "cs_maria.records",
        .columns = {{.name = "id",
                        .type = {.dataType = DbDataType::kBigInt},
                        .identity = ruvia::DbIdentity::kByDefault},
            {.name = "state",
                .type = {.dataType = DbDataType::kVarchar, .length = 32},
                .nullable = true,
                .defaultValue = mariaExpressions.value("ready")}},
        .ifNotExists = true});
    maria.addColumn("cs_maria.records", {.name = "note", .type = {.dataType = DbDataType::kText}, .nullable = true}, true);
    maria.renameColumn("cs_maria.records", "note", "memo");
    maria.setColumnDefault("cs_maria.records", "memo", mariaExpressions.value("n/a"));
    maria.dropColumnDefault("cs_maria.records", "memo");
    maria.dropColumn("cs_maria.records", "memo", ruvia::DbDropBehavior::kCascade, true);
    maria.addConstraint("cs_maria.records", {.name = "records_state_unique", .kind = ruvia::DbConstraintKind::kUnique, .columns = {"state"}});
    maria.dropConstraint("cs_maria.records", "records_state_unique", ruvia::DbDropBehavior::kCascade, true);
    maria.renameTable("cs_maria.records", "records_renamed");
    maria.dropTable("cs_maria.records_renamed", ruvia::DbDropBehavior::kCascade, true);
    RUVIA_CHECK(throwsOn([&] { maria.dropSchema("cs_maria", ruvia::DbDropBehavior::kCascade, true); }));
    maria.dropSchema("cs_maria", ruvia::DbDropBehavior::kRestrict, true);
    const auto mariaMigrations = maria.compile("cs_maria_ops");
    RUVIA_CHECK_EQ(mariaMigrations.size(), std::size_t{12});
    RUVIA_CHECK_EQ(mariaMigrations[0].id(), "cs_maria_ops_1");
    RUVIA_CHECK(mariaMigrations[1].sql().find("CREATE TABLE IF NOT EXISTS `cs_maria`.`records`") != std::string_view::npos);
    RUVIA_CHECK(mariaMigrations[1].sql().find("AUTO_INCREMENT") != std::string_view::npos);
    RUVIA_CHECK(mariaMigrations[2].sql().find("ADD COLUMN IF NOT EXISTS `note` TEXT") != std::string_view::npos);
    RUVIA_CHECK(mariaMigrations[9].sql().find("ALTER TABLE `cs_maria`.`records` RENAME TO `cs_maria`.`records_renamed`") != std::string_view::npos);
    RUVIA_CHECK(mariaMigrations[10].sql().find("DROP TABLE IF EXISTS `cs_maria`.`records_renamed` CASCADE") != std::string_view::npos);
    RUVIA_CHECK(throwsOn([&] {
        maria.alterColumnType("cs_maria.records", "state", {.dataType = DbDataType::kText});
    }));
    RUVIA_CHECK(throwsOn([&] { maria.setColumnNullable("cs_maria.records", "state", true); }));
}

RUVIA_TEST(db_schema_constraints_cover_actions_deferred_not_valid_and_rejections) {
    ruvia::DbQuery expressions;
    DbSchema pg({.driver = DbDriver::kPostgreSql});
    pg.addConstraint("cs_child",
        {.name = "child_fk",
            .kind = ruvia::DbConstraintKind::kForeignKey,
            .columns = {"parent_id"},
            .referencedTable = "cs_parent",
            .referencedColumns = {"id"},
            .onDelete = ruvia::DbReferentialAction::kSetNull,
            .onUpdate = ruvia::DbReferentialAction::kSetDefault,
            .deferrable = true,
            .initiallyDeferred = true,
            .notValid = true});
    pg.addConstraint("cs_child",
        {.name = "child_unique", .kind = ruvia::DbConstraintKind::kUnique, .columns = {"parent_id"}, .deferrable = true});
    pg.addConstraint("cs_child",
        {.name = "child_check",
            .kind = ruvia::DbConstraintKind::kCheck,
            .notValid = true,
            .check = expressions.binary(expressions.column("amount"), ruvia::DbBinaryOperator::kGreaterEqual,
                expressions.value(0))});
    pg.validateConstraint("cs_child", "child_fk");
    const auto migrations = pg.compile("cs_constraints");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("ON DELETE SET NULL ON UPDATE SET DEFAULT DEFERRABLE INITIALLY DEFERRED NOT VALID") != std::string_view::npos);
    RUVIA_CHECK(sql.find("CONSTRAINT \"child_unique\" UNIQUE (\"parent_id\") DEFERRABLE") != std::string_view::npos);
    RUVIA_CHECK(sql.find("CONSTRAINT \"child_check\" CHECK ((\"amount\" >= 0)) NOT VALID") != std::string_view::npos);
    RUVIA_CHECK(sql.find("VALIDATE CONSTRAINT \"child_fk\"") != std::string_view::npos);

    DbTableDefinition invalidTable{.name = "cs_invalid",
        .columns = {{.name = "id", .type = {.dataType = DbDataType::kBigInt}}},
        .constraints = {{.name = "bad", .kind = ruvia::DbConstraintKind::kUnique, .columns = {"id"}, .notValid = true}}};
    RUVIA_CHECK(throwsOn([&] { pg.createTable(invalidTable); }));
    RUVIA_CHECK(throwsOn([&] {
        pg.addConstraint("cs_child", {.name = "bad", .kind = ruvia::DbConstraintKind::kUnique, .columns = {"id"}, .initiallyDeferred = true});
    }));
    RUVIA_CHECK(throwsOn([&] {
        pg.addConstraint("cs_child", {.name = "bad", .kind = ruvia::DbConstraintKind::kCheck, .deferrable = true, .check = expressions.value(true)});
    }));
    RUVIA_CHECK(throwsOn([&] {
        pg.addConstraint("cs_child", {.name = "bad", .kind = ruvia::DbConstraintKind::kUnique, .columns = {"id"}, .notValid = true});
    }));
    RUVIA_CHECK(throwsOn([&] {
        pg.addConstraint("cs_child", {.name = "bad", .kind = ruvia::DbConstraintKind::kForeignKey, .columns = {"id"}, .referencedTable = "cs_parent", .referencedColumns = {}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        pg.addConstraint("cs_child", {.name = "bad", .kind = ruvia::DbConstraintKind::kCheck});
    }));

    DbSchema maria({.driver = DbDriver::kMariaDb});
    RUVIA_CHECK(throwsOn([&] {
        maria.addConstraint("cs_child", {.name = "fk", .kind = ruvia::DbConstraintKind::kForeignKey, .columns = {"id"}, .referencedTable = "cs_parent", .referencedColumns = {"id"}, .deferrable = true});
    }));
    RUVIA_CHECK(throwsOn([&] {
        maria.addConstraint("cs_child", {.name = "check", .kind = ruvia::DbConstraintKind::kCheck, .notValid = true, .check = expressions.value(true)});
    }));
}

RUVIA_TEST(db_schema_constructor_table_flags_and_compile_boundaries_are_checked) {
    RUVIA_CHECK(throwsOn([&] { DbSchema invalid({}); }));
    DbSchema empty({.driver = DbDriver::kPostgreSql});
    RUVIA_CHECK(empty.compile("cs_empty").empty());
    RUVIA_CHECK(throwsOn([&] { (void)empty.compile(""); }));

    DbSchema pg({.driver = DbDriver::kPostgreSql});
    pg.createTable({.name = "cs_temp_pg", .columns = {{.name = "id", .type = {.dataType = DbDataType::kInteger}}}, .temporary = true});
    RUVIA_CHECK(pg.compile("cs_temp_pg")[0].sql().find("CREATE TEMPORARY TABLE") != std::string_view::npos);
    DbSchema maria({.driver = DbDriver::kMariaDb});
    maria.createTable({.name = "cs_temp_maria", .columns = {{.name = "id", .type = {.dataType = DbDataType::kInteger}}}, .temporary = true});
    RUVIA_CHECK(maria.compile("cs_temp_maria")[0].sql().find("CREATE TEMPORARY TABLE") != std::string_view::npos);
    RUVIA_CHECK(throwsOn([&] {
        maria.createTable({.name = "cs_invalid_flags", .columns = {{.name = "id", .type = {.dataType = DbDataType::kInteger}}}, .temporary = true, .unlogged = true});
    }));
    RUVIA_CHECK(throwsOn([&] {
        maria.createTable({.name = "cs_unlogged", .columns = {{.name = "id", .type = {.dataType = DbDataType::kInteger}}}, .unlogged = true});
    }));
    RUVIA_CHECK(throwsOn([&] {
        maria.createTable({.name = "cs_always", .columns = {{.name = "id", .type = {.dataType = DbDataType::kInteger}, .identity = ruvia::DbIdentity::kAlways}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        pg.createTable({.name = "cs_duplicate", .columns = {{.name = "id", .type = {.dataType = DbDataType::kInteger}}, {.name = "id", .type = {.dataType = DbDataType::kInteger}}}});
    }));
}

RUVIA_TEST(db_schema_index_variants_and_dialect_rejections) {
    ruvia::DbQuery expressions;
    const auto predicate = expressions.binary(expressions.column("active"), ruvia::DbBinaryOperator::kEqual, expressions.value(true));
    DbSchema pg({.driver = DbDriver::kPostgreSql});
    pg.createIndex({.name = "cs_idx", .table = "cs_records", .keys = {{.column = "name", .order = ruvia::DbOrderDirection::kDesc, .nulls = ruvia::DbNullsOrder::kLast, .operatorClass = "text_ops"}, {.expression = expressions.column("lower_name")}}, .unique = true, .ifNotExists = true, .method = ruvia::DbIndexMethod::kBtree, .include = {"id"}, .where = predicate});
    const auto pgMigrations = pg.compile("cs_index");
    RUVIA_CHECK_EQ(pgMigrations.size(), std::size_t{1});
    RUVIA_CHECK(pgMigrations[0].sql().find("CREATE UNIQUE INDEX IF NOT EXISTS \"cs_idx\" ON \"cs_records\" USING btree") != std::string_view::npos);
    RUVIA_CHECK(pgMigrations[0].sql().find("\"name\" \"text_ops\" DESC NULLS LAST, (\"lower_name\")") != std::string_view::npos);
    RUVIA_CHECK(pgMigrations[0].sql().find("INCLUDE (\"id\") WHERE (\"active\" = TRUE)") != std::string_view::npos);

    for (const auto method : {ruvia::DbIndexMethod::kHash, ruvia::DbIndexMethod::kGin, ruvia::DbIndexMethod::kGist,
             ruvia::DbIndexMethod::kSpGist, ruvia::DbIndexMethod::kBrin}) {
        DbSchema methodSchema({.driver = DbDriver::kPostgreSql});
        methodSchema.createIndex({.name = "cs_method", .table = "cs_records", .keys = {{.column = "name"}}, .method = method});
        RUVIA_CHECK(methodSchema.compile("cs_method")[0].sql().find("USING ") != std::string_view::npos);
    }
    DbSchema concurrent({.driver = DbDriver::kPostgreSql});
    concurrent.createIndex({.name = "cs_concurrent", .table = "cs_records", .keys = {{.column = "id"}}, .concurrently = true});
    const auto concurrentMigrations = concurrent.compile("cs_concurrent");
    RUVIA_CHECK_EQ(concurrentMigrations[0].atomicity(), ruvia::DbMigrationAtomicity::kUnwrapped);
    DbSchema drop({.driver = DbDriver::kPostgreSql});
    drop.dropIndex("cs_records.cs_idx", true, true);
    RUVIA_CHECK(drop.compile("cs_drop_index")[0].sql().find("DROP INDEX CONCURRENTLY IF EXISTS \"cs_records\".\"cs_idx\"") != std::string_view::npos);

    RUVIA_CHECK(throwsOn([&] {
        DbSchema invalid({.driver = DbDriver::kPostgreSql});
        invalid.createIndex({.name = "bad", .table = "t", .keys = {{.column = "id", .expression = expressions.column("id")}}});
    }));
    RUVIA_CHECK(throwsOn([&] {
        DbSchema invalid({.driver = DbDriver::kPostgreSql});
        invalid.createIndex({.name = "bad", .table = "t", .keys = {{.column = "id"}, {.column = ""}}});
    }));
    DbSchema maria({.driver = DbDriver::kMariaDb});
    maria.createIndex({.name = "cs_maria_idx", .table = "cs_records", .keys = {{.column = "name", .order = ruvia::DbOrderDirection::kDesc}}, .unique = true, .ifNotExists = true});
    RUVIA_CHECK(maria.compile("cs_maria_idx")[0].sql().find("CREATE UNIQUE INDEX IF NOT EXISTS `cs_maria_idx` USING btree ON `cs_records`") != std::string_view::npos);
    RUVIA_CHECK(throwsOn([&] {
        maria.createIndex({.name = "bad", .table = "t", .keys = {{.column = "id"}}, .concurrently = true});
    }));
    RUVIA_CHECK(throwsOn([&] {
        maria.createIndex({.name = "bad", .table = "t", .keys = {{.column = "id"}}, .method = ruvia::DbIndexMethod::kHash});
    }));
    RUVIA_CHECK(throwsOn([&] {
        maria.createIndex({.name = "bad", .table = "t", .keys = {{.column = "id"}}, .include = {"extra"}});
    }));
    RUVIA_CHECK(throwsOn([&] { maria.dropIndex("idx"); }));
}

RUVIA_TEST(db_schema_views_enums_extensions_and_commands_cover_dialect_boundaries) {
    ruvia::DbQuery query;
    query.select(query.column("id")).from("cs_records");
    DbSchema pg({.driver = DbDriver::kPostgreSql});
    const std::array<std::string_view, 2> values{"new", "done"};
    pg.createEnum("cs_status", values);
    pg.dropEnum("cs_status", ruvia::DbDropBehavior::kCascade, true);
    pg.createExtension("hstore", true);
    pg.createView("cs_view", query, true);
    pg.dropView("cs_view", false, ruvia::DbDropBehavior::kCascade, true);
    pg.createView("cs_mat", query, false, true);
    pg.dropView("cs_mat", true, ruvia::DbDropBehavior::kRestrict, true);
    ruvia::DbQuery command;
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
    DbSchema enumAddition({.driver = DbDriver::kPostgreSql});
    enumAddition.addEnumValue("cs_status", "archived", true);
    const auto enumMigrations = enumAddition.compile("cs_enum_addition");
    RUVIA_CHECK_EQ(enumMigrations.size(), std::size_t{1});
    RUVIA_CHECK(enumMigrations[0].sql().find("ALTER TYPE \"cs_status\" ADD VALUE IF NOT EXISTS E'archived'") != std::string_view::npos);

    DbSchema maria({.driver = DbDriver::kMariaDb});
    maria.createView("cs_view", query);
    maria.dropView("cs_view", false, ruvia::DbDropBehavior::kCascade, true);
    maria.execute(command);
    RUVIA_CHECK(maria.compile("cs_objects").size() == std::size_t{3});
    RUVIA_CHECK(throwsOn([&] { maria.createEnum("status", values); }));
    RUVIA_CHECK(throwsOn([&] { maria.addEnumValue("status", "x"); }));
    RUVIA_CHECK(throwsOn([&] { maria.dropEnum("status"); }));
    RUVIA_CHECK(throwsOn([&] { maria.createExtension("hstore"); }));
    RUVIA_CHECK(throwsOn([&] { maria.createView("mat", query, false, true); }));
    RUVIA_CHECK(throwsOn([&] { maria.dropView("mat", true); }));
    RUVIA_CHECK(throwsOn([&] { pg.createView("bad", command); }));
    RUVIA_CHECK(throwsOn([&] { pg.createView("bad", query, true, true); }));
    RUVIA_CHECK(throwsOn([&] { pg.createEnum("empty", {}); }));
    ruvia::DbQuery returned;
    returned.deleteFrom("cs_records").returning({returned.column("id")});
    RUVIA_CHECK(throwsOn([&] { pg.execute(returned); }));
}

RUVIA_TEST(db_schema_trigger_variants_cover_events_timing_when_arguments_and_rejections) {
    ruvia::DbQuery expressions;
    const auto when = expressions.binary(expressions.column("enabled"), ruvia::DbBinaryOperator::kEqual, expressions.value(true));
    DbProcedure procedure;
    procedure.returnValue();
    DbSchema pg({.driver = DbDriver::kPostgreSql});
    pg.createTriggerFunction("cs_trigger_fn", procedure, true);
    pg.createTrigger({.name = "cs_before_insert", .table = "cs_records", .function = "cs_trigger_fn", .events = {ruvia::DbTriggerEvent::kInsert}});
    pg.createTrigger({.name = "cs_after_update", .table = "cs_records", .function = "cs_trigger_fn", .timing = ruvia::DbTriggerTiming::kAfter, .events = {ruvia::DbTriggerEvent::kUpdate}, .updateColumns = {"state", "enabled"}, .when = when, .arguments = {"arg 'one'", "two"}});
    pg.createTrigger({.name = "cs_statement", .table = "cs_records", .function = "cs_trigger_fn", .events = {ruvia::DbTriggerEvent::kDelete, ruvia::DbTriggerEvent::kTruncate}, .forEachRow = false});
    pg.createTrigger({.name = "cs_instead", .table = "cs_records_view", .function = "cs_trigger_fn", .timing = ruvia::DbTriggerTiming::kInsteadOf, .events = {ruvia::DbTriggerEvent::kInsert}});
    pg.dropTrigger("cs_records", "cs_after_update", true);
    pg.dropTriggerFunction("cs_trigger_fn", ruvia::DbDropBehavior::kCascade, true);
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

    RUVIA_CHECK(throwsOn([&] { pg.createTrigger({.name = "bad", .table = "t", .function = "f"}); }));
    RUVIA_CHECK(throwsOn([&] { pg.createTrigger({.name = "bad", .table = "t", .function = "f", .events = {ruvia::DbTriggerEvent::kInsert, ruvia::DbTriggerEvent::kInsert}}); }));
    RUVIA_CHECK(throwsOn([&] { pg.createTrigger({.name = "bad", .table = "t", .function = "f", .events = {ruvia::DbTriggerEvent::kInsert}, .updateColumns = {"x"}}); }));
    RUVIA_CHECK(throwsOn([&] { pg.createTrigger({.name = "bad", .table = "t", .function = "f", .events = {ruvia::DbTriggerEvent::kTruncate}}); }));
    RUVIA_CHECK(throwsOn([&] { pg.createTrigger({.name = "bad", .table = "t", .function = "f", .timing = ruvia::DbTriggerTiming::kInsteadOf, .events = {ruvia::DbTriggerEvent::kInsert}, .forEachRow = false}); }));
    RUVIA_CHECK(throwsOn([&] { pg.createTrigger({.name = "bad", .table = "t", .function = "f", .timing = ruvia::DbTriggerTiming::kInsteadOf, .events = {ruvia::DbTriggerEvent::kInsert}, .when = when}); }));
    DbSchema maria({.driver = DbDriver::kMariaDb});
    RUVIA_CHECK(throwsOn([&] { maria.createTriggerFunction("f", procedure); }));
    RUVIA_CHECK(throwsOn([&] { maria.createTrigger({.name = "f", .table = "t", .function = "f", .events = {ruvia::DbTriggerEvent::kInsert}}); }));
    RUVIA_CHECK(throwsOn([&] { maria.dropTrigger("t", "f"); }));
    RUVIA_CHECK(throwsOn([&] { maria.perform(expressions.value(true)); }));
    RUVIA_CHECK(throwsOn([&] { maria.run(procedure); }));
}

RUVIA_TEST(db_schema_timescale_options_cover_direct_settings_and_rejections) {
    ruvia::DbQuery expressions;
    const auto interval = expressions.cast(expressions.value("1 day"), DbDataType::kInterval);
    DbSchema pg({.driver = DbDriver::kPostgreSql});
    const std::array tableOptions{DbTableOption{"autovacuum_enabled", true}, DbTableOption{"fillfactor", std::int64_t{80}}, DbTableOption{"toast_tuple_target", "128"}};
    pg.setTableOptions("cs_events", tableOptions);
    pg.setDatabaseOption("cs_db", "timescaledb.max_background_workers", "8");
    pg.setChunkTimeInterval("cs_events", interval);
    pg.setCompression("cs_events", {.enabled = false});
    pg.removeCompressionPolicy("cs_events", true);
    pg.removeRetentionPolicy("cs_events", true);
    const auto migrations = pg.compile("cs_timescale_options");
    RUVIA_CHECK_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    RUVIA_CHECK(sql.find("ALTER TABLE \"cs_events\" SET (\"autovacuum_enabled\" = true, \"fillfactor\" = 80, \"toast_tuple_target\" = E'128')") != std::string_view::npos);
    RUVIA_CHECK(sql.find("ALTER DATABASE \"cs_db\" SET \"timescaledb\".\"max_background_workers\" TO E'8'") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"set_chunk_time_interval\"(E'\"cs_events\"', CAST(E'1 day' AS INTERVAL))") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"timescaledb\".\"compress\" = false") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"remove_compression_policy\"(E'\"cs_events\"', \"if_exists\" => TRUE)") != std::string_view::npos);
    RUVIA_CHECK(sql.find("\"remove_retention_policy\"(E'\"cs_events\"', \"if_exists\" => TRUE)") != std::string_view::npos);
    RUVIA_CHECK(throwsOn([&] { pg.setTableOptions("cs_events", std::span<const DbTableOption>()); }));
    const std::array duplicateOptions{DbTableOption{"fillfactor", std::int64_t{80}}, DbTableOption{"fillfactor", std::int64_t{90}}};
    RUVIA_CHECK(throwsOn([&] {
        pg.setTableOptions("cs_events", duplicateOptions);
    }));
    RUVIA_CHECK(throwsOn([&] { pg.setCompression("cs_events", {.enabled = false, .segmentBy = {"device_id"}}); }));
    DbSchema maria({.driver = DbDriver::kMariaDb});
    const std::array mariaOptions{DbTableOption{"x", true}};
    RUVIA_CHECK(throwsOn([&] { maria.setTableOptions("t", mariaOptions); }));
    RUVIA_CHECK(throwsOn([&] { maria.setDatabaseOption("d", "x", "y"); }));
    RUVIA_CHECK(throwsOn([&] { maria.createHypertable({.table = "t", .timeColumn = "ts"}); }));
    RUVIA_CHECK(throwsOn([&] { maria.setChunkTimeInterval("t", interval); }));
    RUVIA_CHECK(throwsOn([&] { maria.setCompression("t", {}); }));
    RUVIA_CHECK(throwsOn([&] { maria.addCompressionPolicy("t", interval); }));
    RUVIA_CHECK(throwsOn([&] { maria.removeCompressionPolicy("t"); }));
    RUVIA_CHECK(throwsOn([&] { maria.addRetentionPolicy("t", interval); }));
    RUVIA_CHECK(throwsOn([&] { maria.removeRetentionPolicy("t"); }));
}

RUVIA_TEST(db_procedure_public_helpers_render_and_validate) {
    ruvia::DbQuery expressions;
    ruvia::DbProcedure procedure;
    procedure.declareVariable({.name = "counter", .type = {.dataType = DbDataType::kInteger}, .defaultValue = expressions.value(0)});
    procedure.declareRow("row_value", "cs_records");
    procedure.declareRecord("record_value");
    procedure.assign("counter", expressions.binary(expressions.column("counter"), ruvia::DbBinaryOperator::kAdd, expressions.value(1)));
    ruvia::DbQuery select;
    select.select(select.column("id")).from("cs_records");
    const std::array<std::string_view, 1> targets{"row_value"};
    procedure.selectInto(select, targets, false);
    procedure.selectInto(select, targets, true);
    ruvia::DbQuery command;
    command.update("cs_records").set("state", command.value("done"));
    procedure.execute(command);
    procedure.perform(expressions.call("pg_notify", {expressions.value("cs"), expressions.value("body")}));
    procedure.beginIf(expressions.column("enabled"));
    procedure.elseIf(expressions.column("fallback"));
    procedure.otherwise();
    procedure.endIf();
    procedure.beginBlock();
    procedure.catchSqlState("23505");
    procedure.endBlock();
    procedure.returnValue();
    procedure.raiseException("failed", "P0001");
    const auto body = procedure.body();
    RUVIA_CHECK(body.find("DECLARE\n") == 0);
    RUVIA_CHECK(body.find("\"counter\" INTEGER := 0;") != std::string_view::npos);
    RUVIA_CHECK(body.find("\"row_value\" \"cs_records\"%ROWTYPE;") != std::string_view::npos);
    RUVIA_CHECK(body.find("SELECT \"id\" FROM \"cs_records\" INTO \"row_value\";") != std::string_view::npos);
    RUVIA_CHECK(body.find("INTO STRICT \"row_value\";") != std::string_view::npos);
    RUVIA_CHECK(body.find("ELSIF \"fallback\" THEN") != std::string_view::npos);
    RUVIA_CHECK(body.find("EXCEPTION\nWHEN SQLSTATE E'23505'") != std::string_view::npos);
    RUVIA_CHECK(body.find("RETURN;\nRAISE EXCEPTION USING MESSAGE = E'failed'") != std::string_view::npos);

    DbSchema pg({.driver = DbDriver::kPostgreSql});
    pg.createSchema("cs_nested");
    ruvia::DbProcedure applied;
    applied.apply(pg);
    RUVIA_CHECK(applied.body().find("CREATE SCHEMA \"cs_nested\";") != std::string_view::npos);
    DbSchema nonTransactional({.driver = DbDriver::kPostgreSql});
    nonTransactional.createIndex({.name = "cs_idx", .table = "cs_records", .keys = {{.column = "id"}}, .concurrently = true});
    RUVIA_CHECK(throwsOn([&] { applied.apply(nonTransactional); }));
    DbSchema maria({.driver = DbDriver::kMariaDb});
    RUVIA_CHECK(throwsOn([&] { applied.apply(maria); }));

    RUVIA_CHECK(throwsOn([&] { procedure.declareRecord("counter"); }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.execute(select);
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.selectInto(command, targets);
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.selectInto(select, {});
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.elseIf(expressions.value(true));
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.otherwise();
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.endIf();
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.catchSqlState("23505");
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.beginBlock();
        invalid.catchSqlState("bad");
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.endBlock();
    }));
    RUVIA_CHECK(throwsOn([&] {
        ruvia::DbProcedure invalid;
        invalid.raiseException("bad", "00000");
    }));
}
