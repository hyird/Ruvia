#include "test_harness.h"

#include <chrono>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/App.h"
#include "ruvia/core/detail/config/ConfigValidation.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"

namespace {

using ruvia::detail::ensureConfigHost;
using ruvia::detail::ensureNonZeroPort;
using ruvia::detail::ensurePositiveDuration;
using ruvia::detail::ensurePositiveSize;
using ruvia::detail::isValidConfigHost;
using ruvia::detail::isValidSniHost;
using ruvia::detail::kSeparatedPortHostRules;

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
    RUVIA_CHECK_EQ(caughtMessage([] { ensureConfigHost("", "was-empty", "was-invalid"); }), std::string("was-empty"));
    RUVIA_CHECK_EQ(caughtMessage([] { ensureConfigHost("bad host", "was-empty", "was-invalid"); }), std::string("was-invalid"));
    RUVIA_CHECK(caughtMessage([] { ensureConfigHost("example.com", "was-empty", "was-invalid"); }).empty());
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
    RUVIA_CHECK(throwsInvalid([&disabledRefresh] { ruvia::app().documentRoot(std::move(disabledRefresh)); }));

}

RUVIA_TEST(app_compression_rejects_invalid_thresholds_at_configuration) {
    RUVIA_CHECK(throwsInvalid([] {
        ruvia::app().compression({.minBytes = 1024, .syncBytes = 512});
    }));
    RUVIA_CHECK(throwsInvalid([] {
        ruvia::app().compression({.minBytes = 1024, .syncBytes = 2048, .maxBytes = 1024});
    }));
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
