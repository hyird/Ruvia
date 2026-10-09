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

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

#include "backend_tls.h"

namespace {

void assign_if_present(std::string& target, std::optional<std::string_view> value) {
    if (value) {
        target.assign(value->data(), value->size());
    }
}

ruvia::redis_config redis_config(const example::environment& env_value) {
    ruvia::redis_config config;
    config.tls_ = example::backend_tls("RUVIA_REDIS", env_value);
    assign_if_present(config.host_, env_value.get("RUVIA_REDIS_HOST"));
    assign_if_present(config.username_, env_value.get("RUVIA_REDIS_USER"));
    assign_if_present(config.password_, env_value.get("RUVIA_REDIS_PASSWORD"));
    if (const auto port = env_value.get<std::uint16_t>("RUVIA_REDIS_PORT")) {
        config.port_ = *port;
    }
    if (const auto database = env_value.get<std::uint32_t>("RUVIA_REDIS_DATABASE")) {
        config.database_ = *database;
    }
    if (const auto pool_size = env_value.get<std::uint32_t>("RUVIA_REDIS_POOL_SIZE_PER_WORKER")) {
        config.pool_size_per_worker_ = *pool_size;
    }
    if (const auto pool_size = env_value.get<std::uint32_t>("RUVIA_REDIS_BLOCKING_POOL_SIZE_PER_WORKER")) {
        config.blocking_pool_size_per_worker_ = *pool_size;
    }
    return config;
}

void append_signed(std::pmr::string& output, std::int64_t value) {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format integer");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

void append_bool(std::pmr::string& output, bool value) {
    output.append(value ? "true" : "false");
}

void append_ttl(std::pmr::string& output, const ruvia::redis_ttl& ttl) {
    switch (ttl.state()) {
        case ruvia::redis_ttl_state::missing:
            output.append("missing");
            break;
        case ruvia::redis_ttl_state::persistent:
            output.append("persistent");
            break;
        case ruvia::redis_ttl_state::expiring:
            append_signed(output, ttl.remaining()->count());
            output.append("ms");
            break;
    }
}

class redis_controller final : public ruvia::controller<redis_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/redis")

    RUVIA_ROUTES_BEGIN
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
            .with_options({.timeout_ = std::chrono::seconds(2), .stop_token_ = c.get_stop_token()})
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
        std::pmr::string body(c.allocator<char>());
        append_signed(body, value);
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

        std::pmr::string body(c.allocator<char>());
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

        std::pmr::string body(c.allocator<char>());
        body.append("previous=");
        body.append(previous.previous().value_or(""));
        body.append("\ninserted=");
        append_bool(body, inserted.applied());
        body.append("\nreplaced=");
        body.append(replaced.previous().value_or(""));
        body.append("\nappended=");
        append_signed(body, appended);
        body.append("\nlength=");
        append_signed(body, length);
        body.append("\ndeleted=");
        body.append(deleted.value_or(""));
        body.append("\nmget=");
        append_signed(body, static_cast<std::int64_t>(values.size()));
        body.append("\ndecr=");
        append_signed(body, decremented);
        body.append("\ndecr-by=");
        append_signed(body, decremented_by);
        body.append("\nincr-by=");
        append_signed(body, incremented_by);
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

        std::pmr::string body(c.allocator<char>());
        body.append("changed=");
        append_signed(body, changed + extra);
        body.append("\ncount=");
        append_signed(body, count);
        body.append("\nname=");
        body.append(name.value_or(""));
        body.append("\nhmget=");
        append_signed(body, static_cast<std::int64_t>(values.size()));
        body.append("\nall=");
        append_signed(body, static_cast<std::int64_t>(all.size()));
        body.append("\nkeys=");
        append_signed(body, static_cast<std::int64_t>(keys.size()));
        body.append("\nvalues=");
        append_signed(body, static_cast<std::int64_t>(hvals.size()));
        body.append("\nexists=");
        append_bool(body, exists);
        body.append("\nlength=");
        append_signed(body, length);
        body.append("\nremoved=");
        append_signed(body, removed);
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

        std::pmr::string body(c.allocator<char>());
        body.append("pushed=");
        append_signed(body, left + right);
        body.append("\nlength=");
        append_signed(body, length);
        body.append("\nrange=");
        append_signed(body, static_cast<std::int64_t>(values.size()));
        body.append("\nfirst=");
        body.append(first.value_or(""));
        body.append("\nremoved=");
        append_signed(body, removed);
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

        std::pmr::string body(c.allocator<char>());
        body.append("added=");
        append_signed(body, added + also_added);
        body.append("\nmembers=");
        append_signed(body, static_cast<std::int64_t>(members.size()));
        body.append("\nsize=");
        append_signed(body, size);
        body.append("\nis-member=");
        append_bool(body, member);
        body.append("\nrandom=");
        body.append(random.value_or(""));
        body.append("\nintersection=");
        append_signed(body, static_cast<std::int64_t>(intersection.size()));
        body.append("\nunion=");
        append_signed(body, static_cast<std::int64_t>(union_values.size()));
        body.append("\ndifference=");
        append_signed(body, static_cast<std::int64_t>(difference.size()));
        body.append("\npopped=");
        body.append(popped.value_or(""));
        body.append("\nremoved=");
        append_signed(body, removed);
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

        std::pmr::string body(c.allocator<char>());
        body.append("added=");
        append_signed(body, added + added_two);
        body.append("\nvalues=");
        append_signed(body, static_cast<std::int64_t>(values.size()));
        body.append("\nscored=");
        append_signed(body, static_cast<std::int64_t>(scored.size()));
        body.append("\nscore=");
        append_bool(body, score.has_value());
        body.append("\nsize=");
        append_signed(body, size);
        body.append("\ncounted=");
        append_signed(body, counted);
        body.append("\nremoved=");
        append_signed(body, removed);
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

        std::pmr::string body(c.allocator<char>());
        body.append("keys=");
        append_signed(body, static_cast<std::int64_t>(keys.values().size()));
        body.append("\nhash=");
        append_signed(body, static_cast<std::int64_t>(hash.entries().size()));
        body.append("\nset=");
        append_signed(body, static_cast<std::int64_t>(set.values().size()));
        body.append("\nzset=");
        append_signed(body, static_cast<std::int64_t>(zset.entries().size()));
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

        std::pmr::string body(c.allocator<char>());
        body.append("pipeline results=");
        append_signed(body, static_cast<std::int64_t>(results.size()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> transaction(ruvia::context& c) {
        auto tx = c.redis().transaction();
        tx.watch("ruvia:example:tx")
            .set("ruvia:example:tx", "1")
            .incr("ruvia:example:tx")
            .get("ruvia:example:tx");
        auto results = co_await std::move(tx).exec();

        std::pmr::string body(c.allocator<char>());
        body.append("transaction results=");
        append_signed(body, static_cast<std::int64_t>(results.size()));
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

        std::pmr::string body(c.allocator<char>());
        body.append("eval-kind=");
        append_signed(body, static_cast<std::int64_t>(value.kind()));
        body.append("\nsha-kind=");
        append_signed(body, static_cast<std::int64_t>(sha_value.kind()));
        body.append("\nexists=");
        append_signed(body, static_cast<std::int64_t>(exists.size()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> blocking_pop(ruvia::context& c) {
        const std::array<std::string_view, 1> keys{"ruvia:example:blocking"};
        auto left = co_await c.redis().blpop(
            keys, ruvia::redis_block_wait::for_duration(std::chrono::seconds(1)));
        auto right = co_await c.redis().brpop(
            keys, ruvia::redis_block_wait::for_duration(std::chrono::seconds(1)));

        std::pmr::string body(c.allocator<char>());
        body.append("left=");
        append_bool(body, left.has_value());
        body.append("\nright=");
        append_bool(body, right.has_value());
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> alias_value(ruvia::context& c) {
        auto value = co_await c.redis("cache").get(c.req().param("key").value_or(""));
        std::pmr::string body(c.allocator<char>());
        if (value) {
            body.append(*value);
        }
        co_return c.text(std::move(body));
    }
};

}  // namespace

int main() {
    auto& app = ruvia::app();
    app.load_dotenv();
    const example::environment env_value(&app.env());
    auto config = redis_config(env_value);
    app.redis({.config_ = config})
        .redis({.alias_ = "cache", .config_ = config})
        .listen({.address_ = "0.0.0.0",
            .http_ = env_value.get<std::uint16_t>("RUVIA_PORT").value_or(8090)})
        .server({
            .worker_count_ = env_value.get<std::uint32_t>("RUVIA_WORKERS").value_or(2),
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
        })
        .run();
}
