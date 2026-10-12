// HTTP/1 CONNECT byte streams and CONNECT-UDP capsules.
// Run ruvia_example_tunnels for the server on port 8094. In another terminal,
// run ruvia_example_tunnels --client [all|bytes|udp].
// RUVIA_TLS_CERT/KEY enables server TLS on port 8445; RUVIA_CLIENT_TLS=true
// selects it for the client and RUVIA_TLS_CA supplies its trust root.
// The server echoes locally and does not forward to an arbitrary host.

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/http_udp_tunnel.h"

#include "environment.h"

namespace {

class tunnel_controller final {
public:
    RUVIA_ROUTES_BEGIN(tunnel_controller)
    RUVIA_GET("/health", health);
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::seconds(2), .datagrams_ = true};
    RUVIA_CONNECT("echo.example:443", bytes);
    RUVIA_CONNECT_PROTOCOL_OPTIONS("connect-udp", "/udp/:host/:port", udp, options);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return c.text("ready\n");
    }

    ruvia::task<void> bytes(ruvia::context& c) {
        auto& tunnel = c.tunnel();
        while (auto chunk = co_await tunnel.read()) {
            // The server owns each received chunk; moving it into write can
            // transfer compatible PMR storage without another payload copy.
            co_await tunnel.write(std::move(*chunk));
        }
        co_await tunnel.finish();
    }

    ruvia::task<void> udp(ruvia::context& c) {
        ruvia::http_udp_tunnel stream(c.tunnel().datagrams());
        while (auto packet = co_await stream.read()) {
            // The adapter handles context ID 0 and ignores unknown contexts.
            // A zero-byte payload is a valid UDP packet, not an EOF marker.
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
    }
};

std::string_view characters(std::span<const std::byte> bytes_value) {
    return {reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()};
}

ruvia::task<void> client_session(ruvia::event_loop loop, ruvia::http_client_config config,
    std::string_view selection) {
    ruvia::http_client client(loop, config);
    std::exception_ptr failure;
    try {
        const auto origin = client.handle();
        {
            auto response = co_await origin.send({.target_ = "/health"});
            auto body = co_await response.body().read_all();
            std::cout << "service: " << characters(body.bytes());
        }
        if (selection == "all" || selection == "bytes") {
            auto opened = co_await origin.open_tunnel({.authority_ = "echo.example:443"});
            if (auto* rejection = opened.response()) {
                auto body = co_await rejection->body().read_all();
                throw std::runtime_error("CONNECT rejected: " + std::string(characters(body.bytes())));
            }
            auto& tunnel = *opened.tunnel();
            co_await tunnel.write("byte stream");
            // finish() half-closes only sending. Keep reading to the peer FIN.
            co_await tunnel.finish();
            std::string echoed;
            while (const auto chunk = co_await tunnel.read()) {
                echoed.append(characters(*chunk));
            }
            if (echoed != "byte stream") {
                throw std::runtime_error("incorrect CONNECT echo");
            }
            std::cout << "CONNECT: " << echoed << '\n';
        }
        if (selection == "all" || selection == "udp") {
            auto opened = co_await origin.open_udp_tunnel({.target_ = "/udp/echo.example/443"});
            if (!opened.tunnel()) {
                auto body = co_await opened.response()->body().read_all();
                throw std::runtime_error("CONNECT-UDP rejected: " + std::string(characters(body.bytes())));
            }
            auto stream = std::move(*opened.tunnel()).udp();
            co_await stream.send("udp payload");
            auto reply = co_await stream.read();
            if (!reply || characters(reply->payload()) != "udp payload") {
                throw std::runtime_error("incorrect UDP echo");
            }
            std::cout << "UDP: " << characters(reply->payload()) << '\n';
            co_await stream.finish();
            while (co_await stream.read()) {
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    // All adapters and their owned packets have left scope. Await cancellation
    // and teardown before the client or its event loop can be destroyed.
    co_await client.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        if (argc > 1) {
            if (std::string_view(argv[1]) != "--client" || argc > 3) {
                throw std::invalid_argument("usage: ruvia_example_tunnels [--client [all|bytes|udp]]");
            }
            const std::string_view selection = argc > 2 ? argv[2] : "all";
            if (selection != "all" && selection != "bytes" && selection != "udp") {
                throw std::invalid_argument("unknown tunnel selection");
            }
            const example::environment env_value;
            const bool tls = env_value.get<bool>("RUVIA_CLIENT_TLS").value_or(false);
            const ruvia::http_client_config config{
                .scheme_ = tls ? ruvia::http_scheme::https : ruvia::http_scheme::http,
                .host_ = "localhost",
                .port_ = tls ? 8445 : 8094,
                .request_timeout_ = std::chrono::seconds(5),
                .tls_ = {.ca_file_ = std::string(env_value.get("RUVIA_TLS_CA").value_or(""))},
            };
            ruvia::event_loop_pool loops({.loop_count_ = 1});
            loops.start();
            auto done = loops.loop(0).start(client_session(loops.loop(0), config, selection));
            done.get();
            loops.stop();
            loops.join();
        } else {
            auto& app = ruvia::app();
            app.load_dotenv();
            const example::environment env_value(&app.env());
            ruvia::listen_config listener_value{.address_ = "127.0.0.1", .http_ = 8094};
            const auto cert = env_value.get("RUVIA_TLS_CERT");
            const auto key = env_value.get("RUVIA_TLS_KEY");
            if (cert.has_value() != key.has_value()) {
                throw std::invalid_argument("set both RUVIA_TLS_CERT and RUVIA_TLS_KEY");
            }
            if (cert && key) {
                listener_value.https_ = 8445;
                listener_value.tls_.certificate_chain_file_ = std::string(*cert);
                listener_value.tls_.private_key_file_ = std::string(*key);
            }
            app.server({.worker_count_ = 2,
                           .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
                .listen(std::move(listener_value))
                .on_connection_failure([](const ruvia::connection_failure_record& record) noexcept {
                    try {
                        std::rethrow_exception(record.exception());
                    } catch (const std::exception& error) {
                        std::cerr << "tunnel connection failed: " << error.what() << '\n';
                    } catch (...) {
                        std::cerr << "tunnel connection failed\n";
                    }
                })
                .run();
        }
    } catch (const ruvia::http_client_error& error) {
        std::cerr << "client error " << static_cast<int>(error.code()) << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
