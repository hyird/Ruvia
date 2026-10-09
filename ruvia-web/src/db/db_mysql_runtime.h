#pragma once

#include <mysql.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace ruvia::detail {

void ensure_mysql_thread_initialized();
[[nodiscard]] bool set_mysql_timeout(st_mysql& connection, mysql_option option,
    std::optional<std::chrono::milliseconds> timeout) noexcept;

enum class mysql_wait_deadline_source : std::uint8_t {
    none,
    operation,
    driver,
};

struct mysql_wait_deadline final {
    std::optional<std::chrono::milliseconds> timeout_;
    mysql_wait_deadline_source source_{mysql_wait_deadline_source::none};
};

[[nodiscard]] mysql_wait_deadline select_mysql_wait_deadline(
    std::optional<std::chrono::milliseconds> operation_timeout_value,
    std::optional<std::chrono::milliseconds> driver_timeout) noexcept;

}  // namespace ruvia::detail
