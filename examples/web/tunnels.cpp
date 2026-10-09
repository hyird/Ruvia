// CONNECT, Extended CONNECT, Capsule Protocol, HTTP Datagrams, and CONNECT-UDP.
// Run ruvia_example_tunnels to start a local echo service on port 8094.
// In another terminal: ruvia_example_tunnels --client 1 (or 2, or 3).
// An optional last argument selects bytes, capsules, datagrams, or udp.
// HTTP/1 covers ordinary CONNECT and CONNECT-UDP Upgrade. HTTP/2/3 also cover
// custom capsule/datagram protocols. Successful clients print each echoed payload.
// Set RUVIA_TLS_CERT/KEY on the server for TLS/QUIC port 8445 and RUVIA_TLS_CA
// on the client. RUVIA_CLIENT_TLS=true enables TLS for 1/2; 3 always uses TLS.
// This service echoes locally; it does not forward traffic to an arbitrary host.

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_udp_tunnel.h"
#include "ruvia/web/middleware.h"

#include "environment.h"

namespace {

constexpr std::uint64_t echo_capsule_type = 0xff37;

class capsule_handshake final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        // Negotiate before next(): the tunnel handler starts after the response
        // head is committed, so changing its status or headers there is too late.
        if (c.req().header("capsule-protocol") != "?1") {
            c.respond(c.error({.status_ = ruvia::http_status::bad_request,
                .code_ = "capsules_required",
                .message_ = "send Capsule-Protocol: ?1"}));
            co_return;
        }
        c.header("Capsule-Protocol", "?1");
        co_await next_value();
    }
};

class tunnel_controller final : public ruvia::controller<tunnel_controller> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::seconds(2), .datagrams_ = true};
    RUVIA_CONNECT("echo.example:443", bytes);
    RUVIA_CONNECT_PROTOCOL("example-capsules", "/capsules", capsules, capsule_handshake);
    RUVIA_CONNECT_PROTOCOL_OPTIONS("example-datagrams", "/datagrams", datagrams, options);
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

    ruvia::task<void> capsules(ruvia::context& c) {
        // capsule_handshake negotiated the custom protocol. CONNECT-UDP's
        // dedicated driver performs its own negotiation automatically.
        auto stream = c.tunnel().capsules({.max_capsule_length_ = 4096});
        while (auto capsule = co_await stream.read()) {
            // Unknown types must be ignored by the application protocol.
            if (capsule->type() == echo_capsule_type) {
                co_await stream.write(capsule->type(), capsule->payload());
            }
        }
        co_await stream.finish();
    }

    ruvia::task<void> datagrams(ruvia::context& c) {
        auto stream = c.tunnel().datagrams();
        while (auto packet = co_await stream.read()) {
            // Automatic mode selects native QUIC DATAGRAM when negotiated and
            // within the packet bound; otherwise it uses reliable capsules.
            // Native datagrams are best effort and can be dropped under load.
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
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
        {
            auto response = co_await client.send({.target_ = "/health"});
            auto body = co_await response.body().read_all();
            std::cout << "service: " << characters(body.bytes());
        }
        if (selection == "all" || selection == "bytes") {
            auto opened = co_await client.open_tunnel({.authority_ = "echo.example:443"});
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
        if (config.protocol_ != ruvia::http_client_protocol::http1_only) {
            if (selection == "all" || selection == "capsules") {
                const std::array<ruvia::http_header_view, 1> headers{{{"capsule-protocol", "?1"}}};
                auto opened = co_await client.open_tunnel({.authority_ = "localhost",
                    .protocol_ = "example-capsules",
                    .target_ = "/capsules",
                    .headers_ = headers});
                if (!opened.tunnel()) {
                    throw std::runtime_error("capsule handshake rejected");
                }
                // The adapter takes client tunnel ownership. Never mix raw
                // tunnel reads/writes with capsule operations after this move.
                auto stream = std::move(*opened.tunnel()).capsules({.max_capsule_length_ = 4096});
                co_await stream.write(echo_capsule_type, "capsule");
                auto reply = co_await stream.read();
                if (!reply || reply->type() != echo_capsule_type || reply->payload() != "capsule") {
                    throw std::runtime_error("incorrect capsule echo");
                }
                std::cout << "capsule: " << reply->payload() << '\n';
                co_await stream.finish();
                while (co_await stream.read()) {
                }
            }
            if (selection == "all" || selection == "datagrams") {
                auto opened = co_await client.open_tunnel({.authority_ = "localhost",
                                                              .protocol_ = "example-datagrams",
                                                              .target_ = "/datagrams"},
                    {.datagrams_ = true});
                if (!opened.tunnel()) {
                    throw std::runtime_error("datagram handshake rejected");
                }
                auto stream = std::move(*opened.tunnel()).datagrams();
                co_await stream.send("datagram");
                auto reply = co_await stream.read();
                if (!reply || characters(reply->payload()) != "datagram") {
                    throw std::runtime_error("incorrect datagram echo");
                }
                std::cout << "datagram: " << characters(reply->payload()) << '\n';
                co_await stream.finish();
                while (co_await stream.read()) {
                }
            }
        }
        if (selection == "all" || selection == "udp") {
            auto opened = co_await client.open_udp_tunnel({.target_ = "/udp/echo.example/443"});
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
            if (std::string_view(argv[1]) != "--client" || argc > 4) {
                throw std::invalid_argument("usage: ruvia_example_tunnels [--client [1|2|3] [all|bytes|capsules|datagrams|udp]]");
            }
            const std::string_view protocol = argc > 2 ? argv[2] : "2";
            if (protocol != "1" && protocol != "2" && protocol != "3") {
                throw std::invalid_argument("protocol must be 1, 2, or 3");
            }
            const std::string_view selection = argc > 3 ? argv[3] : "all";
            if (selection != "all" && selection != "bytes" && selection != "capsules" &&
                selection != "datagrams" && selection != "udp") {
                throw std::invalid_argument("unknown tunnel selection");
            }
            if (protocol == "1" && (selection == "capsules" || selection == "datagrams")) {
                throw std::invalid_argument("custom capsules/datagrams require HTTP/2 or HTTP/3");
            }
            const example::environment env_value;
            const bool tls = protocol == "3" || env_value.get<bool>("RUVIA_CLIENT_TLS").value_or(false);
            const ruvia::http_client_config config{
                .scheme_ = tls ? ruvia::http_scheme::https : ruvia::http_scheme::http,
                .host_ = "localhost",
                .port_ = tls ? 8445 : 8094,
                .request_timeout_ = std::chrono::seconds(5),
                .protocol_ = protocol == "3"   ? ruvia::http_client_protocol::http3_only
                             : protocol == "2" ? ruvia::http_client_protocol::http2_only
                                               : ruvia::http_client_protocol::http1_only,
                .ca_file_ = std::string(env_value.get("RUVIA_TLS_CA").value_or("")),
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
