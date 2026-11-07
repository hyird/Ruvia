#include "ruvia/core/failure_report.h"

#include <gtest/gtest.h>

#include <exception>
#include <stdexcept>
#include <string>

TEST(Ruvia, equivalent_failures_compare_type_and_message) {
    const auto first = std::make_exception_ptr(std::runtime_error("redis connect timed out"));
    const auto same = std::make_exception_ptr(std::runtime_error("redis connect timed out"));
    // Same length, different bytes: a dangling what() comparison could see the
    // same freed storage for both and call them equal.
    const auto other = std::make_exception_ptr(std::runtime_error("redis connect timed in!"));
    const auto other_type = std::make_exception_ptr(std::logic_error("redis connect timed out"));
    const auto non_standard = std::make_exception_ptr(42);

    EXPECT_TRUE(ruvia::equivalent_failures(first, first));
    EXPECT_TRUE(ruvia::equivalent_failures(first, same));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, other));
    EXPECT_TRUE(!ruvia::equivalent_failures(other, first));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, other_type));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, non_standard));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, nullptr));
    EXPECT_TRUE(ruvia::equivalent_failures(non_standard, non_standard));
}

TEST(Ruvia, equivalent_failures_distinguish_long_messages) {
    // Messages too long for any small-buffer storage, differing only at the end.
    const std::string prefix(512, 'x');
    const auto left = std::make_exception_ptr(std::runtime_error(prefix + "a"));
    const auto right = std::make_exception_ptr(std::runtime_error(prefix + "b"));
    EXPECT_TRUE(!ruvia::equivalent_failures(left, right));
    EXPECT_TRUE(ruvia::equivalent_failures(left, std::make_exception_ptr(std::runtime_error(prefix + "a"))));
}
