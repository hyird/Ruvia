// Redis Pub/Sub: a dedicated subscription connection fanned out as server-sent events.
// Requires RUVIA_ENABLE_REDIS=ON and Redis configured with RUVIA_REDIS_* below.
// Run ruvia_example_redis_pubsub on port 8096, then:
// curl -N http://127.0.0.1:8096/pubsub/events
// curl --data 'hello' http://127.0.0.1:8096/pubsub/publish
// Every subscriber connection gets its own Redis connection; the request's
// stop token closes it when the SSE client goes away. Pub/Sub delivers at most
// once: a closed subscription has missed what was published meanwhile.
// backend_tls.h defines RUVIA_REDIS_TLS/CA/CERT/KEY for Redis transport.

#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/body_limit.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/streaming.h"

#include "backend_tls.h"

namespace {

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
        std::pmr::string output(c.arena());
        output.append("receivers=");
        append_number(output, receivers);
        output.push_back('\n');
        co_return c.text(std::move(output));
    }

    ruvia::task<void> events_value(ruvia::context& c) {
        auto events_value = c.stream_sse();
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
            switch (event->kind()) {
                case ruvia::redis_pubsub_event_kind::subscribed:
                case ruvia::redis_pubsub_event_kind::unsubscribed:
                    co_await events_value.write({.data_ = event->pattern().empty() ? event->channel() : event->pattern(),
                        .event_ = event->kind() == ruvia::redis_pubsub_event_kind::subscribed ? "subscribed" : "unsubscribed"});
                    break;
                case ruvia::redis_pubsub_event_kind::message:
                    // The event borrows the subscription's reply storage only
                    // until the next read(); the SSE write copies it first.
                    co_await events_value.write({.data_ = event->payload(),
                        .event_ = event->pattern().empty() ? "message" : "pmessage"});
                    break;
            }
        }
    }

    static void append_number(std::pmr::string& output, std::int64_t value) {
        char buffer[32]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    }
};

}  // namespace

int main() {
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        app.server({.process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .listen({.address_ = "127.0.0.1", .http_ = 8096})
            .redis({.config_ = {
                        .host_ = std::string(env_value.get("RUVIA_REDIS_HOST").value_or("127.0.0.1")),
                        .port_ = env_value.get<std::uint16_t>("RUVIA_REDIS_PORT").value_or(6379),
                        .username_ = std::string(env_value.get("RUVIA_REDIS_USER").value_or("")),
                        .password_ = std::string(env_value.get("RUVIA_REDIS_PASSWORD").value_or("")),
                        .tls_ = example::backend_tls("RUVIA_REDIS", env_value),
                        .database_ = env_value.get<std::uint32_t>("RUVIA_REDIS_DATABASE").value_or(0),
                    }})
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
