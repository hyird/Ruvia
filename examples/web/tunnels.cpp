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

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/HttpUdpTunnel.h"
#include "ruvia/web/Middleware.h"

#include "environment.h"

namespace {

constexpr std::uint64_t echo_capsule_type = 0xff37;

class capsule_handshake final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        // Negotiate before next(): the tunnel handler starts after the response
        // head is committed, so changing its status or headers there is too late.
        if (c.req().header("capsule-protocol") != "?1") {
            c.respond(c.error({.status = ruvia::http_status::kBadRequest,
                .code = "capsules_required",
                .message = "send Capsule-Protocol: ?1"}));
            co_return;
        }
        c.header("Capsule-Protocol", "?1");
        co_await next();
    }
};

class tunnel_controller final : public ruvia::Controller<tunnel_controller> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    const ruvia::HttpTunnelRouteConfig options{
        .peerTransportFinTimeout = std::chrono::seconds(2), .datagrams = true};
    RUVIA_CONNECT("echo.example:443", bytes);
    RUVIA_CONNECT_PROTOCOL("example-capsules", "/capsules", capsules, capsule_handshake);
    RUVIA_CONNECT_PROTOCOL_OPTIONS("example-datagrams", "/datagrams", datagrams, options);
    RUVIA_CONNECT_PROTOCOL_OPTIONS("connect-udp", "/udp/:host/:port", udp, options);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_return c.text("ready\n");
    }

    ruvia::Task<void> bytes(ruvia::Context& c) {
        auto& tunnel = c.tunnel();
        while (auto chunk = co_await tunnel.read()) {
            // The server owns each received chunk; moving it into write can
            // transfer compatible PMR storage without another payload copy.
            co_await tunnel.write(std::move(*chunk));
        }
        co_await tunnel.finish();
    }

    ruvia::Task<void> capsules(ruvia::Context& c) {
        // capsule_handshake negotiated the custom protocol. CONNECT-UDP's
        // dedicated driver performs its own negotiation automatically.
        auto stream = c.tunnel().capsules({.maxCapsuleLength = 4096});
        while (auto capsule = co_await stream.read()) {
            // Unknown types must be ignored by the application protocol.
            if (capsule->type() == echo_capsule_type) {
                co_await stream.write(capsule->type(), capsule->payload());
            }
        }
        co_await stream.finish();
    }

    ruvia::Task<void> datagrams(ruvia::Context& c) {
        auto stream = c.tunnel().datagrams();
        while (auto packet = co_await stream.read()) {
            // Automatic mode selects native QUIC DATAGRAM when negotiated and
            // within the packet bound; otherwise it uses reliable capsules.
            // Native datagrams are best effort and can be dropped under load.
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
    }

    ruvia::Task<void> udp(ruvia::Context& c) {
        ruvia::HttpUdpTunnel stream(c.tunnel().datagrams());
        while (auto packet = co_await stream.read()) {
            // The adapter handles Context ID 0 and ignores unknown contexts.
            // A zero-byte payload is a valid UDP packet, not an EOF marker.
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
    }
};

std::string_view characters(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

ruvia::Task<void> client_session(ruvia::EventLoop loop, ruvia::HttpClientConfig config,
    std::string_view selection) {
    ruvia::HttpClient client(loop, config);
    std::exception_ptr failure;
    try {
        {
            auto response = co_await client.send({.target = "/health"});
            auto body = co_await response.body().readAll();
            std::cout << "service: " << characters(body.bytes());
        }
        if (selection == "all" || selection == "bytes") {
            auto opened = co_await client.openTunnel({.authority = "echo.example:443"});
            if (auto* rejection = opened.response()) {
                auto body = co_await rejection->body().readAll();
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
        if (config.protocol != ruvia::HttpClientProtocol::kHttp1Only) {
            if (selection == "all" || selection == "capsules") {
                const std::array<ruvia::HttpHeaderView, 1> headers{{{"capsule-protocol", "?1"}}};
                auto opened = co_await client.openTunnel({.authority = "localhost",
                    .protocol = "example-capsules",
                    .target = "/capsules",
                    .headers = headers});
                if (!opened.tunnel()) {
                    throw std::runtime_error("capsule handshake rejected");
                }
                // The adapter takes client tunnel ownership. Never mix raw
                // tunnel reads/writes with capsule operations after this move.
                auto stream = std::move(*opened.tunnel()).capsules({.maxCapsuleLength = 4096});
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
                auto opened = co_await client.openTunnel({.authority = "localhost",
                                                             .protocol = "example-datagrams",
                                                             .target = "/datagrams"},
                    {.datagrams = true});
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
            auto opened = co_await client.openUdpTunnel({.target = "/udp/echo.example/443"});
            if (!opened.tunnel()) {
                auto body = co_await opened.response()->body().readAll();
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
            const example::environment env;
            const bool tls = protocol == "3" || env.get<bool>("RUVIA_CLIENT_TLS").value_or(false);
            const ruvia::HttpClientConfig config{
                .scheme = tls ? ruvia::HttpScheme::kHttps : ruvia::HttpScheme::kHttp,
                .host = "localhost",
                .port = tls ? 8445 : 8094,
                .requestTimeout = std::chrono::seconds(5),
                .protocol = protocol == "3"   ? ruvia::HttpClientProtocol::kHttp3Only
                            : protocol == "2" ? ruvia::HttpClientProtocol::kHttp2Only
                                              : ruvia::HttpClientProtocol::kHttp1Only,
                .caFile = std::string(env.get("RUVIA_TLS_CA").value_or("")),
            };
            ruvia::EventLoopPool loops({.loopCount = 1});
            loops.start();
            auto done = loops.loop(0).start(client_session(loops.loop(0), config, selection));
            done.get();
            loops.stop();
            loops.join();
        } else {
            auto& app = ruvia::app();
            app.loadDotenv();
            const example::environment env(&app.env());
            ruvia::ListenConfig listener{.address = "127.0.0.1", .http = 8094};
            const auto cert = env.get("RUVIA_TLS_CERT");
            const auto key = env.get("RUVIA_TLS_KEY");
            if (cert.has_value() != key.has_value()) {
                throw std::invalid_argument("set both RUVIA_TLS_CERT and RUVIA_TLS_KEY");
            }
            if (cert && key) {
                listener.https = 8445;
                listener.tls.certificateChainFile = std::string(*cert);
                listener.tls.privateKeyFile = std::string(*key);
            }
            app.server({.worker_count = 2,
                           .process_signal_handlers = ruvia::process_signal_handler_policy::install})
                .listen(std::move(listener))
                .onConnectionFailure([](const ruvia::ConnectionFailureRecord& record) noexcept {
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
    } catch (const ruvia::HttpClientError& error) {
        std::cerr << "client error " << static_cast<int>(error.code()) << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
