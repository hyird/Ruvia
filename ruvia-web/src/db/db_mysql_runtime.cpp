#include "db/db_mysql_runtime.h"

#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/web/db/db_types.h"

namespace ruvia::detail {
namespace {

class mysql_library_env final {
public:
    mysql_library_env() {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw db_error(db_error::code_type::connect_failed, db_driver::mariadb,
                "failed to initialize the MariaDB client library");
        }
    }

    ~mysql_library_env() {
        mysql_library_end();
    }
};

class mysql_thread_env final {
public:
    mysql_thread_env() {
        if (mysql_thread_init() != 0) {
            throw db_error(db_error::code_type::connect_failed, db_driver::mariadb,
                "failed to initialize the MariaDB client thread");
        }
    }

    ~mysql_thread_env() {
        mysql_thread_end();
    }
};

[[nodiscard]] unsigned int timeout_seconds(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        return 0;
    }

    const auto seconds = std::chrono::ceil<std::chrono::seconds>(timeout).count();
    return std::in_range<unsigned int>(seconds) ? static_cast<unsigned int>(seconds)
                                                : std::numeric_limits<unsigned int>::max();
}

}  // namespace

void ensure_mysql_thread_initialized() {
    static mysql_library_env library_env;
    static thread_local mysql_thread_env thread_env;
    (void)library_env;
    (void)thread_env;
}

bool set_mysql_timeout(st_mysql& connection, mysql_option option,
    std::optional<std::chrono::milliseconds> timeout) noexcept {
    if (!timeout.has_value()) {
        return true;
    }
    const auto seconds = timeout_seconds(*timeout);
    return mysql_optionsv(&connection, option, &seconds) == 0;
}

mysql_wait_deadline select_mysql_wait_deadline(std::optional<std::chrono::milliseconds> operation_timeout_value,
    std::optional<std::chrono::milliseconds> driver_timeout) noexcept {
    if (operation_timeout_value && driver_timeout) {
        if (*operation_timeout_value <= *driver_timeout) {
            return {*operation_timeout_value, mysql_wait_deadline_source::operation};
        }
        return {*driver_timeout, mysql_wait_deadline_source::driver};
    }
    if (operation_timeout_value) {
        return {*operation_timeout_value, mysql_wait_deadline_source::operation};
    }
    if (driver_timeout) {
        return {*driver_timeout, mysql_wait_deadline_source::driver};
    }
    return {};
}

}  // namespace ruvia::detail
