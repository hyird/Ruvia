// Benchmark server mirroring hical's docker/bench_main.cpp endpoints so the
// two frameworks can be driven by the same wrk scenarios.
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

RUVIA_REQUEST_MODEL(UserDTO,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(age, ruvia::UInt32),
    RUVIA_FIELD(email, ruvia::String)
);

RUVIA_RESPONSE_MODEL(UserEcho,
    RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD(age, ruvia::UInt32),
    RUVIA_FIELD(email, ruvia::String)
);

RUVIA_RESPONSE_MODEL(StatusResponse,
    RUVIA_FIELD(status, ruvia::String),
    RUVIA_FIELD(framework, ruvia::String)
);

RUVIA_RESPONSE_MODEL(UserByIdResponse,
    RUVIA_FIELD(userId, ruvia::String),
    RUVIA_FIELD(name, ruvia::String)
);

RUVIA_RESPONSE_MODEL(MiddlewareResponse,
    RUVIA_FIELD(middleware_count, ruvia::UInt32)
);

template <int N>
class Passthrough final : public ruvia::Middleware<Passthrough<N>> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
    }
};

class BenchController final : public ruvia::Controller<BenchController> {
public:
    RUVIA_CONTROLLER_GROUP("")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", hello);
    RUVIA_GET("/api/status", status);
    RUVIA_POST("/api/echo", echo);
    RUVIA_GET("/users/:id", user);
    RUVIA_GET("/middleware/0", middleware0);
    RUVIA_GET("/middleware/3", middleware3,
              Passthrough<0>, Passthrough<1>, Passthrough<2>);
    RUVIA_GET("/middleware/10", middleware10,
              Passthrough<0>, Passthrough<1>, Passthrough<2>, Passthrough<3>,
              Passthrough<4>, Passthrough<5>, Passthrough<6>, Passthrough<7>,
              Passthrough<8>, Passthrough<9>);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c) {
        co_return c.text("Hello, World!");
    }

    ruvia::Task<ruvia::HttpResponse> status(ruvia::Context& c) {
        StatusResponse response(c);
        response.status("running").framework("ruvia");
        co_return c.json(response);
    }

    ruvia::Task<ruvia::HttpResponse> echo(ruvia::Context& c) {
        const auto user = co_await c.req().json<UserDTO>();
        UserEcho response(c);
        if (const auto& name = user.name()) {
            response.name(name->view());
        }
        if (const auto& age = user.age()) {
            response.age(*age);
        }
        if (const auto& email = user.email()) {
            response.email(email->view());
        }
        co_return c.json(response);
    }

    ruvia::Task<ruvia::HttpResponse> user(ruvia::Context& c) {
        const auto id = c.req().param("id").value_or("");
        UserByIdResponse response(c);
        std::pmr::string name(c.allocator<char>());
        name.append("User ");
        name.append(id);
        response.userId(id).name(name);
        co_return c.json(response);
    }

    ruvia::Task<ruvia::HttpResponse> middleware0(ruvia::Context& c) {
        co_return middlewareResponse(c, 0);
    }

    ruvia::Task<ruvia::HttpResponse> middleware3(ruvia::Context& c) {
        co_return middlewareResponse(c, 3);
    }

    ruvia::Task<ruvia::HttpResponse> middleware10(ruvia::Context& c) {
        co_return middlewareResponse(c, 10);
    }

    static ruvia::HttpResponse middlewareResponse(ruvia::Context& c, std::uint32_t count) {
        MiddlewareResponse response(c);
        response.middleware_count(ruvia::UInt32{count});
        return c.json(response);
    }
};

int main() {
    const char* portEnv = std::getenv("PORT");
    const auto port = static_cast<std::uint16_t>(portEnv ? std::atoi(portEnv) : 8080);

    ruvia::app()
        .setListenAddress("0.0.0.0")
        .setHttpListenPort(port)
        .setThreadNum(4)
        .setKeepaliveRequests(1u << 30)
        .setMaxConnectionsPerWorker(20000)
        .run();
}
