#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <utility>

#include "ruvia/web/model.h"

namespace {

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
    {
        ruvia::boxed_array<tracked_value> values;
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
}

TEST(Ruvia, model_list_move_assignment_keeps_element_resource_owner) {
    {
        ruvia::boxed_array<tracked_value> source_value;
        source_value.emplace_back(4);
        source_value.emplace_back(5);

        ruvia::boxed_array<tracked_value> target;
        target.emplace_back(9);
        EXPECT_EQ(tracked_value::alive(), std::size_t{3});

        target = std::move(source_value);
        EXPECT_EQ(target.size(), std::size_t{2});
        EXPECT_EQ(target[0].value(), 4);
        EXPECT_EQ(target[1].value(), 5);
        EXPECT_TRUE(source_value.empty());
        EXPECT_EQ(tracked_value::alive(), std::size_t{2});

        source_value.emplace_back(6);
        EXPECT_EQ(source_value.front().value(), 6);
        EXPECT_EQ(tracked_value::alive(), std::size_t{3});
    }

    EXPECT_EQ(tracked_value::alive(), std::size_t{0});
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
