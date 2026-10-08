# Redis access

[Documentation index](../README.md#contents)

## Direct commands

Enable `RUVIA_ENABLE_REDIS` and register Redis before `App::run()`:

```cpp
ruvia::app().redis({
    .alias = "default",
    .config = {.host = "127.0.0.1", .port = 6379},
});

// In a handler:
auto redis = c.redis().withOptions({.timeout = std::chrono::seconds(2)});
co_await redis.set("status", "ready");
auto value = co_await redis.get("status");
```

TLS verifies identity by default. Set `.tls.ca_file` for a private CA, or
explicitly use `.tls.mode = client_tls_mode::disabled` for a plaintext service.
Standalone applications can use `RedisClient(loop, config)` with `connect()`
and `shutdown()`; see [event_loop_data.cpp](../examples/web/event_loop_data.cpp).

- `set(key, value, RedisSetOptions)` covers expiry, NX/XX, and returning the
  previous value; inspect the result's `applied()` and `previous()`.
- `ttl()` / `pttl()` return missing, persistent, or expiring `RedisTtl` values.
- Scan results expose `done()` and `nextCursor()`.
- Blocking commands use an isolated pool automatically. Use `RedisBlockWait`
  and a finite timeout or stoppable token for an indefinite block.
- Pipelines and transactions inherit `withOptions()` from their handle.

See [redis.cpp](../examples/web/redis.cpp) for commands, batches, and Lua.

## Redis ORM

Include `<ruvia/web/redis/RedisRepository.h>` and declare a Redis entity:

```cpp
RUVIA_REDIS_ENTITY(User, "users",
    RUVIA_REDIS_COLUMN(id, ruvia::String,
        ruvia::RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::String),
    RUVIA_REDIS_COLUMN(age, std::uint32_t));

const ruvia::RedisRepositoryConfig config{
    .prefix = "app:users",
    .indexes = {
        {.field = "name", .kind = ruvia::RedisIndexKind::kTag, .sortable = true},
        {.field = "age", .kind = ruvia::RedisIndexKind::kNumeric},
    },
};
auto users = c.redis().getRepository<User>(config);
User user(c.pool());
user.set<"id">("u-42");
user.set<"name">("Alice");
user.set<"age">(25);
co_await users.insert(user, {.ttl = std::chrono::hours(1)});
auto loaded = co_await users.findOne({.where = User::field<"id">() == "u-42"});
```

Provide exactly one non-nullable string/integer primary key. Use a distinct
prefix for each schema and let the repository manage its keys. SQL entity
macros are separate from Redis entity macros.

| Operation | Behavior |
| --- | --- |
| `insert` | Create only if absent. |
| `update` | Change supplied fields on an existing entity. |
| `upsert` | Merge supplied fields or insert. |
| `deleteBy`, `remove` | Delete the entity. |
| `expire`, `ttl` | Change or inspect expiration. |

`update`, `deleteBy`, `expire`, and `ttl` require one exact primary-key equality.
Inspect `affected_entities()` for write results. Unset fields remain unchanged;
`setNull<"field">()` removes a nullable field. New entities need all non-nullable
fields. Omitted TTL preserves expiration; use a positive `.ttl` or `.persist = true`.

### Redis Search queries

Primary-key access works with ordinary Redis. Other field queries need Redis
Search with HASH indexing and dialect 2. Create the index explicitly at deployment:

```cpp
co_await users.createIndex();
auto [matches, total] = co_await users.findAndCount({
    .where = (User::field<"name">() == "Alice") && (User::field<"age">() >= 18),
    .order = {{.field = "name", .direction = ruvia::redis_order_direction::ascending}},
    .take = 20,
});
```

Use `find`, `findOne`, `findAndCount`, `count`, and `exists`. `kTag` supports exact
case-sensitive matches; `kNumeric` supports comparisons within the exact integer
range ±(2^53−1). One sortable field is supported per query. `dropIndex()` preserves
entity hashes. Keep repository results on their owning worker until destruction.

[redis_orm.cpp](../examples/web/redis_orm.cpp) is a complete example. Run
`ruvia_example_redis_orm --create-index` once against Redis Search, then run
without arguments to serve on `127.0.0.1:8091`. Configure it with
`RUVIA_REDIS_HOST`, `RUVIA_REDIS_PORT`, `RUVIA_REDIS_USER`, and
`RUVIA_REDIS_PASSWORD` in the environment or `.env`.
