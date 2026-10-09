#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
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
    RUVIA_CHECK_EQ(value.resource(), &resource);
    RUVIA_CHECK(resource.live_allocations() > 0);
}

RUVIA_TEST(model_string_parser_factory_can_borrow_input) {
    counting_memory_resource resource;
    const std::string input(128, 'c');
    const auto value = ruvia::detail::model_value_factory::make_string(input, &resource);

    RUVIA_CHECK_EQ(value.view(), std::string_view(input));
    RUVIA_CHECK_EQ(value.data(), input.data());
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(model_string_owned_assignment_is_alias_safe) {
    counting_memory_resource resource;
    ruvia::string value(std::string(128, 'd'), {.resource_ = &resource});
    const auto alias = value.view().substr(31, 64);

    value.assign_owned(alias);
    const std::string expected(64, 'd');

    RUVIA_CHECK_EQ(value.view(), std::string_view(expected));
    RUVIA_CHECK_EQ(value.resource(), &resource);
}

RUVIA_TEST(model_string_move_assignment_keeps_target_resource) {
    counting_memory_resource source_resource;
    counting_memory_resource target_resource;
    {
        const std::string source_text(128, 's');
        const std::string target_text(128, 't');
        ruvia::string source_value(source_text, {.resource_ = &source_resource});
        ruvia::string target(target_text, {.resource_ = &target_resource});

        target = std::move(source_value);
        RUVIA_CHECK_EQ(target.resource(), &target_resource);
        RUVIA_CHECK_EQ(target.view(), std::string_view(source_text));
        RUVIA_CHECK(target_resource.live_allocations() > 0);
        RUVIA_CHECK_EQ(source_value.resource(), &source_resource);
        RUVIA_CHECK_EQ(source_value.view(), std::string_view(source_text));

        source_value.assign_owned(std::string(128, 'm'));
        const std::string moved_from_text(128, 'm');
        RUVIA_CHECK_EQ(source_value.resource(), &source_resource);
        RUVIA_CHECK_EQ(source_value.view(), std::string_view(moved_from_text));
    }

    RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(model_array_public_insertion_owns_borrowed_string) {
    counting_memory_resource resource;
    std::string input(128, 'a');
    const std::string expected(128, 'a');
    const auto borrowed = ruvia::detail::model_value_factory::make_string(input, &resource);

    ruvia::array<ruvia::string> values({.resource_ = &resource});
    values.emplace_back(borrowed);
    values.emplace_back("direct");

    RUVIA_CHECK(values[0].data() != input.data());
    RUVIA_CHECK_EQ(values[0].view(), std::string_view(expected));
    RUVIA_CHECK_EQ(values[1].view(), std::string_view("direct"));
    RUVIA_CHECK_EQ(values[0].resource(), &resource);
    RUVIA_CHECK_EQ(values[1].resource(), &resource);

    input.assign(input.size(), 'b');
    RUVIA_CHECK_EQ(values[0].view(), std::string_view(expected));
}
