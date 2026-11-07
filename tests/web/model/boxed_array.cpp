#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <utility>

#include "ruvia/web/model.h"

#include "memory_resource_fixture.h"

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

TEST(Ruvia, model_list_clear_and_destructor_release_owned_elements) {
    counting_memory_resource resource;
    {
        ruvia::boxed_array<tracked_value> values({.resource_ = &resource});
        values.emplace_back(1);
        values.emplace_back(2);
        EXPECT_EQ(tracked_value::alive(), std::size_t{2});

        values.clear();
        EXPECT_TRUE(values.empty());
        EXPECT_EQ(tracked_value::alive(), std::size_t{0});

        values.emplace_back(3);
        EXPECT_EQ(tracked_value::alive(), std::size_t{1});
    }

    EXPECT_EQ(tracked_value::alive(), std::size_t{0});
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(resource.allocation_count(), resource.deallocation_count());
}

TEST(Ruvia, model_list_move_assignment_keeps_element_resource_owner) {
    counting_memory_resource source_resource;
    counting_memory_resource target_resource;
    {
        ruvia::boxed_array<tracked_value> source_value({.resource_ = &source_resource});
        source_value.emplace_back(4);
        source_value.emplace_back(5);

        ruvia::boxed_array<tracked_value> target({.resource_ = &target_resource});
        target.emplace_back(9);
        EXPECT_EQ(tracked_value::alive(), std::size_t{3});

        target = std::move(source_value);
        EXPECT_EQ(target.size(), std::size_t{2});
        EXPECT_EQ(target[0].value(), 4);
        EXPECT_EQ(target[1].value(), 5);
        EXPECT_TRUE(source_value.empty());
        EXPECT_EQ(tracked_value::alive(), std::size_t{2});
        EXPECT_TRUE(target_resource.live_allocations() > 0);

        source_value.emplace_back(6);
        EXPECT_EQ(source_value.front().value(), 6);
        EXPECT_EQ(tracked_value::alive(), std::size_t{3});
    }

    EXPECT_EQ(tracked_value::alive(), std::size_t{0});
    EXPECT_EQ(source_resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(target_resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(source_resource.allocation_count(), source_resource.deallocation_count());
    EXPECT_EQ(target_resource.allocation_count(), target_resource.deallocation_count());
}

TEST(Ruvia, model_boxed_array_supports_standard_range_algorithms) {
    ruvia::boxed_array<tracked_value> values;
    EXPECT_TRUE(std::ranges::find_if(values, [](const auto&) { return true; }) == values.end());
    EXPECT_EQ(std::ranges::distance(values), std::ptrdiff_t{0});
    values.emplace_back(1);
    values.emplace_back(2);
    values.emplace_back(3);

    const auto found = std::ranges::find_if(values, [](const auto& value) { return value.value() == 2; });
    EXPECT_TRUE(found != values.end());
    if (found != values.end()) {
        EXPECT_TRUE(&*found == &values[1]);
    }
    EXPECT_EQ(std::ranges::distance(values), std::ptrdiff_t{3});
    const auto& constant_values = std::as_const(values);
    EXPECT_EQ(std::ranges::count_if(constant_values,
                  [](const auto& value) { return value.value() % 2 != 0; }),
        std::ptrdiff_t{2});
    EXPECT_TRUE(std::ranges::find_if(constant_values,
                    [](const auto& value) { return value.value() == 4; }) == constant_values.end());
}
