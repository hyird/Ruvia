#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/redis/redis_entity.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_REDIS_ENTITY(redis_user, "user",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, std::pmr::string),
    RUVIA_REDIS_COLUMN(active, bool),
    RUVIA_REDIS_COLUMN(age, std::int32_t, ruvia::redis_column_options{.nullable_ = true}),
    RUVIA_REDIS_COLUMN(score, ruvia::double_value, ruvia::redis_column_options{.nullable_ = true}));

RUVIA_REDIS_ENTITY(nullable_redis_text, "text_value",
    RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(text, ruvia::string, ruvia::redis_column_options{.nullable_ = true}));

}  // namespace

RUVIA_TEST(redis_entity_tracks_unset_value_and_null_states) {
    ruvia::test::counting_memory_resource resource;
    redis_user user_value(&resource);

    RUVIA_CHECK_EQ(user_value.resource(), &resource);
    RUVIA_CHECK_EQ(redis_user::prefix(), std::string_view("user"));
    RUVIA_CHECK(!user_value.is_set<"id">());
    RUVIA_CHECK(!user_value.is_null<"id">());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)user_value.get<"id">(); }));

    user_value.set<"id">("u-1");
    user_value.set<"name">("Alice");
    user_value.set<"active">(true);
    user_value.set<"age">(32);
    user_value.set<"score">(ruvia::double_value{4.5});
    RUVIA_CHECK(user_value.is_set<"id">());
    RUVIA_CHECK_EQ(user_value.get<"id">().view(), std::string_view("u-1"));
    RUVIA_CHECK_EQ(std::string_view(user_value.get<"name">()), std::string_view("Alice"));
    RUVIA_CHECK(user_value.get<"active">());
    RUVIA_CHECK_EQ(user_value.get<"age">(), 32);
    RUVIA_CHECK_EQ(static_cast<double>(user_value.get<"score">()), 4.5);

    user_value.set_null<"age">();
    RUVIA_CHECK(user_value.is_set<"age">());
    RUVIA_CHECK(user_value.is_null<"age">());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)user_value.get<"age">(); }));

    user_value.reset<"age">();
    RUVIA_CHECK(!user_value.is_set<"age">());
    RUVIA_CHECK(!user_value.is_null<"age">());
}

RUVIA_TEST(redis_entity_move_preserves_owned_values_and_resource) {
    ruvia::test::counting_memory_resource resource;
    {
        redis_user source_value(&resource);
        source_value.set<"id">(std::string(120, 'i'));
        source_value.set<"name">(std::string(120, 'n'));
        source_value.set<"active">(true);

        redis_user moved(std::move(source_value));
        RUVIA_CHECK_EQ(moved.resource(), &resource);
        RUVIA_CHECK_EQ(moved.get<"id">().size(), std::size_t{120});
        RUVIA_CHECK_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
        RUVIA_CHECK(moved.get<"active">());
        source_value.set<"id">("reused");
        source_value.set<"name">("new name");
        RUVIA_CHECK_EQ(moved.get<"id">().size(), std::size_t{120});
        RUVIA_CHECK_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(redis_entity_failed_owning_assignment_preserves_each_column_state) {
    const std::string replacement(256, 'x');
    for (int state_value = 0; state_value < 3; ++state_value) {
        ruvia::test::rejecting_memory_resource resource;
        nullable_redis_text entity(&resource);
        entity.set<"id">(7);
        if (state_value == 1) {
            entity.set_null<"text">();
        } else if (state_value == 2) {
            entity.set<"text">("retained");
        }
        resource.reject_allocations();
        RUVIA_CHECK(ruvia::testing::throws_on([&] { entity.set<"text">(replacement); }));
        resource.reject_allocations(false);
        RUVIA_CHECK_EQ(entity.is_set<"text">(), state_value != 0);
        RUVIA_CHECK_EQ(entity.is_null<"text">(), state_value == 1);
        RUVIA_CHECK_EQ(entity.get<"id">(), 7);
        if (state_value == 2) {
            RUVIA_CHECK_EQ(entity.get<"text">().view(), std::string_view("retained"));
            RUVIA_CHECK(entity.get<"text">().resource() == &resource);
        } else {
            RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)entity.get<"text">(); }));
        }
    }
}
