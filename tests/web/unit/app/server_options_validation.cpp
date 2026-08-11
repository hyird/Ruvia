#include "test_harness.h"

#include <chrono>
#include <concepts>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/App.h"

namespace {

class ReleasableMemoryResource final : public std::pmr::memory_resource {
public:
    void release() noexcept {
        released_ = true;
    }

    [[nodiscard]] bool deallocatedAfterRelease() const noexcept {
        return deallocatedAfterRelease_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        deallocatedAfterRelease_ = deallocatedAfterRelease_ || released_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool released_{false};
    bool deallocatedAfterRelease_{false};
};

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
    static_assert(std::same_as<decltype(HttpServerOptions{}.idleTimeout), std::optional<std::chrono::milliseconds>>);
    static_assert(std::same_as<decltype(HttpServerOptions{}.requestHeaderTimeout), std::optional<std::chrono::milliseconds>>);
    static_assert(std::same_as<decltype(HttpServerOptions{}.requestBodyTimeout), std::optional<std::chrono::milliseconds>>);
    static_assert(std::same_as<decltype(HttpServerOptions{}.writeTimeout), std::optional<std::chrono::milliseconds>>);
    static_assert(std::same_as<decltype(HttpServerOptions{}.maxConnections), std::optional<std::size_t>>);
    static_assert(std::same_as<decltype(HttpServerOptions{}.maxRequestsPerConnection), std::optional<std::size_t>>);
    static_assert(std::same_as<decltype(HttpServerOptions{}.maxStreamBodyBytes), std::optional<std::size_t>>);
    static_assert(std::same_as<decltype(HttpServerOptions{}.defaultRateLimitPerWorker), std::optional<ruvia::RateLimitRule>>);
    static_assert(std::same_as<decltype(HttpServerOptions{}.rateLimitSlotsPerWorker), std::size_t>);
    RUVIA_CHECK(!HttpServerOptions{}.maxStreamBodyBytes.has_value());
    RUVIA_CHECK_EQ(HttpServerOptions{}.rateLimitSlotsPerWorker, ruvia::kDefaultRateLimitSlotsPerWorker);
    // An unconfigured server is bounded by default against connection floods.
    RUVIA_CHECK(HttpServerOptions{}.maxConnections.has_value());
    RUVIA_CHECK_EQ(*HttpServerOptions{}.maxConnections, std::size_t{1024});
    RUVIA_CHECK(!throwsInvalid([] { validateHttpServerOptions(HttpServerOptions{}); }));
}

RUVIA_TEST(validate_server_options_owns_document_root_runtime_policy) {
    HttpServerOptions options;
    options.documentRoot.runtimeOptions.refreshMode = ruvia::DocumentRootRefreshMode::kPolling;
    options.documentRoot.runtimeOptions.refreshInterval = std::chrono::milliseconds::zero();
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.documentRoot.runtimeOptions.refreshInterval = std::chrono::milliseconds(1);
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.documentRoot.runtimeOptions.refreshMode = ruvia::DocumentRootRefreshMode::kImmutable;
    options.documentRoot.runtimeOptions.refreshInterval = std::chrono::milliseconds::zero();
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerOptions(options); }));

    // Browser reload assets need a changing root revision. Exposing them
    // while the root is immutable would silently promise live reload that can
    // never observe a filesystem change.
    options.documentRoot.runtimeOptions.enableLiveReload = true;
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
}

RUVIA_TEST(validate_server_options_rejects_configured_nonpositive_timeout) {
    // Every connection timeout feeds the same positive optional fold. Each one bounds
    // how long a slow client can hold a connection (a slowloris defense), so a
    // nonpositive value in ANY of them must be rejected -- checking only idleTimeout
    // would miss a field dropped from the fold call.
    using std::chrono::milliseconds;
    {
        HttpServerOptions options;
        options.idleTimeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.requestHeaderTimeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.requestBodyTimeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.writeTimeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.idleTimeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.requestHeaderTimeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.requestBodyTimeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.writeTimeout = milliseconds(-1);
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
        options.maxRequestsPerConnection = 0;
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
        tls.clientCertificates.emplace(std::pmr::get_default_resource(), ruvia::TlsClientCertificateRequirement::kOptional);
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
        sni.host = "example.com:443";
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

RUVIA_TEST(listener_config_rejects_invalid_listener_and_tls_states_at_construction) {
    static_assert(!std::is_default_constructible_v<ruvia::TlsIdentity>);
    static_assert(!std::is_default_constructible_v<ruvia::TlsClientCertificatePolicy>);
    static_assert(!std::is_default_constructible_v<ruvia::TlsConfig>);
    static_assert(!std::is_default_constructible_v<ruvia::ListenerConfig>);
    static_assert(!std::is_aggregate_v<ruvia::ListenerConfig>);

    RUVIA_CHECK(throwsInvalid([] { (void)ruvia::TlsIdentity::fromFiles({}, "key.pem"); }));
    RUVIA_CHECK(throwsInvalid([] { (void)ruvia::TlsIdentity::fromFiles("cert.pem", {}); }));
    RUVIA_CHECK(throwsInvalid([] { (void)ruvia::TlsClientCertificatePolicy::required({}); }));
    RUVIA_CHECK(throwsInvalid([] { (void)ruvia::ListenerConfig::http({}, 8080); }));
    RUVIA_CHECK(throwsInvalid([] { (void)ruvia::ListenerConfig::http("127.0.0.1", 0); }));
    RUVIA_CHECK(throwsInvalid([] { (void)ruvia::ListenerConfig::redirectHttpToHttps({}, 8080, 8443); }));
    RUVIA_CHECK(throwsInvalid([] { (void)ruvia::ListenerConfig::redirectHttpToHttps("127.0.0.1", 8443, 8443); }));
    RUVIA_CHECK(throwsInvalid([] { ruvia::app().setListeners({}); }));
    RUVIA_CHECK(throwsInvalid([] { ruvia::app().setListeners({ruvia::ListenerConfig::http("127.0.0.1", 8080), ruvia::ListenerConfig::http("0.0.0.0", 8080)}); }));
    RUVIA_CHECK(throwsInvalid([] { ruvia::app().setListeners({ruvia::ListenerConfig::redirectHttpToHttps("127.0.0.1", 8080, 8443)}); }));
}

RUVIA_TEST(tls_config_rejects_empty_or_duplicate_sni_identity) {
    auto tls = ruvia::TlsConfig(ruvia::TlsIdentity::fromFiles("cert.pem", "key.pem"));
    RUVIA_CHECK(throwsInvalid([&] { tls.addSniIdentity({}, ruvia::TlsIdentity::fromFiles("other.pem", "other.key")); }));
    RUVIA_CHECK(throwsInvalid([&] { tls.addSniIdentity("example.com:443", ruvia::TlsIdentity::fromFiles("other.pem", "other.key")); }));
    RUVIA_CHECK(throwsInvalid([&] { tls.addSniIdentity("127.0.0.1", ruvia::TlsIdentity::fromFiles("other.pem", "other.key")); }));
    tls.addSniIdentity("Example.com", ruvia::TlsIdentity::fromFiles("other.pem", "other.key"));
    RUVIA_CHECK(throwsInvalid([&] { tls.addSniIdentity("example.COM", ruvia::TlsIdentity::fromFiles("third.pem", "third.key")); }));
}

RUVIA_TEST(tls_identity_owns_password_independently_of_caller_resource) {
    ReleasableMemoryResource callerResource;
    const std::string expected(80, 's');
    std::optional<ruvia::TlsIdentity> identity;
    {
        const std::pmr::string password(expected, &callerResource);
        identity.emplace(ruvia::TlsIdentity::fromFiles("cert.pem", "key.pem", password));
    }
    callerResource.release();
    RUVIA_CHECK_EQ(identity->privateKeyPassword(), std::string_view(expected));
    identity.reset();
    RUVIA_CHECK(!callerResource.deallocatedAfterRelease());
}

RUVIA_TEST(self_contained_app_callbacks_release_owned_state) {
    std::weak_ptr<int> accessState;
    {
        auto state = std::make_shared<int>(1);
        accessState = state;
        ruvia::AccessLogCallback callback([state](const ruvia::AccessLogRecord&) noexcept { (void)state; });
        auto copy = callback;
        state.reset();
        RUVIA_CHECK(!accessState.expired());
        (void)copy;
    }
    RUVIA_CHECK(accessState.expired());

    std::weak_ptr<int> failureState;
    {
        auto state = std::make_shared<int>(1);
        failureState = state;
        ruvia::ConnectionFailureCallback callback([state](const ruvia::ConnectionFailureRecord&) noexcept { (void)state; });
        state.reset();
        RUVIA_CHECK(!failureState.expired());
    }
    RUVIA_CHECK(failureState.expired());

    std::weak_ptr<int> errorState;
    {
        auto state = std::make_shared<int>(1);
        errorState = state;
        ruvia::HttpErrorHandler callback([state](ruvia::Context&, ruvia::HttpErrorInfo) -> ruvia::Task<ruvia::HttpResponse> {
            (void)state;
            co_return ruvia::HttpResponse{};
        });
        state.reset();
        RUVIA_CHECK(!errorState.expired());
    }
    RUVIA_CHECK(errorState.expired());

    std::weak_ptr<int> notFoundState;
    {
        auto state = std::make_shared<int>(1);
        notFoundState = state;
        ruvia::HttpNotFoundHandler callback([state](ruvia::Context&) -> ruvia::Task<ruvia::HttpResponse> {
            (void)state;
            co_return ruvia::HttpResponse{};
        });
        state.reset();
        RUVIA_CHECK(!notFoundState.expired());
    }
    RUVIA_CHECK(notFoundState.expired());
}
