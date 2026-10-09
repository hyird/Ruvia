#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/web/redis/redis_entity.h"
#include "ruvia/web/redis/redis_find_options.h"

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
        auto filter = redis_query_user::field<"email">() == text;
        text.assign(200, 'y');
        auto range = redis_query_user::field<"age">().between(18, 65);
        ruvia::redis_find_options options{.where_ = std::move(filter) && range};
        RUVIA_CHECK(filter.empty());
        RUVIA_CHECK(!options.where_.empty());
        RUVIA_CHECK(!range.empty());
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}
