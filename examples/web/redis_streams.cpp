// Redis Streams consumer groups and the generic command escape hatch.
// Requires RUVIA_ENABLE_REDIS=ON and Redis configured with RUVIA_REDIS_* below.
// Run ruvia_example_redis_streams, then POST /streams/setup once on port 8095.
// POST /streams/publish with a text body; GET /streams/consume prints and ACKs it.
// An idle read waits at most 500 ms and returns "no messages". Setup deliberately
// reports an existing group instead of deleting somebody else's stream/group.
// Demo data stays under ruvia:example:stream; use a disposable Redis database.
// backend_tls.h defines RUVIA_REDIS_TLS/CA/CERT/KEY for Redis transport.

#include <array>
#include <chrono>
#include <string>

#include "ruvia/web/App.h"
#include "ruvia/web/BodyLimit.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/redis/Redis.h"

#include "backend_tls.h"

namespace {

constexpr std::string_view stream_key = "ruvia:example:stream";
constexpr std::string_view group_name = "example-readers";

class streams_controller final : public ruvia::Controller<streams_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/streams")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/setup", setup);
    RUVIA_POST("/publish", publish, ruvia::BodyLimit<4096>);
    RUVIA_GET("/consume", consume);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> setup(ruvia::Context& c) {
        // Commands without a typed convenience API still use the same bounded
        // pool, cancellation token, PMR result ownership, and error handling.
        auto created = co_await c.redis().command("XGROUP", "CREATE", stream_key, group_name, "0", "MKSTREAM");
        co_return c.text(created.string());
    }

    ruvia::Task<ruvia::HttpResponse> publish(ruvia::Context& c) {
        const auto body = co_await c.req().text();
        auto result = co_await c.redis().command("XADD", stream_key, "*", "body", body);
        co_return c.text(result.string());
    }

    ruvia::Task<ruvia::HttpResponse> consume(ruvia::Context& c) {
        auto redis = c.redis().withOptions({.timeout = std::chrono::seconds(2)});
        // ">" requests never-delivered entries. A real consumer also reclaims
        // abandoned pending work (XAUTOCLAIM) and makes its effects idempotent.
        const std::array<ruvia::RedisStreamReadView, 1> streams{{{stream_key, ">"}}};
        auto result = co_await redis.xreadGroup(group_name, "example-consumer", streams,
            {.count = 10, .block = ruvia::RedisBlockWait::forDuration(std::chrono::milliseconds(500))});
        if (!result) {
            co_return c.text("no messages\n");
        }
        std::pmr::string output(c.arena());
        for (const auto& stream : result->streams()) {
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

}  // namespace

int main() {
    auto& app = ruvia::app();
    app.loadDotenv();
    const example::environment env(&app.env());
    app.server({.process_signal_handlers = ruvia::process_signal_handler_policy::install})
        .listen({.address = "127.0.0.1", .http = 8095})
        .redis({.config = {
                    .host = std::string(env.get("RUVIA_REDIS_HOST").value_or("127.0.0.1")),
                    .port = env.get<std::uint16_t>("RUVIA_REDIS_PORT").value_or(6379),
                    .username = std::string(env.get("RUVIA_REDIS_USER").value_or("")),
                    .password = std::string(env.get("RUVIA_REDIS_PASSWORD").value_or("")),
                    .tls = example::backend_tls("RUVIA_REDIS", env),
                    .database = env.get<std::uint32_t>("RUVIA_REDIS_DATABASE").value_or(0),
                    // Blocking stream reads use a separate pool, leaving normal
                    // commands usable while a consumer waits for new messages.
                    .blockingPoolSizePerWorker = 2,
                }})
        .run();
}
