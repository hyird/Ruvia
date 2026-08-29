#include "test_harness.h"
#include "memory_resource_fixture.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/ModelTypes.h"

namespace {

using ruvia::test::CountingMemoryResource;

}  // namespace

template <typename T>
concept ExposesAnyRvalueModelStringBorrow =
    requires { std::declval<const T&&>().view(); } || requires {
        std::declval<const T&&>().data();
    } || requires { static_cast<std::string_view>(std::declval<const T&&>()); };

template <typename T>
concept ExposesRvalueFixedStringView = requires { std::declval<const T&&>().view(); };

static_assert(!std::is_copy_constructible_v<ruvia::String>);
static_assert(!std::is_copy_assignable_v<ruvia::String>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::String>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::String>);
static_assert(!ExposesAnyRvalueModelStringBorrow<ruvia::String>);
static_assert(!ExposesRvalueFixedStringView<ruvia::FixedString<6>>);

RUVIA_TEST(model_string_public_construction_owns_input) {
    CountingMemoryResource resource;
    std::string input(128, 'a');
    ruvia::String value(input, {.resource = &resource});
    input.assign(input.size(), 'b');
    const std::string expected(128, 'a');

    RUVIA_CHECK_EQ(value.view(), std::string_view(expected));
    RUVIA_CHECK_EQ(value.resource(), &resource);
    RUVIA_CHECK(resource.liveAllocations() > 0);
}

RUVIA_TEST(model_string_parser_factory_can_borrow_input) {
    CountingMemoryResource resource;
    const std::string input(128, 'c');
    const auto value = ruvia::detail::ModelValueFactory::makeString(input, &resource);

    RUVIA_CHECK_EQ(value.view(), std::string_view(input));
    RUVIA_CHECK_EQ(value.data(), input.data());
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(model_string_owned_assignment_is_alias_safe) {
    CountingMemoryResource resource;
    ruvia::String value(std::string(128, 'd'), {.resource = &resource});
    const auto alias = value.view().substr(31, 64);

    value.assignOwned(alias);
    const std::string expected(64, 'd');

    RUVIA_CHECK_EQ(value.view(), std::string_view(expected));
    RUVIA_CHECK_EQ(value.resource(), &resource);
}

RUVIA_TEST(model_string_move_assignment_transfers_resource) {
    CountingMemoryResource sourceResource;
    CountingMemoryResource targetResource;
    {
        const std::string sourceText(128, 's');
        const std::string targetText(128, 't');
        ruvia::String source(sourceText, {.resource = &sourceResource});
        ruvia::String target(targetText, {.resource = &targetResource});

        target = std::move(source);
        RUVIA_CHECK_EQ(target.resource(), &sourceResource);
        RUVIA_CHECK_EQ(target.view(), std::string_view(sourceText));
        RUVIA_CHECK_EQ(targetResource.liveAllocations(), std::size_t{0});

        source.assignOwned(std::string(128, 'm'));
        const std::string movedFromText(128, 'm');
        RUVIA_CHECK_EQ(source.resource(), &sourceResource);
        RUVIA_CHECK_EQ(source.view(), std::string_view(movedFromText));
    }

    RUVIA_CHECK_EQ(sourceResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(targetResource.liveAllocations(), std::size_t{0});
}
