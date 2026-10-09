#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/model.h"
#include "ruvia/web/model_json.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
RUVIA_MODEL(binary_model,
    RUVIA_REQUIRED_FIELD(bytes, ruvia::bytes),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::bytes, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::bytes, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(binary_array_model,
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::bytes>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<ruvia::bytes>));
RUVIA_MODEL(binary_rules,
    RUVIA_REQUIRED_FIELD(bytes, ruvia::bytes, RUVIA_MIN(2, "short"), RUVIA_MAX(3, "long")));
class binary_failing_resource final : public std::pmr::memory_resource {
public:
    // MSVC debug STL allocates small iterator-proxy nodes from the container
    // allocator, sometimes inside a noexcept move or while a debug lock is held.
    // Failing those allocations terminates or deadlocks instead of testing
    // unwinding. Payload buffers in this file are larger than this cutoff.
    static constexpr std::size_t min_payload_bytes = 64;

    explicit binary_failing_resource(std::size_t allowed_payload)
        : allowed_payload_(allowed_payload) {}
    ruvia::test::counting_memory_resource allocations_;

    [[nodiscard]] std::size_t payload_allocations() const noexcept {
        return payload_allocations_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value >= min_payload_bytes) {
            if (allowed_payload_ == 0) {
                throw std::bad_alloc();
            }
            --allowed_payload_;
            ++payload_allocations_;
        }
        return allocations_.allocate(bytes_value, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        allocations_.deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t allowed_payload_;
    std::size_t payload_allocations_{0};
};

void check_bytes(auto& ruvia_ctx, const ruvia::bytes& actual, std::span<const std::uint8_t> expected) {
    RUVIA_CHECK_EQ(actual.size(), expected.size());
    RUVIA_CHECK(std::equal(actual.view().begin(), actual.view().end(), expected.begin()));
}
}  // namespace

RUVIA_TEST(model_json_bytes_roundtrip_empty_and_all_octets) {
    std::vector<std::uint8_t> octets(256);
    for (std::size_t i = 0; i < octets.size(); ++i) {
        octets[i] = static_cast<std::uint8_t>(i);
    }
    const auto encoded = ruvia::to_json(ruvia::bytes(std::span<const std::uint8_t>(octets)));
    RUVIA_CHECK(encoded.front() == '"' && encoded.back() == '"');
    RUVIA_CHECK(encoded.find("AAECAwQF") != std::pmr::string::npos);
    RUVIA_CHECK(encoded.find("/w==") != std::pmr::string::npos);
    auto parsed_value = ruvia::from_json<ruvia::bytes>(encoded);
    RUVIA_CHECK(parsed_value.has_value());
    if (parsed_value) {
        check_bytes(ruvia_ctx, *parsed_value, octets);
    }
    auto empty = ruvia::from_json<ruvia::bytes>(R"("")");
    RUVIA_CHECK(empty && empty->empty());
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(ruvia::bytes{})), R"("")");
}

RUVIA_TEST(model_json_bytes_accepts_padding_and_rejects_noncanonical_inputs) {
    auto one = ruvia::from_json<ruvia::bytes>(R"("Zg==")");
    auto two = ruvia::from_json<ruvia::bytes>(R"("Zm8=")");
    RUVIA_CHECK(one && two && one->size() == 1 && two->size() == 2);
    if (one && two) {
        RUVIA_CHECK_EQ(one->view().front(), std::uint8_t{0x66});
        RUVIA_CHECK_EQ(two->view().back(), std::uint8_t{0x6f});
    }
    auto escaped = ruvia::from_json<ruvia::bytes>(R"("\u005ag\u003d=")");
    RUVIA_CHECK(escaped && escaped->view().front() == 0x66);
    for (const auto input : {R"("Zg")", R"("Zh==")", R"("Zm=9")", R"("Zm9v=")",
             R"("Zm-v")", R"("Zm_v")", R"("Zm9v\n")", R"("Zm9=")", R"("====")",
             R"("Zg==AAAA")", R"("AA== ")", "null", "[1]", "123"}) {
        RUVIA_CHECK(!ruvia::from_json<ruvia::bytes>(input));
    }
}

RUVIA_TEST(model_json_bytes_supports_nested_arrays_nullable_and_omit_empty) {
    auto nested = ruvia::from_json<binary_array_model>(R"({"items":["AQI=",""],"boxed":["/w=="]})");
    RUVIA_CHECK(nested.has_value());
    if (nested) {
        RUVIA_CHECK_EQ(nested->get<"items">().size(), std::size_t{2});
        RUVIA_CHECK_EQ(nested->get<"items">()[0].view().size(), std::size_t{2});
        RUVIA_CHECK_EQ(nested->get<"boxed">().front().view().front(), std::uint8_t{255});
    }
    auto model = ruvia::from_json<binary_model>(R"({"bytes":"AQI=","optional":null,"empty":""})");
    RUVIA_CHECK(model && model->is_null<"optional">());
    if (model) {
        RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*model)), R"({"bytes":"AQI=","optional":null})");
    }
}

RUVIA_TEST(model_json_bytes_rules_measure_decoded_bytes_only) {
    auto short_value = ruvia::from_json<binary_rules>(R"({"bytes":"AQ=="})");
    auto valid_value = ruvia::from_json<binary_rules>(R"({"bytes":"AQI="})");
    auto long_value = ruvia::from_json<binary_rules>(R"({"bytes":"AQIDBA=="})");
    RUVIA_CHECK(short_value && valid_value && long_value);
    if (short_value && valid_value && long_value) {
        ruvia::validator validator;
        ruvia::detail::model_validation_access::validate_model(*short_value, validator);
        RUVIA_CHECK(!validator.ok());
        ruvia::validator valid_validator;
        ruvia::detail::model_validation_access::validate_model(*valid_value, valid_validator);
        RUVIA_CHECK(valid_validator.ok());
        ruvia::validator long_validator;
        ruvia::detail::model_validation_access::validate_model(*long_value, long_validator);
        RUVIA_CHECK(!long_validator.ok());
    }
}

RUVIA_TEST(model_bytes_set_and_move_normalize_resources) {
    std::pmr::monotonic_buffer_resource source;
    std::pmr::monotonic_buffer_resource target;
    binary_model model({.resource_ = &target});
    const std::uint8_t raw[] = {1, 2, 3};
    model.set<"bytes">(std::span<const std::uint8_t>(raw));
    RUVIA_CHECK_EQ(model.get<"bytes">().resource(), &target);
    std::pmr::vector<std::uint8_t> owned({4, 5}, &source);
    model.set<"bytes">(std::move(owned));
    RUVIA_CHECK_EQ(model.get<"bytes">().resource(), &target);
    RUVIA_CHECK_EQ(model.get<"bytes">().view()[1], std::uint8_t{5});
}

RUVIA_TEST(model_bytes_repeated_operations_reclaim_storage_and_keep_results) {
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
            RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
        }
        const auto output_before = output_memory.allocation_count();
        const auto saved = ruvia::to_json(retained.get<"bytes">(), {.resource_ = &output_memory});
        const auto produced = output_memory.allocation_count() - output_before;
        // One result buffer. MSVC debug may also allocate iterator-proxy metadata
        // from the same resource; a short reserve can grow the buffer once.
        RUVIA_CHECK(produced >= 1 && produced <= 4);
        const auto baseline = memory.live_allocations();
        const auto output_baseline = output_memory.live_allocations();
        for (int i = 0; i < 32; ++i) {
            {
                auto parsed_value = ruvia::from_json<ruvia::bytes>(saved, {.resource_ = &memory});
                RUVIA_CHECK(parsed_value && *parsed_value == retained.get<"bytes">());
                if (parsed_value) {
                    const auto output = ruvia::to_json(*parsed_value, {.resource_ = &output_memory});
                    RUVIA_CHECK_EQ(output, saved);
                }
                RUVIA_CHECK(!ruvia::from_json<ruvia::array<ruvia::bytes>>(
                    R"(["AQIDBA==","Zh=="])", {.resource_ = &memory}));
            }
            RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
            RUVIA_CHECK_EQ(output_memory.live_allocations(), output_baseline);
            RUVIA_CHECK_EQ(retained.get<"bytes">().view().front(), std::uint8_t{255});
        }
        std::pmr::vector<std::uint8_t> compatible(128, 7, &memory);
        const auto* original = compatible.data();
        retained.set<"bytes">(std::move(compatible));
        RUVIA_CHECK_EQ(retained.get<"bytes">().view().data(), original);
        retained.ensure<"bytes">().assign_owned(retained.get<"bytes">().view().subspan(1));
        RUVIA_CHECK_EQ(retained.get<"bytes">().size(), std::size_t{127});
        RUVIA_CHECK_EQ(retained.get<"bytes">().view().back(), std::uint8_t{7});
        RUVIA_CHECK(saved.size() > 256);
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(output_memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
    RUVIA_CHECK_EQ(output_memory.allocation_count(), output_memory.deallocation_count());
}

RUVIA_TEST(model_bytes_partial_parse_and_output_failures_release_storage) {
    const std::vector<std::uint8_t> first(256, 1);
    const std::vector<std::uint8_t> second(256, 2);
    const std::string input = "[" + std::string(ruvia::to_json(ruvia::bytes(first))) + "," +
                              std::string(ruvia::to_json(ruvia::bytes(second))) + "] ";
    binary_failing_resource reference(1024);
    {
        auto parsed_value = ruvia::from_json<ruvia::array<ruvia::bytes>>(input, {.resource_ = &reference});
        RUVIA_CHECK(parsed_value && parsed_value->size() == 2);
    }
    RUVIA_CHECK_EQ(reference.allocations_.live_allocations(), std::size_t{0});
    const auto count = reference.payload_allocations();
    RUVIA_CHECK(count > 1);
    RUVIA_CHECK(count < 1024);
    for (std::size_t failure = 0; failure < count; ++failure) {
        binary_failing_resource memory(failure);
        bool threw = false;
        try {
            (void)ruvia::from_json<ruvia::array<ruvia::bytes>>(input, {.resource_ = &memory});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(memory.payload_allocations(), failure);
        RUVIA_CHECK_EQ(memory.allocations_.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocations_.allocation_count(), memory.allocations_.deallocation_count());
    }
    const std::vector<std::uint8_t> octets(256, 1);
    const ruvia::bytes retained(octets);
    binary_failing_resource output(0);
    bool threw = false;
    try {
        (void)ruvia::to_json(retained, {.resource_ = &output});
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(output.allocations_.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(retained.size(), octets.size());
    RUVIA_CHECK(ruvia::to_json(retained).size() > 256);
}
