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

constexpr std::size_t initial_payload_size = 4096;

RUVIA_MODEL(payload_initial_model,
    RUVIA_OPTIONAL_FIELD(payload, ruvia::string, RUVIA_INITIAL(std::string(initial_payload_size, 'p'))));

// Counts only allocations large enough to hold an INITIAL payload.
class payload_counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t payload_allocations_{0};
    ruvia::test::counting_memory_resource allocations_;

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        payload_allocations_ += bytes_value >= initial_payload_size ? 1 : 0;
        return allocations_.allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        allocations_.deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

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
    RUVIA_CHECK(parsed_value.get<"number">()->value_ == 9);
    RUVIA_CHECK(!parsed_value.get<"name">());
    RUVIA_CHECK(!parsed_value.is_present<"number">());
    RUVIA_CHECK(!parsed_value.is_null<"name">());
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
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
    retained = std::move(parsed_value);
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

RUVIA_TEST(model_required_initial_is_not_used_by_parsing) {
    evaluations = 0;
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<required_initial_model>("{}"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_form<required_initial_model>(""); }));
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
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

RUVIA_TEST(model_initial_and_default_modes_are_evaluated_once_across_codecs_and_moves) {
    evaluations = 0;
    default_evaluations = 0;
    ruvia::test::counting_memory_resource source;
    counted_lifecycle explicit_value({.resource_ = &source});
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK_EQ(default_evaluations, std::size_t{0});

    auto json = ruvia::from_json<counted_lifecycle>("{}", {.resource_ = &source});
    auto form = ruvia::from_form<counted_lifecycle>("", {.resource_ = &source});
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK_EQ(default_evaluations, std::size_t{2});
    counted_lifecycle moved(std::move(json));
    RUVIA_CHECK(!json.get<"value">());
    RUVIA_CHECK_EQ(moved.get<"value">()->value_, 11);
    RUVIA_CHECK_EQ(form.get<"value">()->value_, 11);
    RUVIA_CHECK(!moved.is_present<"value">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(moved)), std::string_view(R"({"value":11})"));
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK_EQ(default_evaluations, std::size_t{2});
}

RUVIA_TEST(model_initial_fresh_container_elements_are_built_once_on_the_container_resource) {
    payload_counting_resource resource;
    {
        ruvia::array<payload_initial_model> dense({.resource_ = &resource});
        ruvia::boxed_array<payload_initial_model> boxed({.resource_ = &resource});
        dense.reserve(4);
        boxed.reserve(4);
        for (std::size_t index = 0; index != 4; ++index) {
            (void)dense.emplace_back();
            (void)boxed.emplace_back();
        }
        RUVIA_CHECK_EQ(resource.payload_allocations_, std::size_t{8});
        dense.resize(6);
        boxed.resize(6);
        RUVIA_CHECK_EQ(resource.payload_allocations_, std::size_t{12});
        for (const auto& value : dense) {
            RUVIA_CHECK_EQ(value.get<"payload">()->size(), initial_payload_size);
        }
        for (const auto& value : boxed) {
            RUVIA_CHECK_EQ(value.get<"payload">()->size(), initial_payload_size);
        }
    }
    RUVIA_CHECK_EQ(resource.allocations_.live_allocations(), std::size_t{0});
}
