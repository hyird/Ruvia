// websocket: upgrade routes, subprotocol options, lifecycle timeouts,
// text/binary echo and the RFC close handshake.
// Run ruvia_example_websocket, then connect to ws://localhost:8084/ws/echo.
// /ws/chat requires one of chat_options()'s advertised subprotocols.
// Read until close; join the close handshake before releasing the connection.

#include <chrono>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

class websocket_controller final : public ruvia::controller<websocket_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/ws")

    RUVIA_ROUTES_BEGIN
    const auto chat_options = ruvia::websocket_route_config{
        .subprotocols_ = {"chat.v1"},
        .lifecycle_ =
            {
                .heartbeat_ =
                    {
                        .ping_interval_ = std::chrono::seconds(30),
                        .pong_timeout_ = std::chrono::seconds(10),
                    },
                .close_handshake_timeout_ = std::chrono::seconds(5),
            },
    };
    RUVIA_GET_WS("/echo", echo);
    RUVIA_GET_WS_OPTIONS("/chat", chat, chat_options);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> echo(ruvia::context& c) {
        auto& ws = c.get_websocket();
        while (auto message = co_await ws.read()) {
            if (message->text()) {
                co_await ws.text(message->payload());
            } else if (message->binary()) {
                co_await ws.binary(message->payload());
            }
        }
    }

    ruvia::task<void> chat(ruvia::context& c) {
        auto& ws = c.get_websocket();
        co_await ws.text("welcome");
        while (auto message = co_await ws.read()) {
            if (message->text()) {
                co_await ws.text(message->payload());
            }
        }
        co_await ws.close({.code_ = 1000, .reason_ = "bye"});
    }
};

int main() {
    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8084})
        .server({
            .worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
            .max_websocket_message_bytes_ = 16 * 1024 * 1024,
        })
        .run();
}
