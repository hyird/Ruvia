#include "DbMysqlRuntime.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <limits>

namespace ruvia::detail {
namespace {

void configureMariaDbTlsWorkaround() noexcept {
#if defined(_WIN32)
    (void)_putenv_s("MARIADB_TLS_DISABLE_PEER_VERIFICATION", "1");
#else
    (void)setenv("MARIADB_TLS_DISABLE_PEER_VERIFICATION", "1", 1);
#endif
}

class MysqlLibraryEnv final {
public:
    MysqlLibraryEnv() {
        configureMariaDbTlsWorkaround();
        (void)mysql_library_init(0, nullptr, nullptr);
    }

    ~MysqlLibraryEnv() {
        mysql_library_end();
    }
};

class MysqlThreadEnv final {
public:
    MysqlThreadEnv() {
        (void)mysql_thread_init();
    }

    ~MysqlThreadEnv() {
        mysql_thread_end();
    }
};

[[nodiscard]] unsigned int timeoutSeconds(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        return 0;
    }

    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        timeout + std::chrono::milliseconds(999));
    return static_cast<unsigned int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(std::max<std::int64_t>(1, seconds.count())),
        std::numeric_limits<unsigned int>::max()));
}

}  // namespace

void ensureMysqlThreadInitialized() {
    static MysqlLibraryEnv libraryEnv;
    static thread_local MysqlThreadEnv threadEnv;
    (void)libraryEnv;
    (void)threadEnv;
}

void setMysqlTimeout(st_mysql& connection, mysql_option option, std::chrono::milliseconds timeout) noexcept {
    const auto seconds = timeoutSeconds(timeout);
    if (seconds == 0) {
        return;
    }
    (void)mysql_options(&connection, option, &seconds);
}

}  // namespace ruvia::detail
