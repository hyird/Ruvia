#include <gtest/gtest.h>

#include <string_view>

#include "ruvia/http/http_field_values.h"

TEST(Ruvia, http_field_name_list_preserves_fields_and_skips_empty_members) {
    ruvia::http_field_name_list names(" , X-One,\tX-Two\t, ");
    EXPECT_EQ(names.next().value(), std::string_view("X-One"));
    EXPECT_EQ(names.next().value(), std::string_view("X-Two"));
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(names.valid());
}

TEST(Ruvia, http_field_name_list_borrows_input_and_keeps_terminal_state) {
    constexpr std::string_view field = "\tX-One , X-Two";
    ruvia::http_field_name_list names(field);
    EXPECT_TRUE(!names.valid());
    const auto first = names.next();
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(first->data(), field.data() + 1);
    EXPECT_TRUE(!names.valid());
    const auto second = names.next();
    EXPECT_TRUE(second.has_value());
    EXPECT_EQ(second->data(), field.data() + 9);
    EXPECT_TRUE(names.valid());
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(names.valid());
}

TEST(Ruvia, http_field_name_list_accepts_empty_lists_and_rejects_invalid_names) {
    ruvia::http_field_name_list empty(" , \t, ");
    EXPECT_TRUE(!empty.next());
    EXPECT_TRUE(empty.valid());

    ruvia::http_field_name_list invalid("X-Ok, bad name");
    EXPECT_EQ(invalid.next().value(), std::string_view("X-Ok"));
    EXPECT_TRUE(!invalid.next());
    EXPECT_TRUE(!invalid.valid());
    EXPECT_TRUE(!invalid.next());
    EXPECT_TRUE(!invalid.valid());
}
