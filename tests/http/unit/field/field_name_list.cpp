#include <string_view>

#include "ruvia/http/HttpFieldNameList.h"

#include "test_harness.h"

RUVIA_TEST(http_field_name_list_preserves_fields_and_skips_empty_members) {
    ruvia::http_field_name_list names(" , X-One,\tX-Two\t, ");
    RUVIA_CHECK_EQ(names.next().value(), std::string_view("X-One"));
    RUVIA_CHECK_EQ(names.next().value(), std::string_view("X-Two"));
    RUVIA_CHECK(!names.next());
    RUVIA_CHECK(names.valid());
}

RUVIA_TEST(http_field_name_list_borrows_input_and_keeps_terminal_state) {
    constexpr std::string_view field = "\tX-One , X-Two";
    ruvia::http_field_name_list names(field);
    RUVIA_CHECK(!names.valid());
    const auto first = names.next();
    RUVIA_CHECK(first.has_value());
    RUVIA_CHECK_EQ(first->data(), field.data() + 1);
    RUVIA_CHECK(!names.valid());
    const auto second = names.next();
    RUVIA_CHECK(second.has_value());
    RUVIA_CHECK_EQ(second->data(), field.data() + 9);
    RUVIA_CHECK(names.valid());
    RUVIA_CHECK(!names.next());
    RUVIA_CHECK(!names.next());
    RUVIA_CHECK(names.valid());
}

RUVIA_TEST(http_field_name_list_accepts_empty_lists_and_rejects_invalid_names) {
    ruvia::http_field_name_list empty(" , \t, ");
    RUVIA_CHECK(!empty.next());
    RUVIA_CHECK(empty.valid());

    ruvia::http_field_name_list invalid("X-Ok, bad name");
    RUVIA_CHECK_EQ(invalid.next().value(), std::string_view("X-Ok"));
    RUVIA_CHECK(!invalid.next());
    RUVIA_CHECK(!invalid.valid());
    RUVIA_CHECK(!invalid.next());
    RUVIA_CHECK(!invalid.valid());
}
