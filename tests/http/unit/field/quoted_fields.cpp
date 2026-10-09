#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_field_values.h"

#include "test_harness.h"

namespace {

RUVIA_TEST(quoted_field_members_keep_escaped_quotes_delimiters_and_empty_items) {
    constexpr std::string_view input = R"( first , "second,\"still quoted",, last, )";
    std::vector<std::string_view> items;
    ruvia::http_visit_comma_separated_quoted_field_items(input, [&](std::string_view item) {
        items.push_back(item);
        return true;
    });
    RUVIA_CHECK_EQ(items, (std::vector<std::string_view>{"first", R"("second,\"still quoted")", "", "last", ""}));
    RUVIA_CHECK_EQ(items.front().data(), input.data() + 1);
    int empty_visits{};
    ruvia::http_visit_comma_separated_quoted_field_items({}, [&](std::string_view item) {
        ++empty_visits;
        RUVIA_CHECK(item.empty());
        return true;
    });
    RUVIA_CHECK_EQ(empty_visits, 1);
}

RUVIA_TEST(quoted_field_parameters_preserve_quotes_and_skip_non_parameters) {
    constexpr std::string_view input = R"( text/plain ; name = "a;\"still quoted" ; ignored ; empty= ;=value; )";
    std::vector<std::pair<std::string_view, std::string_view>> parameters;
    ruvia::http_visit_semicolon_parameters_quoted_field(input, [&](std::string_view name, std::string_view value) {
        parameters.emplace_back(name, value);
        return true;
    });
    RUVIA_CHECK_EQ(parameters, (std::vector<std::pair<std::string_view, std::string_view>>{
                                   {"name", R"("a;\"still quoted")"}, {"empty", ""}, {"", "value"}}));
}

RUVIA_TEST(quoted_field_visitors_stop_without_visiting_remaining_segments) {
    std::vector<std::string_view> members;
    ruvia::http_visit_comma_separated_quoted_field_items("first,second,third", [&](std::string_view item) {
        members.push_back(item);
        return members.size() != 2;
    });
    RUVIA_CHECK_EQ(members, (std::vector<std::string_view>{"first", "second"}));
    std::vector<std::string_view> names;
    ruvia::http_visit_semicolon_parameters_quoted_field("ignored;a=1;b=2;c=3", [&](std::string_view name, std::string_view) {
        names.push_back(name);
        return false;
    });
    RUVIA_CHECK_EQ(names, (std::vector<std::string_view>{"a"}));
}

}  // namespace
