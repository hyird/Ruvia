#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model_types.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::test::counting_memory_resource;

}  // namespace

RUVIA_TEST(model_string_public_construction_owns_input) {
    counting_memory_resource resource;
    std::string input(128, 'a');
    ruvia::string value(input, {.resource_ = &resource});
    input.assign(input.size(), 'b');
    const std::string expected(128, 'a');

    RUVIA_CHECK_EQ(value.view(), std::string_view(expected));
    RUVIA_CHECK(resource.live_allocations() > 0);
}

RUVIA_TEST(model_string_owned_assignment_is_alias_safe) {
    counting_memory_resource resource;
    ruvia::string value(std::string(128, 'd'), {.resource_ = &resource});
    const auto alias = value.view().substr(31, 64);

    value.assign_owned(alias);
    const std::string expected(64, 'd');

    RUVIA_CHECK_EQ(value.view(), std::string_view(expected));
}

RUVIA_TEST(model_string_move_assignment_preserves_values_and_reclaims_storage) {
    counting_memory_resource source_resource;
    counting_memory_resource target_resource;
    {
        const std::string source_text(128, 's');
        const std::string target_text(128, 't');
        ruvia::string source_value(source_text, {.resource_ = &source_resource});
        ruvia::string target(target_text, {.resource_ = &target_resource});

        target = std::move(source_value);
        RUVIA_CHECK_EQ(target.view(), std::string_view(source_text));
        RUVIA_CHECK(target_resource.live_allocations() > 0);

        source_value.assign_owned(std::string(128, 'm'));
        const std::string moved_from_text(128, 'm');
        RUVIA_CHECK_EQ(source_value.view(), std::string_view(moved_from_text));
    }

    RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
}
