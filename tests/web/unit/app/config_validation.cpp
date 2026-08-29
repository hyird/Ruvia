#include "test_harness.h"

#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/App.h"
#include "ruvia/core/detail/config/ConfigValidation.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/WebSocketClientConfigStorage.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"

namespace {

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocationCount_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocationCount_{0};
};

using ruvia::detail::ClientPortTextBuffer;
using ruvia::detail::clientTransportConfigView;
using ruvia::detail::ensureConfigHost;
using ruvia::detail::ensureNonZeroPort;
using ruvia::detail::ensurePositiveDuration;
using ruvia::detail::ensurePositiveSize;
using ruvia::detail::formatClientPort;
using ruvia::detail::isValidConfigHost;
using ruvia::detail::isValidSniHost;
using ruvia::detail::kSeparatedPortHostRules;
using ruvia::detail::validateClientOriginHost;
using ruvia::detail::validateClientTransportConfig;

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

}  // namespace

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
    RUVIA_CHECK(isValidConfigHost("::1", kSeparatedPortHostRules));  // two colons is not "single"
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
        config.host = std::string(80, 'h');
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
    RUVIA_CHECK(storage->subprotocolRanges.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->subprotocolHeader.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->userAgent.get_allocator().resource() == &resource);
    RUVIA_CHECK_EQ(std::string(storage->host), std::string(80, 'h'));
    RUVIA_CHECK_EQ(std::string(storage->target), "/" + std::string(80, 't'));
    RUVIA_CHECK_EQ(std::string(storage->headers.front().name), "X-Test");
    RUVIA_CHECK_EQ(std::string(storage->headers.front().value), std::string(80, 'v'));
    RUVIA_CHECK_EQ(storage->subprotocolRanges.size(), std::size_t{2});
    RUVIA_CHECK_EQ(std::string(storage->subprotocolHeader), "chat, superchat");
    RUVIA_CHECK(storage->offersSubprotocol("chat"));
    RUVIA_CHECK(storage->offersSubprotocol("superchat"));
    RUVIA_CHECK(!storage->offersSubprotocol("Chat"));
    RUVIA_CHECK(!storage->offersSubprotocol("super"));
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
        source.host = std::string(80, 'h');
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
    RUVIA_CHECK_EQ(std::string(database->host), std::string(80, 'h'));
#endif
    RUVIA_CHECK_EQ(std::string(redis->host), std::string(80, 'r'));
}
