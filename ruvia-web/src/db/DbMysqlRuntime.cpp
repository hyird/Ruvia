#include "ruvia/web/detail/db/DbMysqlRuntime.h"

#include "ruvia/web/db/DbTypes.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {
namespace {

class MysqlLibraryEnv final {
public:
    MysqlLibraryEnv() {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw DbError(DbError::Code::kConnectFailed, DbDriver::kMariaDb,
                "failed to initialize the MariaDB client library");
        }
    }

    ~MysqlLibraryEnv() {
        mysql_library_end();
    }
};

class MysqlThreadEnv final {
public:
    MysqlThreadEnv() {
        if (mysql_thread_init() != 0) {
            throw DbError(DbError::Code::kConnectFailed, DbDriver::kMariaDb,
                "failed to initialize the MariaDB client thread");
        }
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
    return std::in_range<unsigned int>(seconds) ? static_cast<unsigned int>(seconds)
                                                : std::numeric_limits<unsigned int>::max();
}

}  // namespace

void ensureMysqlThreadInitialized() {
    static MysqlLibraryEnv libraryEnv;
    static thread_local MysqlThreadEnv threadEnv;
    (void)libraryEnv;
    (void)threadEnv;
}

bool setMysqlTimeout(st_mysql& connection, mysql_option option,
    std::optional<std::chrono::milliseconds> timeout) noexcept {
    if (!timeout.has_value()) {
        return true;
    }
    const auto seconds = timeoutSeconds(*timeout);
    return mysql_optionsv(&connection, option, &seconds) == 0;
}

MysqlWaitDeadline selectMysqlWaitDeadline(std::optional<std::chrono::milliseconds> operationTimeout,
    std::optional<std::chrono::milliseconds> driverTimeout) noexcept {
    if (operationTimeout && driverTimeout) {
        if (*operationTimeout <= *driverTimeout) {
            return {*operationTimeout, MysqlWaitDeadlineSource::kOperation};
        }
        return {*driverTimeout, MysqlWaitDeadlineSource::kDriver};
    }
    if (operationTimeout) {
        return {*operationTimeout, MysqlWaitDeadlineSource::kOperation};
    }
    if (driverTimeout) {
        return {*driverTimeout, MysqlWaitDeadlineSource::kDriver};
    }
    return {};
}

}  // namespace ruvia::detail
