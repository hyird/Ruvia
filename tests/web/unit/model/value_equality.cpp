#include <string_view>

#include "ruvia/web/Model.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_MODEL(ComparableModel,
    RUVIA_OPTIONAL_FIELD(number, ruvia::Int32, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(text, ruvia::String, RUVIA_NULLABLE));
RUVIA_MODEL(ComparableTree,
    RUVIA_REQUIRED_FIELD(value, ruvia::Int32),
    RUVIA_OPTIONAL_FIELD(items, ruvia::Array<ComparableModel>),
    RUVIA_OPTIONAL_FIELD(children, ruvia::BoxedArray<ComparableTree>));

}  // namespace

RUVIA_TEST(model_equality_compares_state_and_value_but_not_presence) {
    ComparableModel missing;
    ComparableModel parsedValue;
    parsedValue.set<"number">(7);
    RUVIA_CHECK(missing != parsedValue);

    auto parsed = ruvia::fromJson<ComparableModel>(R"({"number":7})");
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(parsed->isPresent<"number">());
    RUVIA_CHECK(*parsed == parsedValue);

    parsedValue.set<"number">(8);
    RUVIA_CHECK(*parsed != parsedValue);
    parsedValue.set<"number">(nullptr);
    RUVIA_CHECK(*parsed != parsedValue);
}

RUVIA_TEST(model_equality_distinguishes_missing_and_null) {
    ComparableModel missing;
    ComparableModel nullValue;
    nullValue.set<"number">(nullptr);
    RUVIA_CHECK(missing != nullValue);

    missing.set<"number">(nullptr);
    RUVIA_CHECK(missing == nullValue);
}

RUVIA_TEST(model_equality_ignores_resources_for_normalized_values) {
    ruvia::test::CountingMemoryResource leftResource;
    ruvia::test::CountingMemoryResource rightResource;
    ComparableModel left({.resource = &leftResource});
    ComparableModel right({.resource = &rightResource});
    left.set<"text">(std::string_view("same value"));
    right.set<"text">(std::string_view("same value"));
    RUVIA_CHECK(left == right);

    right.set<"text">(std::string_view("different"));
    RUVIA_CHECK(left != right);
}

RUVIA_TEST(model_equality_compares_nested_arrays_and_recursive_boxed_values) {
    ruvia::test::CountingMemoryResource firstResource;
    ruvia::test::CountingMemoryResource secondResource;
    constexpr auto input = R"({"value":1,"items":[{"text":"a"},{"text":"b"}],"children":[{"value":2,"children":[{"value":3}]}]})";
    {
        auto first = ruvia::fromJson<ComparableTree>(input, {.resource = &firstResource});
        auto second = ruvia::fromJson<ComparableTree>(input, {.resource = &secondResource});
        RUVIA_CHECK(first && second);
        if (!first || !second) {
            return;
        }
        RUVIA_CHECK(*first == *second);
        second->ensure<"items">()[0].set<"text">("b");
        RUVIA_CHECK(*first != *second);
        second->ensure<"items">()[0].set<"text">("a");
        RUVIA_CHECK(*first == *second);
        second->ensure<"children">().front().ensure<"children">().front().set<"value">(4);
        RUVIA_CHECK(*first != *second);
    }
    RUVIA_CHECK_EQ(firstResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(secondResource.liveAllocations(), std::size_t{0});
}
