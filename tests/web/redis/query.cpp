#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/web/redis/redis_repository.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

namespace {
RUVIA_REDIS_ENTITY(redis_query_user, "query-user",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(email, ruvia::string),
    RUVIA_REDIS_COLUMN(age, std::int32_t));
}

TEST(Ruvia, redis_predicate_composition_moves_source_and_preserves_range) {
    ruvia::test::counting_memory_resource resource;
    struct default_resource_guard {
        std::pmr::memory_resource* previous_;
        ~default_resource_guard() {
            std::pmr::set_default_resource(previous_);
        }
    } guard_value{std::pmr::set_default_resource(&resource)};
    {
        std::string text(200, 'x');
        auto filter = redis_query_user::column<"email">() == text;
        text.assign(200, 'y');
        auto range = redis_query_user::column<"age">().between(18, 65);
        ruvia::redis_find_options options{.where_ = std::move(filter) && range};
        EXPECT_TRUE(filter.empty());
        EXPECT_TRUE(!options.where_.empty());
        EXPECT_TRUE(!range.empty());
    }
    EXPECT_EQ(resource.allocation_count(), std::size_t{0});
}

TEST(Ruvia, redis_predicate_supports_copy_and_where_or_where_combination) {
    ruvia::test::counting_memory_resource resource;
    struct default_resource_guard {
        std::pmr::memory_resource* previous_;
        ~default_resource_guard() {
            std::pmr::set_default_resource(previous_);
        }
    } guard_value{std::pmr::set_default_resource(&resource)};
    {
        auto base = redis_query_user::column<"email">() == "alice@example.com";
        auto copied = base;
        EXPECT_TRUE(!base.empty());
        EXPECT_TRUE(!copied.empty());

        ruvia::redis_predicate combined;
        combined.and_where(copied);
        combined.or_where(redis_query_user::column<"age">() >= 21);
        EXPECT_TRUE(!combined.empty());
    }
    EXPECT_EQ(resource.allocation_count(), std::size_t{0});
}

namespace {
RUVIA_REDIS_ENTITY(redis_unsigned_pk_entity, "unsigned-pk",
    RUVIA_REDIS_COLUMN(id, std::uint64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string));

RUVIA_REDIS_ENTITY(redis_signed_pk_entity, "signed-pk",
    RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string));
}  // namespace

TEST(Ruvia, redis_predicate_holds_signed_and_unsigned_primary_key_comparisons) {
    const auto unsigned_eq = redis_unsigned_pk_entity::column<"id">() == std::uint64_t{42};
    const auto signed_eq = redis_signed_pk_entity::column<"id">() == std::int64_t{-7};
    EXPECT_TRUE(!unsigned_eq.empty());
    EXPECT_TRUE(!signed_eq.empty());
}

TEST(Ruvia, redis_predicate_self_combination_preserves_operands) {
    for (const bool use_or : {false, true}) {
        auto predicate = redis_signed_pk_entity::column<"id">() == std::int64_t{1};
        if (use_or) {
            predicate.or_where(predicate);
        } else {
            predicate.and_where(predicate);
        }
        EXPECT_TRUE(!predicate.empty());
    }
}

TEST(Ruvia, redis_predicate_negation_and_entity_column_name) {
    static_assert(redis_query_user::column_name<"email">() == "email");
    const auto pred = !(redis_query_user::column<"email">() == "alice@example.com");
    EXPECT_TRUE(!pred.empty());
    const ruvia::redis_predicate empty;
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)!empty; }));
}
