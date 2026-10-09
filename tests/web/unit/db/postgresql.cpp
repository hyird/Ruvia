#include <array>
#include <cmath>
#include <limits>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "db/db_config_validation.h"
#include "db/db_migration_validation.h"
#include "db/db_postgresql.h"
#include "test_harness.h"

namespace {

template <typename f_type>
bool throws_invalid_argument(f_type&& function) {
    try {
        function();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

class rejecting_default_resource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t, std::size_t) override {
        throw std::bad_alloc();
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {}

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class default_resource_guard final {
public:
    explicit default_resource_guard(std::pmr::memory_resource* resource) noexcept
        : previous_(std::pmr::set_default_resource(resource)) {}

    ~default_resource_guard() {
        std::pmr::set_default_resource(previous_);
    }

    default_resource_guard(const default_resource_guard&) = delete;
    default_resource_guard& operator=(const default_resource_guard&) = delete;

private:
    std::pmr::memory_resource* previous_;
};

}  // namespace

RUVIA_TEST(postgresql_config_selects_default_port_when_omitted) {
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
    RUVIA_CHECK(config.driver_ == ruvia::db_driver::postgresql);
    RUVIA_CHECK(!config.port_.has_value());
    RUVIA_CHECK_EQ(ruvia::detail::configured_db_port(config), std::uint16_t{5432});
    RUVIA_CHECK(!throws_invalid_argument([&] { ruvia::detail::validate_db_config(config); }));
}

RUVIA_TEST(postgresql_parameter_encoding_preserves_types_and_null) {
    const std::array<ruvia::db_value, 6> params{
        ruvia::db_value{nullptr},
        ruvia::db_value{"hello"},
        ruvia::db_value{-42},
        ruvia::db_value{std::uint64_t{99}},
        ruvia::db_value{1.25},
        ruvia::db_value{true},
    };
    auto encoded = ruvia::detail::encode_postgresql_params(
        std::span<const ruvia::db_value>(params), std::pmr::get_default_resource());
    RUVIA_CHECK(encoded.values_.size() == params.size());
    RUVIA_CHECK(encoded.lengths_.size() == params.size());
    RUVIA_CHECK(encoded.values_[0] == nullptr);
    RUVIA_CHECK(encoded.lengths_[0] == 0);
    RUVIA_CHECK(std::string_view(encoded.values_[1]) == "hello");
    RUVIA_CHECK(encoded.lengths_[1] == 5);
    RUVIA_CHECK(std::string_view(encoded.values_[2]) == "-42");
    RUVIA_CHECK(encoded.lengths_[2] == 3);
    RUVIA_CHECK(std::string_view(encoded.values_[3]) == "99");
    RUVIA_CHECK(encoded.lengths_[3] == 2);
    RUVIA_CHECK(std::string_view(encoded.values_[4]) == "1.25");
    RUVIA_CHECK(encoded.lengths_[4] == 4);
    RUVIA_CHECK(std::string_view(encoded.values_[5]) == "true");
    RUVIA_CHECK(encoded.lengths_[5] == 4);
}

RUVIA_TEST(postgresql_parameter_encoding_rejects_embedded_nul_text_params) {
    const std::string text("a\0b", 3);
    const std::array<ruvia::db_value, 1> params{
        ruvia::db_value{std::string_view(text.data(), text.size())}};
    RUVIA_CHECK(throws_invalid_argument([&] {
        (void)ruvia::detail::encode_postgresql_params(
            std::span<const ruvia::db_value>(params), std::pmr::get_default_resource());
    }));
}

RUVIA_TEST(postgresql_parameter_encoding_rejects_non_finite_double) {
    const std::array<ruvia::db_value, 1> params{
        ruvia::db_value{std::numeric_limits<double>::infinity()}};
    RUVIA_CHECK(throws_invalid_argument([&] {
        (void)ruvia::detail::encode_postgresql_params(
            std::span<const ruvia::db_value>(params), std::pmr::get_default_resource());
    }));
}

RUVIA_TEST(postgresql_parameter_encoding_uses_explicit_memory_resource) {
    std::pmr::unsynchronized_pool_resource explicit_resource;
    const std::string text(128, 'x');
    const std::array<ruvia::db_value, 2> params{
        ruvia::db_value{std::string_view(text)},
        ruvia::db_value{std::uint64_t{18446744073709551615ULL}},
    };

    rejecting_default_resource rejecting_default;
    bool used_default_resource = false;
    {
        default_resource_guard guard_value(&rejecting_default);
        try {
            auto encoded = ruvia::detail::encode_postgresql_params(
                std::span<const ruvia::db_value>(params), &explicit_resource);
            RUVIA_CHECK(encoded.values_.size() == params.size());
            RUVIA_CHECK(std::string_view(encoded.values_[0]) == text);
            RUVIA_CHECK(std::string_view(encoded.values_[1]) == "18446744073709551615");
        } catch (const std::bad_alloc&) {
            used_default_resource = true;
        }
    }
    RUVIA_CHECK(!used_default_resource);
}

RUVIA_TEST(postgresql_migration_identifier_uses_63_byte_limit) {
    using ruvia::detail::is_valid_migration_table_name;
    constexpr auto driver = ruvia::db_driver::postgresql;
    RUVIA_CHECK(is_valid_migration_table_name(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
    RUVIA_CHECK(!is_valid_migration_table_name(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
}
