#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/ModelTypes.h"

namespace {

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t liveAllocations() const noexcept {
        return liveAllocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* const storage = upstream_->allocate(bytes, alignment);
        ++liveAllocations_;
        return storage;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
        --liveAllocations_;
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_{std::pmr::get_default_resource()};
    std::size_t liveAllocations_{0};
};

}  // namespace

static_assert(!std::is_copy_constructible_v<ruvia::String>);
static_assert(!std::is_copy_assignable_v<ruvia::String>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::String>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::String>);

RUVIA_TEST(model_string_public_construction_owns_input) {
    CountingMemoryResource resource;
    std::string input(128, 'a');
    ruvia::String value(input, &resource);
    input.assign(input.size(), 'b');
    const std::string expected(128, 'a');

    RUVIA_CHECK_EQ(value.view(), std::string_view(expected));
    RUVIA_CHECK_EQ(value.resource(), &resource);
    RUVIA_CHECK(resource.liveAllocations() > 0);
}

RUVIA_TEST(model_string_parser_factory_can_borrow_input) {
    CountingMemoryResource resource;
    const std::string input(128, 'c');
    const auto value = ruvia::detail::ModelValueFactory::makeString(
        input,
        &resource);

    RUVIA_CHECK_EQ(value.view(), std::string_view(input));
    RUVIA_CHECK_EQ(value.data(), input.data());
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(model_string_owned_assignment_is_alias_safe) {
    CountingMemoryResource resource;
    ruvia::String value(std::string(128, 'd'), &resource);
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
        ruvia::String source(sourceText, &sourceResource);
        ruvia::String target(targetText, &targetResource);

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
