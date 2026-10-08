# Database access

[Documentation index](../README.md#contents)

## Database Drivers

Enable `RUVIA_ENABLE_POSTGRESQL` or `RUVIA_ENABLE_MARIADB`, then register a
connection before `App::run()`:

```cpp
ruvia::DbConfig config{
    .driver = ruvia::DbDriver::kPostgreSql,
    .host = "127.0.0.1",
    .username = "app",
    .password = "replace-with-your-secret",
    .database = "app",
};
ruvia::app().database({.config = config});
```

Use `c.db()` in handlers. Outside an App, construct `DbClient(loop, config)`,
await `connect()`, and await `shutdown()` when finished; see
[event_loop_data.cpp](../examples/web/event_loop_data.cpp).

TLS verifies server identity by default. Set `config.tls.ca_file` for a private
CA; client certificates use `certificate_file` and `private_key_file` together.
PostgreSQL supports a `server_name` override. MariaDB verified TLS currently
requires a numeric host and a certificate valid for that IP. For an intentionally
plaintext service, explicitly select `client_tls_mode::disabled`.

### Direct SQL and structured queries

```cpp
auto db = c.db().withOptions({.timeout = std::chrono::seconds(2)});
auto rows = co_await db.query("SELECT name FROM users WHERE id = $1", userId);
for (const auto& row : rows) {
    auto name = row["name"].as<std::string_view>();
}
```

PostgreSQL uses `$1`, `$2`, …; MariaDB uses `?`. Bind values as arguments rather
than interpolating SQL. Fixed SQL can check parameter count at compile time:

```cpp
auto rows = co_await db.query<"SELECT name FROM users WHERE id = $1",
    ruvia::DbDriver::kPostgreSql>(userId);
```

| Operation | Use |
| --- | --- |
| `query()` | Owned, iterable `DbRows`; also use for PostgreSQL `RETURNING`. |
| `execute()` | `affectedRows()` and optional `lastInsertId()`. |
| `queryStream()` | Incremental row reading. |
| `withOptions()` | Per-operation timeout and additional cancellation token. |

Rows support index/name lookup. `DbField::value()` distinguishes NULL from an
empty string; `as<T>()` performs checked conversion. `DbError` provides a stable
code plus available SQLSTATE, native code, and constraint name. Connect/query/
acquire defaults are 5/30/5 seconds; `std::nullopt` disables an individual timeout.

For generated SQL without entities, use `DbQuery`:

```cpp
ruvia::DbQuery query;
query.select(query.column("name")).from("devices");
query.where(query.binary(query.column("enabled"),
    ruvia::DbBinaryOperator::kEqual, query.value(true)));
auto rows = co_await c.db().query(query);
```

`value()` binds data; `column()` quotes identifiers. `sql()` accepts trusted
syntax only. Database-specific expressions require their corresponding driver.

### ORM and QueryBuilder

Include `<ruvia/web/db/DbRepository.h>`, declare an entity, and obtain its
repository from `DbHandle`, `DbClient`, or a transaction:

```cpp
RUVIA_DB_ENTITY(Device, "device",
    RUVIA_DB_COLUMN(id, std::int64_t,
        ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(enabled, bool))

auto devices = c.db().getRepository<Device>();
auto rows = co_await devices.find({
    .where = Device::column<"enabled">() == true,
    .order = {{"id", ruvia::DbOrderDirection::kDesc}},
    .take = 50,
});

Device changes;
changes.set<"name">("Main pump");
co_await devices.update(Device::column<"id">() == deviceId, changes);
```

| Operation | Result / requirement |
| --- | --- |
| `find`, `findOne` | Entities or `std::optional<Entity>`. |
| `findAndCount` | Page plus total before pagination. |
| `count`, `exists` | Matching count or existence. |
| `insert`, `upsert` | Accept one entity or a span. |
| `update`, `deleteBy` | Require a condition. |
| `remove` | Requires all primary-key fields. |
| `increment`, `decrement` | Atomic numeric updates with a condition. |

Unset insert fields use database defaults; unset update fields stay unchanged.
Declare `.nullable = true` and use `setNull<"field">()` for SQL NULL.
PostgreSQL upserts select `.conflictPaths`; MariaDB uses `.anyUniqueKey = true`.
For bulk upserts with differing field sets, specify `updateColumns` explicitly.

Build richer reads with the entity-bound query builder:

```cpp
auto query = devices.createQueryBuilder("d");
query.where(Device::column<"enabled">() == true)
    .orderBy("id", ruvia::DbOrderDirection::kDesc)
    .take(20);
auto selected = co_await query.getMany();
```

Use `getOne`, `getCount`, `getExists`, or `getManyAndCount` for other results;
inspect generated SQL with `getQueryAndParameters()`.

Relations support `ManyToOne`, `OneToMany`, `OneToOne`, and `ManyToMany`.
Load them through `.relations` or `leftJoinAndSelect`; write foreign keys and
junction entities explicitly. See [orm_relations.cpp](../examples/web/orm_relations.cpp).

### Transactions

```cpp
auto transaction = co_await c.db().beginTransaction({
    .isolation = ruvia::DbTransactionIsolation::kRepeatableRead,
    .accessMode = ruvia::DbTransactionAccessMode::kReadOnly,
});
auto [page, total] = co_await transaction.getRepository<Device>().findAndCount({
    .take = 20,
});
co_await transaction.commit();
```

Page-and-count reads run as two statements; use a suitable transaction when they
must share a snapshot. Await transaction/stream operations sequentially. After a
failed operation, begin a new transaction. Keep its repositories and operations
within the transaction's lifetime, and keep all results on their owning worker.

### Expressions, projections, and writes

| Need | API |
| --- | --- |
| Computed values, JSON, aggregates, windows | `DbExpressions` with bound `value()` arguments. |
| Partial entities or DTOs | `select`, `RUVIA_DB_PROJECTION`, `getMany<Dto>()`. |
| PostgreSQL write results | `insertReturning`, `updateReturning`, `upsertReturning`, `deleteReturning`. |
| Composable writes | `createInsertBuilder`, `createUpdateBuilder`, `createDeleteBuilder`. |
| Joins and CTEs | `join`, `joinCte`, `with`, `fromCte`, `insertFrom`. |

Write builders execute through `execute()` or a `returning()` selection followed
by `getMany<Output>()`. Update/delete builders require a condition.
`UPDATE FROM`, write CTEs, and `RETURNING` require PostgreSQL in this API. Attach
write CTEs to the outer statement and pass their results through `RETURNING`;
a SELECT limit does not limit how many rows a CTE changes.

### Redis query result caching

Enable Redis as well as the database driver, register the cache, and opt queries in:

```cpp
using namespace std::chrono_literals;
ruvia::app()
    .redis({.alias = "cache", .config = {.host = "127.0.0.1", .port = 6379}})
    .database({.config = config,
        .query_cache = ruvia::db_query_cache_registration{
            .redis_alias = "cache",
            .policy = {.duration = 1s, .nameSpace = "devices"}}});

// In a handler:
auto query = c.db().getRepository<Device>().createQueryBuilder("d");
query.where(Device::column<"id">() == deviceId).cache("device-one", 10s);
auto device = co_await query.getOne();
```

Use automatic keys with `cache(true)` / `cache(5s)`, or a distinct explicit ID
for each filter, page, tenant, and result shape. Find options also accept `.cache`.
Redis uses the same verified TLS defaults; configure its TLS for the deployment.

Writes do not invalidate cache entries. Remove IDs with
`db.queryResultCache().remove(ids)` or clear the namespace with `clear()`.
Disable caching with `cache(false)` for transaction isolation or read-your-writes;
cached reads can otherwise escape a transaction's snapshot. Raw SQL strings,
streaming reads, locking reads, and writes are not cached.

### Generated schema and migrations

Include `<ruvia/web/db/DbSchema.h>`. Schema changes are explicit:

```cpp
ruvia::DbSchema schema({.driver = ruvia::DbDriver::kPostgreSql});
schema.createTable<Device>({.ifNotExists = true});
schema.createIndex({.name = "device_name_idx", .table = "device",
    .keys = {{.column = "name"}}});
const auto migrations = schema.compile("001_devices");
const auto report = ruvia::DbMigrator::migrate(config, migrations);
```

Schema APIs cover columns, keys, indexes, relations, enums, views, triggers,
computed columns, and TimescaleDB operations. Required extensions must exist on
the server. See [orm_columns.cpp](../examples/web/orm_columns.cpp) for column options.

### SQL migrations

Use `DbMigration` for SQL that is easier to write directly:

```cpp
static constexpr std::array migrations{
    ruvia::DbMigration{{.id = "001_create_users",
        .sql = "CREATE TABLE users (id BIGINT PRIMARY KEY, name VARCHAR(120))"}},
};
const auto report = ruvia::DbMigrator::migrate(config, migrations);
```

The migrator blocks: run it at startup, outside worker loops. Each migration
contains one statement and a unique ID; never edit an applied migration.
PostgreSQL defaults to atomic migrations; use `kUnwrapped` for operations such
as concurrent indexes. MariaDB DDL commits implicitly, so make those migrations
safe to reapply after interruption.

Examples: [direct SQL](../examples/web/database.cpp) and [ORM](../examples/web/orm.cpp).
The ORM example prints SQL by default; `ruvia_example_orm --migrate --run` applies
and runs it using the `RUVIA_DB_HOST`, `PORT`, `USER`, `PASSWORD`, and `DATABASE`
environment variables (each with the `RUVIA_DB_` prefix).
