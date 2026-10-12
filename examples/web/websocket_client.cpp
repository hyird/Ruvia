// Standalone WebSocket client. First run ruvia_example_http_features.
// Connects to localhost:8093; RUVIA_CLIENT_TLS=true selects TLS on port 8444.
// Set RUVIA_TLS_CA to trust the local CA. Expected output: text=hello,
// binary bytes=3, then a clean close.

#include "ruvia/web/error.h"
#include "ruvia/web/websocket_client.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/web/dotenv.h"

#include "environment.h"

namespace {

ruvia::task<void> exchange(ruvia::event_loop loop, ruvia::websocket_client_config config) {
    ruvia::websocket_client client(loop, config);
    std::exception_ptr failure;
    try {
        co_await client.connect();
        // A configured timeout and the worker shutdown token bound all waits.
        // The client, its handles, and payload views stay on this owner loop.
        auto peer = client.with_options({.timeout_ = std::chrono::seconds(5)});
        co_await peer.text("hello");
        auto text = co_await peer.read();
        if (!text || !text->text() || text->payload() != "hello") {
            throw std::runtime_error("unexpected text echo");
        }
        std::cout << "text=" << text->payload() << '\n';
        co_await peer.binary(std::string_view("\0\1\2", 3));
        auto binary = co_await peer.read();
        if (!binary || !binary->binary() || binary->payload() != std::string_view("\0\1\2", 3)) {
            throw std::runtime_error("unexpected binary echo");
        }
        std::cout << "binary bytes=" << binary->payload().size() << '\n';
        // Control frames are handled by the client driver. Close must not
        // overlap a read; all data operations above have already completed.
        co_await peer.ping("alive");
        co_await peer.close({.code_ = 1000, .reason_ = "example complete"});
    } catch (...) {
        failure = std::current_exception();
    }
    // Cancellation/abort only requests shutdown. Always await teardown on
    // both success and failure before destroying the client or retiring its loop.
    co_await client.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace

int main() {
    try {
        const example::environment env;
        const bool tls = env.get<bool>("RUVIA_CLIENT_TLS").value_or(false);
        ruvia::websocket_client_config config{
            .scheme_ = tls ? ruvia::websocket_scheme::wss : ruvia::websocket_scheme::ws,
            .host_ = "localhost",
            .port_ = tls ? 8444 : 8093,
            .target_ = "/features/ws",
            .read_timeout_ = std::chrono::seconds(5),
            .tls_ = {.ca_file_ = std::string(env.get("RUVIA_TLS_CA").value_or(""))},
        };
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        loops.start();
        auto done = loops.loop(0).start(exchange(loops.loop(0), std::move(config)));
        done.get();
        loops.stop();
        loops.join();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
