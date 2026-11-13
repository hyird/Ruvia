// websocket: upgrade routes, subprotocol options, lifecycle timeouts,
// text/binary echo and the RFC close handshake.
// Run ruvia_example_websocket, then connect to ws://localhost:8084/ws/echo.
// /ws/chat requires one of chat_options()'s advertised subprotocols: an upgrade
// offering none (send Sec-WebSocket-Protocol: chat.v1) is refused with 400.
// /ws/ticker pushes ticks and then closes with 4001 while its reader waits.
// Read until close; join the close handshake before releasing the connection.

#include <chrono>
#include <exception>
#include <iostream>
#include <system_error>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"

class websocket_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/ws")

    RUVIA_ROUTES_BEGIN(websocket_controller)
    const auto chat_options = ruvia::websocket_route_config{
        .subprotocols_ = {"chat.v1"},
        .require_subprotocol_ = true,
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
    RUVIA_GET_WS("/chat", chat, chat_options);
    RUVIA_GET_WS("/ticker", ticker);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> echo(ruvia::context& c) {
        auto& ws = c.websocket();
        while (auto message = co_await ws.read()) {
            // Zero-copy echo: the frame borrows the read buffer and keeps the
            // read lane until it is written, preserving the message opcode.
            co_await ws.send(std::move(*message));
        }
    }

    ruvia::task<void> chat(ruvia::context& c) {
        auto& ws = c.websocket();
        co_await ws.text("welcome");
        while (auto message = co_await ws.read()) {
            if (message->text()) {
                co_await ws.send(std::move(*message));
            }
        }
        co_await ws.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "bye"});
    }

    // close() claims only the write lane, so the pending read below returns
    // nullopt once the peer answers the 4001 Close (or abort() ends it).
    ruvia::task<void> ticker(ruvia::context& c) {
        auto& ws = c.websocket();
        ruvia::task_scope readers(c.worker());
        readers.spawn(drain(ws));
        std::exception_ptr failure;
        try {
            for (int tick = 0; tick < 5; ++tick) {
                co_await ws.text("tick");
                (void)co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(1));
            }
            co_await ws.close({.code_ = ruvia::websocket_close_code::from_value(4001), .reason_ = "subscription ended"});
        } catch (...) {
            failure = std::current_exception();
            ws.abort();
        }
        co_await readers.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    static ruvia::task<void> drain(ruvia::websocket& ws) {
        for (;;) {
            try {
                if (!co_await ws.read({.timeout_ = std::chrono::seconds(30)})) {
                    co_return;
                }
            } catch (const std::system_error& error) {
                // A read deadline ends only that read; the session stays open.
                if (error.code() != std::errc::timed_out) {
                    throw;
                }
            }
        }
    }
};

int main() {
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8084})
            .server({
                .worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                .max_websocket_message_bytes_ = 16 * 1024 * 1024,
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
