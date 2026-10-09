#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <utility>

#include "ruvia/web/model_types.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::test::counting_memory_resource;

class tracked_value final {
public:
    explicit tracked_value(int value) noexcept
        : value_(value) {
        ++alive_;
    }

    tracked_value(const tracked_value&) = delete;
    tracked_value& operator=(const tracked_value&) = delete;

    tracked_value(tracked_value&& other) noexcept
        : value_(other.value_) {
        other.value_ = -1;
        ++alive_;
    }

    tracked_value& operator=(tracked_value&&) = delete;

    ~tracked_value() {
        --alive_;
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    [[nodiscard]] static std::size_t alive() noexcept {
        return alive_;
    }

private:
    int value_;
    static inline std::size_t alive_{0};
};

}  // namespace

RUVIA_TEST(model_list_clear_and_destructor_release_owned_elements) {
    counting_memory_resource resource;
    {
        ruvia::boxed_array<tracked_value> values({.resource_ = &resource});
        values.emplace(1);
        values.emplace(2);
        RUVIA_CHECK_EQ(tracked_value::alive(), std::size_t{2});

        values.clear();
        RUVIA_CHECK(values.empty());
        RUVIA_CHECK_EQ(tracked_value::alive(), std::size_t{0});

        values.emplace(3);
        RUVIA_CHECK_EQ(tracked_value::alive(), std::size_t{1});
    }

    RUVIA_CHECK_EQ(tracked_value::alive(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(model_list_move_assignment_keeps_element_resource_owner) {
    counting_memory_resource source_resource;
    counting_memory_resource target_resource;
    {
        ruvia::boxed_array<tracked_value> source_value({.resource_ = &source_resource});
        source_value.emplace(4);
        source_value.emplace(5);

        ruvia::boxed_array<tracked_value> target({.resource_ = &target_resource});
        target.emplace(9);
        RUVIA_CHECK_EQ(tracked_value::alive(), std::size_t{3});

        target = std::move(source_value);
        RUVIA_CHECK_EQ(target.resource(), &target_resource);
        RUVIA_CHECK_EQ(source_value.resource(), &source_resource);
        RUVIA_CHECK_EQ(target.size(), std::size_t{2});
        RUVIA_CHECK_EQ(target[0].value(), 4);
        RUVIA_CHECK_EQ(target[1].value(), 5);
        RUVIA_CHECK(source_value.empty());
        RUVIA_CHECK_EQ(tracked_value::alive(), std::size_t{2});
        RUVIA_CHECK(target_resource.live_allocations() > 0);

        source_value.emplace(6);
        RUVIA_CHECK_EQ(source_value.front().value(), 6);
        RUVIA_CHECK_EQ(tracked_value::alive(), std::size_t{3});
    }

    RUVIA_CHECK_EQ(tracked_value::alive(), std::size_t{0});
    RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(source_resource.allocation_count(), source_resource.deallocation_count());
    RUVIA_CHECK_EQ(target_resource.allocation_count(), target_resource.deallocation_count());
}

RUVIA_TEST(model_boxed_array_supports_standard_range_algorithms) {
    ruvia::boxed_array<tracked_value> values;
    RUVIA_CHECK(std::ranges::find_if(values, [](const auto&) { return true; }) == values.end());
    RUVIA_CHECK_EQ(std::ranges::distance(values), std::ptrdiff_t{0});
    values.emplace(1);
    values.emplace(2);
    values.emplace(3);

    const auto found = std::ranges::find_if(values, [](const auto& value) { return value.value() == 2; });
    RUVIA_CHECK(found != values.end());
    if (found != values.end()) {
        RUVIA_CHECK(&*found == &values[1]);
    }
    RUVIA_CHECK_EQ(std::ranges::distance(values), std::ptrdiff_t{3});
    const auto& constant_values = std::as_const(values);
    RUVIA_CHECK_EQ(std::ranges::count_if(constant_values,
                       [](const auto& value) { return value.value() % 2 != 0; }),
        std::ptrdiff_t{2});
    RUVIA_CHECK(std::ranges::find_if(constant_values,
                    [](const auto& value) { return value.value() == 4; }) == constant_values.end());
}
