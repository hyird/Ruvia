#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/ConfigValidation.h"
#include "ruvia/web/App.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/app/AppListenerOptions.h"
#include "ruvia/web/detail/app/AppState.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/WebSocketClientConfigStorage.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/detail/tls/TlsHost.h"

#include "failing_memory_resource.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::test::CountingMemoryResource;

using ruvia::ensureConfigHost;
using ruvia::ensureNonZeroPort;
using ruvia::ensurePositiveDuration;
using ruvia::ensurePositiveSize;
using ruvia::isValidConfigHost;
using ruvia::kSeparatedPortHostRules;
using ruvia::detail::ClientPortTextBuffer;
using ruvia::detail::clientTransportConfigView;
using ruvia::detail::formatClientPort;
using ruvia::detail::HttpServerListenerDefinition;
using ruvia::detail::HttpServerOptions;
using ruvia::detail::isValidSniHost;
using ruvia::detail::normalizeAltSvcAdvertisement;
using ruvia::detail::validateClientOriginHost;
using ruvia::detail::validateClientTransportConfig;
using ruvia::detail::validateHttpServerConfiguration;
using ruvia::detail::validateHttpServerListener;

HttpServerListenerDefinition http3TlsListener(
    std::uint16_t port, std::chrono::milliseconds handshakeTimeout = std::chrono::seconds(10),
    std::chrono::milliseconds drainTimeout = std::chrono::seconds(30)) {
    HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = "cert.pem";
    tls.identity.privateKeyFile = "key.pem";
    return HttpServerListenerDefinition(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), std::move(tls),
        ruvia::Http3ListenConfig{.mode = ruvia::Http3Mode::kEnabled,
            .handshakeTimeout = handshakeTimeout,
            .drainTimeout = drainTimeout});
}

// Returns the invalid_argument message a call throws, or empty if it does not.
template <typename Fn>
std::string caughtMessage(Fn&& fn) {
    try {
        fn();
        return {};
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
}

template <typename Fn>
bool throwsInvalid(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

template <typename Storage, typename Config, typename Verify>
void verify_config_rebinding(ruvia::testing::TestContext& ruvia_ctx, const Config& config, Verify&& verify) {
    CountingMemoryResource source_resource;
    std::optional<Storage> source(std::in_place, config, &source_resource);
    const auto source_allocations = source_resource.allocationCount();
    bool succeeded = false;
    std::size_t failures = 0;
    for (std::size_t allowance = 0; allowance != 64 && !succeeded; ++allowance) {
        failing_memory_resource target_resource;
        std::optional<Storage> rebound;
        target_resource.fail_after(allowance);
        try {
            rebound.emplace(*source, &target_resource);
            succeeded = true;
        } catch (const std::bad_alloc&) {
            ++failures;
        }
        target_resource.allow_allocations();
        RUVIA_CHECK_EQ(source_resource.allocationCount(), source_allocations);
        if (succeeded) {
            source.reset();
            RUVIA_CHECK_EQ(source_resource.liveAllocations(), std::size_t{0});
            verify(*rebound, &target_resource);
        } else {
            verify(*source, &source_resource);
        }
        rebound.reset();
        RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(succeeded);
    RUVIA_CHECK(failures >= 2);
}

}  // namespace

RUVIA_TEST(app_state_has_no_implicit_listener) {
    const ruvia::detail::AppState state;
    RUVIA_CHECK(state.listeners.empty());
}

RUVIA_TEST(server_configuration_rejects_empty_listeners) {
    RUVIA_CHECK_EQ(caughtMessage([] {
        (void)validateHttpServerConfiguration(
            std::span<const HttpServerListenerDefinition>{}, HttpServerOptions{});
    }),
        std::string("HTTP server worker requires at least one listener"));
}

RUVIA_TEST(http3_listen_config_defaults_to_automatic_and_accepts_designated_values) {
    const ruvia::ListenConfig defaults{};
    RUVIA_CHECK_EQ(defaults.http3.mode, ruvia::Http3Mode::kAutomatic);
    RUVIA_CHECK_EQ(defaults.http3.handshakeTimeout, std::chrono::seconds{10});
    RUVIA_CHECK_EQ(defaults.http3.drainTimeout, std::chrono::seconds{30});

    const ruvia::ListenConfig configured{.https = 8443,
        .http3 = ruvia::Http3ListenConfig{
            .mode = ruvia::Http3Mode::kEnabled,
            .handshakeTimeout = std::chrono::milliseconds{25},
            .drainTimeout = std::chrono::milliseconds{50},
        }};
    RUVIA_CHECK_EQ(configured.http3.mode, ruvia::Http3Mode::kEnabled);
    RUVIA_CHECK_EQ(configured.http3.handshakeTimeout, std::chrono::milliseconds{25});
    RUVIA_CHECK_EQ(configured.http3.drainTimeout, std::chrono::milliseconds{50});
}

RUVIA_TEST(alt_svc_advertisement_normalizes_active_disabled_clear_and_override_modes) {
    std::pmr::monotonic_buffer_resource resource;

    const auto automatic = normalizeAltSvcAdvertisement(
        ruvia::AltSvcConfig{}, std::uint16_t{443}, &resource);
    RUVIA_CHECK_EQ(automatic, "h3=\":443\"; ma=86400");

    const auto customParameters = normalizeAltSvcAdvertisement(
        {.maxAge = std::chrono::seconds{300},
            .persist = true,
            .advertisedPort = std::uint16_t{9443}},
        std::uint16_t{8443}, &resource);
    RUVIA_CHECK_EQ(customParameters, "h3=\":9443\"; ma=300; persist=1");

    RUVIA_CHECK(normalizeAltSvcAdvertisement(
        ruvia::AltSvcConfig{}, std::nullopt, &resource)
            .empty());
    RUVIA_CHECK(normalizeAltSvcAdvertisement(
        {.mode = ruvia::AltSvcMode::kDisabled}, std::uint16_t{443}, &resource)
            .empty());
    RUVIA_CHECK_EQ(normalizeAltSvcAdvertisement(
                       {.mode = ruvia::AltSvcMode::kClear}, std::nullopt, &resource),
        "clear");
    RUVIA_CHECK(throwsInvalid([&] {
        (void)normalizeAltSvcAdvertisement(
            {.maxAge = std::chrono::seconds{-1}}, std::uint16_t{443}, &resource);
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        (void)normalizeAltSvcAdvertisement(
            {.advertisedPort = std::uint16_t{0}}, std::uint16_t{443}, &resource);
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        (void)normalizeAltSvcAdvertisement(
            {.mode = static_cast<ruvia::AltSvcMode>(0xFF)}, std::uint16_t{443}, &resource);
    }));
}

RUVIA_TEST(app_http3_mode_normalizes_against_https_and_validates_effective_config) {
    auto& app = ruvia::app();
    app.server({});
    app.listen({.address = "127.0.0.1", .http = 8080});
    app.server({.maxConnectionsPerWorker = std::nullopt});

    // Automatic mode remains off for HTTP-only listeners, even without an H3
    // connection cap.
    RUVIA_CHECK(!throwsInvalid([&] {
        app.listen({.address = "127.0.0.1", .http = 8081});
    }));

    const auto httpsConfig = [](ruvia::Http3ListenConfig http3 = {}) {
        return ruvia::ListenConfig{
            .address = "127.0.0.1",
            .https = 8443,
            .tls = {.certificateChainFile = "cert.pem", .privateKeyFile = "key.pem"},
            .http3 = http3,
        };
    };

    // Automatic mode enables H3 on HTTPS, so the existing finite-capacity
    // requirement still applies.
    RUVIA_CHECK(throwsInvalid([&] { app.listen(httpsConfig()); }));
    RUVIA_CHECK(throwsInvalid([&] {
        app.listen({.address = "127.0.0.1",
            .http = 8082,
            .http3 = {.mode = ruvia::Http3Mode::kEnabled}});
    }));

    // Disabled mode suppresses H3 even on HTTPS and does not validate unused
    // H3 timeout values.
    RUVIA_CHECK(!throwsInvalid([&] {
        app.listen(httpsConfig({.mode = ruvia::Http3Mode::kDisabled,
            .handshakeTimeout = std::chrono::milliseconds::zero(),
            .drainTimeout = std::chrono::milliseconds::zero()}));
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        app.listen(httpsConfig({.mode = ruvia::Http3Mode::kEnabled}));
    }));

    app.server({});
    RUVIA_CHECK(!throwsInvalid([&] {
        app.listen(httpsConfig({.handshakeTimeout = std::chrono::milliseconds{25},
            .drainTimeout = std::chrono::milliseconds{50}}));
    }));
    RUVIA_CHECK(!throwsInvalid([&] {
        app.listen(httpsConfig({.mode = ruvia::Http3Mode::kEnabled,
            .handshakeTimeout = std::chrono::milliseconds{25},
            .drainTimeout = std::chrono::milliseconds{50}}));
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        app.listen(httpsConfig({.handshakeTimeout = std::chrono::milliseconds::zero()}));
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        app.listen(httpsConfig({.drainTimeout = std::chrono::milliseconds::zero()}));
    }));

    RUVIA_CHECK(throwsInvalid([&] {
        app.listen({.address = "127.0.0.1",
            .http = 8083,
            .http3 = {.mode = static_cast<ruvia::Http3Mode>(0xFF)}});
    }));

    app.server({});
    app.listen({.address = "127.0.0.1", .http = 8080});
}

RUVIA_TEST(http3_listener_uses_one_tls_endpoint_and_rejects_duplicates) {
    auto listener = http3TlsListener(8443);
    RUVIA_CHECK(std::holds_alternative<HttpServerListenerDefinition::Tls>(listener.transport));
    RUVIA_CHECK(listener.http3.has_value());
    RUVIA_CHECK_EQ(listener.endpoint.port(), std::uint16_t{8443});
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerListener(listener); }));

    const std::array single{std::move(listener)};
    const auto validated = validateHttpServerConfiguration(single, HttpServerOptions{});
    RUVIA_CHECK_EQ(validated.listeners().size(), std::size_t{1});
    RUVIA_CHECK_EQ(validated.listeners().front().endpoint.port(), std::uint16_t{8443});

    const std::array duplicate{http3TlsListener(8443), http3TlsListener(9443)};
    RUVIA_CHECK(throwsInvalid([&] {
        (void)validateHttpServerConfiguration(duplicate, HttpServerOptions{});
    }));

    HttpServerListenerDefinition plain(asio::ip::tcp::endpoint(
                                           asio::ip::make_address("127.0.0.1"), 8443),
        HttpServerListenerDefinition::PlainHttp{}, ruvia::Http3ListenConfig{});
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(plain); }));
    RUVIA_CHECK(throwsInvalid([&] {
        validateHttpServerListener(http3TlsListener(8443, std::chrono::milliseconds::zero()));
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        validateHttpServerListener(http3TlsListener(
            8443, std::chrono::seconds(10), std::chrono::milliseconds::zero()));
    }));
}

RUVIA_TEST(http3_server_limits_bound_every_downstream_capacity) {
    const std::array listeners{http3TlsListener(8443)};
    const auto rejects = [&listeners](HttpServerOptions options) {
        return throwsInvalid([&] {
            (void)validateHttpServerConfiguration(listeners, std::move(options));
        });
    };

    auto options = HttpServerOptions{};
    options.maxConnections.reset();
    RUVIA_CHECK(rejects(std::move(options)));

    options = HttpServerOptions{};
    options.maxConnections = std::numeric_limits<std::size_t>::max();
    RUVIA_CHECK(rejects(std::move(options)));

    options = HttpServerOptions{};
    options.workerMailboxCapacity = std::numeric_limits<std::uint32_t>::max();
    RUVIA_CHECK(rejects(std::move(options)));

    options = HttpServerOptions{};
    options.maxRequestsPerConnection.reset();
    RUVIA_CHECK(rejects(std::move(options)));

    options = HttpServerOptions{};
    constexpr auto maxPowerOfTwo =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    options.maxRequestsPerConnection = maxPowerOfTwo / 2 + 1;
    RUVIA_CHECK(rejects(std::move(options)));

    options = HttpServerOptions{};
    options.maxRequestsPerConnection = 1000;
    RUVIA_CHECK(!rejects(std::move(options)));
    RUVIA_CHECK_EQ(ruvia::detail::http3WorkerTrackedStreamCapacity(1000), std::size_t{1192});
    RUVIA_CHECK_EQ(ruvia::detail::http3TransportLifetimeStreamCapacity(1000), std::size_t{1128});

    options = HttpServerOptions{};
    options.maxRequestsPerConnection = std::numeric_limits<std::size_t>::max();
    RUVIA_CHECK(rejects(std::move(options)));

    const auto maxConnectionCapacity =
        static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    RUVIA_CHECK(throwsInvalid([maxConnectionCapacity] {
        ruvia::detail::validateHttp3ServerLimits(
            2, 1024, 1000, maxConnectionCapacity / 2 + 1);
    }));
}

RUVIA_TEST(client_ip_classification_uses_complete_literal_views) {
    for (const std::string_view host : {"127.0.0.1", "0.0.0.0", "255.255.255.255",
             "::", "::1", "2001:db8::1234", "::ffff:192.0.2.1",
             "1:2:3:4:5:6:7:8"}) {
        RUVIA_CHECK(ruvia::detail::isClientIpAddress(host));
    }
    for (const std::string_view host : {"", "localhost", "api.example.test",
             "256.1.2.3", "127.1", "01.2.3.4", "127.0.0.1.example", "[::1]",
             "fe80::1%eth0", "1:2:3:4:5:6:7:8:9", "2001:db8::bad::1"}) {
        RUVIA_CHECK(!ruvia::detail::isClientIpAddress(host));
    }
    constexpr std::string_view bounded = "2001:db8::1/trailing";
    RUVIA_CHECK(ruvia::detail::isClientIpAddress(bounded.substr(0, bounded.find('/'))));
    RUVIA_CHECK(!ruvia::detail::isClientIpAddress(bounded));
    constexpr char embeddedNull[] = "127.0.0.1\0suffix";
    RUVIA_CHECK(!ruvia::detail::isClientIpAddress(
        std::string_view(embeddedNull, sizeof(embeddedNull) - 1)));
    RUVIA_CHECK(!ruvia::detail::isClientIpAddress(std::string(253, 'a')));
}

RUVIA_TEST(config_host_validation_default_rules) {
    RUVIA_CHECK(isValidConfigHost("localhost"));
    RUVIA_CHECK(isValidConfigHost("0.0.0.0"));
    RUVIA_CHECK(isValidConfigHost("example.com"));
    RUVIA_CHECK(isValidConfigHost("::1"));
    RUVIA_CHECK(!isValidConfigHost(""));                           // empty
    RUVIA_CHECK(!isValidConfigHost("has space"));                  // control/space bytes
    RUVIA_CHECK(!isValidConfigHost("a/b"));                        // '/'
    RUVIA_CHECK(!isValidConfigHost("a\\b"));                       // '\\'
    RUVIA_CHECK(!isValidConfigHost(std::string_view("a\rb", 3)));  // CR
    RUVIA_CHECK(
        !isValidConfigHost(std::string_view("a\x7f"
                                            "b",
            3)));  // DEL
}

RUVIA_TEST(config_host_validation_separated_port_rules) {
    // For a "host:port" style listen address, brackets and a single colon are
    // disallowed (the colon separates the port).
    RUVIA_CHECK(!isValidConfigHost("[::1]", kSeparatedPortHostRules));    // brackets rejected
    RUVIA_CHECK(!isValidConfigHost("host:80", kSeparatedPortHostRules));  // single colon rejected
    RUVIA_CHECK(isValidConfigHost("host", kSeparatedPortHostRules));      // bare host is fine
    RUVIA_CHECK(isValidConfigHost("::1", kSeparatedPortHostRules));       // two colons is not "single"
}

RUVIA_TEST(sni_host_validation_accepts_dns_name_only) {
    RUVIA_CHECK(isValidSniHost("localhost"));
    RUVIA_CHECK(isValidSniHost("Example.com"));
    RUVIA_CHECK(isValidSniHost("xn--bcher-kva.example"));
    RUVIA_CHECK(!isValidSniHost(""));
    RUVIA_CHECK(!isValidSniHost("example.com."));
    RUVIA_CHECK(!isValidSniHost("example.com:443"));
    RUVIA_CHECK(!isValidSniHost("[::1]"));
    RUVIA_CHECK(!isValidSniHost("127.0.0.1"));
    RUVIA_CHECK(!isValidSniHost("bad host"));
    RUVIA_CHECK(!isValidSniHost("-bad.example"));
    RUVIA_CHECK(!isValidSniHost("bad-.example"));
    RUVIA_CHECK(!isValidSniHost("bad..example"));
    const auto label = std::string(63, 'a');
    const auto longest = label + "." + label + "." + label + "." + std::string(61, 'b');
    RUVIA_CHECK(isValidSniHost(longest));
    RUVIA_CHECK(!isValidSniHost(longest + "b"));
}

RUVIA_TEST(sni_host_validation_uses_distinct_empty_and_invalid_messages) {
    RUVIA_CHECK_EQ(caughtMessage(
                       [] { ruvia::detail::ensureSniHost("", "was-empty", "was-invalid"); }),
        std::string("was-empty"));
    RUVIA_CHECK_EQ(caughtMessage([] {
        ruvia::detail::ensureSniHost("bad host", "was-empty", "was-invalid");
    }),
        std::string("was-invalid"));
    RUVIA_CHECK(caughtMessage([] {
        ruvia::detail::ensureSniHost(
            "example.com", "was-empty", "was-invalid");
    })
            .empty());
}

RUVIA_TEST(config_size_port_duration_guards) {
    RUVIA_CHECK(throwsInvalid([] { ensurePositiveSize(0, "size"); }));
    RUVIA_CHECK(!throwsInvalid([] { ensurePositiveSize(1, "size"); }));

    RUVIA_CHECK(throwsInvalid([] { ensureNonZeroPort(0, "port"); }));
    RUVIA_CHECK(!throwsInvalid([] { ensureNonZeroPort(8080, "port"); }));

    using namespace std::chrono;
    // Positive means strictly greater than zero.
    RUVIA_CHECK(throwsInvalid([] { ensurePositiveDuration(seconds(0), "d"); }));
    RUVIA_CHECK(!throwsInvalid([] { ensurePositiveDuration(milliseconds(1), "d"); }));
}

RUVIA_TEST(config_ensure_host_throws_distinct_messages) {
    // An empty host reports the empty message; an invalid host reports the
    // invalid message; a valid host does not throw.
    RUVIA_CHECK_EQ(caughtMessage([] { ensureConfigHost("", "was-empty", "was-invalid"); }),
        std::string("was-empty"));
    RUVIA_CHECK_EQ(caughtMessage([] { ensureConfigHost("bad host", "was-empty", "was-invalid"); }),
        std::string("was-invalid"));
    RUVIA_CHECK(
        caughtMessage([] { ensureConfigHost("example.com", "was-empty", "was-invalid"); }).empty());
}

RUVIA_TEST(client_transport_uri_host_preserves_network_host) {
    CountingMemoryResource resource;
    for (const auto& [host, expected] : {
             std::pair{"example.com", "example.com"},
             std::pair{"192.0.2.1", "192.0.2.1"},
             std::pair{"::1", "[::1]"},
             std::pair{"2001:db8::1", "[2001:db8::1]"}}) {
        std::string networkHost(host);
        const auto wireHost = ruvia::detail::clientUriHost(networkHost, &resource);
        RUVIA_CHECK_EQ(std::string_view(wireHost), std::string_view(expected));
        RUVIA_CHECK_EQ(networkHost, std::string(host));
        RUVIA_CHECK(wireHost.get_allocator().resource() == &resource);
    }
}

RUVIA_TEST(client_transport_validation_uses_one_host_and_policy_contract) {
    RUVIA_CHECK(!throwsInvalid(
        [] { validateClientOriginHost("example.com", "host is empty", "host is invalid"); }));
    RUVIA_CHECK(!throwsInvalid(
        [] { validateClientOriginHost("::1", "host is empty", "host is invalid"); }));
    RUVIA_CHECK(throwsInvalid(
        [] { validateClientOriginHost("host:443", "host is empty", "host is invalid"); }));
    RUVIA_CHECK(throwsInvalid(
        [] { validateClientOriginHost("[::1]", "host is empty", "host is invalid"); }));

    ruvia::HttpClientConfig config;
    RUVIA_CHECK(!throwsInvalid(
        [&config] { validateClientTransportConfig(clientTransportConfigView(config)); }));

    config.tcpNoDelay = std::bit_cast<ruvia::TcpNoDelayPolicy>(std::uint8_t{255});
    RUVIA_CHECK(throwsInvalid(
        [&config] { validateClientTransportConfig(clientTransportConfigView(config)); }));
    config.tcpNoDelay = ruvia::TcpNoDelayPolicy::kEnable;
    config.certificateChainFile = "client.pem";
    RUVIA_CHECK(throwsInvalid(
        [&config] { validateClientTransportConfig(clientTransportConfigView(config)); }));
}

RUVIA_TEST(client_origin_accepts_network_names_and_rejects_uri_only_names) {
    for (const auto host : {"localhost", "api.example.com", "api.example.com.",
             "xn--bcher-kva.example", "192.0.2.1", "::1", "::ffff:192.0.2.1"}) {
        RUVIA_CHECK(!throwsInvalid(
            [host] { validateClientOriginHost(host, "empty", "invalid"); }));
    }
    for (const auto host : {"foo%2Ebar", "%C3%BC.example", "bad_name.example",
             "-bad.example", "bad-.example", "bad..example", ".", "example.com..",
             "[::1]", "fe80::1%eth0", "[v1.example]", "example.com:443"}) {
        RUVIA_CHECK(throwsInvalid(
            [host] { validateClientOriginHost(host, "empty", "invalid"); }));
    }
    const auto label = std::string(63, 'a');
    const auto longest = label + "." + label + "." + label + "." + std::string(61, 'b');
    for (const auto& host : {label + ".example", longest, longest + "."}) {
        RUVIA_CHECK(!throwsInvalid(
            [&host] { validateClientOriginHost(host, "empty", "invalid"); }));
    }
    for (const auto& host : {std::string(64, 'a') + ".example", longest + "b"}) {
        RUVIA_CHECK(throwsInvalid(
            [&host] { validateClientOriginHost(host, "empty", "invalid"); }));
    }
}

RUVIA_TEST(client_tls_stream_inherits_verification_and_protocol_policy) {
    asio::io_context loop;
    for (const auto policy : {ruvia::TlsPeerVerificationPolicy::kVerify,
             ruvia::TlsPeerVerificationPolicy::kSkipVerification}) {
        asio::ssl::context tls(asio::ssl::context::tls_client);
        ruvia::detail::configureClientTlsContext(tls, {.tlsPeerVerification = policy});
        asio::ssl::stream<asio::ip::tcp::socket> stream(loop, tls);
        RUVIA_CHECK_EQ(SSL_get_verify_mode(stream.native_handle()),
            policy == ruvia::TlsPeerVerificationPolicy::kVerify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE);
        const auto disabled = SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1;
        RUVIA_CHECK_EQ(SSL_get_options(stream.native_handle()) & disabled, disabled);
    }
}

RUVIA_TEST(client_tls_sni_uses_dns_identity_without_changing_network_host) {
    asio::io_context loop;
    asio::ssl::context tls(asio::ssl::context::tls_client);
    for (const auto& [host, expected] : {
             std::pair{"example.com", "example.com"},
             std::pair{"example.com.", "example.com"},
             std::pair{"127.0.0.1", ""},
             std::pair{"2001:db8::1", ""}}) {
        asio::ssl::stream<asio::ip::tcp::socket> stream(loop, tls);
        std::pmr::string networkHost(host);
        RUVIA_CHECK(ruvia::detail::prepareClientTlsStream(stream, networkHost,
                        {.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kVerify},
                        ruvia::detail::ClientAlpnMode::kHttp11) ==
                    ruvia::detail::ClientTlsSetupError::kNone);
        const auto* name = SSL_get_servername(stream.native_handle(), TLSEXT_NAMETYPE_host_name);
        RUVIA_CHECK_EQ(name ? std::string_view(name) : std::string_view{},
            std::string_view(expected));
        RUVIA_CHECK_EQ(std::string_view(networkHost), std::string_view(host));
    }
}

RUVIA_TEST(client_transport_formats_the_complete_port_domain) {
    ClientPortTextBuffer buffer{};
    RUVIA_CHECK_EQ(formatClientPort(1, buffer), std::string_view("1"));
    RUVIA_CHECK_EQ(formatClientPort(443, buffer), std::string_view("443"));
    RUVIA_CHECK_EQ(formatClientPort(65'535, buffer), std::string_view("65535"));
}

RUVIA_TEST(client_transport_storage_owns_normalized_strings) {
    std::pmr::unsynchronized_pool_resource resource;
    std::optional<ruvia::detail::ClientTransportConfigStorage> storage;
    {
        ruvia::HttpClientConfig config;
        config.caFile = std::string(80, 'c');
        config.certificateChainFile = std::string(80, 'x');
        config.privateKeyFile = std::string(80, 'k');
        config.privateKeyPassword = std::string(80, 'p');
        storage.emplace(clientTransportConfigView(config), &resource);
    }

    const auto view = storage->view();
    const std::string expectedCaFile(80, 'c');
    const std::string expectedCertificate(80, 'x');
    const std::string expectedPrivateKey(80, 'k');
    const std::string expectedPassword(80, 'p');
    RUVIA_CHECK_EQ(view.caFile, std::string_view(expectedCaFile));
    RUVIA_CHECK_EQ(view.certificateChainFile, std::string_view(expectedCertificate));
    RUVIA_CHECK_EQ(view.privateKeyFile, std::string_view(expectedPrivateKey));
    RUVIA_CHECK_EQ(view.privateKeyPassword, std::string_view(expectedPassword));
}

RUVIA_TEST(http_client_config_rebinding_preserves_normalized_origin_and_owned_fields) {
    for (const auto scheme : {ruvia::HttpScheme::kHttp, ruvia::HttpScheme::kHttps}) {
        for (const bool explicit_port : {false, true}) {
            ruvia::HttpClientConfig config;
            config.host = std::string(40, 'h') + ".example";
            config.scheme = scheme;
            if (explicit_port) {
                config.port = 8443;
            }
            config.caFile = std::string(80, 'c');
            config.certificateChainFile = std::string(80, 'x');
            config.privateKeyFile = std::string(80, 'k');
            config.privateKeyPassword = std::string(80, 'p');
            config.userAgent = std::string(80, 'a');
            config.cookies.emplace_back(std::string(80, 'n'), std::string(80, 'v'));
            config.qpack.maxTableCapacity = 8192;
            config.qpack.maxBlockedStreams = 7;
            verify_config_rebinding<ruvia::detail::HttpClientConfigStorage>(ruvia_ctx, config,
                [&](const auto& storage, std::pmr::memory_resource* resource) {
                    RUVIA_CHECK_EQ(std::string_view(storage.host), std::string_view(config.host));
                    RUVIA_CHECK(storage.host.get_allocator().resource() == resource);
                    RUVIA_CHECK(storage.scheme == scheme);
                    RUVIA_CHECK_EQ(storage.port, explicit_port ? 8443 : (scheme == ruvia::HttpScheme::kHttps ? 443 : 80));
                    RUVIA_CHECK_EQ(storage.http3Qpack.maxTableCapacity, config.qpack.maxTableCapacity);
                    RUVIA_CHECK_EQ(storage.http3Qpack.maxBlockedStreams, config.qpack.maxBlockedStreams);
                    RUVIA_CHECK_EQ(std::string_view(storage.userAgent), std::string_view(config.userAgent));
                    RUVIA_CHECK(storage.userAgent.get_allocator().resource() == resource);
                    RUVIA_CHECK_EQ(storage.cookies.size(), std::size_t{1});
                    RUVIA_CHECK(storage.cookies.get_allocator().resource() == resource);
                    RUVIA_CHECK(storage.cookies.front().first.get_allocator().resource() == resource);
                    RUVIA_CHECK(storage.cookies.front().second.get_allocator().resource() == resource);
                    RUVIA_CHECK_EQ(std::string_view(storage.cookies.front().first), std::string_view(config.cookies.front().first));
                    RUVIA_CHECK_EQ(std::string_view(storage.cookies.front().second), std::string_view(config.cookies.front().second));
                    const auto transport = storage.transport.view();
                    RUVIA_CHECK_EQ(transport.caFile, std::string_view(config.caFile));
                    RUVIA_CHECK_EQ(transport.certificateChainFile, std::string_view(config.certificateChainFile));
                    RUVIA_CHECK_EQ(transport.privateKeyFile, std::string_view(config.privateKeyFile));
                    RUVIA_CHECK_EQ(transport.privateKeyPassword, std::string_view(config.privateKeyPassword));
                });
        }
    }
}

RUVIA_TEST(redis_config_rebinding_preserves_tls_credentials_and_limits) {
    ruvia::RedisConfig config;
    config.host = std::string(40, 'h') + ".example";
    config.username = std::string(80, 'u');
    config.password = std::string(80, 'p');
    config.tls.ca_file = std::string(80, 'c');
    config.tls.certificate_file = std::string(80, 'x');
    config.tls.private_key_file = std::string(80, 'k');
    config.tls.server_name = config.host;
    config.database = 2;
    config.poolSizePerWorker = 7;
    config.blockingPoolSizePerWorker = 3;
    config.commandTimeout = std::nullopt;
    config.maxReplyBytes = std::nullopt;
    config.maxArrayDepth = 17;
    verify_config_rebinding<ruvia::detail::RedisConfigStorage>(ruvia_ctx, config,
        [&](const auto& storage, std::pmr::memory_resource* resource) {
            RUVIA_CHECK_EQ(std::string_view(storage.host), std::string_view(config.host));
            RUVIA_CHECK(storage.host.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.username), std::string_view(config.username));
            RUVIA_CHECK(storage.username.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.password), std::string_view(config.password));
            RUVIA_CHECK(storage.password.get_allocator().resource() == resource);
            RUVIA_CHECK(storage.tls.mode == config.tls.mode);
            RUVIA_CHECK_EQ(std::string_view(storage.tls.ca_file), std::string_view(config.tls.ca_file));
            RUVIA_CHECK(storage.tls.ca_file.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.tls.certificate_file), std::string_view(config.tls.certificate_file));
            RUVIA_CHECK(storage.tls.certificate_file.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.tls.private_key_file), std::string_view(config.tls.private_key_file));
            RUVIA_CHECK(storage.tls.private_key_file.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.tls.server_name), std::string_view(config.tls.server_name));
            RUVIA_CHECK(storage.tls.server_name.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(storage.database, config.database);
            RUVIA_CHECK_EQ(storage.poolSizePerWorker, config.poolSizePerWorker);
            RUVIA_CHECK_EQ(storage.blockingPoolSizePerWorker, config.blockingPoolSizePerWorker);
            RUVIA_CHECK(!storage.commandTimeout);
            RUVIA_CHECK(!storage.maxReplyBytes);
            RUVIA_CHECK_EQ(storage.maxArrayDepth, config.maxArrayDepth);
        });
}

RUVIA_TEST(http_client_config_is_validated_before_pmr_normalization) {
    ruvia::HttpClientConfig config;
    config.host = "example.com";
    config.caFile = std::string(128, 'c');
    config.connectTimeout = std::chrono::milliseconds::zero();
    CountingMemoryResource resource;

    RUVIA_CHECK(
        throwsInvalid([&] { (void)ruvia::detail::HttpClientConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
}

RUVIA_TEST(http_client_config_rejects_overflowing_scheduler_capacity) {
    ruvia::HttpClientConfig config;
    config.host = "example.com";
    config.connectionCount = 2;
    config.maxConcurrentHttp2StreamsPerConnection = std::numeric_limits<std::size_t>::max();

    RUVIA_CHECK(throwsInvalid([&] {
        std::pmr::unsynchronized_pool_resource resource;
        (void)ruvia::detail::HttpClientConfigStorage(config, &resource);
    }));
}

RUVIA_TEST(http3_client_config_requires_https_and_bounded_response_storage) {
    ruvia::HttpClientConfig config;
    config.host = "example.com";
    config.protocol = ruvia::HttpClientProtocol::kHttp3Only;

    std::pmr::unsynchronized_pool_resource resource;
    RUVIA_CHECK(!throwsInvalid(
        [&] { (void)ruvia::detail::HttpClientConfigStorage(config, &resource); }));

    config.http3_early_data = true;
    RUVIA_CHECK(!throwsInvalid(
        [&] { (void)ruvia::detail::HttpClientConfigStorage(config, &resource); }));
    config.protocol = ruvia::HttpClientProtocol::kNegotiate;
    config.scheme = ruvia::HttpScheme::kHttp;
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::HttpClientConfigStorage(config, &resource); }));
    config.protocol = ruvia::HttpClientProtocol::kHttp3Only;
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::HttpClientConfigStorage(config, &resource); }));

    config.scheme = ruvia::HttpScheme::kHttps;
    config.http3_early_data = false;
    config.maxResponseBytes = std::size_t{64} * 1024 * 1024 + 1;
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::HttpClientConfigStorage(config, &resource); }));
}

RUVIA_TEST(websocket_client_config_is_validated_before_pmr_normalization) {
    ruvia::WebSocketClientConfig config;
    config.host = "example.com";
    config.caFile = std::string(128, 'c');
    config.connectTimeout = std::chrono::milliseconds::zero();
    CountingMemoryResource resource;

    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::WebSocketClientConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});

    config.connectTimeout = std::chrono::milliseconds{5000};
    config.target = "/events#fragment";
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::WebSocketClientConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});

    config.target = "/";
    config.subprotocols = {"chat", "chat"};
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::WebSocketClientConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});

    config.subprotocols = {"bad token"};
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::WebSocketClientConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});

    config.subprotocols = {""};
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::WebSocketClientConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});

    config.subprotocols.clear();
    config.heartbeat.pongTimeout = std::chrono::milliseconds{1000};
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::WebSocketClientConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});

    config.heartbeat = {.pingInterval = std::chrono::milliseconds{1000},
        .pongTimeout = std::chrono::milliseconds::zero()};
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)ruvia::detail::WebSocketClientConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
}

RUVIA_TEST(websocket_client_config_storage_owns_normalized_strings) {
    std::pmr::unsynchronized_pool_resource resource;
    std::optional<ruvia::detail::WebSocketClientConfigStorage> storage;
    ruvia::WebSocketClientConfig config;
    {
        config.host = std::string(40, 'h') + "." + std::string(40, 'h');
        config.target = "/" + std::string(80, 't');
        config.headers.emplace_back("X-Test", std::string(80, 'v'));
        config.subprotocols = {"chat", "superchat"};
        config.caFile = std::string(80, 'c');
        config.userAgent = std::string(80, 'u');
        storage.emplace(config, &resource);
    }

    RUVIA_CHECK(storage->host.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->target.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->headers.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->headers.front().name.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->headers.front().value.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->subprotocols.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->subprotocols.front().get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->userAgent.get_allocator().resource() == &resource);
    RUVIA_CHECK_EQ(std::string(storage->host), std::string(40, 'h') + "." + std::string(40, 'h'));
    RUVIA_CHECK_EQ(std::string(storage->target), "/" + std::string(80, 't'));
    RUVIA_CHECK_EQ(std::string(storage->headers.front().name), "X-Test");
    RUVIA_CHECK_EQ(std::string(storage->headers.front().value), std::string(80, 'v'));
    RUVIA_CHECK_EQ(storage->subprotocols.size(), std::size_t{2});
    RUVIA_CHECK_EQ(std::string(storage->subprotocols[0]), "chat");
    RUVIA_CHECK_EQ(std::string(storage->subprotocols[1]), "superchat");
    RUVIA_CHECK(!storage->heartbeat.pingInterval.has_value());
    RUVIA_CHECK(!storage->heartbeat.pongTimeout.has_value());
    RUVIA_CHECK_EQ(std::string(storage->userAgent), std::string(80, 'u'));

    config.heartbeat = {.pingInterval = std::chrono::milliseconds{1000}};
    storage.emplace(config, &resource);
    RUVIA_CHECK_EQ(storage->heartbeat.pingInterval->count(), std::int64_t{1000});
    RUVIA_CHECK_EQ(storage->heartbeat.pongTimeout->count(), std::int64_t{1000});
}

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
RUVIA_TEST(database_config_is_validated_before_pmr_normalization) {
#ifdef RUVIA_ENABLE_MARIADB
    ruvia::DbConfig config{.driver = ruvia::DbDriver::kMariaDb};
#else
    ruvia::DbConfig config{.driver = ruvia::DbDriver::kPostgreSql};
#endif
    config.username = std::string(128, 'u');
    config.connectTimeout = std::chrono::milliseconds::zero();
    CountingMemoryResource resource;

    RUVIA_CHECK(throwsInvalid([&] { (void)ruvia::detail::DbConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
}
#endif

RUVIA_TEST(redis_config_is_validated_before_pmr_normalization) {
    ruvia::RedisConfig config;
    config.username = std::string(128, 'u');
    config.connectTimeout = std::chrono::milliseconds::zero();
    CountingMemoryResource resource;

    RUVIA_CHECK(throwsInvalid([&] { (void)ruvia::detail::RedisConfigStorage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
}

RUVIA_TEST(app_document_root_rejects_invalid_static_options_at_configuration) {
    ruvia::DocumentRootConfig config;
    config.root = "public";
    config.staticOptions.cacheControl = " private";

    RUVIA_CHECK(throwsInvalid([&config] { ruvia::app().documentRoot(std::move(config)); }));
}

RUVIA_TEST(app_document_root_rejects_disabled_refresh) {
    ruvia::DocumentRootConfig disabledRefresh;
    disabledRefresh.root = "public";
    disabledRefresh.runtime.refreshInterval = std::chrono::milliseconds::zero();
    RUVIA_CHECK(throwsInvalid(
        [&disabledRefresh] { ruvia::app().documentRoot(std::move(disabledRefresh)); }));
}

RUVIA_TEST(app_compression_rejects_invalid_thresholds_at_configuration) {
    RUVIA_CHECK(
        throwsInvalid([] { ruvia::app().compression({.minBytes = 1024, .syncBytes = 512}); }));
    RUVIA_CHECK(throwsInvalid(
        [] { ruvia::app().compression({.minBytes = 1024, .syncBytes = 2048, .maxBytes = 1024}); }));
}

RUVIA_TEST(integration_config_copies_public_strings_into_internal_pmr_storage) {
    std::pmr::unsynchronized_pool_resource targetResource;
#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
    std::optional<ruvia::detail::DbConfigStorage> database;
#endif
    std::optional<ruvia::detail::RedisConfigStorage> redis;
    {
#ifdef RUVIA_ENABLE_MARIADB
        auto source = ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#elif defined(RUVIA_ENABLE_POSTGRESQL)
        auto source = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
#ifdef RUVIA_ENABLE_MARIADB
        // MariaDB's default verify_identity mode requires an IP-literal host.
        source.host = "127.0.0.1";
#else
        source.host = std::string(40, 'h') + "." + std::string(39, 'h');
#endif
        source.username = std::string(80, 'u');
        source.password = std::string(80, 'p');
        source.database = std::string(80, 'd');
        database.emplace(source, &targetResource);
        RUVIA_CHECK(database->host.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(database->username.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(database->password.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(database->database.get_allocator().resource() == &targetResource);
#endif

        ruvia::RedisConfig sourceRedis{
            .host = std::string(80, 'r'),
            .port = 6379,
            .username = std::string(80, 'x'),
            .password = std::string(80, 'y'),
        };
        redis.emplace(sourceRedis, &targetResource);
        RUVIA_CHECK(redis->host.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(redis->username.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(redis->password.get_allocator().resource() == &targetResource);
    }

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
#ifdef RUVIA_ENABLE_MARIADB
    RUVIA_CHECK_EQ(std::string(database->host), "127.0.0.1");
#else
    RUVIA_CHECK_EQ(std::string(database->host), std::string(40, 'h') + "." + std::string(39, 'h'));
#endif
#endif
    RUVIA_CHECK_EQ(std::string(redis->host), std::string(80, 'r'));
}
