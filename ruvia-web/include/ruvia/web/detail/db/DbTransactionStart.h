#pragma once

#include <array>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/db/DbTypes.h"

namespace ruvia::detail {

// Both views refer to static SQL literals. The default transaction path keeps
// the same single control operation and allocations as an unconfigured BEGIN.
struct DbTransactionStartPlan final {
    std::string_view configure{};
    std::string_view begin{};
};

[[nodiscard]] inline DbTransactionStartPlan makeDbTransactionStartPlan(
    DbDriver driver, DbTransactionOptions options) {
    const auto isolation = static_cast<unsigned>(options.isolation);
    const auto access = static_cast<unsigned>(options.accessMode);
    if (isolation > static_cast<unsigned>(DbTransactionIsolation::kSerializable) ||
        access > static_cast<unsigned>(DbTransactionAccessMode::kReadOnly)) {
        throw std::invalid_argument("unsupported transaction options");
    }
    if (driver == DbDriver::kPostgreSql) {
        static constexpr std::array<std::array<std::string_view, 3>, 5> commands{{
            {"BEGIN", "BEGIN READ WRITE", "BEGIN READ ONLY"},
            {"BEGIN ISOLATION LEVEL READ UNCOMMITTED", "BEGIN ISOLATION LEVEL READ UNCOMMITTED READ WRITE", "BEGIN ISOLATION LEVEL READ UNCOMMITTED READ ONLY"},
            {"BEGIN ISOLATION LEVEL READ COMMITTED", "BEGIN ISOLATION LEVEL READ COMMITTED READ WRITE", "BEGIN ISOLATION LEVEL READ COMMITTED READ ONLY"},
            {"BEGIN ISOLATION LEVEL REPEATABLE READ", "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY"},
            {"BEGIN ISOLATION LEVEL SERIALIZABLE", "BEGIN ISOLATION LEVEL SERIALIZABLE READ WRITE", "BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY"},
        }};
        return {.begin = commands[isolation][access]};
    }
    if (driver == DbDriver::kMariaDb) {
        static constexpr std::array<std::string_view, 5> configure{
            "", "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED",
            "SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
            "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ",
            "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"};
        static constexpr std::array<std::string_view, 3> commands{
            "START TRANSACTION", "START TRANSACTION READ WRITE", "START TRANSACTION READ ONLY"};
        return {.configure = configure[isolation], .begin = commands[access]};
    }
    throw std::invalid_argument("transaction options require a database driver");
}

}  // namespace ruvia::detail
