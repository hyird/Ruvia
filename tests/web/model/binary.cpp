#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/model.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

namespace {
RUVIA_MODEL(binary_model,
    RUVIA_REQUIRED_FIELD(bytes, ruvia::bytes),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::bytes, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::bytes, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(binary_array_model,
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::bytes>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<ruvia::bytes>));
class binary_failing_resource final : public std::pmr::memory_resource {
public:
    explicit binary_failing_resource(std::size_t remaining)
        : remaining_(remaining) {}
    ruvia::test::counting_memory_resource allocations_;

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (remaining_ == 0) {
            throw std::bad_alloc();
        }
        --remaining_;
        return allocations_.allocate(bytes_value, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        allocations_.deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t remaining_;
};

void check_bytes(const ruvia::bytes& actual, std::span<const std::uint8_t> expected) {
    EXPECT_EQ(actual.size(), expected.size());
    EXPECT_TRUE(std::equal(actual.view().begin(), actual.view().end(), expected.begin()));
}
}  // namespace

TEST(Ruvia, model_json_bytes_roundtrip_empty_and_all_octets) {
    std::vector<std::uint8_t> octets(256);
    for (std::size_t i = 0; i < octets.size(); ++i) {
        octets[i] = static_cast<std::uint8_t>(i);
    }
    const auto encoded = ruvia::to_json(ruvia::bytes(std::span<const std::uint8_t>(octets)));
    EXPECT_TRUE(encoded.front() == '"' && encoded.back() == '"');
    EXPECT_TRUE(encoded.find("AAECAwQF") != std::pmr::string::npos);
    EXPECT_TRUE(encoded.find("/w==") != std::pmr::string::npos);
    auto parsed_value = ruvia::from_json<ruvia::bytes>(encoded);
    check_bytes(parsed_value, octets);
    auto empty = ruvia::from_json<ruvia::bytes>(R"("")");
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(std::string_view(ruvia::to_json(ruvia::bytes{})), R"("")");
}

TEST(Ruvia, model_json_bytes_accepts_padding_and_rejects_noncanonical_inputs) {
    auto one = ruvia::from_json<ruvia::bytes>(R"("Zg==")");
    auto two = ruvia::from_json<ruvia::bytes>(R"("Zm8=")");
    EXPECT_TRUE(one.size() == 1 && two.size() == 2);
    EXPECT_EQ(one.view().front(), std::uint8_t{0x66});
    EXPECT_EQ(two.view().back(), std::uint8_t{0x6f});
    auto escaped = ruvia::from_json<ruvia::bytes>(R"("\u005ag\u003d=")");
    EXPECT_EQ(escaped.view().front(), std::uint8_t{0x66});
    for (const auto input : {R"("Zg")", R"("Zh==")", R"("Zm=9")", R"("Zm9v=")",
             R"("Zm-v")", R"("Zm_v")", R"("Zm9v\n")", R"("Zm9=")", R"("====")",
             R"("Zg==AAAA")", R"("AA== ")", "null", "[1]", "123"}) {
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::bytes>(input); }));
    }
}

TEST(Ruvia, model_json_bytes_supports_nested_arrays_nullable_and_omit_empty) {
    auto nested = ruvia::from_json<binary_array_model>(R"({"items":["AQI=",""],"boxed":["/w=="]})");
    EXPECT_EQ(nested.get<"items">().size(), std::size_t{2});
    EXPECT_EQ(nested.get<"items">()[0].view().size(), std::size_t{2});
    EXPECT_EQ(nested.get<"boxed">().front().view().front(), std::uint8_t{255});
    auto model = ruvia::from_json<binary_model>(R"({"bytes":"AQI=","optional":null,"empty":""})");
    EXPECT_TRUE(model.is_null<"optional">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), R"({"bytes":"AQI=","optional":null})");
}

TEST(Ruvia, model_bytes_set_and_move_preserve_values) {
    std::pmr::monotonic_buffer_resource source;
    std::pmr::monotonic_buffer_resource target;
    binary_model model({.resource_ = &target});
    const std::uint8_t raw[] = {1, 2, 3};
    model.set<"bytes">(std::span<const std::uint8_t>(raw));
    std::pmr::vector<std::uint8_t> owned({4, 5}, &source);
    model.set<"bytes">(std::move(owned));
    EXPECT_EQ(model.get<"bytes">().view()[1], std::uint8_t{5});
}

TEST(Ruvia, model_bytes_repeated_operations_reclaim_storage_and_keep_results) {
    ruvia::test::counting_memory_resource memory;
    ruvia::test::counting_memory_resource output_memory;
    {
        binary_model retained({.resource_ = &memory});
        {
            ruvia::test::counting_memory_resource source;
            {
                std::pmr::vector<std::uint8_t> input(256, 0xff, &source);
                retained.set<"bytes">(std::move(input));
            }
            EXPECT_EQ(source.live_allocations(), std::size_t{0});
        }
        const auto saved = ruvia::to_json(retained.get<"bytes">(), {.resource_ = &output_memory});
        const auto baseline = memory.live_allocations();
        const auto output_baseline = output_memory.live_allocations();
        for (int i = 0; i < 32; ++i) {
            {
                auto parsed_value = ruvia::from_json<ruvia::bytes>(saved, {.resource_ = &memory});
                EXPECT_TRUE(parsed_value == retained.get<"bytes">());
                const auto output = ruvia::to_json(parsed_value, {.resource_ = &output_memory});
                EXPECT_EQ(output, saved);
                EXPECT_TRUE(ruvia::testing::throws_on([&] {
                    (void)ruvia::from_json<ruvia::array<ruvia::bytes>>(
                        R"(["AQIDBA==","Zh=="])", {.resource_ = &memory});
                }));
            }
            EXPECT_EQ(memory.live_allocations(), baseline);
            EXPECT_EQ(output_memory.live_allocations(), output_baseline);
            EXPECT_EQ(retained.get<"bytes">().view().front(), std::uint8_t{255});
        }
        std::pmr::vector<std::uint8_t> compatible(128, 7, &memory);
        retained.set<"bytes">(std::move(compatible));
        retained.ensure<"bytes">().assign_owned(retained.get<"bytes">().view().subspan(1));
        EXPECT_EQ(retained.get<"bytes">().size(), std::size_t{127});
        EXPECT_EQ(retained.get<"bytes">().view().back(), std::uint8_t{7});
        EXPECT_TRUE(saved.size() > 256);
    }
    EXPECT_EQ(memory.live_allocations(), std::size_t{0});
    EXPECT_EQ(output_memory.live_allocations(), std::size_t{0});
    EXPECT_EQ(memory.allocation_count(), memory.deallocation_count());
    EXPECT_EQ(output_memory.allocation_count(), output_memory.deallocation_count());
}

TEST(Ruvia, model_bytes_partial_parse_and_output_failures_release_storage) {
    const std::vector<std::uint8_t> first(256, 1);
    const std::vector<std::uint8_t> second(256, 2);
    const std::string input = "[" + std::string(ruvia::to_json(ruvia::bytes(first))) + "," +
                              std::string(ruvia::to_json(ruvia::bytes(second))) + "] ";
    binary_failing_resource reference((std::numeric_limits<std::size_t>::max)());
    {
        auto parsed_value = ruvia::from_json<ruvia::array<ruvia::bytes>>(input, {.resource_ = &reference});
        EXPECT_EQ(parsed_value.size(), std::size_t{2});
    }
    EXPECT_EQ(reference.allocations_.live_allocations(), std::size_t{0});
    const auto count = reference.allocations_.allocation_count();
    EXPECT_TRUE(count > 1);
    EXPECT_TRUE(count < 1024);
    const bool inject = ruvia::test::stl_allocation_failure_injection_enabled();
    if (inject) {
        for (std::size_t failure = 0; failure < count; ++failure) {
            binary_failing_resource memory(failure);
            bool threw = false;
            try {
                (void)ruvia::from_json<ruvia::array<ruvia::bytes>>(input, {.resource_ = &memory});
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            EXPECT_TRUE(threw);
            EXPECT_EQ(memory.allocations_.allocation_count(), failure);
            EXPECT_EQ(memory.allocations_.live_allocations(), std::size_t{0});
            EXPECT_EQ(memory.allocations_.allocation_count(), memory.allocations_.deallocation_count());
        }
    }
    const std::vector<std::uint8_t> octets(256, 1);
    const ruvia::bytes retained(octets);
    binary_failing_resource successful_output((std::numeric_limits<std::size_t>::max)());
    {
        const auto encoded = ruvia::to_json(retained, {.resource_ = &successful_output});
        EXPECT_TRUE(encoded.size() > 256);
    }
    EXPECT_EQ(successful_output.allocations_.live_allocations(), std::size_t{0});
    const auto output_count = successful_output.allocations_.allocation_count();
    EXPECT_TRUE(output_count > 0);
    if (inject) {
        for (std::size_t failure = 0; failure < output_count; ++failure) {
            binary_failing_resource output(failure);
            bool threw = false;
            try {
                (void)ruvia::to_json(retained, {.resource_ = &output});
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            EXPECT_TRUE(threw);
            EXPECT_EQ(output.allocations_.allocation_count(), failure);
            EXPECT_EQ(output.allocations_.live_allocations(), std::size_t{0});
            EXPECT_EQ(output.allocations_.allocation_count(), output.allocations_.deallocation_count());
        }
    }
    EXPECT_EQ(retained.size(), octets.size());
    EXPECT_TRUE(ruvia::to_json(retained).size() > 256);
}
