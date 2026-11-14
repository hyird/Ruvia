// Redis Pub/Sub: a dedicated subscription connection fanned out as server-sent events.
// Requires RUVIA_ENABLE_REDIS=ON and Redis configured with RUVIA_REDIS_* below.
// Run ruvia_example_redis_pubsub on port 8096, then:
// curl -N http://127.0.0.1:8096/pubsub/events
// curl --data 'hello' http://127.0.0.1:8096/pubsub/publish
// Every subscriber connection gets its own Redis connection; the request's
// stop token closes it when the SSE client goes away. Pub/Sub delivers at most
// once: a closed subscription has missed what was published meanwhile.
// backend_config.h backend_tls() reads RUVIA_REDIS_TLS/CA/CERT/KEY for Redis transport.

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "app.h"
#include "backend_config.h"
#include "borrowed_text.h"
#include "controller.h"
#include "middleware.h"
#include "redis.h"
#include "streaming.h"

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
                const bool subscribed = kind == ruvia::redis_pubsub_confirmation_kind::subscribed || kind == ruvia::redis_pubsub_confirmation_kind::pattern_subscribed;
                co_await events_value.write({.data_ = confirmation->name().value_or(""), .event_ = subscribed ? "subscribed" : "unsubscribed"});
            } else if (const auto* message = event->message()) {
                // The event borrows the subscription's reply storage only
                // until the next read(); the SSE write copies it first.
                co_await events_value.write({.data_ = message->payload(), .event_ = message->pattern() ? "pmessage" : "message"});
            }
        }
    }
};

}  // namespace

int main() {
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        app.server({.process_signal_handlers_ = ruvia::process_signal_handler_policy::install}).listen({.address_ = "127.0.0.1", .http_ = 8096}).redis({.config_ = example::redis_config(env_value)}).run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
