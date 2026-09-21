#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/Model.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/Validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
RUVIA_MODEL(BinaryModel,
    RUVIA_REQUIRED_FIELD(bytes, ruvia::Bytes),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::Bytes, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::Bytes, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(BinaryArrayModel,
    RUVIA_REQUIRED_FIELD(items, ruvia::Array<ruvia::Bytes>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::BoxedArray<ruvia::Bytes>));
RUVIA_MODEL(BinaryRules,
    RUVIA_REQUIRED_FIELD(bytes, ruvia::Bytes, RUVIA_MIN(2, "short"), RUVIA_MAX(3, "long")));
class BinaryFailingResource final : public std::pmr::memory_resource {
public:
    explicit BinaryFailingResource(std::size_t allowed)
        : allowed_(allowed) {}
    ruvia::test::CountingMemoryResource allocations;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (allowed_ == 0) {
            throw std::bad_alloc();
        }
        --allowed_;
        return allocations.allocate(bytes, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        allocations.deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t allowed_;
};

void checkBytes(auto& ruvia_ctx, const ruvia::Bytes& actual, std::span<const std::uint8_t> expected) {
    RUVIA_CHECK_EQ(actual.size(), expected.size());
    RUVIA_CHECK(std::equal(actual.view().begin(), actual.view().end(), expected.begin()));
}
}  // namespace

RUVIA_TEST(model_json_bytes_roundtrip_empty_and_all_octets) {
    std::vector<std::uint8_t> octets(256);
    for (std::size_t i = 0; i < octets.size(); ++i) {
        octets[i] = static_cast<std::uint8_t>(i);
    }
    const auto encoded = ruvia::toJson(ruvia::Bytes(std::span<const std::uint8_t>(octets)));
    RUVIA_CHECK(encoded.front() == '"' && encoded.back() == '"');
    RUVIA_CHECK(encoded.find("AAECAwQF") != std::pmr::string::npos);
    RUVIA_CHECK(encoded.find("/w==") != std::pmr::string::npos);
    auto parsed = ruvia::fromJson<ruvia::Bytes>(encoded);
    RUVIA_CHECK(parsed.has_value());
    if (parsed) {
        checkBytes(ruvia_ctx, *parsed, octets);
    }
    auto empty = ruvia::fromJson<ruvia::Bytes>(R"("")");
    RUVIA_CHECK(empty && empty->empty());
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(ruvia::Bytes{})), R"("")");
}

RUVIA_TEST(model_json_bytes_accepts_padding_and_rejects_noncanonical_inputs) {
    auto one = ruvia::fromJson<ruvia::Bytes>(R"("Zg==")");
    auto two = ruvia::fromJson<ruvia::Bytes>(R"("Zm8=")");
    RUVIA_CHECK(one && two && one->size() == 1 && two->size() == 2);
    if (one && two) {
        RUVIA_CHECK_EQ(one->view().front(), std::uint8_t{0x66});
        RUVIA_CHECK_EQ(two->view().back(), std::uint8_t{0x6f});
    }
    auto escaped = ruvia::fromJson<ruvia::Bytes>(R"("\u005ag\u003d=")");
    RUVIA_CHECK(escaped && escaped->view().front() == 0x66);
    for (const auto input : {R"("Zg")", R"("Zh==")", R"("Zm=9")", R"("Zm9v=")",
             R"("Zm-v")", R"("Zm_v")", R"("Zm9v\n")", R"("Zm9=")", R"("====")",
             R"("Zg==AAAA")", R"("AA== ")", "null", "[1]", "123"}) {
        RUVIA_CHECK(!ruvia::fromJson<ruvia::Bytes>(input));
    }
}

RUVIA_TEST(model_json_bytes_supports_nested_arrays_nullable_and_omit_empty) {
    auto nested = ruvia::fromJson<BinaryArrayModel>(R"({"items":["AQI=",""],"boxed":["/w=="]})");
    RUVIA_CHECK(nested.has_value());
    if (nested) {
        RUVIA_CHECK_EQ(nested->get<"items">().size(), std::size_t{2});
        RUVIA_CHECK_EQ(nested->get<"items">()[0].view().size(), std::size_t{2});
        RUVIA_CHECK_EQ(nested->get<"boxed">().front().view().front(), std::uint8_t{255});
    }
    auto model = ruvia::fromJson<BinaryModel>(R"({"bytes":"AQI=","optional":null,"empty":""})");
    RUVIA_CHECK(model && model->isNull<"optional">());
    if (model) {
        RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*model)), R"({"bytes":"AQI=","optional":null})");
    }
}

RUVIA_TEST(model_json_bytes_rules_measure_decoded_bytes_only) {
    auto shortValue = ruvia::fromJson<BinaryRules>(R"({"bytes":"AQ=="})");
    auto validValue = ruvia::fromJson<BinaryRules>(R"({"bytes":"AQI="})");
    auto longValue = ruvia::fromJson<BinaryRules>(R"({"bytes":"AQIDBA=="})");
    RUVIA_CHECK(shortValue && validValue && longValue);
    if (shortValue && validValue && longValue) {
        ruvia::Validator validator;
        ruvia::detail::ModelValidationAccess::validateModel(*shortValue, validator);
        RUVIA_CHECK(!validator.ok());
        ruvia::Validator validValidator;
        ruvia::detail::ModelValidationAccess::validateModel(*validValue, validValidator);
        RUVIA_CHECK(validValidator.ok());
        ruvia::Validator longValidator;
        ruvia::detail::ModelValidationAccess::validateModel(*longValue, longValidator);
        RUVIA_CHECK(!longValidator.ok());
    }
}

RUVIA_TEST(model_bytes_set_and_move_normalize_resources) {
    std::pmr::monotonic_buffer_resource source;
    std::pmr::monotonic_buffer_resource target;
    BinaryModel model({.resource = &target});
    const std::uint8_t raw[] = {1, 2, 3};
    model.set<"bytes">(std::span<const std::uint8_t>(raw));
    RUVIA_CHECK_EQ(model.get<"bytes">().resource(), &target);
    std::pmr::vector<std::uint8_t> owned({4, 5}, &source);
    model.set<"bytes">(std::move(owned));
    RUVIA_CHECK_EQ(model.get<"bytes">().resource(), &target);
    RUVIA_CHECK_EQ(model.get<"bytes">().view()[1], std::uint8_t{5});
}

RUVIA_TEST(model_bytes_repeated_operations_reclaim_storage_and_keep_results) {
    ruvia::test::CountingMemoryResource memory;
    ruvia::test::CountingMemoryResource outputMemory;
    {
        BinaryModel retained({.resource = &memory});
        {
            ruvia::test::CountingMemoryResource source;
            {
                std::pmr::vector<std::uint8_t> input(256, 0xff, &source);
                retained.set<"bytes">(std::move(input));
            }
            RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
        }
        const auto outputBefore = outputMemory.allocationCount();
        const auto saved = ruvia::toJson(retained.get<"bytes">(), {.resource = &outputMemory});
        RUVIA_CHECK_EQ(outputMemory.allocationCount() - outputBefore, std::size_t{1});
        const auto baseline = memory.liveAllocations();
        const auto outputBaseline = outputMemory.liveAllocations();
        for (int i = 0; i < 32; ++i) {
            {
                auto parsed = ruvia::fromJson<ruvia::Bytes>(saved, {.resource = &memory});
                RUVIA_CHECK(parsed && *parsed == retained.get<"bytes">());
                if (parsed) {
                    const auto output = ruvia::toJson(*parsed, {.resource = &outputMemory});
                    RUVIA_CHECK_EQ(output, saved);
                }
                RUVIA_CHECK(!ruvia::fromJson<ruvia::Array<ruvia::Bytes>>(
                    R"(["AQIDBA==","Zh=="])", {.resource = &memory}));
            }
            RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
            RUVIA_CHECK_EQ(outputMemory.liveAllocations(), outputBaseline);
            RUVIA_CHECK_EQ(retained.get<"bytes">().view().front(), std::uint8_t{255});
        }
        std::pmr::vector<std::uint8_t> compatible(128, 7, &memory);
        const auto* original = compatible.data();
        retained.set<"bytes">(std::move(compatible));
        RUVIA_CHECK_EQ(retained.get<"bytes">().view().data(), original);
        retained.ensure<"bytes">().assignOwned(retained.get<"bytes">().view().subspan(1));
        RUVIA_CHECK_EQ(retained.get<"bytes">().size(), std::size_t{127});
        RUVIA_CHECK_EQ(retained.get<"bytes">().view().back(), std::uint8_t{7});
        RUVIA_CHECK(saved.size() > 256);
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(outputMemory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
    RUVIA_CHECK_EQ(outputMemory.allocationCount(), outputMemory.deallocationCount());
}

RUVIA_TEST(model_bytes_partial_parse_and_output_failures_release_storage) {
    constexpr auto input = R"(["AQIDBA==","\/w==","AAEC"] )";
    ruvia::test::CountingMemoryResource reference;
    {
        auto parsed = ruvia::fromJson<ruvia::Array<ruvia::Bytes>>(input, {.resource = &reference});
        RUVIA_CHECK(parsed && parsed->size() == 3);
    }
    RUVIA_CHECK_EQ(reference.liveAllocations(), std::size_t{0});
    const auto count = reference.allocationCount();
    RUVIA_CHECK(count > 1);
    for (std::size_t failure = 0; failure < count; ++failure) {
        BinaryFailingResource memory(failure);
        bool threw = false;
        try {
            (void)ruvia::fromJson<ruvia::Array<ruvia::Bytes>>(input, {.resource = &memory});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(memory.allocations.allocationCount(), failure);
        RUVIA_CHECK_EQ(memory.allocations.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocations.allocationCount(), memory.allocations.deallocationCount());
    }
    const std::vector<std::uint8_t> octets(256, 1);
    const ruvia::Bytes retained(octets);
    BinaryFailingResource output(0);
    bool threw = false;
    try {
        (void)ruvia::toJson(retained, {.resource = &output});
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(output.allocations.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(retained.size(), octets.size());
    RUVIA_CHECK(ruvia::toJson(retained).size() > 256);
}
