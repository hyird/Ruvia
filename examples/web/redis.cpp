// Redis: configuration, aliases, strings, hashes, lists, sets, sorted sets,
// scans, scripts, blocking pops, pipelines and transactions. Built only with
// RUVIA_ENABLE_REDIS=ON.
// Set RUVIA_REDIS_HOST/PORT/USER/PASSWORD and RUVIA_REDIS_DATABASE;
// defaults use localhost:6379.
// Run on port 8090 (RUVIA_PORT overrides it), then GET /redis/ping.
// Use a disposable Redis database: collection routes, scripts and transactions
// write demo keys. Blocking reads use the separate blocking pool.
// See backend_tls.h for RUVIA_REDIS_TLS/CA/CERT/KEY. A local plaintext Redis
// requires the explicit setting RUVIA_REDIS_TLS=false.

#include "ruvia/web/redis.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/http_request.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/redis.h"

#include "backend_config.h"
namespace {

void append_bool(std::string& output, bool value) {
    output.append(value ? "true" : "false");
}

void append_ttl(std::string& output, const ruvia::redis_ttl& ttl) {
    switch (ttl.state()) {
        case ruvia::redis_ttl_state::missing:
            output.append("missing");
            break;
        case ruvia::redis_ttl_state::persistent:
            output.append("persistent");
            break;
        case ruvia::redis_ttl_state::expiring:
            ruvia::http_append_signed(output, ttl.remaining()->count());
            output.append("ms");
            break;
    }
}

class redis_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/redis")

    RUVIA_ROUTES_BEGIN(redis_controller)
    RUVIA_GET("/ping", ping);
    RUVIA_GET("/value/:key", get_value);
    RUVIA_POST("/value/:key", set_value);
    RUVIA_POST("/counter/:key", increment);
    RUVIA_GET("/keys/:key", key_metadata);
    RUVIA_POST("/strings/:key", strings);
    RUVIA_POST("/hashes/:key", hashes);
    RUVIA_POST("/lists/:key", lists);
    RUVIA_POST("/sets/:key", sets);
    RUVIA_POST("/sorted-sets/:key", sorted_sets);
    RUVIA_GET("/scan", scan);
    RUVIA_POST("/pipeline", pipeline);
    RUVIA_POST("/transaction", transaction);
    RUVIA_POST("/scripts", scripts);
    RUVIA_GET("/blocking-pop", blocking_pop);
    RUVIA_GET("/alias/:key", alias_value);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ping(ruvia::context& c) {
        co_await c.redis()
            .with_options({.timeout_ = std::chrono::seconds(2), .stop_token_ = c.stop_token()})
            .ping();
        auto message = co_await c.redis().ping("hello");
        co_return c.text(std::move(message));
    }

    ruvia::task<ruvia::http_response> get_value(ruvia::context& c) {
        auto value = co_await c.redis().get(c.req().param("key").value_or(""));
        if (!value) {
            co_return c.error({.status_ = ruvia::http_status::not_found,
                .code_ = "not_found",
                .message_ = "redis key not found"});
        }
        co_return c.text(std::move(*value));
    }

    ruvia::task<ruvia::http_response> set_value(ruvia::context& c) {
        auto body = co_await c.req().text();
        (void)(co_await c.redis().set(c.req().param("key").value_or(""), body));
        co_return c.text("OK\n");
    }

    ruvia::task<ruvia::http_response> increment(ruvia::context& c) {
        const auto value = co_await c.redis().incr(c.req().param("key").value_or(""));
        std::string body;
        ruvia::http_append_signed(body, value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> key_metadata(ruvia::context& c) {
        const auto key = c.req().param("key").value_or("");
        const auto exists = co_await c.redis().exists(key);
        const auto touched = co_await c.redis().touch(key);
        const auto type = co_await c.redis().type(key);
        const auto ttl = co_await c.redis().ttl(key);
        const auto pttl = co_await c.redis().pttl(key);
        const auto persisted = co_await c.redis().persist(key);

        std::string body;
        body.append("exists=");
        append_bool(body, exists);
        body.append("\ntouched=");
        append_bool(body, touched);
        body.append("\ntype=");
        body.append(type);
        body.append("\nttl=");
        append_ttl(body, ttl);
        body.append("\npttl=");
        append_ttl(body, pttl);
        body.append("\npersisted=");
        append_bool(body, persisted);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> strings(ruvia::context& c) {
        const auto key = c.req().param("key").value_or("");
        const auto value = co_await c.req().text();
        (void)(co_await c.redis().set(key, value));
        ruvia::redis_set_options set_options;
        set_options.previous_value_ = ruvia::redis_set_previous_value_policy::return_value;
        auto previous = co_await c.redis().set(key, "fresh", set_options);
        ruvia::redis_set_options ttl_options;
        ttl_options.expiration_ = ruvia::redis_set_expiration::expires_after(std::chrono::seconds(60));
        (void)(co_await c.redis().set("ruvia:example:ttl", "ttl", ttl_options));
        ruvia::redis_set_options insert_options;
        insert_options.condition_ = ruvia::redis_set_condition::if_absent;
        const auto inserted = co_await c.redis().set("ruvia:example:nx", "first", insert_options);
        ruvia::redis_set_options replace_options;
        replace_options.previous_value_ = ruvia::redis_set_previous_value_policy::return_value;
        auto replaced = co_await c.redis().set(key, "replaced", replace_options);
        const auto appended = co_await c.redis().append(key, "+tail");
        const auto length = co_await c.redis().strlen(key);
        auto deleted = co_await c.redis().get_del("ruvia:example:nx");
        co_await c.redis().mset("ruvia:example:mset:a", "one", "ruvia:example:mset:b", "two");
        auto values = co_await c.redis().mget("ruvia:example:mset:a", "ruvia:example:mset:b");
        // Redis counters must contain integer text. Keep them separate from
        // the string value above, which now contains "replaced+tail".
        constexpr std::string_view counter_key = "ruvia:example:number";
        (void)co_await c.redis().set(counter_key, "10");
        const auto decremented = co_await c.redis().decr(counter_key);
        const auto decremented_by = co_await c.redis().decr_by(counter_key, 2);
        const auto incremented_by = co_await c.redis().incr_by(counter_key, 3);

        std::string body;
        body.append("previous=");
        body.append(previous.previous().value_or(""));
        body.append("\ninserted=");
        append_bool(body, inserted.applied());
        body.append("\nreplaced=");
        body.append(replaced.previous().value_or(""));
        body.append("\nappended=");
        ruvia::http_append_signed(body, appended);
        body.append("\nlength=");
        ruvia::http_append_signed(body, length);
        body.append("\ndeleted=");
        body.append(deleted.value_or(""));
        body.append("\nmget=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(values.size()));
        body.append("\ndecr=");
        ruvia::http_append_signed(body, decremented);
        body.append("\ndecr-by=");
        ruvia::http_append_signed(body, decremented_by);
        body.append("\nincr-by=");
        ruvia::http_append_signed(body, incremented_by);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> hashes(ruvia::context& c) {
        const auto key = c.req().param("key").value_or("");
        const auto changed = co_await c.redis().hset(key, "name", "ruvia", "kind", "framework");
        const auto extra = co_await c.redis().hset(key, "count", "1");
        const auto count = co_await c.redis().hincr_by(key, "count", 1);
        auto name = co_await c.redis().hget(key, "name");
        auto values = co_await c.redis().hmget(key, "name", "kind");
        auto all = co_await c.redis().hget_all(key);
        auto keys = co_await c.redis().hkeys(key);
        auto hvals = co_await c.redis().hvals(key);
        const auto exists = co_await c.redis().hexists(key, "name");
        const auto length = co_await c.redis().hlen(key);
        const auto removed = co_await c.redis().hdel(key, "kind");

        std::string body;
        body.append("changed=");
        ruvia::http_append_signed(body, changed + extra);
        body.append("\ncount=");
        ruvia::http_append_signed(body, count);
        body.append("\nname=");
        body.append(name.value_or(""));
        body.append("\nhmget=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(values.size()));
        body.append("\nall=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(all.size()));
        body.append("\nkeys=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(keys.size()));
        body.append("\nvalues=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(hvals.size()));
        body.append("\nexists=");
        append_bool(body, exists);
        body.append("\nlength=");
        ruvia::http_append_signed(body, length);
        body.append("\nremoved=");
        ruvia::http_append_signed(body, removed);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> lists(ruvia::context& c) {
        const auto key = c.req().param("key").value_or("");
        const auto left = co_await c.redis().lpush(key, "left");
        const auto right = co_await c.redis().rpush(key, "right");
        const auto length = co_await c.redis().llen(key);
        auto values = co_await c.redis().lrange(key, 0, -1);
        auto first = co_await c.redis().lindex(key, 0);
        if (first) {
            co_await c.redis().lset(key, 0, "updated");
        }
        co_await c.redis().ltrim(key, 0, 8);
        const auto removed = co_await c.redis().lrem(key, 0, "missing");
        auto popped_left = co_await c.redis().lpop(key);
        auto popped_right = co_await c.redis().rpop(key);

        std::string body;
        body.append("pushed=");
        ruvia::http_append_signed(body, left + right);
        body.append("\nlength=");
        ruvia::http_append_signed(body, length);
        body.append("\nrange=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(values.size()));
        body.append("\nfirst=");
        body.append(first.value_or(""));
        body.append("\nremoved=");
        ruvia::http_append_signed(body, removed);
        body.append("\nleft=");
        body.append(popped_left.value_or(""));
        body.append("\nright=");
        body.append(popped_right.value_or(""));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> sets(ruvia::context& c) {
        const auto key = c.req().param("key").value_or("");
        const auto added = co_await c.redis().sadd(key, "one");
        const auto also_added = co_await c.redis().sadd("ruvia:example:set:other", "one");
        auto members = co_await c.redis().smembers(key);
        const auto size = co_await c.redis().scard(key);
        const auto member = co_await c.redis().sismember(key, "one");
        auto random = co_await c.redis().srand_member(key);
        // Three commands over the same key set: a prepared sequence still passes
        // as a span, while one-off calls read better as plain arguments.
        const std::array<std::string_view, 2> keys{key, "ruvia:example:set:other"};
        auto intersection = co_await c.redis().sinter(keys);
        auto union_values = co_await c.redis().sunion(keys);
        auto difference = co_await c.redis().sdiff(keys);
        auto popped = co_await c.redis().spop(key);
        const auto removed = co_await c.redis().srem("ruvia:example:set:other", "one");

        std::string body;
        body.append("added=");
        ruvia::http_append_signed(body, added + also_added);
        body.append("\nmembers=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(members.size()));
        body.append("\nsize=");
        ruvia::http_append_signed(body, size);
        body.append("\nis-member=");
        append_bool(body, member);
        body.append("\nrandom=");
        body.append(random.value_or(""));
        body.append("\nintersection=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(intersection.size()));
        body.append("\nunion=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(union_values.size()));
        body.append("\ndifference=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(difference.size()));
        body.append("\npopped=");
        body.append(popped.value_or(""));
        body.append("\nremoved=");
        ruvia::http_append_signed(body, removed);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> sorted_sets(ruvia::context& c) {
        const auto key = c.req().param("key").value_or("");
        const auto added = co_await c.redis().zadd(key, 1.0, "one");
        const auto added_two = co_await c.redis().zadd(key, 2.0, "two");
        auto values = co_await c.redis().zrange(key, 0, -1);
        auto scored = co_await c.redis().zrange_with_scores(key, 0, -1);
        auto score = co_await c.redis().zscore(key, "one");
        const auto size = co_await c.redis().zcard(key);
        const auto counted = co_await c.redis().zcount(key, 0.0, 10.0);
        const auto removed = co_await c.redis().zrem(key, "two");

        std::string body;
        body.append("added=");
        ruvia::http_append_signed(body, added + added_two);
        body.append("\nvalues=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(values.size()));
        body.append("\nscored=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(scored.size()));
        body.append("\nscore=");
        append_bool(body, score.has_value());
        body.append("\nsize=");
        ruvia::http_append_signed(body, size);
        body.append("\ncounted=");
        ruvia::http_append_signed(body, counted);
        body.append("\nremoved=");
        ruvia::http_append_signed(body, removed);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> scan(ruvia::context& c) {
        ruvia::redis_scan_options key_scan;
        key_scan.match_ = "ruvia:example:*";
        key_scan.count_ = 16;
        ruvia::redis_scan_options member_scan;
        member_scan.count_ = 16;
        const auto keys = co_await c.redis().scan(key_scan);
        const auto hash = co_await c.redis().hscan("ruvia:example:hash", member_scan);
        const auto set = co_await c.redis().sscan("ruvia:example:set", member_scan);
        const auto zset = co_await c.redis().zscan("ruvia:example:zset", member_scan);

        std::string body;
        body.append("keys=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(keys.values().size()));
        body.append("\nhash=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(hash.entries().size()));
        body.append("\nset=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(set.values().size()));
        body.append("\nzset=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(zset.entries().size()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> pipeline(ruvia::context& c) {
        auto pipeline = c.redis().pipeline();
        pipeline.set("ruvia:example:pipeline", "1")
            .get("ruvia:example:pipeline")
            .incr("ruvia:example:pipeline")
            .hset("ruvia:example:pipeline:hash", "field", "value")
            .hget("ruvia:example:pipeline:hash", "field")
            .lpush("ruvia:example:pipeline:list", "item")
            .sadd("ruvia:example:pipeline:set", "member")
            .zadd("ruvia:example:pipeline:zset", 1.0, "member")
            .command("TYPE", "ruvia:example:pipeline");
        auto results = co_await std::move(pipeline).exec();

        std::string body;
        body.append("pipeline results=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(results.size()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> transaction(ruvia::context& c) {
        auto tx = c.redis().transaction();
        tx.set("ruvia:example:tx", "1")
            .incr("ruvia:example:tx")
            .get("ruvia:example:tx");
        auto results = co_await std::move(tx).exec();

        std::string body;
        body.append("transaction results=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(results.size()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> scripts(ruvia::context& c) {
        static constexpr std::string_view script = "return ARGV[1]";
        const std::array<std::string_view, 1> args{"hello"};
        auto value = co_await c.redis().eval(script, {}, args);
        auto sha = co_await c.redis().script_load(script);
        auto exists = co_await c.redis().script_exists(sha);
        auto sha_value = co_await c.redis().eval_sha(sha, {}, args);

        std::string body;
        body.append("eval-kind=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(value.kind()));
        body.append("\nsha-kind=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(sha_value.kind()));
        body.append("\nexists=");
        ruvia::http_append_signed(body, static_cast<std::int64_t>(exists.size()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> blocking_pop(ruvia::context& c) {
        const std::array<std::string_view, 1> keys{"ruvia:example:blocking"};
        auto left = co_await c.redis().blpop(
            keys, ruvia::redis_block_wait::for_duration(std::chrono::seconds(1)));
        auto right = co_await c.redis().brpop(
            keys, ruvia::redis_block_wait::for_duration(std::chrono::seconds(1)));

        std::string body;
        body.append("left=");
        append_bool(body, left.has_value());
        body.append("\nright=");
        append_bool(body, right.has_value());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> alias_value(ruvia::context& c) {
        auto value = co_await c.redis("cache").get(c.req().param("key").value_or(""));
        std::string body;
        if (value) {
            body.append(*value);
        }
        co_return c.text(std::move(body));
    }
};

}  // namespace

int main() {
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        auto config = example::redis_config(env_value);
        app.redis({.config_ = config})
            .redis({.alias_ = "cache", .config_ = config})
            .listen({.address_ = "0.0.0.0",
                .http_ = env_value.get<std::uint16_t>("RUVIA_PORT").value_or(8090)})
            .server({
                .worker_count_ = env_value.get<std::uint32_t>("RUVIA_WORKERS").value_or(2),
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}

// HASH CRUD works with ordinary Redis. Run once with --create-index against
// Redis Search before using GET /users. Index creation is explicit and errors
// if the index already exists; this example never drops existing data/indexes.
// Build with RUVIA_ENABLE_REDIS=ON; configure RUVIA_REDIS_HOST/PORT/USER/PASSWORD.
// Run on port 8091. POST {"id":"1","name":"Ada","age":30} as JSON to
// /users, then GET /users/1. GET /users performs the indexed adult search.
// backend_tls.h defines RUVIA_REDIS_TLS/CA/CERT/KEY for Redis transport.

RUVIA_REDIS_ENTITY(cached_user, "users",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string),
    RUVIA_REDIS_COLUMN(age, std::uint32_t));

const ruvia::redis_repository_config user_redis_config{
    .prefix_ = "ruvia:example:users",
    .indexes_ = {
        {.column_ = "name", .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true},
        {.column_ = "age", .kind_ = ruvia::redis_index_kind::numeric},
    },
};
RUVIA_MODEL(create_cached_user,
    RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_MIN(1, "id is required"),
        RUVIA_MAX(64, "id is too long")),
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(1, "name is required"),
        RUVIA_MAX(120, "name is too long")),
    RUVIA_REQUIRED_FIELD(age, ruvia::uint32, RUVIA_MAX(130, "age is too large")));

RUVIA_MODEL(cached_user_response,
    RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(age, ruvia::uint32));

RUVIA_MODEL(cached_users_response,
    RUVIA_REQUIRED_FIELD(users, ruvia::array<cached_user_response>));

class cached_user_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/users")
    RUVIA_ROUTES_BEGIN(cached_user_controller)
    RUVIA_POST("", create, ruvia::json_body<create_cached_user>);
    RUVIA_GET("", adults);
    RUVIA_GET("/:id", find);
    RUVIA_ROUTES_END

private:
    static void fill(cached_user_response& response, const cached_user& user_value) {
        response.set<"id">(user_value.get<"id">().view());
        response.set<"name">(user_value.get<"name">().view());
        response.set<"age">(ruvia::uint32{user_value.get<"age">()});
    }
    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        const auto& request = c.req().validated<create_cached_user>();
        cached_user user_value;
        user_value.set<"id">(request.get<"id">().view());
        user_value.set<"name">(request.get<"name">().view());
        user_value.set<"age">(static_cast<std::uint32_t>(request.get<"age">()));
        auto users = c.redis().repository<cached_user>(user_redis_config);
        const auto result_value = co_await users.insert(user_value, {.ttl_ = std::chrono::hours(1)});
        if (result_value.affected_entities() == 0) {
            co_return c.error({.status_ = ruvia::http_status::conflict, .message_ = "user already exists"});
        }
        cached_user_response response;
        fill(response, user_value);
        c.draft().status(ruvia::http_status::created);
        co_return c.json(response);
    }
    ruvia::task<ruvia::http_response> find(ruvia::context& c) {
        auto user_value = co_await c.redis().repository<cached_user>(user_redis_config).find_one({.where_ = cached_user::column<"id">() == c.req().param("id").value_or("")});
        if (!user_value) {
            co_return c.error({.status_ = ruvia::http_status::not_found, .message_ = "user not found"});
        }
        cached_user_response response;
        fill(response, *user_value);
        co_return c.json(response);
    }
    ruvia::task<ruvia::http_response> adults(ruvia::context& c) {
        const ruvia::redis_find_options find_options{
            .where_ = cached_user::column<"age">() >= 18,
            .order_ = {{.column_ = "name", .direction_ = ruvia::redis_order_direction::asc}},
            .take_ = 20};
        auto users = co_await c.redis().repository<cached_user>(user_redis_config).find(find_options);
        cached_users_response response;
        auto& output = response.ensure<"users">();
        for (const auto& user : users) {
            auto& item = output.emplace_back();
            fill(item, user);
        }
        co_return c.json(response);
    }
};

[[maybe_unused]] int run_redis_orm_main(int argc, char** argv) {
    try {
        const bool create_index = argc == 2 && std::string_view(argv[1]) == "--create-index";
        bool failed = false;
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        auto config = example::redis_config(env_value);
        app.redis({.config_ = config})
            .listen({.address_ = "127.0.0.1", .http_ = 8091})
            .server({.worker_count_ = 1, .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .on_start([create_index, &failed] {
                if (!create_index) {
                    return;
                }
                const auto workers = ruvia::app().workers();
                std::promise<void> completion;
                auto result_value = completion.get_future();
                const auto posted = workers.front().post([completion = std::move(completion)](ruvia::web_worker_context& worker_value) mutable -> ruvia::task<void> {
                    try {
                        co_await worker_value.redis().repository<cached_user>(user_redis_config).create_index();
                        completion.set_value();
                    } catch (...) {
                        completion.set_exception(std::current_exception());
                    }
                });
                if (posted != ruvia::post_status::accepted) {
                    failed = true;
                } else {
                    // Only the lifecycle caller blocks. Workers are ready before
                    // on_start; request admission stays closed until this returns.
                    // Abandoning an unstarted job destroys its promise and wakes get().
                    try {
                        result_value.get();
                        std::cout << "Redis user index created.\n";
                    } catch (...) {
                        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
                        failed = true;
                    }
                }
                // --create-index is a one-shot operation, never a serving mode.
                ruvia::app().stop();
            })
            .run();
        return failed ? 1 : 0;
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// Redis Streams consumer groups and the generic command escape hatch.
// Requires RUVIA_ENABLE_REDIS=ON and Redis configured with RUVIA_REDIS_* below.
// Run ruvia_example_redis_streams, then POST /streams/setup once on port 8095.
// POST /streams/publish with a text body; GET /streams/consume prints and ACKs it.
// An idle read waits at most 500 ms and returns "no messages". Setup deliberately
// reports an existing group instead of deleting somebody else's stream/group.
// Demo data stays under ruvia:example:stream; use a disposable Redis database.
// backend_tls.h defines RUVIA_REDIS_TLS/CA/CERT/KEY for Redis transport.

namespace redis_streams_example {

constexpr std::string_view stream_key = "ruvia:example:stream";
constexpr std::string_view group_name = "example-readers";

class streams_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/streams")
    RUVIA_ROUTES_BEGIN(streams_controller)
    RUVIA_POST("/setup", setup);
    RUVIA_POST("/publish", publish, ruvia::body_limit<4096>);
    RUVIA_GET("/consume", consume);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> setup(ruvia::context& c) {
        // Commands without a typed convenience API still use the same bounded
        // pool, cancellation token, PMR result ownership, and error handling.
        auto created = co_await c.redis().command("XGROUP", "CREATE", stream_key, group_name, "0", "MKSTREAM");
        // command() returns Redis error replies as values; BUSYGROUP means the
        // group already exists and is reported, never recreated.
        if (created.is_error()) {
            if (created.error().starts_with("BUSYGROUP")) {
                co_return c.error({.status_ = ruvia::http_status::conflict, .message_ = "consumer group already exists"});
            }
            co_return c.error({.status_ = ruvia::http_status::bad_gateway, .message_ = "consumer group setup failed"});
        }
        co_return c.text(created.string());
    }

    ruvia::task<ruvia::http_response> publish(ruvia::context& c) {
        const auto body = co_await c.req().text();
        auto result_value = co_await c.redis().command("XADD", stream_key, "*", "body", body);
        co_return c.text(result_value.string());
    }

    ruvia::task<ruvia::http_response> consume(ruvia::context& c) {
        auto redis = c.redis().with_options({.timeout_ = std::chrono::seconds(2)});
        // ">" requests never-delivered entries. A real consumer also reclaims
        // abandoned pending work (XAUTOCLAIM) and makes its effects idempotent.
        const std::array<ruvia::redis_stream_read_view, 1> streams{{{stream_key, ">"}}};
        auto result_value = co_await redis.xread_group(group_name, "example-consumer", streams,
            {.count_ = 10, .block_ = ruvia::redis_block_wait::for_duration(std::chrono::milliseconds(500))});
        if (!result_value) {
            co_return c.text("no messages\n");
        }
        std::string output;
        for (const auto& stream : result_value->streams()) {
            for (const auto& entry : stream.entries()) {
                output.append(entry.id()).append("\n");
                for (const auto& field : entry.fields()) {
                    output.append(field.key()).append("=").append(field.value()).append("\n");
                }
                // ACK only after processing succeeds. A cancelled request or
                // failed command leaves the entry pending for recovery.
                auto ack = co_await redis.command("XACK", stream.stream(), group_name, entry.id());
                output.append("ack=").append(std::to_string(ack.integer())).append("\n");
            }
        }
        co_return c.text(std::move(output));
    }
};

}  // namespace redis_streams_example

[[maybe_unused]] int run_redis_streams_main() {
    using namespace redis_streams_example;
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        auto config = example::redis_config(env_value);
        // Blocking readers do not occupy ordinary command connections.
        config.blocking_pool_size_per_worker_ = 2;
        app.server({.process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .listen({.address_ = "127.0.0.1", .http_ = 8095})
            .redis({.config_ = config})
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// Redis Pub/Sub: a dedicated subscription connection fanned out as server-sent events.
// Requires RUVIA_ENABLE_REDIS=ON and Redis configured with RUVIA_REDIS_* below.
// Run ruvia_example_redis_pubsub on port 8096, then:
// curl -N http://127.0.0.1:8096/pubsub/events
// curl --data 'hello' http://127.0.0.1:8096/pubsub/publish
// Every subscriber connection gets its own Redis connection; the request's
// stop token closes it when the SSE client goes away. Pub/Sub delivers at most
// once: a closed subscription has missed what was published meanwhile.
// backend_tls.h defines RUVIA_REDIS_TLS/CA/CERT/KEY for Redis transport.

namespace redis_pubsub_example {

constexpr std::string_view channel_name = "ruvia:example:channel";
constexpr std::string_view channel_pattern = "ruvia:example:*";

class pubsub_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/pubsub")
    RUVIA_ROUTES_BEGIN(pubsub_controller)
    RUVIA_POST("/publish", publish, ruvia::body_limit<4096>);
    RUVIA_GET("/events", events_value);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> publish(ruvia::context& c) {
        const auto body = co_await c.req().text();
        // PUBLISH is an ordinary command on the pooled connections and reports
        // how many subscriber connections received the message.
        const auto receivers = co_await c.redis().publish(channel_name, body);
        std::string output;
        output.append("receivers=");
        ruvia::http_append_signed(output, receivers);
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::task<void> events_value(ruvia::context& c) {
        auto events_value = c.stream_sse();
        if (co_await events_value.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        // The subscription inherits the request stop token: when the SSE
        // client disconnects, the pending read() ends with std::nullopt.
        auto subscription = c.redis().subscription();
        const std::array<std::string_view, 1> channels{channel_name};
        co_await subscription.subscribe(channels);
        const std::array<std::string_view, 1> patterns{channel_pattern};
        co_await subscription.psubscribe(patterns);
        // Confirmations and messages arrive through read() in wire order. A
        // message published to the channel is delivered twice here: once for
        // the channel subscription and once for the matching pattern.
        while (auto event = co_await subscription.read()) {
            if (events_value.aborted()) {
                co_return;
            }
            if (const auto* confirmation = event->confirmation()) {
                const auto kind = confirmation->kind();
                const bool subscribed = kind == ruvia::redis_pubsub_confirmation_kind::subscribed ||
                                        kind == ruvia::redis_pubsub_confirmation_kind::pattern_subscribed;
                co_await events_value.write({.data_ = confirmation->name().value_or(""),
                    .event_ = subscribed ? "subscribed" : "unsubscribed"});
            } else if (const auto* message = event->message()) {
                // The event borrows the subscription's reply storage only
                // until the next read(); the SSE write copies it first.
                co_await events_value.write({.data_ = message->payload(),
                    .event_ = message->pattern() ? "pmessage" : "message"});
            }
        }
    }
};

}  // namespace redis_pubsub_example

[[maybe_unused]] int run_redis_pubsub_main() {
    using namespace redis_pubsub_example;
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        app.server({.process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .listen({.address_ = "127.0.0.1", .http_ = 8096})
            .redis({.config_ = example::redis_config(env_value)})
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}
