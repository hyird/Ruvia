#include "test_harness.h"

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/detail/json/JsonObjectFields.h"

namespace {

using Field = std::pair<std::string, std::string>;

// Collect (decoded key, raw value) pairs; returns whether parsing succeeded.
bool collect(std::string_view body, std::vector<Field>& out) {
    return ruvia::detail::visitJsonObjectFields(
        body, std::pmr::get_default_resource(),
        [&out](std::string_view key, std::string_view value) {
            out.emplace_back(std::string(key), std::string(value));
        });
}

}  // namespace

RUVIA_TEST(json_object_fields_basic) {
    std::vector<Field> fields;
    RUVIA_CHECK(collect(R"({"a":1,"b":"x","c":true,"d":null})", fields));
    RUVIA_CHECK_EQ(fields.size(), std::size_t{4});
    RUVIA_CHECK_EQ(fields[0].first, std::string("a"));
    RUVIA_CHECK_EQ(fields[0].second, std::string("1"));
    RUVIA_CHECK_EQ(fields[1].first, std::string("b"));
    RUVIA_CHECK_EQ(fields[1].second, std::string(R"("x")"));  // raw value keeps the quotes
    RUVIA_CHECK_EQ(fields[2].second, std::string("true"));
    RUVIA_CHECK_EQ(fields[3].second, std::string("null"));
}

RUVIA_TEST(json_object_fields_emits_duplicate_keys_in_order) {
    // Duplicate keys are a JSON parser-differential surface. The iterator emits
    // every occurrence in source order -- it neither dedups nor rejects -- so the
    // binding layer's last-write-wins is well defined and reviewable.
    std::vector<Field> fields;
    RUVIA_CHECK(collect(R"({"a":1,"b":2,"a":3})", fields));
    RUVIA_CHECK_EQ(fields.size(), std::size_t{3});
    RUVIA_CHECK_EQ(fields[0].first, std::string("a"));
    RUVIA_CHECK_EQ(fields[0].second, std::string("1"));
    RUVIA_CHECK_EQ(fields[2].first, std::string("a"));
    RUVIA_CHECK_EQ(fields[2].second, std::string("3"));
}

RUVIA_TEST(json_object_fields_empty_object) {
    std::vector<Field> fields;
    RUVIA_CHECK(collect("{}", fields));
    RUVIA_CHECK(fields.empty());
}

RUVIA_TEST(json_object_fields_captures_nested_values) {
    std::vector<Field> fields;
    RUVIA_CHECK(collect(R"({"obj":{"x":1},"arr":[1,2,3]})", fields));
    RUVIA_CHECK_EQ(fields.size(), std::size_t{2});
    RUVIA_CHECK_EQ(fields[0].second, std::string(R"({"x":1})"));   // whole nested object
    RUVIA_CHECK_EQ(fields[1].second, std::string("[1,2,3]"));      // whole array
}

RUVIA_TEST(json_object_fields_decodes_escaped_key) {
    std::vector<Field> fields;
    // The escaped key "ab" must be decoded to "ab" before it reaches the visitor.
    RUVIA_CHECK(collect(R"({"a\u0062":1})", fields));
    RUVIA_CHECK_EQ(fields.size(), std::size_t{1});
    RUVIA_CHECK_EQ(fields[0].first, std::string("ab"));
}

RUVIA_TEST(json_object_fields_rejects_malformed) {
    std::vector<Field> fields;
    RUVIA_CHECK(!collect("", fields));            // not an object
    RUVIA_CHECK(!collect("[]", fields));          // array, not object
    RUVIA_CHECK(!collect("{", fields));           // unterminated
    RUVIA_CHECK(!collect(R"({"a"})", fields));    // missing ':'
    RUVIA_CHECK(!collect(R"({"a":})", fields));   // missing value
    RUVIA_CHECK(!collect(R"({"a":1,})", fields)); // trailing comma -> expects another key
    RUVIA_CHECK(!collect(R"({1:2})", fields));    // non-string key (keys must be strings)
    RUVIA_CHECK(!collect(R"({"a":1 "b":2})", fields)); // missing ',' between fields
}

RUVIA_TEST(json_object_fields_visitor_can_stop_early) {
    int visited = 0;
    const bool ok = ruvia::detail::visitJsonObjectFields(
        R"({"a":1,"b":2,"c":3})", std::pmr::get_default_resource(),
        [&visited](std::string_view, std::string_view) {
            ++visited;
            return false;  // stop after the first field
        });
    RUVIA_CHECK(ok);           // an early stop is a success, not a parse error
    RUVIA_CHECK_EQ(visited, 1);
}
