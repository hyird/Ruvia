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

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

namespace {

void assignIfPresent(std::pmr::string& target, std::optional<std::string_view> value) {
    if (value) {
        target.assign(value->data(), value->size());
    }
}

ruvia::RedisConfig redisConfig(const ruvia::Env& env) {
    ruvia::RedisConfig config;
    assignIfPresent(config.host, env.get("RUVIA_REDIS_HOST"));
    assignIfPresent(config.username, env.get("RUVIA_REDIS_USER"));
    assignIfPresent(config.password, env.get("RUVIA_REDIS_PASSWORD"));
    if (const auto port = env.get<std::uint16_t>("RUVIA_REDIS_PORT")) {
        config.port = *port;
    }
    if (const auto database = env.get<std::uint32_t>("RUVIA_REDIS_DATABASE")) {
        config.database = *database;
    }
    if (const auto poolSize = env.get<std::uint32_t>("RUVIA_REDIS_POOL_SIZE")) {
        config.poolSizePerWorker = *poolSize;
    }
    return config;
}

void appendSigned(std::pmr::string& output, std::int64_t value) {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format integer");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

void appendBool(std::pmr::string& output, bool value) {
    output.append(value ? "true" : "false");
}

class RedisController final : public ruvia::Controller<RedisController> {
public:
    RUVIA_CONTROLLER_GROUP("/redis")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/ping", ping);
    RUVIA_GET("/value/:key", getValue);
    RUVIA_POST("/value/:key", setValue);
    RUVIA_POST("/counter/:key", increment);
    RUVIA_GET("/keys/:key", keyMetadata);
    RUVIA_POST("/strings/:key", strings);
    RUVIA_POST("/hashes/:key", hashes);
    RUVIA_POST("/lists/:key", lists);
    RUVIA_POST("/sets/:key", sets);
    RUVIA_POST("/sorted-sets/:key", sortedSets);
    RUVIA_GET("/scan", scan);
    RUVIA_POST("/pipeline", pipeline);
    RUVIA_POST("/transaction", transaction);
    RUVIA_POST("/scripts", scripts);
    RUVIA_GET("/blocking-pop", blockingPop);
    RUVIA_GET("/alias/:key", aliasValue);
    RUVIA_ROUTES_END

    ruvia::Task<ruvia::HttpResponse> ping(ruvia::Context& c) {
        co_await c.redis().ping();
        auto message = co_await c.redis().ping("hello");
        co_return c.text(message);
    }

    ruvia::Task<ruvia::HttpResponse> getValue(ruvia::Context& c) {
        auto value = co_await c.redis().get(c.param("key").toStringView().value_or(""));
        if (!value) {
            co_return c.error(404, "not_found", "redis key not found");
        }
        co_return c.text(*value);
    }

    ruvia::Task<ruvia::HttpResponse> setValue(ruvia::Context& c) {
        auto body = co_await c.body();
        co_await c.redis().set(c.param("key").toStringView().value_or(""), body);
        co_return c.text("OK\n");
    }

    ruvia::Task<ruvia::HttpResponse> increment(ruvia::Context& c) {
        const auto value = co_await c.redis().incr(c.param("key").toStringView().value_or(""));
        std::pmr::string body(c.allocator<char>());
        appendSigned(body, value);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> keyMetadata(ruvia::Context& c) {
        const auto key = c.param("key").toStringView().value_or("");
        const auto exists = co_await c.redis().exists(key);
        const auto touched = co_await c.redis().touch(key);
        const auto type = co_await c.redis().type(key);
        const auto ttl = co_await c.redis().ttl(key);
        const auto pttl = co_await c.redis().pttl(key);
        const auto persisted = co_await c.redis().persist(key);

        std::pmr::string body(c.allocator<char>());
        body.append("exists=");
        appendBool(body, exists);
        body.append("\ntouched=");
        appendBool(body, touched);
        body.append("\ntype=");
        body.append(type);
        body.append("\nttl=");
        appendSigned(body, ttl);
        body.append("\npttl=");
        appendSigned(body, pttl);
        body.append("\npersisted=");
        appendBool(body, persisted);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> strings(ruvia::Context& c) {
        const auto key = c.param("key").toStringView().value_or("");
        const auto value = co_await c.body();
        const std::array<std::pair<std::string_view, std::string_view>, 2> items{{
            {"ruvia:example:mset:a", "one"},
            {"ruvia:example:mset:b", "two"},
        }};
        const std::array<std::string_view, 2> keys{items[0].first, items[1].first};

        co_await c.redis().set(key, value);
        ruvia::RedisSetOptions setOptions;
        setOptions.get = true;
        auto previous = co_await c.redis().set(key, "fresh", setOptions);
        co_await c.redis().setEx("ruvia:example:ttl", std::chrono::seconds(60), "ttl");
        const auto inserted = co_await c.redis().setNx("ruvia:example:nx", "first");
        auto replaced = co_await c.redis().getSet(key, "replaced");
        const auto appended = co_await c.redis().append(key, "+tail");
        const auto length = co_await c.redis().strlen(key);
        auto deleted = co_await c.redis().getDel("ruvia:example:nx");
        co_await c.redis().mset(items);
        auto values = co_await c.redis().mget(keys);
        const auto decremented = co_await c.redis().decr(key);
        const auto decrementedBy = co_await c.redis().decrBy(key, 2);
        const auto incrementedBy = co_await c.redis().incrBy(key, 3);

        std::pmr::string body(c.allocator<char>());
        body.append("previous=");
        body.append(previous.value_or(""));
        body.append("\ninserted=");
        appendBool(body, inserted);
        body.append("\nreplaced=");
        body.append(replaced.value_or(""));
        body.append("\nappended=");
        appendSigned(body, appended);
        body.append("\nlength=");
        appendSigned(body, length);
        body.append("\ndeleted=");
        body.append(deleted.value_or(""));
        body.append("\nmget=");
        appendSigned(body, static_cast<std::int64_t>(values.size()));
        body.append("\ndecr=");
        appendSigned(body, decremented);
        body.append("\ndecr-by=");
        appendSigned(body, decrementedBy);
        body.append("\nincr-by=");
        appendSigned(body, incrementedBy);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> hashes(ruvia::Context& c) {
        const auto key = c.param("key").toStringView().value_or("");
        const std::array<std::pair<std::string_view, std::string_view>, 2> fields{{
            {"name", "ruvia"},
            {"kind", "framework"},
        }};
        const std::array<std::string_view, 2> names{"name", "kind"};

        const auto changed = co_await c.redis().hset(key, fields);
        const auto extra = co_await c.redis().hset(key, "count", "1");
        const auto count = co_await c.redis().hincrBy(key, "count", 1);
        auto name = co_await c.redis().hget(key, "name");
        auto values = co_await c.redis().hmget(key, names);
        auto all = co_await c.redis().hgetAll(key);
        auto keys = co_await c.redis().hkeys(key);
        auto hvals = co_await c.redis().hvals(key);
        const auto exists = co_await c.redis().hexists(key, "name");
        const auto length = co_await c.redis().hlen(key);
        const auto removed = co_await c.redis().hdel(key, "kind");

        std::pmr::string body(c.allocator<char>());
        body.append("changed=");
        appendSigned(body, changed + extra);
        body.append("\ncount=");
        appendSigned(body, count);
        body.append("\nname=");
        body.append(name.value_or(""));
        body.append("\nhmget=");
        appendSigned(body, static_cast<std::int64_t>(values.size()));
        body.append("\nall=");
        appendSigned(body, static_cast<std::int64_t>(all.size()));
        body.append("\nkeys=");
        appendSigned(body, static_cast<std::int64_t>(keys.size()));
        body.append("\nvalues=");
        appendSigned(body, static_cast<std::int64_t>(hvals.size()));
        body.append("\nexists=");
        appendBool(body, exists);
        body.append("\nlength=");
        appendSigned(body, length);
        body.append("\nremoved=");
        appendSigned(body, removed);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> lists(ruvia::Context& c) {
        const auto key = c.param("key").toStringView().value_or("");
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
        auto poppedLeft = co_await c.redis().lpop(key);
        auto poppedRight = co_await c.redis().rpop(key);

        std::pmr::string body(c.allocator<char>());
        body.append("pushed=");
        appendSigned(body, left + right);
        body.append("\nlength=");
        appendSigned(body, length);
        body.append("\nrange=");
        appendSigned(body, static_cast<std::int64_t>(values.size()));
        body.append("\nfirst=");
        body.append(first.value_or(""));
        body.append("\nremoved=");
        appendSigned(body, removed);
        body.append("\nleft=");
        body.append(poppedLeft.value_or(""));
        body.append("\nright=");
        body.append(poppedRight.value_or(""));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> sets(ruvia::Context& c) {
        const auto key = c.param("key").toStringView().value_or("");
        const auto added = co_await c.redis().sadd(key, "one");
        const auto alsoAdded = co_await c.redis().sadd("ruvia:example:set:other", "one");
        auto members = co_await c.redis().smembers(key);
        const auto size = co_await c.redis().scard(key);
        const auto member = co_await c.redis().sismember(key, "one");
        auto random = co_await c.redis().srandMember(key);
        const std::array<std::string_view, 2> keys{key, "ruvia:example:set:other"};
        auto intersection = co_await c.redis().sinter(keys);
        auto unionValues = co_await c.redis().sunion(keys);
        auto difference = co_await c.redis().sdiff(keys);
        auto popped = co_await c.redis().spop(key);
        const auto removed = co_await c.redis().srem("ruvia:example:set:other", "one");

        std::pmr::string body(c.allocator<char>());
        body.append("added=");
        appendSigned(body, added + alsoAdded);
        body.append("\nmembers=");
        appendSigned(body, static_cast<std::int64_t>(members.size()));
        body.append("\nsize=");
        appendSigned(body, size);
        body.append("\nis-member=");
        appendBool(body, member);
        body.append("\nrandom=");
        body.append(random.value_or(""));
        body.append("\nintersection=");
        appendSigned(body, static_cast<std::int64_t>(intersection.size()));
        body.append("\nunion=");
        appendSigned(body, static_cast<std::int64_t>(unionValues.size()));
        body.append("\ndifference=");
        appendSigned(body, static_cast<std::int64_t>(difference.size()));
        body.append("\npopped=");
        body.append(popped.value_or(""));
        body.append("\nremoved=");
        appendSigned(body, removed);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> sortedSets(ruvia::Context& c) {
        const auto key = c.param("key").toStringView().value_or("");
        const auto added = co_await c.redis().zadd(key, 1.0, "one");
        const auto addedTwo = co_await c.redis().zadd(key, 2.0, "two");
        auto values = co_await c.redis().zrange(key, 0, -1);
        auto scored = co_await c.redis().zrangeWithScores(key, 0, -1);
        auto score = co_await c.redis().zscore(key, "one");
        const auto size = co_await c.redis().zcard(key);
        const auto counted = co_await c.redis().zcount(key, 0.0, 10.0);
        const auto removed = co_await c.redis().zrem(key, "two");

        std::pmr::string body(c.allocator<char>());
        body.append("added=");
        appendSigned(body, added + addedTwo);
        body.append("\nvalues=");
        appendSigned(body, static_cast<std::int64_t>(values.size()));
        body.append("\nscored=");
        appendSigned(body, static_cast<std::int64_t>(scored.size()));
        body.append("\nscore=");
        appendBool(body, score.has_value());
        body.append("\nsize=");
        appendSigned(body, size);
        body.append("\ncounted=");
        appendSigned(body, counted);
        body.append("\nremoved=");
        appendSigned(body, removed);
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> scan(ruvia::Context& c) {
        ruvia::RedisScanOptions keyScan;
        keyScan.match = "ruvia:example:*";
        keyScan.count = 16;
        ruvia::RedisScanOptions memberScan;
        memberScan.count = 16;
        const auto keys = co_await c.redis().scan(keyScan);
        const auto hash = co_await c.redis().hscan("ruvia:example:hash", memberScan);
        const auto set = co_await c.redis().sscan("ruvia:example:set", memberScan);
        const auto zset = co_await c.redis().zscan("ruvia:example:zset", memberScan);

        std::pmr::string body(c.allocator<char>());
        body.append("keys=");
        appendSigned(body, static_cast<std::int64_t>(keys.values.size()));
        body.append("\nhash=");
        appendSigned(body, static_cast<std::int64_t>(hash.entries.size()));
        body.append("\nset=");
        appendSigned(body, static_cast<std::int64_t>(set.values.size()));
        body.append("\nzset=");
        appendSigned(body, static_cast<std::int64_t>(zset.entries.size()));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> pipeline(ruvia::Context& c) {
        auto pipeline = c.redis().pipeline();
        auto results = co_await pipeline
            .set("ruvia:example:pipeline", "1")
            .get("ruvia:example:pipeline")
            .incr("ruvia:example:pipeline")
            .hset("ruvia:example:pipeline:hash", "field", "value")
            .hget("ruvia:example:pipeline:hash", "field")
            .lpush("ruvia:example:pipeline:list", "item")
            .sadd("ruvia:example:pipeline:set", "member")
            .zadd("ruvia:example:pipeline:zset", 1.0, "member")
            .command({"TYPE", "ruvia:example:pipeline"})
            .exec();

        std::pmr::string body(c.allocator<char>());
        body.append("pipeline results=");
        appendSigned(body, static_cast<std::int64_t>(results.size()));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> transaction(ruvia::Context& c) {
        const std::array<std::string_view, 1> watched{"ruvia:example:tx"};
        auto tx = c.redis().transaction();
        auto results = co_await tx
            .watch(watched)
            .set("ruvia:example:tx", "1")
            .incr("ruvia:example:tx")
            .get("ruvia:example:tx")
            .exec();

        auto discarded = c.redis().transaction();
        discarded.discard();

        std::pmr::string body(c.allocator<char>());
        body.append("transaction results=");
        appendSigned(body, static_cast<std::int64_t>(results.size()));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> scripts(ruvia::Context& c) {
        static constexpr std::string_view script = "return ARGV[1]";
        const std::array<std::string_view, 1> args{"hello"};
        auto value = co_await c.redis().eval(script, {}, args);
        auto sha = co_await c.redis().scriptLoad(script);
        const std::array<std::string_view, 1> shas{sha};
        auto exists = co_await c.redis().scriptExists(shas);
        auto shaValue = co_await c.redis().evalSha(sha, {}, args);

        std::pmr::string body(c.allocator<char>());
        body.append("eval-kind=");
        appendSigned(body, static_cast<std::int64_t>(value.kind()));
        body.append("\nsha-kind=");
        appendSigned(body, static_cast<std::int64_t>(shaValue.kind()));
        body.append("\nexists=");
        appendSigned(body, static_cast<std::int64_t>(exists.size()));
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> blockingPop(ruvia::Context& c) {
        const std::array<std::string_view, 1> keys{"ruvia:example:blocking"};
        auto left = co_await c.redis().blpop(keys, std::chrono::seconds(1));
        auto right = co_await c.redis().brpop(keys, std::chrono::seconds(1));

        std::pmr::string body(c.allocator<char>());
        body.append("left=");
        appendBool(body, left.has_value());
        body.append("\nright=");
        appendBool(body, right.has_value());
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> aliasValue(ruvia::Context& c) {
        auto value = co_await c.redis("cache").get(c.param("key").toStringView().value_or(""));
        std::pmr::string body(c.allocator<char>());
        if (value) {
            body.append(*value);
        }
        co_return c.text(body);
    }
};

}  // namespace

int main() {
    auto& app = ruvia::app();
    app.loadDotenv();
    auto config = redisConfig(app.env());
    app
        .useRedis(config)
        .useRedis("cache", std::move(config))
        .setListenAddress("0.0.0.0", app.env().get<std::uint16_t>("RUVIA_PORT").value_or(8090))
        .setThreadNum(app.env().get<std::uint32_t>("RUVIA_THREADS").value_or(2))
        .run();
}
