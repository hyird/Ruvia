#include "ruvia/core/failure_report.h"

#include <exception>
#include <stdexcept>
#include <string>

#include "test_harness.h"

RUVIA_TEST(equivalent_failures_compare_type_and_message) {
    const auto first = std::make_exception_ptr(std::runtime_error("redis connect timed out"));
    const auto same = std::make_exception_ptr(std::runtime_error("redis connect timed out"));
    // Same length, different bytes: a dangling what() comparison could see the
    // same freed storage for both and call them equal.
    const auto other = std::make_exception_ptr(std::runtime_error("redis connect timed in!"));
    const auto other_type = std::make_exception_ptr(std::logic_error("redis connect timed out"));
    const auto non_standard = std::make_exception_ptr(42);

    RUVIA_CHECK(ruvia::equivalent_failures(first, first));
    RUVIA_CHECK(ruvia::equivalent_failures(first, same));
    RUVIA_CHECK(!ruvia::equivalent_failures(first, other));
    RUVIA_CHECK(!ruvia::equivalent_failures(other, first));
    RUVIA_CHECK(!ruvia::equivalent_failures(first, other_type));
    RUVIA_CHECK(!ruvia::equivalent_failures(first, non_standard));
    RUVIA_CHECK(!ruvia::equivalent_failures(first, nullptr));
    RUVIA_CHECK(ruvia::equivalent_failures(non_standard, non_standard));
}

RUVIA_TEST(equivalent_failures_distinguish_long_messages) {
    // Messages too long for any small-buffer storage, differing only at the end.
    const std::string prefix(512, 'x');
    const auto left = std::make_exception_ptr(std::runtime_error(prefix + "a"));
    const auto right = std::make_exception_ptr(std::runtime_error(prefix + "b"));
    RUVIA_CHECK(!ruvia::equivalent_failures(left, right));
    RUVIA_CHECK(ruvia::equivalent_failures(left, std::make_exception_ptr(std::runtime_error(prefix + "a"))));
}
