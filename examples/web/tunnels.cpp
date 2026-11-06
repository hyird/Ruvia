// HTTP/1 and HTTP/2 CONNECT byte streams and CONNECT-UDP capsules.
// Run ruvia_example_tunnels for the server on port 8094; RUVIA_TLS_CERT/KEY
// enables TLS on port 8445. Any CONNECT or CONNECT-UDP (RFC 9298) client can
// drive it. The server echoes locally and does not forward to an arbitrary host.

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_tunnel.h"

#include "environment.h"

namespace {

class tunnel_controller final {
public:
    RUVIA_ROUTES_BEGIN(tunnel_controller)
    RUVIA_GET("/health", health);
    // datagrams_ declares the Capsule Protocol (Capsule-Protocol: ?1 on the
    // 2xx, and the tunnel's udp()/datagrams()/capsules() facades); connect-udp
    // implies it, so here it only documents the route's protocol.
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::seconds(2), .datagrams_ = true};
    RUVIA_CONNECT("echo.example:443", bytes);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/udp/:host/:port", udp, options);
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
        // This protocol route has already negotiated CONNECT-UDP. Capsule
        // limits can be passed directly to udp(http_capsule_config).
        auto stream = c.tunnel().udp();
        while (auto packet = co_await stream.read()) {
            // The adapter handles context ID 0 and ignores unknown contexts.
            // A zero-byte payload is a valid UDP packet, not an EOF marker.
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
    }
};

}  // namespace

int main() {
    try {
        std::cout << std::unitbuf;
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
            listener_value.tls_.identity_ = {
                .certificate_chain_file_ = std::filesystem::path(*cert),
                .private_key_file_ = std::filesystem::path(*key),
            };
        }
        app.server({.worker_count_ = 2,
                       .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .listen(std::move(listener_value))
            .on_connection_failure([](const ruvia::connection_failure_record& record) noexcept {
                try {
                    std::cerr << "tunnel connection failed: " << ruvia::exception_text(record.exception()) << '\n';
                } catch (...) {
                    std::cerr << "tunnel connection failed\n";
                }
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
