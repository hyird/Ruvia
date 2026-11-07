// Owning WebSocket session from the fluent HTTP client over HTTP/1 or HTTP/2.
// First run ruvia_example_http_features, then this executable with 1 or 2.
// Connects to localhost:8093; RUVIA_CLIENT_TLS=true selects TLS on port 8444.
// Set RUVIA_TLS_CA to trust the local CA. Expected output: text=hello,
// binary bytes=3, then a clean close.

#include <chrono>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client.h"

#include "environment.h"

namespace {

ruvia::task<void> exchange(ruvia::event_loop loop, ruvia::http_client_config config) {
    ruvia::http_client client(loop, config);
    std::exception_ptr failure;
    try {
        auto peer = co_await client.client().websocket("/features/ws").connect();
        // The owning session and each received payload stay on this owner loop.
        co_await peer.send("hello", {}, {.timeout_ = std::chrono::seconds(5)});
        auto text = co_await peer.receive({.timeout_ = std::chrono::seconds(5)});
        if (!text || !text->text() || text->payload() != "hello") {
            throw std::runtime_error("unexpected text echo");
        }
        std::cout << "text=" << text->payload() << '\n';
        co_await peer.send(std::as_bytes(std::span(std::string_view("\0\1\2", 3))));
        auto binary = co_await peer.receive();
        if (!binary || !binary->binary() || binary->payload() != std::string_view("\0\1\2", 3)) {
            throw std::runtime_error("unexpected binary echo");
        }
        std::cout << "binary bytes=" << binary->payload().size() << '\n';
        // Control frames are handled by the client driver. Close must not
        // overlap a read; all data operations above have already completed.
        co_await peer.ping("alive");
        co_await peer.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "example complete"});
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

int main(int argc, char** argv) {
    try {
        const std::string_view protocol = argc > 1 ? argv[1] : "1";
        if (protocol != "1" && protocol != "2") {
            throw std::invalid_argument("usage: ruvia_example_websocket_client [1|2]");
        }
        const example::environment env;
        const bool tls = env.get<bool>("RUVIA_CLIENT_TLS").value_or(false);
        ruvia::http_client_config config{
            .base_url_ = tls ? "https://localhost:8444" : "http://localhost:8093",
            .read_timeout_ = std::chrono::seconds(5),
            .request_timeout_ = std::chrono::seconds(5),
            .protocol_ = protocol == "2" ? ruvia::http_client_protocol::http2_only
                                         : ruvia::http_client_protocol::http1_only,
            .tls_ = {.ca_file_ = env.path("RUVIA_TLS_CA")},
        };
        // The loop looks "localhost" up on this pool instead of blocking in
        // getaddrinfo; declared first, it outlives the loops.
        ruvia::blocking_pool host_lookups({.thread_count_ = 1});
        ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &host_lookups});
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
