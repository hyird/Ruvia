# 数据库（SQL / ORM）、Redis 与 JWT 认证 API 手册

本文档涵盖 `ruvia::web` 中的可选数据与安全模块：SQL 数据库连接池与直接查询、SQL/DDL/存储过程构建器、关系型强类型 ORM、Redis 客户端与 RediSearch ORM，以及 JWT 认证。所有公开符号均位于 `namespace ruvia`。

> **构建前置条件**：
> - SQL 数据库与 ORM 需要开启 `-DRUVIA_ENABLE_MARIADB=ON` 或 `-DRUVIA_ENABLE_POSTGRESQL=ON`（定义公开宏 `RUVIA_ENABLE_DATABASE` 及对应驱动宏）。
> - Redis、服务端 Session 与数据库查询结果缓存需要开启 `-DRUVIA_ENABLE_REDIS=ON`（定义公开宏 `RUVIA_ENABLE_REDIS`）。
> - JWT 需要开启 `-DRUVIA_ENABLE_JWT=ON`（定义公开宏 `RUVIA_ENABLE_JWT`）。

---

## 1. SQL 数据库连接池、直接查询与事务

聚合头文件：`"ruvia/web/db/db.h"`（或按需包含 `"ruvia/web/db/db_types.h"`、`"ruvia/web/db/db_handle.h"`、`"ruvia/web/db/db_rows.h"`、`"ruvia/web/db/db_schema.h"`）。
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

// 2. 或在独立 event_loop 上使用 db_client（通过 client.handle(options) 获取句柄）：
// ruvia::db_client client(loop, config);
// co_await client.connect();
// auto db = client.handle({.timeout_ = std::chrono::seconds(5)});
// ...
// co_await client.shutdown();
```

数据库 TLS 复用 `client_tls_config`。`verify_identity` 在握手内完成证书链与 RFC 9525 subjectAltName 校验，先于发送登录或启动消息：DNS 名只匹配 DNS SAN，IP 只匹配 IP SAN，不回退到主题 CN；PostgreSQL 可用 `tls_.server_name_` 覆盖验证名，MariaDB 当前要求数字 IP 且不接受覆盖名。`max_outgoing_records_` 默认 `2^23`（范围 `1..2^23`），限制单连接发出的全部 TLS 记录并在到达上限时终止连接。系统 DNS 查询超时、取消、客户端关闭或 Worker 停机时不等待不可抢占的底层 `getaddrinfo` 返回。

### 1.2 直接查询、编译期 SQL 校验与流式游标（`db_handle`）

在路由中通过 `c.db()`（或 `c.db("alias")`）获取 `ruvia::db_handle`。ORM 与直接查询保持独立公开路线，均可直接在 `db_handle` 或 `db_transaction` 上调用。

| `ruvia::db_handle` 方法 | 说明 |
| --- | --- |
| `db_handle with_options(operation_options options) const` | 绑定单次查询的 `timeout_` 与 `stop_token_` |
| `task<db_rows> query(std::string_view sql, Args&&... params) const` | 执行参数化查询并返回缓冲结果集 `db_rows`（亦支持传入 `std::span<const db_value>`） |
| `template <fixed_string Sql, db_driver Driver = db_driver::mariadb, typename... Params> task<db_rows> query(Params&&... params) const` | **编译期校验 SQL 占位符数量**与方言语法的参数化查询（MariaDB 用 `?`，PostgreSQL 用 `$1, $2`） |
| `task<db_stream_result> query_stream(...) const` | 流式拉取大结果集（同上支持运行期 SQL 与编译期 `fixed_string` 模板） |
| `task<db_exec_result> execute(...) const` | 执行 `INSERT` / `UPDATE` / `DELETE` / DDL，返回 `db_exec_result`（提供 `.affected_rows()` 与 `.last_insert_id()`） |
| `task<db_transaction> begin_transaction(db_transaction_options options = {}) const` | 开启事务（可指定隔离级别 `.isolation_` 与读写模式 `.access_mode_`） |
| `task<void> notify(std::string_view channel, std::string_view payload = {}) const` | （PostgreSQL）发送 `NOTIFY` 通知 |
| `template <typename E> db_repository<E, db_handle> repository() const` | 获取实体 `E` 的强类型 ORM Repository |
| `db_query_result_cache query_result_cache() const` | （需 `RUVIA_ENABLE_REDIS`）获取查询结果缓存控制器 |

查询缓存命中时，结果集持有字段所需的底层存储；空结果和没有列的行不保留缓存正文。结果及其字段视图仍须遵守各自的所有权和借用生命周期。

事务外的 `query()` / `execute()` 会与同一 worker 上的其他调用共享连接：PostgreSQL 使用管线模式，MariaDB 连续发送多条命令后按顺序读取各自的结果。每条语句独立成功或失败，并与等待连接的事务、流式查询保持到达顺序。改变会话状态的语句（`SET`、`USE`、`BEGIN`、`LOCK TABLES` 等）会影响之后共享同一连接的语句，应放在事务中执行；若某条语句使连接停留在事务中，该连接在归还时会被关闭而不会交给下一位调用者。MariaDB 的参数在入队时插值为所有 `sql_mode` 与字符集下含义相同的文本；SQL 或字符串参数含反斜杠时，其含义取决于 `NO_BACKSLASH_ESCAPES`，这类语句会等之前的结果全部读完后，按当时的会话状态插值再发送；若字符串参数中的反斜杠紧跟在大于 0x7F 的字节之后，且会话跟踪不能确认客户端字符集仍是 `utf8mb4`（未跟踪、已报告改为其他字符集，或之后执行过 `CALL`），还会先查询当时的 `@@character_set_client`，按该字符集转义，使 GBK、Big5、SJIS 等字符集中以 0x5C 结尾的多字节字符保持完整。`LOAD DATA LOCAL INFILE` 独占连接：前面的结果读完后才发送，读完它的结果前不发送其他命令；libmariadb 只为以 `LOAD` 开头的命令发送文件，前面有空白或注释时客户端拒绝发送文件，该语句单独失败（错误 2000），连接照常复用。服务器表示会话已结束或收到错序数据包的错误（如 1927、1156）按 I/O 错误处理，该连接不再复用。

同一连接上重复执行的 SQL 从第二次起作为服务端预处理语句缓存（按 SQL 与参数类型区分，每条连接最多 128 条，按最近最少使用淘汰；不会以预处理执行的语句不占用缓存位置）。MariaDB 在事务中总是使用它；事务外仅当该语句是所在 worker 上唯一进行中的语句时才以二进制协议执行，否则以管线化文本协议执行。预处理语句保留预处理时的会话状态（`sql_mode`、字符集、当前数据库等），因此服务器支持会话状态跟踪时，MariaDB 连接建立后会执行 `SET SESSION session_track_system_variables = 'sql_mode,old_mode,character_set_client,character_set_connection,character_set_results,time_zone,div_precision_increment,default_week_format,lc_time_names,max_sort_length,group_concat_max_len,session_track_system_variables,session_track_schema', session_track_schema = ON`，替换会话原有的跟踪列表；服务器报告其中任一变量（`collation_connection` 的变化按 `character_set_connection` 报告）或当前数据库的变化（`SET sql_mode`、`SET NAMES`、`USE`、存储过程内执行的 `SET` 等）时，该连接缓存的预处理语句全部作废，之后重新预处理；其他报告（如开启 binlog 时每次提交后的 `last_gtid`、`autocommit`）不影响缓存。存储过程可能在不报告的情况下修改跟踪列表，因此执行 `CALL` 后，下一次预处理执行前会先查询 `@@session_track_system_variables` 确认。服务器不支持跟踪、语句文本中出现 `session_track`（关闭跟踪本身不会被报告）、`CALL` 后的确认发现跟踪列表已变，或 `character_set_client` / `character_set_connection` 不是 `utf8mb4` 时，该连接不使用预处理语句。MariaDB 以文本协议的结果为准：二进制协议的值按文本协议的格式转换为字段文本，凡不能保证一致的情况都改用文本协议执行，包括 `CALL`、`LOAD`、`SET`、`USE`、含 `RETURNING` 或可执行注释（`/*! */`）的语句，预处理结果中有 `YEAR` 列的语句，以及含负数、大于 `INT64_MAX` 的无符号整数、负或非有限浮点数参数的调用，和 `LIMIT` / `OFFSET` / `FETCH` 计数位置上不是非负整数的参数。表结构变化使已预处理语句的结果列数改变，或执行时才出现不带 `ZEROFILL` 的 `YEAR` 列时，服务器已执行过该语句，为避免重复其副作用，这次调用作为语句失败返回而不重试；该语句下次使用时重新预处理或改用文本协议。

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

### 1.3 事务与保存点（`"ruvia/web/db/db_handle.h"`）

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

### 1.4 启动期数据库迁移（`"ruvia/web/db/db_schema.h"`）

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

头文件：`"ruvia/web/db/db_query.h"`、`"ruvia/web/db/db_schema.h"`。
参考示例：[examples/web/sql_queries.cpp](../examples/web/sql_queries.cpp)。

### 2.1 `ruvia::db_query`（跨方言 SQL AST 构建器）

`db_query` 可脱离数据库连接独立构建并编译为目标方言的参数化语句（`auto stmt = query.compile(driver, resource);`，再通过 `db.query(stmt.sql(), stmt.params())` 执行）：

`db_source_options`、`db_cte_options`、`db_lock_options`、`db_window_options`、`db_conflict_options` 中的集合字段为 `std::span<const T>`；Repository 的 `db_find_options`、`db_upsert_options` 同样借用集合及元素中的文本。请使用具名 `const std::array` 或 C 数组保存元素；独立保存 options 时，数组和文本 owner 必须存活到调用入口同步复制或导入完成。`std::span` 不拥有元素，不延长生命周期，也不能阻止所有悬垂。同步导入型 API 的 `initializer_list` 参数不持久保存裸视图，可继续直接传入列表。

- **查询子句**：`select`、`add_select`、`distinct`、`distinct_on`、`from`、`from_function`、`join`、`where` / `and_where` / `or_where`、`group_by`、`having`、`order_by`、`limit`、`offset`、`lock`（`FOR UPDATE` / `FOR SHARE` + `NOWAIT` / `SKIP LOCKED`）、`with`（CTE / `RECURSIVE`）、`combine`（`UNION` / `INTERSECT` / `EXCEPT`）。
- **变更子句**：`insert_into`、`values`、`insert_from`、`on_conflict`（Upsert；MariaDB 仅支持 `.any_unique_key_` 的 `ON DUPLICATE KEY UPDATE`，`excluded(col)` 编译为 `VALUES(col)`）、`update`、`set`、`update_from`、`delete_from`、`delete_using`、`returning`（MariaDB 需 ≥ 10.5，支持 `INSERT ... RETURNING` 与 `DELETE ... RETURNING`，`UPDATE ... RETURNING` 仅 PostgreSQL；`db_repository::save()` 依赖此能力）。
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

## 3. 强类型关系型 ORM（`RUVIA_DB_ENTITY` 与 `db_repository`）

头文件：`"ruvia/web/db/db_entity.h"`、`"ruvia/web/db/db_query.h"`、`"ruvia/web/db/db_repository.h"`。
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

通过 `c.db().repository<device>()` 或 `tx.repository<device>()` 获取：

| 分类 | 方法 | 说明 |
| --- | --- | --- |
| 查询 | `find(db_find_options = {})` | 查询实体列表 `entity_rows<Entity>`（支持 `.where_`、`.relations_` 关联预加载、`.order_`、`.skip_`、`.take_`、`.lock_`、`.cache_`） |
| 查询 | `find_one(db_find_options)` | 查询单条实体 `std::optional<Entity>` |
| 查询 | `find_and_count(db_find_options = {})` | 分页查询并同时返回总行数 `std::pair<entity_rows<Entity>, std::uint64_t>` |
| 统计与存在性 | `count(db_find_options = {})` / `exists(db_find_options = {})` | 按条件统计行数或判断是否存在匹配行 |
| 写入与变更 | `save` / `insert` / `upsert(entity, db_upsert_options)` | 保存、插入或冲突更新 Upsert（支持单条与批量 `std::span`） |
| 更新与删除 | `update(where, changes)` / `increment` / `decrement` | 按条件更新指定字段或原子自增/自减 |
| 删除与软删 | `delete_by(where)` / `remove(entity)` / `soft_delete(where)` / `restore(where)` / `clear()` | 硬删除、按软删列（`.delete_date_ = true`）软删除与恢复、清空表 |
| 构建器与 `RETURNING` | `query_builder(alias)` / `update_builder` / `delete_builder` / `insert_builder` | 链式构建复杂 JOIN（`left_join_and_select`）、`offset`/`limit`、CTE、窗口函数、`on_conflict(db_upsert_options)` 与子查询；写构建器通过 `.returning(...)` + `.get_many()` / `.get_one()` / `.get_scalar<T>()` 映射 `RETURNING` 结果 |

### 3.4 Builder 与直接 Query 的组合边界

读写 Builder 统一提供 `const db_query& query() const&`：这是只读、零拷贝的 AST 借用，不是快照，不延长 Builder 或其 PMR resource 的寿命。Builder 销毁或移动前必须结束借用；后续 Builder 修改会反映在视图中。直接在右值 Builder 上调用 `query()` 被拒绝，应先保存具名 Builder。`db_query::with()`、子查询 JOIN 等消费者在调用返回前克隆输入，因此导入完成后源 Query / Builder 即可销毁，外层仍拥有完整 AST、文本和参数。

读 Builder 的 `query()` 保留原组合视图语义：它只暴露平面查询，拒绝 relation hydration；**不会应用** `get_many()` / `get_query_and_parameters()` 等执行入口经 `effective_query()` 添加的软删除过滤。组合时若需要排除软删除行，应在源 Builder 显式添加相应条件。写 Builder 的视图先执行既有写安全校验（例如 UPDATE / DELETE 必须有条件），保留完整 INSERT / UPDATE / DELETE / `on_conflict()` 与 `RETURNING`；没有增加新的 SQL 生成路线。

```cpp
auto repo = c.db().repository<device>();
auto changed = repo.update_builder("d");
changed.set("name", "edge-2")
    .where(device::column<"id">() == 7)
    .returning({"id", "name"});

ruvia::db_query outer;
outer.with("changed", changed.query())
    .select(outer.star("changed"))
    .from("changed");
auto statement = outer.compile(ruvia::db_driver::postgresql, nullptr);
// statement 包含 WITH ... UPDATE ... RETURNING 和统一编号的绑定参数；
// 外层不持有 changed 的借用，可在 changed 析构后编译或执行。
```

读写 Builder 的 `join(type, const db_query&, alias, on, db_source_options)` 接受无实体子查询；读 Builder 的 `inner_join()` / `left_join()` 同样支持此入口。`.lateral_ = true` 直接保留为相关 `JOIN LATERAL (SELECT ...)`，不会提升为普通 CTE。`on` 沿既有 predicate 导入规则把根实体限定名重绑定为外层别名；无实体子查询及其输出列没有可推断的实体表映射，相关引用和 JOIN 输出引用请显式使用外层及子查询别名。`options.columns_` 仅在调用方确实需要 SQL 列别名列表时提供，不要求枚举未知输出字段；方言限制沿底层 Query 的编译校验。

```cpp
auto read = repo.query_builder("d");
ruvia::db_query latest;
latest.select(latest.column("score", "s")).from("samples", "s")
    .where(latest.binary(latest.column("device_id", "s"),
        ruvia::db_binary_operator::equal, latest.column("id", "d")));
read.left_join(latest, "matched", ruvia::db_expr::value(true).is_true(),
    {.lateral_ = true});
```

`db_selection::star()` / `db_selection::star("samples")` 是真实的 `*` / `samples.*` 投影项，可与具名表达式一起传给 `select()`、`add_select()` 和写 Builder 的 `returning()`。通配项不生成 `AS`，也不根据实体或 CTE 描述展开列名；具名项仍要求非空且相互唯一的输出名，并生成 `AS`。

```cpp
ruvia::db_query samples;
samples.select(samples.star()).from("measurements");
auto mixed = repo.query_builder("samples");
mixed.with("samples", samples).from_cte("samples")
    .select({ruvia::db_selection::star("samples"),
        ruvia::db_expr::value(true).as("matched")});
// WITH samples AS (...) SELECT samples.*, ... AS matched ...
```

通配投影的结果仍按数据库返回的**列名元数据**映射，不按位置猜测，也不假设 CTE 与根实体有相同列清单：

- 实体 / DTO 中与实际返回列同名的字段被解码；通配部分没有返回的字段保持 unset。通配带出的其他字段保留在 raw rows 中，不强迫 DTO 声明它们。
- 显式具名项仍是必需列：在映射类型内声明后，结果缺少该列时抛出 `db_conversion_error::missing_column`；具名项不在映射类型内时仍沿既有规则拒绝，`get_raw_and_entities()` 继续允许额外具名列。
- 通配项的真实名称在构建期未知，无法静态判断它与具名项是否重名。结果若含两个同名且需要映射的列，抛出 `db_conversion_error::invalid_format`，不会任选第一列；只取 raw rows 时不替数据库重命名或丢弃列。
- 对包含通配项的 `insert_from(columns, builder)`，Builder 不伪造源宽度或列清单；目标列名照常校验，实际源/目标宽度是否匹配交由数据库检查。无通配项的显式投影保留既有宽度检查。

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
- 同样支持在独立 `event_loop` 上构造 `ruvia::redis_client client(loop, config)`，通过 `client.handle(options = {})` 获取 `redis_handle`。
- 每条连接复用回复解析根槽；嵌套值和字符串仍由当前操作指定的资源持有，失败或取消时随未完成的回复一起回收。

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
| RediSearch ORM | `repository<Entity>(redis_repository_config)` |

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

auto repo = c.redis().repository<cached_user>(user_repo_config);
// 建索引（通常在启动时执行一次）：co_await repo.create_index();
// 带 TTL 插入：co_await repo.insert(entity, {.ttl_ = std::chrono::minutes(10)});
// 条件查询：
auto rows = co_await repo.find({
    .where_ = (cached_user::column<"role">() == "admin") && (cached_user::column<"score">() >= 80),
    .order_ = {{.field_ = "score", .direction_ = ruvia::redis_order_direction::desc}},
    .take_ = 20,
});
```

ORM 写入和删除先按脚本摘要执行；收到 `NOSCRIPT` 后释放该错误回复，再以脚本正文重试。两次执行共享同一操作超时与取消边界。

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

- `jwt_bearer_token` 只返回符合 RFC 6750 `b64token` 语法（`ALPHA / DIGIT / - . _ ~ + /`，末尾可跟 `=`）的令牌；含空格、逗号等其他字符时返回 `std::nullopt`。
- 自定义声明的值可以是任意 JSON 值（RFC 7519 §2）。载荷整体仍校验 JSON 语法与声明名唯一性；字符串值经 `claims()` / `claim(name)` 暴露，数字、布尔、`null`、数组与对象等非字符串自定义声明按 RFC 7519 §4 忽略，不导致令牌被拒绝。注册声明（`iss` / `sub` / `aud` / `jti` / `exp` / `nbf` / `iat`）类型不符时仍拒绝。
- `jwt_decode_unverified` 在不校验签名与时间声明的前提下解码载荷，但仍按 RFC 7519 §7.2 校验三段式结构与 JOSE Header（要求合法的 Base64url/JSON 对象、唯一的键名、非空字符串 `"alg"`，并拒绝 `"crit"` 与 `"b64"` 扩展）。
