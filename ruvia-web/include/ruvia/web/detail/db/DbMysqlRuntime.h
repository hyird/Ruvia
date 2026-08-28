#pragma once

#include <mysql/mysql.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace ruvia::detail {

void ensureMysqlThreadInitialized();
[[nodiscard]] bool setMysqlTimeout(st_mysql& connection, mysql_option option, std::optional<std::chrono::milliseconds> timeout) noexcept;

enum class MysqlWaitDeadlineSource : std::uint8_t {
    kNone,
    kOperation,
    kDriver,
};

struct MysqlWaitDeadline final {
    std::optional<std::chrono::milliseconds> timeout;
    MysqlWaitDeadlineSource source{MysqlWaitDeadlineSource::kNone};
};

[[nodiscard]] MysqlWaitDeadline selectMysqlWaitDeadline(std::optional<std::chrono::milliseconds> operationTimeout, std::optional<std::chrono::milliseconds> driverTimeout) noexcept;

}  // namespace ruvia::detail
