# 数据库（SQL / ORM）、Redis 与 JWT 认证 API 手册

本文档涵盖 `ruvia::web` 中的可选数据与安全模块：SQL 数据库连接池与直接查询、SQL/DDL/存储过程构建器、关系型强类型 ORM、Redis 客户端与 RediSearch ORM，以及 JWT 认证。所有公开符号均位于 `namespace ruvia`。

> **构建前置条件**：
> - SQL 数据库与 ORM 需要开启 `-DRUVIA_ENABLE_MARIADB=ON` 或 `-DRUVIA_ENABLE_POSTGRESQL=ON`（定义公开宏 `RUVIA_ENABLE_DATABASE` 及对应驱动宏）。
> - Redis、服务端 Session 与数据库查询结果缓存需要开启 `-DRUVIA_ENABLE_REDIS=ON`（定义公开宏 `RUVIA_ENABLE_REDIS`）。
> - JWT 需要开启 `-DRUVIA_ENABLE_JWT=ON`（定义公开宏 `RUVIA_ENABLE_JWT`）。

---

## 1. SQL 数据库连接池、直接查询与事务

聚合头文件：`"ruvia/web/db/db.h"`（或按需包含 `"ruvia/web/db/db_types.h"`、`"ruvia/web/db/db_handle.h"`、`"ruvia/web/db/db_client.h"`、`"ruvia/web/db/db_transaction.h"`、`"ruvia/web/db/db_migration.h"`）。
参考示例：[examples/web/database.cpp](../examples/web/database.cpp)、[examples/web/event_loop_data.cpp](../examples/web/event_loop_data.cpp)。

### 1.1 连接配置 `ruvia::db_config` 与注册

```cpp
#include "ruvia/web/app.h"
#include "ruvia/web/db/db.h"

ruvia::db_config config{
    .driver_ = ruvia::db_driver::postgresql, // 或 ruvia::db_driver::mariadb
    .host_ = "127.0.0.1",
    .port_ = 5432,
    .username_ = "postgres",
    .password_ = "secret",
    .database_ = "app_db",
    .pool_size_per_worker_ = 4,
    .connect_timeout_ = std::chrono::seconds(5),
    .query_timeout_ = std::chrono::seconds(30),
    .acquire_timeout_ = std::chrono::seconds(5),
};

// 1. 注册到生产应用（每个业务 worker 独立持有 pool_size_per_worker_ 条连接）：
ruvia::app().database({
    .alias_ = "default",
    .config_ = config,
});

// 2. 或在独立 event_loop 上使用 db_client：
// ruvia::db_client client(loop, config);
// co_await client.connect();
// ...
// co_await client.shutdown();
```

### 1.2 直接查询、编译期 SQL 校验与流式游标（`db_handle`）

在路由中通过 `c.db()`（或 `c.db("alias")`）获取 `ruvia::db_handle`。ORM 与直接查询保持独立公开路线，均可直接在 `db_handle` 或 `db_transaction` 上调用。

| `ruvia::db_handle` 方法 | 说明 |
| --- | --- |
| `db_handle with_options(operation_options options) const` | 绑定单次查询的 `timeout_` 与 `stop_token_` |
| `scoped_operation<db_rows> query(std::string_view sql, Args&&... params) const` | 执行参数化查询并返回缓冲结果集 `db_rows`（亦支持传入 `std::span<const db_value>`） |
| `template <fixed_string Sql, db_driver Driver = db_driver::mariadb, typename... Params> scoped_operation<db_rows> query(Params&&... params) const` | **编译期校验 SQL 占位符数量**与方言语法的参数化查询（MariaDB 用 `?`，PostgreSQL 用 `$1, $2`） |
| `scoped_operation<db_stream_result> query_stream(...) const` | 流式拉取大结果集（同上支持运行期 SQL 与编译期 `fixed_string` 模板） |
| `scoped_operation<db_exec_result> execute(...) const` | 执行 `INSERT` / `UPDATE` / `DELETE` / DDL，返回 `db_exec_result`（提供 `.affected_rows()` 与 `.last_insert_id()`） |
| `scoped_operation<db_transaction> begin_transaction(db_transaction_options options = {}) const` | 开启事务（可指定隔离级别 `.isolation_` 与读写模式 `.access_mode_`） |
| `scoped_operation<void> notify(std::string_view channel, std::string_view payload = {}) const` | （PostgreSQL）发送 `NOTIFY` 通知 |
| `template <typename E> db_repository<E, db_handle> get_repository() const` | 获取实体 `E` 的强类型 ORM Repository |
| `db_query_result_cache query_result_cache() const` | （需 `RUVIA_ENABLE_REDIS`）获取查询结果缓存控制器 |

```cpp
// 编译期检查 PostgreSQL 的 $1, $2 占位符数量：
auto rows = co_await c.db().query<
    "SELECT id, name FROM users WHERE active = $1 AND score >= $2",
    ruvia::db_driver::postgresql>(true, 100);

for (const auto& row : rows) {
    auto id = row["id"].as<std::int64_t>().value_or(0);
    auto name = row["name"].value().value_or("");
}

// 流式结果集读取：
auto stream = co_await c.db().query_stream("SELECT id FROM events");
while (auto row = co_await stream.read()) {
    // 处理单行 *row
}
co_await stream.close();
```

### 1.3 事务与保存点（`"ruvia/web/db/db_transaction.h"`）

`db_transaction` 独占一条数据库连接；若在析构或作用域退出前未调用 `co_await tx.commit()`，底层会自动回滚并回收连接：

```cpp
auto tx = co_await c.db().begin_transaction({
    .isolation_ = ruvia::db_transaction_isolation::repeatable_read,
    .access_mode_ = ruvia::db_transaction_access_mode::read_write,
});

co_await tx.execute("UPDATE accounts SET balance = balance - 100 WHERE id = 1");
co_await tx.savepoint("sp_bonus");
co_await tx.execute("UPDATE accounts SET balance = balance + 100 WHERE id = 2");
// 如需局部回滚：co_await tx.rollback_to_savepoint("sp_bonus");
co_await tx.commit();
```

PostgreSQL 事务还额外支持：
- **咨询锁（Advisory Locks）**：`co_await tx.advisory_lock(key)` / `try_advisory_lock(key)`
- **服务端游标**：`declare_cursor(name, sql, params, scroll)`、`fetch_cursor(name, count)`、`close_cursor(name)`
- **大对象（Large Objects）**：`lo_create`、`lo_put`、`lo_get_text`、`lo_unlink`

### 1.4 启动期数据库迁移（`"ruvia/web/db/db_migration.h"`）

`ruvia::db_migrator::migrate` 使用独立连接同步执行有序迁移列表，并在迁移表（默认 `ruvia_schema_migrations`）中记录与核对每个版本 `id_` 的 SQL 校验摘要：

```cpp
const std::array<ruvia::db_migration, 1> migrations{
    ruvia::db_migration({
        .id_ = "0001_create_users",
        .sql_ = "CREATE TABLE IF NOT EXISTS users (id BIGINT PRIMARY KEY, name TEXT NOT NULL);",
    }),
};
auto report = ruvia::db_migrator::migrate(config, migrations);
// report.applied(), report.skipped(), report.changed()
```

---

## 2. SQL 构建器、Schema DDL 与存储过程

头文件：`"ruvia/web/db/db_query.h"`、`"ruvia/web/db/db_schema.h"`、`"ruvia/web/db/db_procedure.h"`。
参考示例：[examples/web/sql_queries.cpp](../examples/web/sql_queries.cpp)。

### 2.1 `ruvia::db_query`（跨方言 SQL AST 构建器）

`db_query` 可脱离数据库连接独立构建并编译为目标方言的参数化语句（`auto stmt = query.compile(driver, resource);`，再通过 `db.query(stmt.sql(), stmt.params())` 执行）：

- **查询子句**：`select`、`add_select`、`distinct`、`distinct_on`、`from`、`from_function`、`join`、`where` / `and_where` / `or_where`、`group_by`、`having`、`order_by`、`limit`、`offset`、`lock`（`FOR UPDATE` / `FOR SHARE` + `NOWAIT` / `SKIP LOCKED`）、`with`（CTE / `RECURSIVE`）、`combine`（`UNION` / `INTERSECT` / `EXCEPT`）。
- **变更子句**：`insert_into`、`values`、`insert_from`、`on_conflict`（Upsert）、`update`、`set`、`update_from`、`delete_from`、`delete_using`、`returning`。
- **表达式**：`column`、`value`、`excluded`、`call`、`coalesce`、`null_if`、`greatest`、`least`、`case_when`、`cast`、`aggregate`、`filter`、`over`（窗口函数）、`exists`、`subquery`。

### 2.2 `ruvia::db_schema` 与 `ruvia::db_procedure`

- **`db_schema`**：显式 DDL 与迁移生成器，支持：
  - 表与关系表：`create_table<Entity>(options)`、`create_relation_tables<Entity>()`、`create_table(definition)`、`drop_table`、`rename_table`
  - 列与约束：`add_column`、`drop_column`、`rename_column`、`alter_column_type`、`set_column_nullable`、`set_column_default`、`add_constraint`、`drop_constraint`
  - 索引、视图、枚举与扩展：`create_index`（支持 B-Tree / GIN / GiST 与部分索引条件）、`create_view`、`create_enum`、`create_extension`
  - 函数、触发器与 TimescaleDB：`create_function`、`create_trigger_function`、`create_trigger`、`create_hypertable`、`set_compression`、`add_compression_policy`、`add_retention_policy`
  - 调用 `schema.compile("0001_init")` 直接生成 `std::pmr::vector<db_migration>` 供 `db_migrator::migrate` 执行。
- **`db_procedure`**：结构化构建 PL/pgSQL 过程块（变量声明、`select_into`、`begin_if`/`else_if`/`otherwise`/`end_if`、异常捕获块 `begin_block`/`catch_sql_state`、循环与 `raise_exception`）。

---

## 3. 强类型关系型 ORM（`db_entity` 与 `db_repository`）

头文件：`"ruvia/web/db/db_entity.h"`、`"ruvia/web/db/db_projection.h"`、`"ruvia/web/db/db_predicate.h"`、`"ruvia/web/db/db_find_options.h"`、`"ruvia/web/db/db_repository.h"`。
参考示例：[examples/web/orm.cpp](../examples/web/orm.cpp)、[examples/web/orm_columns.cpp](../examples/web/orm_columns.cpp)、[examples/web/orm_relations.cpp](../examples/web/orm_relations.cpp)。

### 3.1 实体、生成列、关系与投影声明

```cpp
#include "ruvia/web/db/db.h"

RUVIA_DB_ENTITY(device, "devices",
    RUVIA_DB_COLUMN(id, std::int64_t,
        ruvia::db_column_options{.primary_key_ = true, .generated_ = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(notes, std::pmr::string,
        ruvia::db_column_options{.nullable_ = true}),
    RUVIA_DB_COLUMN(tags, std::pmr::vector<std::pmr::string>,
        ruvia::db_column_options{.data_type_ = ruvia::db_data_type::array}),
    RUVIA_DB_COLUMN(version, std::int32_t,
        ruvia::db_column_options{.version_ = true, .default_expression_ = "1"})
);

// 无表名 DTO 投影结构体：
RUVIA_DB_PROJECTION(device_summary,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(name, std::pmr::string)
);
```

- **可空列约定**：顶层列类型直接使用值类型（如 `std::int64_t`、`std::pmr::string`），并在 `db_column_options` 中设置 `.nullable_ = true`。通过 `entity.set_null<"notes">()` 置空，通过 `entity.is_null<"notes">()` 检查是否为 `NULL`。
- **生成列（Computed Columns）**：设置 `db_column_options{.generated_type_ = ruvia::db_generated_type::stored}`，ORM 在 `INSERT` / `UPDATE` 时自动跳过该列并在 `RETURNING` 中回填数据库计算结果。
- **实体关联宏**：
  - `RUVIA_DB_MANY_TO_ONE(field, TargetEntity, RUVIA_DB_JOIN_COLUMN("local_fk", "id"))`
  - `RUVIA_DB_ONE_TO_MANY(field, TargetEntity, RUVIA_DB_INVERSE("many_to_one_field"))`
  - `RUVIA_DB_ONE_TO_ONE(field, TargetEntity, RUVIA_DB_JOIN_COLUMN(...))`（或反向端 `RUVIA_DB_INVERSE(...)`）
  - `RUVIA_DB_MANY_TO_MANY(field, TargetEntity, RUVIA_DB_JOIN_TABLE("join_table", RUVIA_DB_JOIN_COLUMNS(...), RUVIA_DB_JOIN_COLUMNS(...)))`

### 3.2 类型安全字段引用与条件表达式（`Entity::column<"name">()`）

每个 `RUVIA_DB_ENTITY` 自动生成静态方法 `Entity::column<"field">(alias = {})`，返回强类型字段引用：

- **比较与逻辑组合**：支持 `==`、`!=`、`<`、`<=`、`>`、`>=`、`like`、`ilike`、`between`、`in`、`not_in`、`is_null()`、`is_not_null()`、PostgreSQL 数组操作（`array_contains`、`array_overlap`）与 JSONB 操作（`json_contains`、`json_has_key`、`json_path_text_equals`）；多个谓词使用 `&&`、`||`、`!` 组合。
- **更新赋值（生成 `db_assignment`）**：`device::column<"name">() = "edge-1"`、`device::column<"notes">().set_null()`、`device::column<"version">() += 1`、`device::column<"name">().set_excluded()`。

### 3.3 `ruvia::db_repository<Entity, Executor>` 核心操作

通过 `c.db().get_repository<device>()` 或 `tx.get_repository<device>()` 获取：

| 分类 | 方法 | 说明 |
| --- | --- | --- |
| 查询 | `find(db_find_options = {})` | 查询实体列表 `entity_rows<Entity>`（支持 `.where_`、`.relations_` 关联预加载、`.order_`、`.skip_`、`.take_`、`.lock_`、`.cache_`） |
| 查询 | `find_one(db_find_options)` | 查询单条实体 `std::optional<Entity>` |
| 查询 | `find_and_count(db_find_options = {})` | 分页查询并同时返回总行数 `std::pair<entity_rows<Entity>, std::uint64_t>` |
| 统计与存在性 | `count(db_find_options = {})` / `exists(db_find_options = {})` | 按条件统计行数或判断是否存在匹配行 |
| 写入与变更 | `save` / `insert` / `upsert(entity, db_upsert_options)` | 保存、插入或冲突更新 Upsert（支持单条与批量 `std::span`） |
| 更新与删除 | `update(where, changes)` / `increment` / `decrement` | 按条件更新指定字段或原子自增/自减 |
| 删除与软删 | `delete_by(where)` / `remove(entity)` / `soft_delete(where)` / `restore(where)` / `clear()` | 硬删除、按软删列（`.delete_date_ = true`）软删除与恢复、清空表 |
| 构建器与 `RETURNING` | `create_query_builder(alias)` / `create_update_builder` / `create_delete_builder` / `create_insert_builder` | 链式构建复杂 JOIN（`left_join_and_select`）、`offset`/`limit`、CTE、窗口函数与子查询；写构建器通过 `.returning(...)` + `.get_many()` / `.get_one()` / `.get_scalar<T>()` 映射 `RETURNING` 结果 |

---

## 4. Redis 客户端、Pub/Sub、Streams 与 RediSearch ORM

聚合头文件：`"ruvia/web/redis/redis.h"`。
参考示例：[examples/web/redis.cpp](../examples/web/redis.cpp)、[examples/web/redis_pubsub.cpp](../examples/web/redis_pubsub.cpp)、[examples/web/redis_streams.cpp](../examples/web/redis_streams.cpp)、[examples/web/redis_orm.cpp](../examples/web/redis_orm.cpp)。

### 4.1 连接池配置 `ruvia::redis_config`

```cpp
ruvia::redis_config config{
    .host_ = "127.0.0.1",
    .port_ = 6379,
    .pool_size_per_worker_ = 4,
    .blocking_pool_size_per_worker_ = 2,
};

// 注册默认池与命名别名池：
ruvia::app()
    .redis({.alias_ = "default", .config_ = config})
    .redis({.alias_ = "cache", .config_ = config});
```

- 每个 worker 维护 `pool_size_per_worker_` 条常规命令连接，以及按需建立的 `blocking_pool_size_per_worker_` 条阻塞命令专用连接（供 `blpop`、`brpop`、`xread_group` 等阻塞操作自动使用，不阻塞常规命令池）。
- 同样支持在独立 `event_loop` 上构造 `ruvia::redis_client client(loop, config)`。

### 4.2 `ruvia::redis_handle` 命令分类总览

在路由中通过 `c.redis()` 或 `c.redis("cache")` 获取：

| 分类 | 方法 |
| --- | --- |
| 连接与通用 | `with_options(operation_options)`、`ping()`、`command(args...)`（发送任意原始 Redis 命令并返回 `redis_value`） |
| String 与 Key | `get`、`mget`、`set(key, val, redis_set_options)`、`mset`、`get_del`、`append`、`strlen`、`incr`、`incr_by`、`decr`、`decr_by`、`del`、`unlink`、`exists`、`touch`、`type`、`rename`、`rename_nx`、`expire`、`expire_at`、`persist`、`ttl`、`pttl` |
| Hash | `hget`、`hset`、`hmget`、`hget_all`、`hdel`、`hexists`、`hlen`、`hkeys`、`hvals`、`hincr_by` |
| List | `lpush`、`rpush`、`lpop`、`rpop`、`llen`、`lrange`、`lindex`、`lset`、`ltrim`、`lrem`、`blpop`、`brpop` |
| Set & Sorted Set | `sadd`、`srem`、`smembers`、`scard`、`sismember`、`spop`、`srand_member`、`sinter`、`sunion`、`sdiff`；`zadd`、`zrem`、`zrange`、`zrange_with_scores`、`zscore`、`zcard`、`zcount` |
| 游标迭代与脚本 | `scan`、`hscan`、`sscan`、`zscan`；`eval`、`eval_sha`、`script_load`、`script_exists` |
| 批处理与事务 | `pipeline()`（返回 `redis_pipeline`，链式追加命令后 `co_await std::move(pipe).exec()`）、`transaction()`（返回 `redis_transaction`，封装 `MULTI`/`EXEC`） |
| Streams | `xread_group(group, consumer, streams, redis_x_read_group_options)` |
| Pub/Sub | `publish(channel, payload)`、`subscription()` |
| RediSearch ORM | `get_repository<Entity>(redis_repository_config)` |

### 4.3 Pub/Sub 独占订阅连接（`redis_subscription`）

Pub/Sub 订阅不共享常规连接池：每次调用 `c.redis().subscription()` 都会创建一条独占的 Redis 订阅通道（在首次 `subscribe` 时打开连接），直到调用 `abort()`、取消或析构：

```cpp
auto sub = c.redis().with_options({.stop_token_ = c.stop_token()}).subscription();
co_await sub.subscribe("news");
while (auto event = co_await sub.read()) {
    if (event->kind() == ruvia::redis_pubsub_event_kind::message) {
        // event->channel(), event->payload()
    }
}
```

### 4.4 Redis HASH 实体与 RediSearch ORM（`RUVIA_REDIS_ENTITY`）

使用 `RUVIA_REDIS_ENTITY` 将结构映射为 Redis HASH 键（`<prefix>:<primary_key>`），并可通过 RediSearch 索引进行多条件过滤与排序分页：

```cpp
RUVIA_REDIS_ENTITY(cached_user, "user:",
    RUVIA_REDIS_COLUMN(id, std::pmr::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(role, std::pmr::string),
    RUVIA_REDIS_COLUMN(score, std::int64_t),
    RUVIA_REDIS_COLUMN(bio, std::pmr::string, ruvia::redis_column_options{.nullable_ = true})
);

const ruvia::redis_repository_config user_repo_config{
    .prefix_ = "user:",
    .indexes_ = {
        {.field_ = "role", .kind_ = ruvia::redis_index_kind::tag},
        {.field_ = "score", .kind_ = ruvia::redis_index_kind::numeric, .sortable_ = true},
    },
};

auto repo = c.redis().get_repository<cached_user>(user_repo_config);
// 建索引（通常在启动时执行一次）：co_await repo.create_index();
// 带 TTL 插入：co_await repo.insert(entity, {.ttl_ = std::chrono::minutes(10)});
// 条件查询：
auto rows = co_await repo.find({
    .where_ = (cached_user::column<"role">() == "admin") && (cached_user::column<"score">() >= 80),
    .order_ = {{.field_ = "score", .direction_ = ruvia::redis_order_direction::desc}},
    .take_ = 20,
});
```

---

## 5. JWT 认证（`"ruvia/web/auth/jwt.h"`）

参考示例：[examples/web/auth_jwt.cpp](../examples/web/auth_jwt.cpp)。

支持 `jwt_algorithm::hs256`、`hs384`、`hs512`。遵循 RFC 7518 密钥最小长度要求（HS256 至少 32 字节、HS384 至少 48 字节、HS512 至少 64 字节）：

```cpp
#include "ruvia/web/auth/jwt.h"

// 1. 签发 Token：
inline constexpr std::array<ruvia::jwt_claim_view, 1> custom_claims{{
    {.name_ = "role", .value_ = "admin"},
}};
std::pmr::string token = ruvia::jwt_sign({
    .algorithm_ = ruvia::jwt_algorithm::hs256,
    .secret_ = secret_key_32_bytes_minimum,
    .issuer_ = "ruvia-auth",
    .subject_ = "user-42",
    .audience_ = "ruvia-api",
    .expires_in_ = std::chrono::hours(1),
    .claims_ = custom_claims,
    .resource_ = c.arena(),
});

// 2. 提取 Bearer Token 并验证签名与声明：
auto header_val = c.req().header("Authorization").value_or("");
auto bearer = ruvia::jwt_bearer_token(header_val);
if (bearer) {
    ruvia::jwt_payload payload = ruvia::jwt_verify({
        .token_ = *bearer,
        .algorithm_ = ruvia::jwt_algorithm::hs256,
        .secret_ = secret_key_32_bytes_minimum,
        .issuer_ = "ruvia-auth",
        .audience_ = "ruvia-api",
        .resource_ = c.arena(),
    });
    std::string_view user_id = payload.subject();
    std::optional<std::string_view> role = payload.claim("role");
}
```
