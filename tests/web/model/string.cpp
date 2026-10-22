#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"

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

RUVIA_TEST(model_string_assign_owned_pmr_rvalue_rebinds_equivalent_resources) {
    counting_memory_resource upstream;
    {
        ruvia::test::equivalent_tracking_resource source(upstream);
        ruvia::test::equivalent_tracking_resource target(upstream);
        RUVIA_CHECK(source.is_equal(target));

        const std::string expected(200, 'a');
        std::optional<ruvia::string> value(std::in_place, ruvia::model_options{.resource_ = &target});
        {
            std::pmr::string input(expected, &source);
            value->assign_owned(std::move(input));
        }
        source.retire();
        RUVIA_CHECK_EQ(value->view(), std::string_view(expected));
        value.reset();
        RUVIA_CHECK_EQ(source.callbacks_after_retirement(), std::size_t{0});

        const std::string same_expected(200, 'b');
        ruvia::string same_resource_target(ruvia::model_options{.resource_ = &target});
        std::pmr::string same_resource_input(same_expected, &target);
        const auto attempts = target.allocation_attempts();
        target.reject_allocations_after(0);
        same_resource_target.assign_owned(std::move(same_resource_input));
        target.allow_allocations();
        RUVIA_CHECK_EQ(target.allocation_attempts(), attempts);
        RUVIA_CHECK_EQ(same_resource_target.view(), std::string_view(same_expected));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
