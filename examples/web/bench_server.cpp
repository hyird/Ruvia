// Benchmark server mirroring hical's docker/bench_main.cpp endpoints so the
// two frameworks can be driven by the same wrk scenarios.
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

RUVIA_MODEL(user, RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(age, ruvia::uint32), RUVIA_OPTIONAL_FIELD(email, ruvia::string));

RUVIA_MODEL(status_response, RUVIA_OPTIONAL_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD(framework, ruvia::string));

RUVIA_MODEL(user_by_id_response, RUVIA_OPTIONAL_FIELD_NAME("userId", user_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string));

RUVIA_MODEL(middleware_response, RUVIA_OPTIONAL_FIELD(middleware_count, ruvia::uint32));

template <int n>
class passthrough final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }
};

class bench_controller final : public ruvia::controller<bench_controller> {
public:
    RUVIA_CONTROLLER_GROUP("")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", hello);
    RUVIA_GET("/api/status", status);
    RUVIA_POST("/api/echo", echo, ruvia::json_body<::user>);
    RUVIA_GET("/users/:id", user);
    RUVIA_GET("/middleware/0", middleware0);
    RUVIA_GET("/middleware/3", middleware3, passthrough<0>, passthrough<1>, passthrough<2>);
    RUVIA_GET("/middleware/10", middleware10, passthrough<0>, passthrough<1>, passthrough<2>,
        passthrough<3>, passthrough<4>, passthrough<5>, passthrough<6>, passthrough<7>,
        passthrough<8>, passthrough<9>);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> hello(ruvia::context& c) {
        co_return c.text("Hello, World!");
    }

    ruvia::task<ruvia::http_response> status(ruvia::context& c) {
        status_response response({.resource_ = c.arena()});
        response.set<"status">("running").set<"framework">("ruvia");
        co_return c.json(response);
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto& user_value = c.req().validated<::user>();
        co_return c.json(user_value);
    }

    ruvia::task<ruvia::http_response> user(ruvia::context& c) {
        const auto id = c.req().param("id").value_or("");
        user_by_id_response response({.resource_ = c.arena()});
        std::pmr::string name(c.allocator<char>());
        name.append("User ");
        name.append(id);
        response.set<"user_id">(id).set<"name">(name);
        co_return c.json(response);
    }

    ruvia::task<ruvia::http_response> middleware0(ruvia::context& c) {
        co_return middleware_response(c, 0);
    }

    ruvia::task<ruvia::http_response> middleware3(ruvia::context& c) {
        co_return middleware_response(c, 3);
    }

    ruvia::task<ruvia::http_response> middleware10(ruvia::context& c) {
        co_return middleware_response(c, 10);
    }

    static ruvia::http_response middleware_response(ruvia::context& c, std::uint32_t count) {
        ::middleware_response response({.resource_ = c.arena()});
        response.set<"middleware_count">(ruvia::uint32{count});
        return c.json(response);
    }
};

int main() {
    const char* port_env = std::getenv("PORT");
    const auto port = static_cast<std::uint16_t>(port_env ? std::atoi(port_env) : 8080);

    const char* tls_cert = std::getenv("TLS_CERT");
    const char* tls_key = std::getenv("TLS_KEY");
    const auto listener_value = [&] {
        if (tls_cert == nullptr || tls_key == nullptr) {
            return ruvia::listen_config{
                .address_ = "0.0.0.0",
                .http_ = port,
            };
        }
        return ruvia::listen_config{
            .address_ = "0.0.0.0",
            .https_ = port,
            .tls_ =
                {
                    .certificate_chain_file_ = tls_cert,
                    .private_key_file_ = tls_key,
                },
        };
    }();

    auto& app = ruvia::app();
    app.listen(listener_value).server({
        .worker_count_ = 4,
        .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
        .max_connections_per_worker_ = 20000,
        .max_requests_per_connection_ = 1u << 30,
    });

    // Response compression is off by default. Enable it explicitly when the
    // benchmark is intended to include negotiation and encoding work.
    if (const char* compression = std::getenv("COMPRESSION");
        compression != nullptr && compression[0] == '1') {
        app.compression({});
    }

    app.run();
}
