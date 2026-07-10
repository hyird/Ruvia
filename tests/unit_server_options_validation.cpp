#include "test_harness.h"

#include <chrono>
#include <exception>

#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/App.h"

namespace {

using ruvia::HttpServerOptions;
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
    RUVIA_CHECK(!throwsInvalid([] { validateHttpServerOptions(HttpServerOptions{}); }));
}

RUVIA_TEST(validate_server_options_rejects_negative_timeout) {
    // Every connection timeout feeds the same non-negative fold. Each one bounds
    // how long a slow client can hold a connection (a slowloris defense), so a
    // negative value in ANY of them must be rejected -- checking only keepaliveTimeout
    // would miss a field dropped from the fold call.
    using std::chrono::milliseconds;
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
