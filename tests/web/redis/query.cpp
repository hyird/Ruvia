#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/web/redis/redis_repository.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
RUVIA_REDIS_ENTITY(redis_query_user, "query-user",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(email, ruvia::string),
    RUVIA_REDIS_COLUMN(age, std::int32_t));
}

RUVIA_TEST(redis_predicate_composition_moves_source_and_preserves_range) {
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
        RUVIA_CHECK(filter.empty());
        RUVIA_CHECK(!options.where_.empty());
        RUVIA_CHECK(!range.empty());
    }
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
}

RUVIA_TEST(redis_predicate_supports_copy_and_where_or_where_combination) {
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
        RUVIA_CHECK(!base.empty());
        RUVIA_CHECK(!copied.empty());

        ruvia::redis_predicate combined;
        combined.and_where(copied);
        combined.or_where(redis_query_user::column<"age">() >= 21);
        RUVIA_CHECK(!combined.empty());
    }
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
}

namespace {
RUVIA_REDIS_ENTITY(redis_unsigned_pk_entity, "unsigned-pk",
    RUVIA_REDIS_COLUMN(id, std::uint64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string));

RUVIA_REDIS_ENTITY(redis_signed_pk_entity, "signed-pk",
    RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string));
}  // namespace

RUVIA_TEST(redis_predicate_holds_signed_and_unsigned_primary_key_comparisons) {
    const auto unsigned_eq = redis_unsigned_pk_entity::column<"id">() == std::uint64_t{42};
    const auto signed_eq = redis_signed_pk_entity::column<"id">() == std::int64_t{-7};
    RUVIA_CHECK(!unsigned_eq.empty());
    RUVIA_CHECK(!signed_eq.empty());
}

RUVIA_TEST(redis_predicate_self_combination_preserves_operands) {
    for (const bool use_or : {false, true}) {
        auto predicate = redis_signed_pk_entity::column<"id">() == std::int64_t{1};
        if (use_or) {
            predicate.or_where(predicate);
        } else {
            predicate.and_where(predicate);
        }
        RUVIA_CHECK(!predicate.empty());
    }
}
