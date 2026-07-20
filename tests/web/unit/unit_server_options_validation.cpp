#include "test_harness.h"

#include <chrono>
#include <concepts>
#include <exception>
#include <optional>
#include <type_traits>

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
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.defaultRateLimitPerWorker),
                  std::optional<ruvia::RateLimitRule>>);
    static_assert(std::same_as<
                  decltype(HttpServerOptions{}.rateLimitSlotsPerWorker),
                  std::size_t>);
    RUVIA_CHECK(!HttpServerOptions{}.maxStreamBodyBytes.has_value());
    RUVIA_CHECK_EQ(
        HttpServerOptions{}.rateLimitSlotsPerWorker,
        ruvia::kDefaultRateLimitSlotsPerWorker);
    // An unconfigured server is bounded by default against connection floods.
    RUVIA_CHECK(HttpServerOptions{}.maxConnections.has_value());
    RUVIA_CHECK_EQ(*HttpServerOptions{}.maxConnections, std::size_t{1024});
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
        options.workerMailboxCapacity = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.rateLimitSlotsPerWorker = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.rateLimitSlotsPerWorker = 3;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.memoryConfig.requestInitialBufferBytes = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
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
    missing.transport = HttpServerOptions::Tls{};
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(missing); }));

    // With both files present it is accepted.
    HttpServerOptions configured;
    HttpServerOptions::Tls tls;
    tls.identity.certificateChainFile = "cert.pem";
    tls.identity.privateKeyFile = "key.pem";
    configured.transport = std::move(tls);
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerOptions(configured); }));
}

RUVIA_TEST(validate_server_options_enforces_nested_tls_material) {
    const auto validTls = [] {
        HttpServerOptions::Tls tls;
        tls.identity.certificateChainFile = "cert.pem";
        tls.identity.privateKeyFile = "key.pem";
        return tls;
    };

    {
        HttpServerOptions options;
        auto tls = validTls();
        tls.clientCertificates.emplace(
            std::pmr::string{},
            ruvia::TlsClientCertificateRequirement::kOptional);
        options.transport = std::move(tls);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        auto tls = validTls();
        auto& sni = tls.sniIdentities.emplace_back();
        sni.identity.certificateChainFile = "sni-cert.pem";
        sni.identity.privateKeyFile = "sni-key.pem";
        options.transport = std::move(tls);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        auto tls = validTls();
        auto& sni = tls.sniIdentities.emplace_back();
        sni.host = "example.com";
        options.transport = std::move(tls);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        auto tls = validTls();
        for (const auto* host : {"Example.com", "example.COM"}) {
            auto& sni = tls.sniIdentities.emplace_back();
            sni.host = host;
            sni.identity.certificateChainFile = "sni-cert.pem";
            sni.identity.privateKeyFile = "sni-key.pem";
        }
        options.transport = std::move(tls);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
}

RUVIA_TEST(validate_server_options_requires_redirect_https_port) {
    HttpServerOptions options;
    options.transport = HttpServerOptions::RedirectHttpToHttps{0};
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
}

RUVIA_TEST(server_topology_rejects_invalid_listener_and_tls_states_at_construction) {
    static_assert(!std::is_default_constructible_v<ruvia::TlsIdentity>);
    static_assert(!std::is_default_constructible_v<ruvia::TlsClientCertificatePolicy>);
    static_assert(!std::is_default_constructible_v<ruvia::TlsConfig>);
    static_assert(!std::is_aggregate_v<ruvia::ServerTopology>);

    RUVIA_CHECK(throwsInvalid([] {
        (void)ruvia::TlsIdentity::fromFiles({}, "key.pem");
    }));
    RUVIA_CHECK(throwsInvalid([] {
        (void)ruvia::TlsIdentity::fromFiles("cert.pem", {});
    }));
    RUVIA_CHECK(throwsInvalid([] {
        (void)ruvia::TlsClientCertificatePolicy::required({});
    }));
    RUVIA_CHECK(throwsInvalid([] {
        (void)ruvia::ServerTopology::http(0);
    }));
    RUVIA_CHECK(throwsInvalid([] {
        auto tls = ruvia::TlsConfig(
            ruvia::TlsIdentity::fromFiles("cert.pem", "key.pem"));
        (void)ruvia::ServerTopology::httpAndHttps(8443, 8443, std::move(tls));
    }));
}

RUVIA_TEST(tls_config_rejects_empty_or_duplicate_sni_identity) {
    auto tls = ruvia::TlsConfig(
        ruvia::TlsIdentity::fromFiles("cert.pem", "key.pem"));
    RUVIA_CHECK(throwsInvalid([&] {
        tls.addSniIdentity(
            {},
            ruvia::TlsIdentity::fromFiles("other.pem", "other.key"));
    }));
    tls.addSniIdentity(
        "Example.com",
        ruvia::TlsIdentity::fromFiles("other.pem", "other.key"));
    RUVIA_CHECK(throwsInvalid([&] {
        tls.addSniIdentity(
            "example.COM",
            ruvia::TlsIdentity::fromFiles("third.pem", "third.key"));
    }));
}
