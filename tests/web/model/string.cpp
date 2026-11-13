#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"

TEST(Ruvia, model_string_public_construction_owns_input) {
    std::string input(128, 'a');
    ruvia::string value(input);
    input.assign(input.size(), 'b');
    const std::string expected(128, 'a');

    EXPECT_EQ(value.view(), std::string_view(expected));
}

TEST(Ruvia, model_string_owned_assignment_is_alias_safe) {
    ruvia::string value(std::string(128, 'd'));
    const auto alias = value.view().substr(31, 64);

    value.assign_owned(alias);
    const std::string expected(64, 'd');

    EXPECT_EQ(value.view(), std::string_view(expected));
}

TEST(Ruvia, model_string_move_assignment_preserves_values_and_reclaims_storage) {
    const std::string source_text(128, 's');
    const std::string target_text(128, 't');
    ruvia::string source_value(source_text);
    ruvia::string target(target_text);

    target = std::move(source_value);
    EXPECT_EQ(target.view(), std::string_view(source_text));

    source_value.assign_owned(std::string(128, 'm'));
    const std::string moved_from_text(128, 'm');
    EXPECT_EQ(source_value.view(), std::string_view(moved_from_text));
}

TEST(Ruvia, model_string_assign_owned_rvalue_steals_storage) {
    const std::string expected(200, 'a');
    std::string input(expected);
    const char* const original_data = input.data();
    ruvia::string target;
    target.assign_owned(std::move(input));
    EXPECT_EQ(target.data(), original_data);
    EXPECT_EQ(target.view(), std::string_view(expected));
}
