#include "test_harness.h"

#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbMigrationValidation.h"
#include "ruvia/web/detail/db/DbPostgreSql.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
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

class RejectingDefaultResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t, std::size_t) override {
        throw std::bad_alloc();
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {}

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class DefaultResourceGuard final {
public:
    explicit DefaultResourceGuard(std::pmr::memory_resource* resource) noexcept
        : previous_(std::pmr::set_default_resource(resource)) {}

    ~DefaultResourceGuard() {
        std::pmr::set_default_resource(previous_);
    }

    DefaultResourceGuard(const DefaultResourceGuard&) = delete;
    DefaultResourceGuard& operator=(const DefaultResourceGuard&) = delete;

private:
    std::pmr::memory_resource* previous_;
};

}  // namespace

RUVIA_TEST(postgresql_config_selects_default_port_when_omitted) {
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
    RUVIA_CHECK(config.driver == ruvia::DbDriver::kPostgreSql);
    RUVIA_CHECK(!config.port.has_value());
    RUVIA_CHECK_EQ(ruvia::detail::configuredDbPort(config), std::uint16_t{5432});
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
    auto encoded = ruvia::detail::encodePostgreSqlParams(
        std::span<const ruvia::DbValue>(params), std::pmr::get_default_resource());
    RUVIA_CHECK(encoded.values.size() == params.size());
    RUVIA_CHECK(encoded.lengths.size() == params.size());
    RUVIA_CHECK(encoded.values[0] == nullptr);
    RUVIA_CHECK(encoded.lengths[0] == 0);
    RUVIA_CHECK(std::string_view(encoded.values[1]) == "hello");
    RUVIA_CHECK(encoded.lengths[1] == 5);
    RUVIA_CHECK(std::string_view(encoded.values[2]) == "-42");
    RUVIA_CHECK(encoded.lengths[2] == 3);
    RUVIA_CHECK(std::string_view(encoded.values[3]) == "99");
    RUVIA_CHECK(encoded.lengths[3] == 2);
    RUVIA_CHECK(std::string_view(encoded.values[4]) == "1.25");
    RUVIA_CHECK(encoded.lengths[4] == 4);
    RUVIA_CHECK(std::string_view(encoded.values[5]) == "true");
    RUVIA_CHECK(encoded.lengths[5] == 4);
}

RUVIA_TEST(postgresql_parameter_encoding_rejects_embedded_nul_text_params) {
    const std::string text("a\0b", 3);
    const std::array<ruvia::DbValue, 1> params{
        ruvia::DbValue{std::string_view(text.data(), text.size())}};
    RUVIA_CHECK(throwsInvalidArgument([&] {
        (void)ruvia::detail::encodePostgreSqlParams(
            std::span<const ruvia::DbValue>(params), std::pmr::get_default_resource());
    }));
}

RUVIA_TEST(postgresql_parameter_encoding_rejects_non_finite_double) {
    const std::array<ruvia::DbValue, 1> params{
        ruvia::DbValue{std::numeric_limits<double>::infinity()}};
    RUVIA_CHECK(throwsInvalidArgument([&] {
        (void)ruvia::detail::encodePostgreSqlParams(
            std::span<const ruvia::DbValue>(params), std::pmr::get_default_resource());
    }));
}

RUVIA_TEST(postgresql_parameter_encoding_uses_explicit_memory_resource) {
    std::pmr::unsynchronized_pool_resource explicitResource;
    const std::string text(128, 'x');
    const std::array<ruvia::DbValue, 2> params{
        ruvia::DbValue{std::string_view(text)},
        ruvia::DbValue{std::uint64_t{18446744073709551615ULL}},
    };

    RejectingDefaultResource rejectingDefault;
    bool usedDefaultResource = false;
    {
        DefaultResourceGuard guard(&rejectingDefault);
        try {
            auto encoded = ruvia::detail::encodePostgreSqlParams(
                std::span<const ruvia::DbValue>(params), &explicitResource);
            RUVIA_CHECK(encoded.values.size() == params.size());
            RUVIA_CHECK(std::string_view(encoded.values[0]) == text);
            RUVIA_CHECK(std::string_view(encoded.values[1]) == "18446744073709551615");
        } catch (const std::bad_alloc&) {
            usedDefaultResource = true;
        }
    }
    RUVIA_CHECK(!usedDefaultResource);
}

RUVIA_TEST(postgresql_migration_identifier_uses_63_byte_limit) {
    using ruvia::detail::isValidMigrationTableName;
    constexpr auto driver = ruvia::DbDriver::kPostgreSql;
    RUVIA_CHECK(isValidMigrationTableName(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
    RUVIA_CHECK(!isValidMigrationTableName(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
}
