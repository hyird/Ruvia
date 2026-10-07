#include <cstddef>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/ModelObject.h"
#include "ruvia/web/ModelTypes.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {

template <typename value_type>
value_type borrow_text(std::string_view text, std::pmr::memory_resource* resource) {
    if constexpr (std::is_same_v<value_type, ruvia::String>) {
        return ruvia::detail::ModelValueFactory::makeString(text, resource);
    } else {
        return std::move(*value_type::parse(text, {.resource = resource}));
    }
}

template <typename value_type>
void verify_text_ownership(ruvia::testing::TestContext& ruvia_ctx) {
    failing_memory_resource source_resource;
    failing_memory_resource target_resource;
    const std::string original = "{\"text\":\"" + std::string(128, 'x') + "\"}";
    {
        auto input = original;
        auto borrowed = borrow_text<value_type>(input, &source_resource);
        auto moved_borrow = std::move(borrowed);
        RUVIA_CHECK(moved_borrow.view().data() == input.data());
        value_type target({.resource = &target_resource});
        target = std::move(moved_borrow);
        RUVIA_CHECK(target.resource() == &target_resource);
        RUVIA_CHECK(target.view().data() != input.data());
        input[9] = 'y';
        RUVIA_CHECK_EQ(target.view(), std::string_view(original));

        const auto* target_bytes = target.view().data();
        auto& self = target;
        target = std::move(self);
        RUVIA_CHECK(target.view().data() == target_bytes);

        const auto const_source = ruvia::detail::rebindModelValue(target, &target_resource);
        const auto* const_bytes = const_source.view().data();
        auto const_copy = ruvia::detail::rebindModelValue(std::move(const_source), &target_resource);
        RUVIA_CHECK_EQ(const_copy.view(), std::string_view(original));
        RUVIA_CHECK(const_copy.view().data() != const_bytes);
        RUVIA_CHECK(const_source.view().data() == const_bytes);

        auto owned = ruvia::detail::rebindModelValue(target, &target_resource);
        const auto* owned_bytes = owned.view().data();
        auto transferred = ruvia::detail::rebindModelValue(std::move(owned), &target_resource);
        RUVIA_CHECK(transferred.view().data() == owned_bytes);
        target = std::move(transferred);
        RUVIA_CHECK(target.view().data() == owned_bytes);

        // A borrowed facade may refer into the destination's current owned
        // token. Prepare its copy before publishing and releasing that token.
        auto aliased = borrow_text<value_type>(target.view(), &target_resource);
        target_resource.fail_after(0);
        bool alias_failed = false;
        try {
            target = std::move(aliased);
        } catch (const std::bad_alloc&) {
            alias_failed = true;
        }
        target_resource.allow_allocations();
        RUVIA_CHECK(alias_failed);
        RUVIA_CHECK(target.view().data() == owned_bytes);
        RUVIA_CHECK_EQ(aliased.view(), std::string_view(original));
        target = std::move(aliased);
        RUVIA_CHECK(target.view().data() != owned_bytes);
        RUVIA_CHECK_EQ(target.view(), std::string_view(original));

        auto foreign = ruvia::detail::rebindModelValue(target, &source_resource);
        const auto* foreign_bytes = foreign.view().data();
        const auto* old_target_bytes = target.view().data();
        target_resource.fail_after(0);
        bool failed = false;
        try {
            target = std::move(foreign);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        target_resource.allow_allocations();
        RUVIA_CHECK(failed);
        RUVIA_CHECK(target.view().data() == old_target_bytes);
        RUVIA_CHECK_EQ(target.view(), std::string_view(original));
        RUVIA_CHECK(foreign.view().data() == foreign_bytes);
        RUVIA_CHECK_EQ(foreign.view(), std::string_view(original));
        target = std::move(foreign);
        RUVIA_CHECK(target.resource() == &target_resource);
        RUVIA_CHECK(target.view().data() != foreign_bytes);
        RUVIA_CHECK_EQ(target.view(), std::string_view(original));
        RUVIA_CHECK_EQ(foreign.view(), std::string_view(original));
    }
    RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
}

}  // namespace

RUVIA_TEST(model_text_facades_share_borrow_clone_transfer_and_failed_assignment_rules) {
    verify_text_ownership<ruvia::String>(ruvia_ctx);
    verify_text_ownership<ruvia::JsonValue>(ruvia_ctx);
    verify_text_ownership<ruvia::JsonObject>(ruvia_ctx);
}
