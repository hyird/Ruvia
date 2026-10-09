#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

std::size_t evaluations{0};
std::size_t default_evaluations{0};

int counted_default() {
    ++default_evaluations;
    return 11;
}

int initial_number() {
    ++evaluations;
    return 7;
}

std::string_view initial_name() {
    ++evaluations;
    return "seed";
}

int failing_initial() {
    ++evaluations;
    throw std::runtime_error("initial evaluation failed");
}

RUVIA_MODEL(initial_model,
    RUVIA_REQUIRED_FIELD(required, ruvia::int32),
    RUVIA_OPTIONAL_FIELD(number, ruvia::int32, RUVIA_INITIAL(initial_number()),
        RUVIA_DEFAULT(9)),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string, RUVIA_INITIAL(initial_name())));

RUVIA_MODEL(failing_initial_model,
    RUVIA_OPTIONAL_FIELD(name, ruvia::string, RUVIA_INITIAL(std::string(128, 'n'))),
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_INITIAL(failing_initial())));

RUVIA_MODEL(required_initial_model,
    RUVIA_REQUIRED_FIELD(value, ruvia::int32, RUVIA_INITIAL(initial_number())));

RUVIA_MODEL(null_initial_model,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE, RUVIA_INITIAL(nullptr)));

RUVIA_MODEL(nested_initial_model,
    RUVIA_OPTIONAL_FIELD(child, initial_model),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<initial_model>));

RUVIA_MODEL(counted_lifecycle,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_INITIAL(initial_number()),
        RUVIA_DEFAULT(counted_default())));

}  // namespace

RUVIA_TEST(model_initial_values_only_run_for_explicit_construction) {
    evaluations = 0;
    initial_model model;
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    RUVIA_CHECK_EQ(model.get<"number">()->value_, 7);
    RUVIA_CHECK_EQ(model.get<"name">()->view(), std::string_view("seed"));
    RUVIA_CHECK(!model.is_present<"number">());
    RUVIA_CHECK(!model.is_present<"name">());

    auto parsed_value = ruvia::from_json<initial_model>(R"({"required":1})");
    RUVIA_CHECK(parsed_value.has_value());
    if (parsed_value) {
        RUVIA_CHECK(parsed_value->get<"number">()->value_ == 9);
        RUVIA_CHECK(!parsed_value->get<"name">());
        RUVIA_CHECK(!parsed_value->is_present<"number">());
        RUVIA_CHECK(!parsed_value->is_null<"name">());
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
}

RUVIA_TEST(model_initial_values_do_not_recur_through_moves_or_rebind) {
    evaluations = 0;
    ruvia::test::counting_memory_resource source;
    ruvia::test::counting_memory_resource destination;
    initial_model retained({.resource_ = &destination});
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    auto parsed_value = ruvia::from_json<initial_model>(R"({"required":1,"number":3})",
        {.resource_ = &source});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    retained = std::move(*parsed_value);
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    RUVIA_CHECK_EQ(retained.get<"number">()->value_, 3);
}

RUVIA_TEST(model_initial_exception_releases_partial_values) {
    evaluations = 0;
    ruvia::test::counting_memory_resource memory;
    bool failed = false;
    try {
        failing_initial_model model({.resource_ = &memory});
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()) == "initial evaluation failed";
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(model_null_initial_is_a_value_state_without_allocation) {
    ruvia::test::counting_memory_resource memory;
    null_initial_model model({.resource_ = &memory});
    RUVIA_CHECK(model.is_null<"value">());
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(model_required_initial_is_not_used_by_partial_parsing) {
    evaluations = 0;
    auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<required_initial_model>(
        "{}", std::pmr::get_default_resource());
    RUVIA_CHECK(parsed_value.has_value());
    if (parsed_value) {
        RUVIA_CHECK(!parsed_value->is_present<"value">());
        RUVIA_CHECK(!ruvia::detail::model_validation_access::structure_valid(*parsed_value));
    }
    auto form_parsed = ruvia::detail::model_parse_access::parse_form_borrowed_partial<required_initial_model>(
        "", std::pmr::get_default_resource());
    RUVIA_CHECK(form_parsed.has_value());
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    RUVIA_CHECK(!ruvia::from_json<required_initial_model>("{}").has_value());
}

RUVIA_TEST(model_initial_is_only_for_explicit_nested_construction) {
    evaluations = 0;
    nested_initial_model model;
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    (void)model.ensure<"child">();
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
}

RUVIA_TEST(model_initial_does_not_recur_in_nested_move_or_array_emplace) {
    evaluations = 0;
    ruvia::test::counting_memory_resource source_resource;
    ruvia::test::counting_memory_resource destination_resource;
    initial_model child_value({.resource_ = &source_resource});
    nested_initial_model parent_value({.resource_ = &destination_resource});
    parent_value.set<"child">(std::move(child_value));
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});

    initial_model array_child({.resource_ = &source_resource});
    parent_value.ensure<"children">().emplace_back(std::move(array_child));
    RUVIA_CHECK_EQ(evaluations, std::size_t{4});
    RUVIA_CHECK_EQ(parent_value.get<"children">()->size(), std::size_t{1});
}

RUVIA_TEST(model_initial_and_default_modes_are_evaluated_once_across_codecs_and_rebind) {
    evaluations = 0;
    default_evaluations = 0;
    ruvia::test::counting_memory_resource source;
    ruvia::test::counting_memory_resource destination;
    counted_lifecycle explicit_value({.resource_ = &source});
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK_EQ(default_evaluations, std::size_t{0});

    auto json = ruvia::from_json<counted_lifecycle>("{}", {.resource_ = &source});
    auto form = ruvia::from_form<counted_lifecycle>("", {.resource_ = &source});
    RUVIA_CHECK(json && form);
    if (!json || !form) {
        return;
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK_EQ(default_evaluations, std::size_t{2});
    counted_lifecycle moved(std::move(*json));
    RUVIA_CHECK(!json->get<"value">());
    auto owned = ruvia::detail::model_value_rebind_access::own(std::move(moved), &destination);
    auto copied = ruvia::detail::model_value_rebind_access::own(std::as_const(*form), &destination);
    RUVIA_CHECK_EQ(owned.get<"value">()->value_, 11);
    RUVIA_CHECK_EQ(copied.get<"value">()->value_, 11);
    RUVIA_CHECK(!owned.is_present<"value">());
    RUVIA_CHECK(!copied.is_present<"value">());
    RUVIA_CHECK(!moved.get<"value">());
    RUVIA_CHECK_EQ(owned.resource(), &destination);
    RUVIA_CHECK_EQ(copied.resource(), &destination);
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(owned)), std::string_view(R"({"value":11})"));
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK_EQ(default_evaluations, std::size_t{2});
}
