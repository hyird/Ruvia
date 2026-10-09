#pragma once

#include <array>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/db/db_types.h"

namespace ruvia::detail {

// Both views refer to static SQL literals. The default transaction path keeps
// the same single control operation and allocations as an unconfigured BEGIN.
struct db_transaction_start_plan final {
    std::string_view configure_{};
    std::string_view begin_{};
};

[[nodiscard]] inline db_transaction_start_plan make_db_transaction_start_plan(
    db_driver driver, db_transaction_options options) {
    const auto isolation = static_cast<unsigned>(options.isolation_);
    const auto access = static_cast<unsigned>(options.access_mode_);
    if (isolation > static_cast<unsigned>(db_transaction_isolation::serializable) ||
        access > static_cast<unsigned>(db_transaction_access_mode::read_only)) {
        throw std::invalid_argument("unsupported transaction options");
    }
    if (driver == db_driver::postgresql) {
        static constexpr std::array<std::array<std::string_view, 3>, 5> commands{{
            {"BEGIN", "BEGIN READ WRITE", "BEGIN READ ONLY"},
            {"BEGIN ISOLATION LEVEL READ UNCOMMITTED", "BEGIN ISOLATION LEVEL READ UNCOMMITTED READ WRITE", "BEGIN ISOLATION LEVEL READ UNCOMMITTED READ ONLY"},
            {"BEGIN ISOLATION LEVEL READ COMMITTED", "BEGIN ISOLATION LEVEL READ COMMITTED READ WRITE", "BEGIN ISOLATION LEVEL READ COMMITTED READ ONLY"},
            {"BEGIN ISOLATION LEVEL REPEATABLE READ", "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY"},
            {"BEGIN ISOLATION LEVEL SERIALIZABLE", "BEGIN ISOLATION LEVEL SERIALIZABLE READ WRITE", "BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY"},
        }};
        return {.begin_ = commands[isolation][access]};
    }
    if (driver == db_driver::mariadb) {
        static constexpr std::array<std::string_view, 5> configure{
            "", "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED",
            "SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
            "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ",
            "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"};
        static constexpr std::array<std::string_view, 3> commands{
            "START TRANSACTION", "START TRANSACTION READ WRITE", "START TRANSACTION READ ONLY"};
        return {.configure_ = configure[isolation], .begin_ = commands[access]};
    }
    throw std::invalid_argument("transaction options require a database driver");
}

}  // namespace ruvia::detail
