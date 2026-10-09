#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/App.h"
#include "ruvia/web/StaticFiles.h"

#include "app/AppState.h"
#include "http/CorsOptions.h"
#include "server/HttpServerOptionsValidation.h"
#include "test_harness.h"

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

using ruvia::detail::HttpServerListenerDefinition;
using ruvia::detail::HttpServerOptions;
using ruvia::detail::ValidatedHttpServerConfiguration;
using ruvia::detail::validateHttpServerConfiguration;
using ruvia::detail::validateHttpServerListener;
using ruvia::detail::validateHttpServerOptions;

template <typename Transport>
HttpServerListenerDefinition makeListener(Transport&& transport) {
    return HttpServerListenerDefinition(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 8080),
        std::forward<Transport>(transport));
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

ruvia::TlsConfig tlsConfig(std::filesystem::path certificateChainFile,
    std::filesystem::path privateKeyFile, std::string privateKeyPassword = {}) {
    return {
        .certificateChainFile = std::move(certificateChainFile),
        .privateKeyFile = std::move(privateKeyFile),
        .privateKeyPassword = std::move(privateKeyPassword),
    };
}

}  // namespace

RUVIA_TEST(validate_server_options_accepts_defaults) {
    RUVIA_CHECK(!HttpServerOptions{}.max_stream_body_bytes.has_value());
    RUVIA_CHECK(!HttpServerOptions{}.compression.has_value());
    RUVIA_CHECK_EQ(ruvia::CompressionConfig{}.minBytes, std::size_t{1024});
    RUVIA_CHECK_EQ(ruvia::CompressionConfig{}.syncBytes, std::size_t{64} * 1024);
    RUVIA_CHECK_EQ(ruvia::CompressionConfig{}.maxBytes, std::size_t{64} * 1024 * 1024);
    RUVIA_CHECK_EQ(
        HttpServerOptions{}.rateLimitCapacityPerWorker, ruvia::kDefaultRateLimitCapacityPerWorker);
    // An unconfigured server is bounded by default against connection floods.
    const auto defaultMaxConnections = HttpServerOptions{}.maxConnections;
    RUVIA_CHECK(defaultMaxConnections.has_value());
    RUVIA_CHECK_EQ(defaultMaxConnections.value_or(0), std::size_t{1024});
    RUVIA_CHECK(!throwsInvalid([] { validateHttpServerOptions(HttpServerOptions{}); }));
}

RUVIA_TEST(server_configuration_owns_listener_inputs_until_retirement) {
    ReleasableMemoryResource caller_resource;
    std::optional<ValidatedHttpServerConfiguration> configuration;
    const std::string certificate(128, 'c');
    const std::string key(128, 'k');
    {
        HttpServerListenerDefinition::Tls tls(&caller_resource);
        tls.identity.certificateChainFile = certificate;
        tls.identity.privateKeyFile = key;
        tls.http3_early_data = true;
        auto& sni = tls.sniIdentities.emplace_back(&caller_resource);
        sni.host = "api.example.test";
        sni.identity.certificateChainFile = certificate;
        sni.identity.privateKeyFile = key;
        const std::array listeners{makeListener(std::move(tls))};
        configuration.emplace(validateHttpServerConfiguration(listeners, HttpServerOptions{}));
    }
    caller_resource.release();
    const auto& listener = configuration->listeners().front();
    const auto& tls = std::get<HttpServerListenerDefinition::Tls>(listener.transport);
    RUVIA_CHECK_EQ(tls.identity.certificateChainFile, std::string_view(certificate));
    RUVIA_CHECK_EQ(tls.identity.privateKeyFile, std::string_view(key));
    RUVIA_CHECK(tls.http3_early_data);
    RUVIA_CHECK_EQ(tls.sniIdentities.front().host, std::string_view("api.example.test"));
    {
        ruvia::detail::HttpServerSessionConfig session(listener, std::pmr::get_default_resource());
        RUVIA_CHECK(session.tls()->http3_early_data);
    }
    configuration.reset();
    RUVIA_CHECK(!caller_resource.deallocatedAfterRelease());
}

RUVIA_TEST(server_limits_reject_zero_client_result_budgets_at_each_entry) {
    for (const bool retained : {false, true}) {
        ruvia::server_config config;
        auto& limit = retained ? config.http_client_result_budget.maxRetainedBytes
                               : config.http_client_result_budget.max_in_flight_bytes;
        limit = 0;
        RUVIA_CHECK(throwsInvalid([&] {
            (void)ruvia::detail::normalize_server_options(config, HttpServerOptions{});
        }));
        HttpServerOptions options;
        options.http_client_result_budget = config.http_client_result_budget;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
}

RUVIA_TEST(app_enables_a_bounded_blocking_pool_by_default) {
    ruvia::detail::AppState state;
    RUVIA_CHECK(state.blockingPool.has_value());
    if (state.blockingPool.has_value()) {
        RUVIA_CHECK_EQ(state.blockingPool->threadCount, std::size_t{0});
        RUVIA_CHECK_EQ(state.blockingPool->queueCapacity, std::size_t{0});
    }
}

RUVIA_TEST(validate_server_options_rejects_invalid_compression_thresholds) {
    HttpServerOptions options;
    options.compression = ruvia::CompressionConfig{.minBytes = 0};
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.compression = ruvia::CompressionConfig{.minBytes = 1024, .syncBytes = 512};
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.compression =
        ruvia::CompressionConfig{.minBytes = 1024, .syncBytes = 4096, .maxBytes = 2048};
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.compression = ruvia::CompressionConfig{};
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerOptions(options); }));
}

RUVIA_TEST(validate_server_options_owns_document_root_runtime_policy) {
    namespace fs = std::filesystem;
    HttpServerOptions options;
    RUVIA_CHECK(options.documentRoot.root() == nullptr);
    RUVIA_CHECK(options.documentRoot.refreshOptions() == nullptr);
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerOptions(options); }));

    const auto dir = fs::temp_directory_path() / "ruvia_server_options_document_root";
    fs::remove_all(dir);
    fs::create_directories(dir);
    ruvia::StaticRoot root(dir);
    options.documentRoot = HttpServerOptions::DocumentRoot::standalone(root);
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.documentRoot = HttpServerOptions::DocumentRoot::refreshing(
        root, {.refreshInterval = std::chrono::milliseconds(1)});
    // An App-managed refreshing root needs the blocking pool that performs
    // directory scans off the event loop.
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    ruvia::BlockingPool pool(ruvia::BlockingPoolOptions{.threadCount = 1});
    options.blockingPool = &pool;
    options.documentRoot = HttpServerOptions::DocumentRoot::refreshing(
        root, {.refreshInterval = std::chrono::milliseconds::zero()});
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.documentRoot = HttpServerOptions::DocumentRoot::refreshing(
        root, {.refreshInterval = std::chrono::milliseconds(1)});
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.documentRoot = HttpServerOptions::DocumentRoot::refreshing(
        root, {.refreshInterval = std::chrono::milliseconds(1)}, {.gzip = true, .minBytes = 0});
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    options.documentRoot = HttpServerOptions::DocumentRoot::refreshing(root,
        {.refreshInterval = std::chrono::milliseconds(1)},
        {.gzip = true, .minBytes = 1024, .maxBytes = 512});
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));

    fs::remove_all(dir);
}

RUVIA_TEST(validate_server_options_rejects_configured_nonpositive_timeout) {
    // Every connection timeout feeds the same positive optional fold. Each one bounds
    // how long a slow client can hold a connection (a slowloris defense), so a
    // nonpositive value in ANY of them must be rejected -- checking only idle_timeout
    // would miss a field dropped from the fold call.
    using std::chrono::milliseconds;
    {
        HttpServerOptions options;
        options.idle_timeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.request_header_timeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.request_body_timeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.write_timeout = milliseconds(0);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.idle_timeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.request_header_timeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.request_body_timeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.write_timeout = milliseconds(-1);
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
}

RUVIA_TEST(validate_server_options_rejects_nonpositive_limits) {
    {
        HttpServerOptions options;
        options.worker_queue_capacity = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.rateLimitCapacityPerWorker = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.rateLimitCapacityPerWorker = 3;
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
        options.max_requests_per_connection = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.max_stream_body_bytes = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.max_buffered_body_bytes = 0;
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerOptions(options); }));
    }
    {
        HttpServerOptions options;
        options.max_web_socket_message_bytes = 0;
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
    auto missing = makeListener(HttpServerListenerDefinition::Tls{});
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(missing); }));

    // With both files present it is accepted.
    HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = "cert.pem";
    tls.identity.privateKeyFile = "key.pem";
    auto configured = makeListener(std::move(tls));
    RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerListener(configured); }));
}

RUVIA_TEST(http3_early_data_is_opt_in_and_rejects_client_certificate_policy) {
    const auto validTls = [] {
        HttpServerListenerDefinition::Tls tls;
        tls.identity.certificateChainFile = "cert.pem";
        tls.identity.privateKeyFile = "key.pem";
        return tls;
    };
    {
        auto tls = validTls();
        RUVIA_CHECK(!tls.http3_early_data);
        auto listener = makeListener(std::move(tls));
        RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerListener(listener); }));
    }
    {
        auto tls = validTls();
        tls.http3_early_data = true;
        auto listener = makeListener(std::move(tls));
        RUVIA_CHECK(!throwsInvalid([&] { validateHttpServerListener(listener); }));
    }
    {
        auto tls = validTls();
        tls.http3_early_data = true;
        tls.clientCertificates.emplace(std::pmr::get_default_resource(),
            ruvia::TlsClientCertificateRequirement::kOptional);
        auto listener = makeListener(std::move(tls));
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(listener); }));
    }
}

RUVIA_TEST(validate_server_options_enforces_nested_tls_material) {
    const auto validTls = [] {
        HttpServerListenerDefinition::Tls tls;
        tls.identity.certificateChainFile = "cert.pem";
        tls.identity.privateKeyFile = "key.pem";
        return tls;
    };

    {
        auto tls = validTls();
        tls.clientCertificates.emplace(
            std::pmr::get_default_resource(), ruvia::TlsClientCertificateRequirement::kOptional);
        auto listener = makeListener(std::move(tls));
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(listener); }));
    }
    {
        auto tls = validTls();
        auto& sni = tls.sniIdentities.emplace_back();
        sni.identity.certificateChainFile = "sni-cert.pem";
        sni.identity.privateKeyFile = "sni-key.pem";
        auto listener = makeListener(std::move(tls));
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(listener); }));
    }
    {
        auto tls = validTls();
        auto& sni = tls.sniIdentities.emplace_back();
        sni.host = "example.com:443";
        sni.identity.certificateChainFile = "sni-cert.pem";
        sni.identity.privateKeyFile = "sni-key.pem";
        auto listener = makeListener(std::move(tls));
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(listener); }));
    }
    {
        auto tls = validTls();
        auto& sni = tls.sniIdentities.emplace_back();
        sni.host = "example.com";
        auto listener = makeListener(std::move(tls));
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(listener); }));
    }
    {
        auto tls = validTls();
        for (const auto* host : {"Example.com", "example.COM"}) {
            auto& sni = tls.sniIdentities.emplace_back();
            sni.host = host;
            sni.identity.certificateChainFile = "sni-cert.pem";
            sni.identity.privateKeyFile = "sni-key.pem";
        }
        auto listener = makeListener(std::move(tls));
        RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(listener); }));
    }
}

RUVIA_TEST(validated_server_configuration_requires_complete_validation) {
    RUVIA_CHECK(throwsInvalid([] { (void)validateHttpServerConfiguration({}, {}); }));
    const std::array listeners{
        makeListener(HttpServerListenerDefinition::PlainHttp{}),
    };

    HttpServerOptions invalidOptions;
    invalidOptions.max_buffered_body_bytes = 0;
    RUVIA_CHECK(throwsInvalid([&listeners, &invalidOptions] {
        (void)validateHttpServerConfiguration(listeners, std::move(invalidOptions));
    }));

    const auto configuration = validateHttpServerConfiguration(listeners, HttpServerOptions{});
    RUVIA_CHECK_EQ(configuration.listeners().size(), std::size_t{1});
    RUVIA_CHECK(configuration.options().max_buffered_body_bytes > 0);
}

RUVIA_TEST(web_internal_config_defaults_use_the_process_resource) {
    auto* const expected = ruvia::detail::processResource();

    const ruvia::detail::CorsOptions cors;
    RUVIA_CHECK(cors.origin.get_allocator().resource() == expected);
    RUVIA_CHECK(cors.requestHeaders.get_allocator().resource() == expected);
    RUVIA_CHECK(cors.exposeHeaders.get_allocator().resource() == expected);

    const HttpServerListenerDefinition::Tls tls;
    RUVIA_CHECK(tls.identity.certificateChainFile.get_allocator().resource() == expected);
    RUVIA_CHECK(tls.identity.privateKeyFile.get_allocator().resource() == expected);
    RUVIA_CHECK(tls.identity.privateKeyPassword.get_allocator().resource() == expected);
    RUVIA_CHECK(tls.sniIdentities.get_allocator().resource() == expected);

    const HttpServerListenerDefinition::TlsClientCertificatePolicy clientCertificates;
    RUVIA_CHECK(clientCertificates.verifyFile.get_allocator().resource() == expected);

    const HttpServerListenerDefinition::Tls::SniIdentity sni;
    RUVIA_CHECK(sni.host.get_allocator().resource() == expected);
    RUVIA_CHECK(sni.identity.certificateChainFile.get_allocator().resource() == expected);
}

RUVIA_TEST(validate_server_options_requires_redirect_https_port) {
    auto listener = makeListener(HttpServerListenerDefinition::RedirectHttpToHttps{0});
    RUVIA_CHECK(throwsInvalid([&] { validateHttpServerListener(listener); }));
}

RUVIA_TEST(listener_config_rejects_invalid_listener_and_tls_states_at_construction) {
    RUVIA_CHECK(throwsInvalid([] { ruvia::app().listen({}); }));
    RUVIA_CHECK(throwsInvalid([] { ruvia::app().listen({.address = {}, .http = 8080}); }));
    RUVIA_CHECK(throwsInvalid([] { ruvia::app().listen({.address = "localhost", .http = 8080}); }));
    RUVIA_CHECK(throwsInvalid([] { ruvia::app().listen({.address = std::string("127.0.0.1\0bad", 13), .http = 8080}); }));
    RUVIA_CHECK(throwsInvalid([] { ruvia::app().listen({.address = "127.0.0.1", .http = 0}); }));
    RUVIA_CHECK(throwsInvalid(
        [] { ruvia::app().listen({.address = "127.0.0.1", .http = 8080, .https = 8080}); }));
    RUVIA_CHECK(throwsInvalid([] {
        ruvia::app().listen({.address = "127.0.0.1", .http = 8080, .autoHttpsRedirect = true});
    }));
    RUVIA_CHECK(
        throwsInvalid([] { ruvia::app().listen({.address = "127.0.0.1", .https = 8443}); }));
    RUVIA_CHECK(throwsInvalid([] {
        ruvia::app().listen({
            .address = "127.0.0.1",
            .https = 8443,
            .tls = {.certificateChainFile = "cert.pem"},
        });
    }));
    RUVIA_CHECK(throwsInvalid([] {
        ruvia::app().listen({
            .address = "127.0.0.1",
            .http = 8080,
            .tls = {.certificateChainFile = "cert.pem", .privateKeyFile = "key.pem"},
        });
    }));
    RUVIA_CHECK(throwsInvalid([] {
        ruvia::app().listen({
            .address = "127.0.0.1",
            .https = 8443,
            .tls =
                {
                    .certificateChainFile = "cert.pem",
                    .privateKeyFile = "key.pem",
                    .clientCertificates =
                        {
                            .requirement = ruvia::TlsClientCertificateRequirement::kRequired,
                        },
                },
        });
    }));
    RUVIA_CHECK(throwsInvalid([] {
        ruvia::app().server(
            {.process_signal_handlers = static_cast<ruvia::process_signal_handler_policy>(0xFF)});
    }));
}

RUVIA_TEST(tls_config_rejects_empty_or_duplicate_sni_identity) {
    const auto rejects = [](std::vector<ruvia::TlsSniConfig> sni) {
        return throwsInvalid([&] {
            ruvia::app().listen({
                .address = "127.0.0.1",
                .https = 8443,
                .tls =
                    {
                        .certificateChainFile = "cert.pem",
                        .privateKeyFile = "key.pem",
                        .sni = std::move(sni),
                    },
            });
        });
    };
    RUVIA_CHECK(rejects(
        {{.host = {}, .certificateChainFile = "other.pem", .privateKeyFile = "other.key"}}));
    RUVIA_CHECK(rejects({{.host = "example.com:443",
        .certificateChainFile = "other.pem",
        .privateKeyFile = "other.key"}}));
    RUVIA_CHECK(rejects({{.host = "127.0.0.1",
        .certificateChainFile = "other.pem",
        .privateKeyFile = "other.key"}}));
    RUVIA_CHECK(rejects({
        {.host = "Example.com", .certificateChainFile = "other.pem", .privateKeyFile = "other.key"},
        {.host = "example.COM", .certificateChainFile = "third.pem", .privateKeyFile = "third.key"},
    }));
}

RUVIA_TEST(tls_identity_owns_password_independently_of_caller_resource) {
    ReleasableMemoryResource callerResource;
    const std::string expected(80, 's');
    std::optional<ruvia::TlsConfig> identity;
    {
        const std::pmr::string password(expected, &callerResource);
        identity.emplace(
            tlsConfig("cert.pem", "key.pem", std::string(password.data(), password.size())));
    }
    callerResource.release();
    RUVIA_CHECK_EQ(identity->privateKeyPassword, std::string_view(expected));
    identity.reset();
    RUVIA_CHECK(!callerResource.deallocatedAfterRelease());
}

RUVIA_TEST(self_contained_app_callbacks_release_owned_state) {
    std::weak_ptr<int> accessState;
    {
        auto state = std::make_shared<int>(1);
        accessState = state;
        ruvia::AccessLogCallback callback(
            [state](const ruvia::AccessLogRecord&) noexcept { (void)state; });
        // Copy ownership is the behavior under test.
        const auto copy = callback;  // NOLINT(performance-unnecessary-copy-initialization)
        state.reset();
        RUVIA_CHECK(!accessState.expired());
        (void)copy;
    }
    RUVIA_CHECK(accessState.expired());

    std::weak_ptr<int> failureState;
    {
        auto state = std::make_shared<int>(1);
        failureState = state;
        ruvia::ConnectionFailureCallback callback(
            [state](const ruvia::ConnectionFailureRecord&) noexcept { (void)state; });
        state.reset();
        RUVIA_CHECK(!failureState.expired());
    }
    RUVIA_CHECK(failureState.expired());

    std::weak_ptr<int> errorState;
    {
        auto state = std::make_shared<int>(1);
        errorState = state;
        ruvia::HttpErrorHandler callback(
            [state](ruvia::Context&, ruvia::HttpErrorInfo) -> ruvia::Task<ruvia::HttpResponse> {
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
        ruvia::HttpNotFoundHandler callback(
            [state](ruvia::Context&) -> ruvia::Task<ruvia::HttpResponse> {
                (void)state;
                co_return ruvia::HttpResponse{};
            });
        state.reset();
        RUVIA_CHECK(!notFoundState.expired());
    }
    RUVIA_CHECK(notFoundState.expired());
}
