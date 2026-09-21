#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/Model.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

std::size_t evaluations{0};

int initialNumber() {
    ++evaluations;
    return 7;
}

std::string_view initialName() {
    ++evaluations;
    return "seed";
}

int failingInitial() {
    ++evaluations;
    throw std::runtime_error("initial evaluation failed");
}

RUVIA_MODEL(InitialModel,
    RUVIA_REQUIRED_FIELD(required, ruvia::Int32),
    RUVIA_OPTIONAL_FIELD(number, ruvia::Int32, RUVIA_INITIAL(initialNumber()),
        RUVIA_DEFAULT(9)),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String, RUVIA_INITIAL(initialName())));

RUVIA_MODEL(FailingInitialModel,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String, RUVIA_INITIAL(std::string(128, 'n'))),
    RUVIA_OPTIONAL_FIELD(value, ruvia::Int32, RUVIA_INITIAL(failingInitial())));

RUVIA_MODEL(RequiredInitialModel,
    RUVIA_REQUIRED_FIELD(value, ruvia::Int32, RUVIA_INITIAL(initialNumber())));

RUVIA_MODEL(NullInitialModel,
    RUVIA_OPTIONAL_FIELD(value, ruvia::String, RUVIA_NULLABLE, RUVIA_INITIAL(nullptr)));

RUVIA_MODEL(NestedInitialModel,
    RUVIA_OPTIONAL_FIELD(child, InitialModel),
    RUVIA_OPTIONAL_FIELD(children, ruvia::Array<InitialModel>));

}  // namespace

RUVIA_TEST(model_initial_values_only_run_for_explicit_construction) {
    evaluations = 0;
    InitialModel model;
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    RUVIA_CHECK_EQ(model.get<"number">()->value, 7);
    RUVIA_CHECK_EQ(model.get<"name">()->view(), std::string_view("seed"));
    RUVIA_CHECK(!model.isPresent<"number">());
    RUVIA_CHECK(!model.isPresent<"name">());

    auto parsed = ruvia::fromJson<InitialModel>(R"({"required":1})");
    RUVIA_CHECK(parsed.has_value());
    if (parsed) {
        RUVIA_CHECK(parsed->get<"number">()->value == 9);
        RUVIA_CHECK(!parsed->get<"name">());
        RUVIA_CHECK(!parsed->isPresent<"number">());
        RUVIA_CHECK(!parsed->isNull<"name">());
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
}

RUVIA_TEST(model_initial_values_do_not_recur_through_moves_or_rebind) {
    evaluations = 0;
    ruvia::test::CountingMemoryResource source;
    ruvia::test::CountingMemoryResource destination;
    InitialModel retained({.resource = &destination});
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    auto parsed = ruvia::fromJson<InitialModel>(R"({"required":1,"number":3})",
        {.resource = &source});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    retained = std::move(*parsed);
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    RUVIA_CHECK_EQ(retained.get<"number">()->value, 3);
}

RUVIA_TEST(model_initial_exception_releases_partial_values) {
    evaluations = 0;
    ruvia::test::CountingMemoryResource memory;
    bool failed = false;
    try {
        FailingInitialModel model({.resource = &memory});
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()) == "initial evaluation failed";
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(model_null_initial_is_a_value_state_without_allocation) {
    ruvia::test::CountingMemoryResource memory;
    NullInitialModel model({.resource = &memory});
    RUVIA_CHECK(model.isNull<"value">());
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(model_required_initial_is_not_used_by_partial_parsing) {
    evaluations = 0;
    auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<RequiredInitialModel>(
        "{}", std::pmr::get_default_resource());
    RUVIA_CHECK(parsed.has_value());
    if (parsed) {
        RUVIA_CHECK(!parsed->isPresent<"value">());
        RUVIA_CHECK(!ruvia::detail::ModelValidationAccess::structureValid(*parsed));
    }
    auto formParsed = ruvia::detail::ModelParseAccess::parseFormBorrowedPartial<RequiredInitialModel>(
        "", std::pmr::get_default_resource());
    RUVIA_CHECK(formParsed.has_value());
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    RUVIA_CHECK(!ruvia::fromJson<RequiredInitialModel>("{}").has_value());
}

RUVIA_TEST(model_initial_is_only_for_explicit_nested_construction) {
    evaluations = 0;
    NestedInitialModel model;
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    (void)model.ensure<"child">();
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
}

RUVIA_TEST(model_initial_does_not_recur_in_nested_move_or_array_emplace) {
    evaluations = 0;
    ruvia::test::CountingMemoryResource sourceResource;
    ruvia::test::CountingMemoryResource destinationResource;
    InitialModel child({.resource = &sourceResource});
    NestedInitialModel parent({.resource = &destinationResource});
    parent.set<"child">(std::move(child));
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});

    InitialModel arrayChild({.resource = &sourceResource});
    parent.ensure<"children">().emplace_back(std::move(arrayChild));
    RUVIA_CHECK_EQ(evaluations, std::size_t{4});
    RUVIA_CHECK_EQ(parent.get<"children">()->size(), std::size_t{1});
}
