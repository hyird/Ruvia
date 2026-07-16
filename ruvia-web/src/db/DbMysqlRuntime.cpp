#include "ruvia/web/detail/db/DbMysqlRuntime.h"

#include <cstdlib>
#include <limits>
#include <utility>

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

    const auto seconds = std::chrono::ceil<std::chrono::seconds>(timeout).count();
    return std::in_range<unsigned int>(seconds)
        ? static_cast<unsigned int>(seconds)
        : std::numeric_limits<unsigned int>::max();
}

}  // namespace

void ensureMysqlThreadInitialized() {
    static MysqlLibraryEnv libraryEnv;
    static thread_local MysqlThreadEnv threadEnv;
    (void)libraryEnv;
    (void)threadEnv;
}

void setMysqlTimeout(
    st_mysql& connection,
    mysql_option option,
    std::optional<std::chrono::milliseconds> timeout) noexcept {
    if (!timeout.has_value()) {
        return;
    }
    const auto seconds = timeoutSeconds(*timeout);
    (void)mysql_options(&connection, option, &seconds);
}

}  // namespace ruvia::detail
