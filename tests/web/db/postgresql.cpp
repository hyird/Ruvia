#include "test_harness.h"

#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbMigrationValidation.h"
#include "ruvia/web/detail/db/DbPostgreSql.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

template <typename F>
bool throwsInvalidArgument(F&& function) {
    try {
        function();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(postgresql_config_factory_selects_driver_and_port) {
    const auto config = ruvia::DbConfig::postgreSql();
    RUVIA_CHECK(config.driver == ruvia::DbDriver::kPostgreSql);
    RUVIA_CHECK(config.port == 5432);
    RUVIA_CHECK(!throwsInvalidArgument([&] { ruvia::detail::validateDbConfig(config); }));
}

RUVIA_TEST(postgresql_parameter_encoding_preserves_types_and_null) {
    const std::array<ruvia::DbValue, 6> params{
        ruvia::DbValue{nullptr},
        ruvia::DbValue{"hello"},
        ruvia::DbValue{-42},
        ruvia::DbValue{std::uint64_t{99}},
        ruvia::DbValue{1.25},
        ruvia::DbValue{true},
    };
    auto encoded = ruvia::detail::encodePostgreSqlParams(std::span<const ruvia::DbValue>(params), std::pmr::get_default_resource());
    RUVIA_CHECK(encoded.values.size() == params.size());
    RUVIA_CHECK(encoded.values[0] == nullptr);
    RUVIA_CHECK(std::string_view(encoded.values[1]) == "hello");
    RUVIA_CHECK(std::string_view(encoded.values[2]) == "-42");
    RUVIA_CHECK(std::string_view(encoded.values[3]) == "99");
    RUVIA_CHECK(std::string_view(encoded.values[4]) == "1.25");
    RUVIA_CHECK(std::string_view(encoded.values[5]) == "true");
}

RUVIA_TEST(postgresql_parameter_encoding_rejects_non_finite_double) {
    const std::array<ruvia::DbValue, 1> params{ruvia::DbValue{std::numeric_limits<double>::infinity()}};
    RUVIA_CHECK(throwsInvalidArgument([&] { (void)ruvia::detail::encodePostgreSqlParams(std::span<const ruvia::DbValue>(params), std::pmr::get_default_resource()); }));
}

RUVIA_TEST(postgresql_migration_identifier_uses_63_byte_limit) {
    using ruvia::detail::isValidMigrationTableName;
    constexpr auto driver = ruvia::DbDriver::kPostgreSql;
    RUVIA_CHECK(isValidMigrationTableName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
    RUVIA_CHECK(!isValidMigrationTableName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
}
