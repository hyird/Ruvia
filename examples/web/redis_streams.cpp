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

#include "ruvia/web/app.h"
#include "ruvia/web/body_limit.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/redis/redis.h"

#include "backend_tls.h"

namespace {

constexpr std::string_view stream_key = "ruvia:example:stream";
constexpr std::string_view group_name = "example-readers";

class streams_controller final : public ruvia::controller<streams_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/streams")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/setup", setup);
    RUVIA_POST("/publish", publish, ruvia::body_limit<4096>);
    RUVIA_GET("/consume", consume);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> setup(ruvia::context& c) {
        // Commands without a typed convenience API still use the same bounded
        // pool, cancellation token, PMR result ownership, and error handling.
        auto created = co_await c.redis().command("XGROUP", "CREATE", stream_key, group_name, "0", "MKSTREAM");
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
        std::pmr::string output(c.arena());
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

}  // namespace

int main() {
    auto& app = ruvia::app();
    app.load_dotenv();
    const example::environment env_value(&app.env());
    app.server({.process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .listen({.address_ = "127.0.0.1", .http_ = 8095})
        .redis({.config_ = {
                    .host_ = std::string(env_value.get("RUVIA_REDIS_HOST").value_or("127.0.0.1")),
                    .port_ = env_value.get<std::uint16_t>("RUVIA_REDIS_PORT").value_or(6379),
                    .username_ = std::string(env_value.get("RUVIA_REDIS_USER").value_or("")),
                    .password_ = std::string(env_value.get("RUVIA_REDIS_PASSWORD").value_or("")),
                    .tls_ = example::backend_tls("RUVIA_REDIS", env_value),
                    .database_ = env_value.get<std::uint32_t>("RUVIA_REDIS_DATABASE").value_or(0),
                    // Blocking stream reads use a separate pool, leaving normal
                    // commands usable while a consumer waits for new messages.
                    .blocking_pool_size_per_worker_ = 2,
                }})
        .run();
}
