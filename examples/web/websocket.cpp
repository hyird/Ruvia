// WebSocket: upgrade routes, subprotocol options, lifecycle timeouts,
// text/binary echo and the RFC close handshake.

#include <chrono>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

class WebSocketController final : public ruvia::Controller<WebSocketController> {
public:
    RUVIA_CONTROLLER_GROUP("/ws")

    RUVIA_ROUTES_BEGIN
    const auto chatOptions = ruvia::WebSocketRouteOptions{
        .subprotocols = "chat.v1",
        .lifecycle =
            {
                .heartbeat = ruvia::WebSocketHeartbeatPolicy::periodic(std::chrono::seconds(30), std::chrono::seconds(10)),
                .closeHandshakeTimeout = std::chrono::seconds(5),
            },
    };
    RUVIA_GET_WS("/echo", echo);
    RUVIA_GET_WS_OPTIONS("/chat", chat, chatOptions);
    RUVIA_ROUTES_END

private:
    ruvia::Task<void> echo(ruvia::Context& c) {
        auto& ws = c.webSocket();
        while (auto message = co_await ws.read()) {
            if (message->text()) {
                co_await ws.text(message->payload());
            } else if (message->binary()) {
                co_await ws.binary(message->payload());
            }
        }
    }

    ruvia::Task<void> chat(ruvia::Context& c) {
        auto& ws = c.webSocket();
        co_await ws.text("welcome");
        while (auto message = co_await ws.read()) {
            if (message->text()) {
                co_await ws.text(message->payload());
            }
        }
        co_await ws.close(1000, "bye");
    }
};

int main() {
    ruvia::app().setListeners({ruvia::ListenerConfig::http("0.0.0.0", 8084)}).setWorkersPerListener(2).setSignalShutdown(true).setMaxWebSocketMessageBytes(16 * 1024 * 1024).run();
}
