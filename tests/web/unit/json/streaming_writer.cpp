#include "test_harness.h"

// The streaming JSON writer: the framework owns escaping, comma placement and
// bracket balance, so a run-time-shaped body cannot be malformed by the caller.

#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/Json.h"
#include "ruvia/web/Model.h"

struct JsonWriterPoint final {
    RUVIA_FIELD(x, ruvia::Int64);
    RUVIA_FIELD(y, ruvia::Int64);
    RUVIA_MODEL(JsonWriterPoint, x, y);
};

namespace {

std::string writeObject(auto&& build) {
    std::pmr::string out{std::pmr::get_default_resource()};
    {
        ruvia::JsonObjectWriter writer(out);
        build(writer);
    }
    return std::string(out);
}

}  // namespace

RUVIA_TEST(json_writer_emits_scalars_with_correct_separators) {
    const auto json = writeObject([](ruvia::JsonObjectWriter& out) {
        out.add("name", "ada");
        out.add("age", 36);
        out.add("active", true);
        out.add("ratio", 0.5);
        out.addNull("middle");
    });
    RUVIA_CHECK_EQ(json, std::string(R"({"name":"ada","age":36,"active":true,"ratio":0.5,"middle":null})"));
}

RUVIA_TEST(json_writer_emits_empty_object_and_array) {
    RUVIA_CHECK_EQ(writeObject([](ruvia::JsonObjectWriter&) {}), std::string("{}"));

    std::pmr::string out{std::pmr::get_default_resource()};
    {
        ruvia::JsonArrayWriter writer(out);
    }
    RUVIA_CHECK_EQ(std::string(out), std::string("[]"));
}

RUVIA_TEST(json_writer_escapes_keys_and_string_values) {
    const auto json = writeObject([](ruvia::JsonObjectWriter& out) {
        out.add("qu\"ote", "back\\slash");
        out.add("ctrl", std::string_view("a\nb\tc"));
    });
    RUVIA_CHECK_EQ(json, std::string(R"({"qu\"ote":"back\\slash","ctrl":"a\nb\tc"})"));
}

RUVIA_TEST(json_writer_treats_string_literals_as_strings_not_bools) {
    // const char* converts to bool by a standard conversion and to string_view
    // only by a user-defined one, so an unguarded overload set would render
    // every literal as true.
    const auto json = writeObject([](ruvia::JsonObjectWriter& out) {
        out.add("literal", "text");
    });
    RUVIA_CHECK_EQ(json, std::string(R"({"literal":"text"})"));
}

RUVIA_TEST(json_writer_nests_objects_and_arrays) {
    const auto json = writeObject([](ruvia::JsonObjectWriter& out) {
        out.add("id", 1);
        {
            auto tags = out.beginArray("tags");
            tags.add("a");
            tags.add("b");
            {
                auto nested = tags.beginObject();
                nested.add("deep", true);
            }
        }
        {
            auto meta = out.beginObject("meta");
            meta.add("count", 2);
        }
    });
    RUVIA_CHECK_EQ(json, std::string(R"({"id":1,"tags":["a","b",{"deep":true}],"meta":{"count":2}})"));
}

RUVIA_TEST(json_writer_rejects_writing_a_parent_while_a_child_is_open) {
    std::pmr::string out{std::pmr::get_default_resource()};
    bool rejected = false;
    {
        ruvia::JsonObjectWriter writer(out);
        auto child = writer.beginArray("items");
        try {
            // Would interleave bytes into the child's brackets.
            writer.add("oops", 1);
        } catch (const std::logic_error&) {
            rejected = true;
        }
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(json_writer_closes_brackets_when_an_exception_unwinds) {
    std::pmr::string out{std::pmr::get_default_resource()};
    try {
        ruvia::JsonObjectWriter writer(out);
        auto items = writer.beginArray("items");
        items.add(1);
        throw std::runtime_error("abort mid-body");
    } catch (const std::runtime_error&) {
    }
    // Both scopes closed on unwind, so the partial body is still well formed.
    RUVIA_CHECK_EQ(std::string(out), std::string(R"({"items":[1]})"));
}

RUVIA_TEST(json_writer_renders_non_finite_numbers_as_null) {
    const auto json = writeObject([](ruvia::JsonObjectWriter& out) {
        out.add("inf", std::numeric_limits<double>::infinity());
        out.add("nan", std::numeric_limits<double>::quiet_NaN());
    });
    RUVIA_CHECK_EQ(json, std::string(R"({"inf":null,"nan":null})"));
}

RUVIA_TEST(json_writer_splices_a_compile_time_model) {
    const auto json = writeObject([](ruvia::JsonObjectWriter& out) {
        JsonWriterPoint point;
        point.x(ruvia::Int64{1});
        point.y(ruvia::Int64{2});
        out.addModel("origin", point);
    });
    RUVIA_CHECK_EQ(json, std::string(R"({"origin":{"x":1,"y":2}})"));
}
