#include "test_harness.h"

#include <chrono>
#include <concepts>
#include <exception>
#include <optional>

#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/App.h"

namespace {

using ruvia::detail::HttpServerOptions;
using ruvia::detail::validateHttpServerOptions;

template <typename Fn>
bool throwsInvalid(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(validate_server_options_accepts_defaults) {
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.keepaliveTimeout),
                  std::optional<std::chrono::milliseconds>>);
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.clientHeaderTimeout),
                  std::optional<std::chrono::milliseconds>>);
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.clientBodyTimeout),
                  std::optional<std::chrono::milliseconds>>);
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.sendTimeout),
                  std::optional<std::chrono::milliseconds>>);
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.maxConnections),
                  std::optional<std::size_t>>);
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.keepaliveRequests),
                  std::optional<std::size_t>>);
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.maxStreamBodyBytes),
                  std::optional<std::size_t>>);
    RUVIA_CHECK(!HttpServerOptions{}.maxStreamBodyBytes.has_value());
    RUVIA_CHECK(!throwsInvalid([] { validateHttpServerOptions(HttpServerOptions{}); }));
}

RUVIA_TEST(validate_server_options_rejects_configured_nonpositive_timeout) {
    // Every connection timeout feeds the same positive optional fold. Each one bounds
    // how long a slow client can hold a connection (a slowloris defense), so a
    // nonpositive value in ANY of them must be rejected -- checking only keepaliveTimeout
    // would miss a field dropped from the fold call.
    using std::chrono::milliseconds;
    {
        HttpServerOptions options;
        options.keepaliveTimeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.clientHeaderTimeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.clientBodyTimeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.sendTimeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.keepaliveTimeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.clientHeaderTimeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.clientBodyTimeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.sendTimeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
}

RUVIA_TEST(validate_server_options_rejects_nonpositive_limits) {
    {
        HttpServerOptions options;
        options.maxConnections = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.keepaliveRequests = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.maxStreamBodyBytes = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.maxBufferedBodyBytes = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.maxWebSocketMessageBytes = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.scanInterval = std::chrono::milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
}

RUVIA_TEST(validate_server_options_enforces_tls_material) {
    // TLS on but no certificate / key files is rejected.
    HttpServerOptions missing;
    missing.tls.enabled = true;
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(missing); }));

    // With both files present it is accepted.
    HttpServerOptions configured;
    configured.tls.enabled = true;
    configured.tls.certificateChainFile = "cert.pem";
    configured.tls.privateKeyFile = "key.pem";
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerOptions(configured); }));
}

RUVIA_TEST(validate_server_options_requires_auto_https_port) {
    HttpServerOptions options;
    options.autoHttps.enabled = true;
    options.autoHttps.httpsPort = 0;  // default is 443; zero is invalid
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
}
