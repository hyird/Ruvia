// Runtime configuration: dotenv, app-wide middleware via application::use, memory
// pool, timeouts, limits, compression and optional TLS. Typed Env::get<T>()
// returns nullopt only when a variable is absent; malformed values fail fast.
// Run ruvia_example_runtime_config and GET http://127.0.0.1:8087/runtime.
// RUVIA_HTTP_PORT/PORT and RUVIA_WORKERS override the listener and worker count.
// RUVIA_GZIP=true enables negotiated gzip/Brotli/zstd; RUVIA_CORS=true enables
// the CORS policy below. A .env file is optional and preserves existing values.
// For HTTPS, set RUVIA_TLS_CERT/KEY and optionally RUVIA_HTTPS_PORT (8443).
// RUVIA_TLS_VERIFY_FILE supplies a client CA; RUVIA_REQUIRE_CLIENT_CERT=true
// requires mTLS. SNI uses RUVIA_SNI_HOST/CERT/KEY. RUVIA_AUTO_HTTPS=true redirects
// plain HTTP. Never replace certificate verification with an insecure policy.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

#include "environment.h"

namespace {

void assign_if_present(std::string& target, std::optional<std::string_view> value) {
    if (value) {
        target.assign(value->data(), value->size());
    }
}

std::filesystem::path path_or_empty(std::optional<std::string_view> value) {
    if (!value) {
        return {};
    }
    return std::filesystem::path(std::string_view(*value));
}

}  // namespace

// Registered app-wide below (application::use): one instance per worker runs before the
// controller and route middlewares of EVERY matched route, in use() order.
// Requests that match no route (404/405) never enter a middleware chain.
class global_header_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Runtime-Example", "true");
    }
};

class scoped_header_middleware final : public ruvia::middleware {
public:
    explicit scoped_header_middleware(std::string value)
        : value_(std::move(value)) {}

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Scoped-Example", value_);
    }

private:
    std::string value_;
};

class runtime_controller final : public ruvia::controller<runtime_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/runtime")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", runtime);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> runtime(ruvia::context& c) {
        co_return c.text("runtime configured\n");
    }
};

int main() {
    auto& app = ruvia::app();
    app.load_dotenv();
    const example::environment env_value(&app.env());
    app.use<global_header_middleware>();
    // Prefix selection is compiled into the route plan. Constructor arguments
    // are owned by the registration and materialized separately on each worker.
    app.use_at<scoped_header_middleware>({.prefix_ = "/runtime"}, "configured");

    const auto http_port = app.env()
                               .get<std::uint16_t>("RUVIA_HTTP_PORT")
                               .value_or(env_value.get<std::uint16_t>("RUVIA_PORT").value_or(8087));
    app.server({
        .worker_count_ = env_value.get<std::uint32_t>("RUVIA_WORKERS").value_or(2),
        .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
        .idle_timeout_ = std::chrono::seconds(75),
        .connection_scan_interval_ = std::chrono::seconds(1),
        .request_header_timeout_ = std::chrono::seconds(60),
        .request_body_timeout_ = std::chrono::seconds(60),
        .write_timeout_ = std::chrono::seconds(60),
        .max_connections_per_worker_ = 10000,
        .max_requests_per_connection_ = 1000,
        .max_buffered_body_bytes_ = 16 * 1024 * 1024,
        .max_websocket_message_bytes_ = 16 * 1024 * 1024,
        .memory_pool_ =
            {
                .request_initial_buffer_bytes_ = 4096,
            },
    });
    if (env_value.get<bool>("RUVIA_GZIP").value_or(false)) {
        app.compression({});
    }
    if (env_value.get<bool>("RUVIA_CORS").value_or(false)) {
        app.cors({
            .request_headers_ =
                {
                    .mode_ = ruvia::cors_request_headers_mode::fixed,
                    .names_ = {"content-type", "authorization"},
                },
            .max_age_ = std::chrono::seconds(600),
        });
    }

    const auto cert = path_or_empty(env_value.get("RUVIA_TLS_CERT"));
    const auto key = path_or_empty(env_value.get("RUVIA_TLS_KEY"));
    if (!cert.empty() && !key.empty()) {
        std::string password;
        assign_if_present(password, env_value.get("RUVIA_TLS_PASSWORD"));
        ruvia::listen_config listener_value{
            .address_ = "0.0.0.0",
            .http_ = http_port,
            .https_ = env_value.get<std::uint16_t>("RUVIA_HTTPS_PORT").value_or(8443),
            .tls_ =
                {
                    .certificate_chain_file_ = cert,
                    .private_key_file_ = key,
                    .private_key_password_ = password,
                },
            .auto_https_redirect_ = env_value.get<bool>("RUVIA_AUTO_HTTPS").value_or(false),
        };
        const auto verify_file = path_or_empty(env_value.get("RUVIA_TLS_VERIFY_FILE"));
        if (!verify_file.empty()) {
            listener_value.tls_.client_certificates_.verify_file_ = verify_file;
            listener_value.tls_.client_certificates_.requirement_ =
                env_value.get<bool>("RUVIA_REQUIRE_CLIENT_CERT").value_or(false)
                    ? ruvia::tls_client_certificate_requirement::required
                    : ruvia::tls_client_certificate_requirement::optional;
        }
        if (const auto host = env_value.get("RUVIA_SNI_HOST")) {
            // Each SNI entry has its own certificate/key. The default listener
            // certificate remains the fallback for other server names.
            listener_value.tls_.sni_.push_back({
                .host_ = std::string(*host),
                .certificate_chain_file_ = path_or_empty(env_value.get("RUVIA_SNI_CERT")),
                .private_key_file_ = path_or_empty(env_value.get("RUVIA_SNI_KEY")),
            });
        }
        app.listen(std::move(listener_value));
    } else {
        app.listen({
            .address_ = "0.0.0.0",
            .http_ = http_port,
        });
    }

    app.run();
}
