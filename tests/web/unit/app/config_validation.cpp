#include "test_harness.h"

#include <chrono>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/detail/app/ConfigValidation.h"
#include "ruvia/web/detail/integration/DataAccessDefinitions.h"

namespace {

using ruvia::detail::ensureConfigHost;
using ruvia::detail::ensureNonZeroPort;
using ruvia::detail::ensurePositiveDuration;
using ruvia::detail::ensurePositiveSize;
using ruvia::detail::isValidConfigHost;
using ruvia::detail::kSeparatedPortHostRules;

class TrackingResource final : public std::pmr::memory_resource {
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

RUVIA_TEST(data_access_config_clones_nested_pmr_strings) {
    TrackingResource sourceResource;
    std::pmr::unsynchronized_pool_resource targetResource;
    std::optional<ruvia::DbConfig> database;
    std::optional<ruvia::RedisConfig> redis;
    {
        ruvia::DbConfig source{
            .driver = ruvia::DbDriver::kMariaDb,
            .host = std::pmr::string(80, 'h', &sourceResource),
            .port = 3306,
            .username = std::pmr::string(80, 'u', &sourceResource),
            .password = std::pmr::string(80, 'p', &sourceResource),
            .database = std::pmr::string(80, 'd', &sourceResource),
        };
        database.emplace(ruvia::detail::cloneDbConfig(source, &targetResource));
        RUVIA_CHECK(database->host.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(database->username.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(database->password.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(database->database.get_allocator().resource() == &targetResource);

        ruvia::RedisConfig sourceRedis{
            .host = std::pmr::string(80, 'r', &sourceResource),
            .port = 6379,
            .username = std::pmr::string(80, 'x', &sourceResource),
            .password = std::pmr::string(80, 'y', &sourceResource),
        };
        redis.emplace(ruvia::detail::cloneRedisConfig(sourceRedis, &targetResource));
        RUVIA_CHECK(redis->host.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(redis->username.get_allocator().resource() == &targetResource);
        RUVIA_CHECK(redis->password.get_allocator().resource() == &targetResource);
    }

    // The source configuration can now be destroyed/released independently;
    // long-lived app definitions must not retain its PMR allocator.
    sourceResource.release();
    RUVIA_CHECK(!sourceResource.deallocatedAfterRelease());
}
