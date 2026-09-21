#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/Model.h"
#include "ruvia/web/Validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

std::size_t defaultEvaluations = 0;
std::size_t predicateEvaluations = 0;
std::size_t nestedDefaultEvaluations = 0;

std::string nestedDefault() {
    if (++nestedDefaultEvaluations == 2) {
        throw std::runtime_error("array default failed");
    }
    return std::string(256, 'd');
}

int codecDefault() {
    ++defaultEvaluations;
    return 7;
}

bool countedPredicate(const ruvia::String& value) {
    ++predicateEvaluations;
    return value.view() == "valid";
}

RUVIA_MODEL(CodecLeaf,
    RUVIA_REQUIRED_FIELD_NAME("wire\"id", id, ruvia::UInt64),
    RUVIA_OPTIONAL_FIELD(text, ruvia::String));
RUVIA_MODEL(CodecNode,
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(children, ruvia::BoxedArray<CodecNode>));
RUVIA_MODEL(CodecEnvelope,
    RUVIA_REQUIRED_FIELD(single, CodecLeaf),
    RUVIA_REQUIRED_FIELD(array, ruvia::Array<CodecLeaf>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::BoxedArray<CodecLeaf>),
    RUVIA_REQUIRED_FIELD(tree, CodecNode));
RUVIA_MODEL(CodecPresence,
    RUVIA_REQUIRED_FIELD(required, ruvia::String),
    RUVIA_OPTIONAL_FIELD(nullable, ruvia::String, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(defaulted, ruvia::Int32, RUVIA_DEFAULT(codecDefault())),
    RUVIA_OPTIONAL_FIELD(emitted, ruvia::String, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::String, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(CodecRules,
    RUVIA_REQUIRED_FIELD(value, ruvia::String, RUVIA_CUSTOM("not valid", countedPredicate)));
RUVIA_MODEL(CodecDynamic,
    RUVIA_REQUIRED_FIELD(value, ruvia::JsonValue),
    RUVIA_REQUIRED_FIELD(object, ruvia::JsonObject));
RUVIA_MODEL(CodecEmpty);
RUVIA_MODEL(CodecRootNested,
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_REQUIRED_FIELD(values, ruvia::Array<ruvia::String>));
RUVIA_MODEL(CodecRootTypes,
    RUVIA_REQUIRED_FIELD(child, CodecRootNested),
    RUVIA_REQUIRED_FIELD(flag, ruvia::Bool));
RUVIA_MODEL(CodecArrayDefault,
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(value, ruvia::String, RUVIA_DEFAULT(nestedDefault())));

class FailingResource final : public std::pmr::memory_resource {
public:
    explicit FailingResource(std::size_t remaining)
        : remaining_(remaining) {}

    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocations_.allocationCount();
    }

    [[nodiscard]] std::size_t liveAllocations() const noexcept {
        return allocations_.liveAllocations();
    }

    [[nodiscard]] std::size_t deallocationCount() const noexcept {
        return allocations_.deallocationCount();
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (remaining_ == 0) {
            throw std::bad_alloc();
        }
        --remaining_;
        return allocations_.allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        allocations_.deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t remaining_;
    ruvia::test::CountingMemoryResource allocations_;
};

}  // namespace

RUVIA_TEST(model_json_codec_supports_public_scalar_and_array_roots) {
    auto string = ruvia::fromJson<ruvia::String>(R"("escaped\\\"text")");
    RUVIA_CHECK(string && string->view() == "escaped\\\"text");
    auto integer = ruvia::fromJson<ruvia::Int64>("-9223372036854775808");
    RUVIA_CHECK(integer && integer->value == INT64_MIN);
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*integer)), "-9223372036854775808");
    auto boolean = ruvia::fromJson<ruvia::Bool>("true");
    RUVIA_CHECK(boolean && boolean->value);
    auto strings = ruvia::fromJson<ruvia::Array<ruvia::String>>(R"(["a","b"])");
    RUVIA_CHECK(strings && strings->size() == 2);
    auto boxed = ruvia::fromJson<ruvia::BoxedArray<ruvia::String>>(R"(["x",[1]])");
    RUVIA_CHECK(!boxed.has_value());
    RUVIA_CHECK(!ruvia::fromJson<ruvia::Int64>("1 trailing"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::String>("null"));
}

RUVIA_TEST(model_json_codec_rejects_nested_wrong_null_and_duplicate_fields) {
    RUVIA_CHECK(!ruvia::fromJson<CodecRootTypes>(R"({"child":{"name":null,"values":[]},"flag":true})"));
    RUVIA_CHECK(!ruvia::fromJson<CodecRootTypes>(R"({"child":{"name":"x","values":[1]},"flag":true})"));
    RUVIA_CHECK(!ruvia::fromJson<CodecRootTypes>(R"({"child":{"name":"x","values":[]},"flag":true,"flag":false})"));
    RUVIA_CHECK(!ruvia::fromJson<CodecRootTypes>(R"({"child":{"name":"x","values":[]},"flag":true}junk)"));
}

RUVIA_TEST(model_json_codec_root_array_failure_cleans_partial_elements) {
    constexpr std::string_view input = R"(["a long value","another value","last value"] )";
    ruvia::test::CountingMemoryResource successfulResource;
    auto successful = ruvia::fromJson<ruvia::Array<ruvia::String>>(input,
        {.resource = &successfulResource});
    RUVIA_CHECK(successful.has_value());
    if (!successful) {
        return;
    }
    const auto allocations = successfulResource.allocationCount();
    RUVIA_CHECK(allocations > 1);
    for (std::size_t failure = 1; failure <= allocations; ++failure) {
        FailingResource resource(failure - 1);
        bool threw = false;
        try {
            (void)ruvia::fromJson<ruvia::Array<ruvia::String>>(input, {.resource = &resource});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
    }
}

RUVIA_TEST(model_json_codec_root_model_arrays_validate_and_roundtrip_recursively) {
    constexpr auto input = R"([{"name":"root","children":[{"name":"child"}]}])";
    auto dense = ruvia::fromJson<ruvia::Array<CodecNode>>(input);
    auto boxed = ruvia::fromJson<ruvia::BoxedArray<CodecNode>>(input);
    RUVIA_CHECK(dense && boxed);
    if (dense && boxed) {
        RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*dense)), input);
        RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*boxed)), input);
    }
    for (const auto bad : {R"([{}])", R"([{"name":null}])",
             R"([{"name":"root","children":[{}]}])", R"([{"name":"a","name":"b"}])"}) {
        RUVIA_CHECK(!ruvia::fromJson<ruvia::Array<CodecNode>>(bad));
        RUVIA_CHECK(!ruvia::fromJson<ruvia::BoxedArray<CodecNode>>(bad));
    }
}

RUVIA_TEST(model_json_codec_array_default_exception_releases_previous_elements) {
    const std::string input = "[{\"name\":\"" + std::string(256, 'a') +
                              "\"},{\"name\":\"" + std::string(256, 'b') + "\"}]";
    const auto check = [&]<typename ArrayT>() {
        nestedDefaultEvaluations = 0;
        ruvia::test::CountingMemoryResource memory;
        bool threw = false;
        try {
            (void)ruvia::fromJson<ArrayT>(input, {.resource = &memory});
        } catch (const std::runtime_error& error) {
            threw = std::string_view(error.what()) == "array default failed";
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(nestedDefaultEvaluations, std::size_t{2});
        RUVIA_CHECK(memory.allocationCount() >= 3);
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
    };
    check.template operator()<ruvia::Array<CodecArrayDefault>>();
    check.template operator()<ruvia::BoxedArray<CodecArrayDefault>>();
}

RUVIA_TEST(model_json_codec_owned_root_string_survives_input_destruction) {
    ruvia::test::CountingMemoryResource resource;
    std::string input = R"("retained")";
    auto parsed = ruvia::fromJson<ruvia::String>(input, {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    input.assign(input.size(), 'x');
    if (parsed) {
        RUVIA_CHECK_EQ(parsed->view(), "retained");
    }
}

RUVIA_TEST(model_json_codec_roundtrips_nested_and_recursive_models) {
    constexpr std::string_view input =
        R"({"single":{"wire\"id":18446744073709551615,"text":"a\\b\"c"},"array":[{"wire\"id":1}],"boxed":[{"wire\"id":2,"text":"two"}],"tree":{"name":"root","children":[{"name":"leaf","children":[{"name":"end"}]}]}})";
    auto parsed = ruvia::fromJson<CodecEnvelope>(input);
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    const auto& single = parsed->get<"single">();
    const auto& tree = parsed->get<"tree">();
    RUVIA_CHECK_EQ(single.get<"id">().value, std::uint64_t{18446744073709551615ULL});
    RUVIA_CHECK_EQ(single.get<"text">()->view(), "a\\b\"c");
    RUVIA_CHECK_EQ(parsed->get<"array">().size(), std::size_t{1});
    RUVIA_CHECK_EQ(parsed->get<"boxed">().size(), std::size_t{1});
    RUVIA_CHECK_EQ(tree.get<"children">()->front().get<"name">().view(), "leaf");

    const auto output = ruvia::toJson(*parsed);
    RUVIA_CHECK_EQ(std::string_view(output), input);
    auto reparsed = ruvia::fromJson<CodecEnvelope>(output);
    RUVIA_CHECK(reparsed.has_value());
    if (reparsed) {
        RUVIA_CHECK_EQ(reparsed->get<"single">().get<"id">().value,
            std::uint64_t{18446744073709551615ULL});
        RUVIA_CHECK_EQ(reparsed->get<"tree">().get<"children">()->front().get<"name">().view(),
            "leaf");
    }
}

RUVIA_TEST(model_json_codec_handles_presence_defaults_and_serialization_options) {
    defaultEvaluations = 0;
    auto missing = ruvia::fromJson<CodecPresence>(R"({"required":"x"})");
    RUVIA_CHECK(missing.has_value());
    if (!missing) {
        return;
    }
    RUVIA_CHECK_EQ(defaultEvaluations, std::size_t{1});
    RUVIA_CHECK(!missing->isPresent<"defaulted">());
    RUVIA_CHECK(!missing->isPresent<"nullable">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*missing)),
        std::string_view(R"({"required":"x","defaulted":7,"emitted":null})"));
    RUVIA_CHECK_EQ(defaultEvaluations, std::size_t{1});

    const auto serialized = ruvia::toJson(*missing);
    const std::string roundtripInput(serialized);
    auto roundtrip = ruvia::fromJson<CodecPresence>(roundtripInput);
    RUVIA_CHECK(roundtrip && roundtrip->isPresent<"defaulted">());
    if (roundtrip) {
        RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*roundtrip)),
            std::string_view(R"({"required":"x","defaulted":7,"emitted":null})"));
    }

    auto values = ruvia::fromJson<CodecPresence>(
        R"({"required":"x","nullable":null,"empty":""})");
    RUVIA_CHECK(values.has_value());
    if (!values) {
        return;
    }
    RUVIA_CHECK(values->isPresent<"nullable">() && values->isNull<"nullable">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*values)),
        std::string_view(R"({"required":"x","nullable":null,"defaulted":7,"emitted":null})"));
}

RUVIA_TEST(model_json_codec_does_not_validate_until_validation_access) {
    predicateEvaluations = 0;
    auto parsed = ruvia::fromJson<CodecRules>(R"({"value":"invalid"})");
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK_EQ(predicateEvaluations, std::size_t{0});
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*parsed)), R"({"value":"invalid"})");
    RUVIA_CHECK_EQ(predicateEvaluations, std::size_t{0});

    ruvia::Validator validator;
    ruvia::detail::ModelValidationAccess::validateModel(*parsed, validator);
    RUVIA_CHECK_EQ(predicateEvaluations, std::size_t{1});
    RUVIA_CHECK(!validator.ok());
}

RUVIA_TEST(model_json_codec_owns_dynamic_tokens_and_preserves_retained_results) {
    ruvia::test::CountingMemoryResource modelResource;
    ruvia::test::CountingMemoryResource outputResource;
    const auto modelBaseline = modelResource.liveAllocations();
    const auto outputBaseline = outputResource.liveAllocations();
    {
        CodecDynamic retained({.resource = &modelResource});
        std::pmr::string retainedJson(&outputResource);
        std::string input = R"({"value":{"keep":")";
        input.append(256, 'q');
        input += R"("},"object":{"n":1}})";
        auto parsed = ruvia::fromJson<CodecDynamic>(input, {.resource = &modelResource});
        RUVIA_CHECK(parsed.has_value());
        if (!parsed) {
            return;
        }
        retained = std::move(*parsed);
        input.assign(input.size(), 'x');
        std::string expectedToken = R"({"keep":")";
        expectedToken.append(256, 'q');
        expectedToken += R"("})";
        RUVIA_CHECK_EQ(retained.get<"value">().view(), expectedToken);
        retainedJson = ruvia::toJson(retained, {.resource = &outputResource});
        const auto saved = std::string(retainedJson);
        const auto retainedModelLive = modelResource.liveAllocations();
        const auto retainedOutputLive = outputResource.liveAllocations();
        const std::string temporaryInput = "{\"value\":\"" + std::string(160, 't') +
                                           "\",\"object\":{\"later\":true}}";
        for (std::size_t i = 0; i < 32; ++i) {
            {
                auto temporary = ruvia::fromJson<CodecDynamic>(temporaryInput, {.resource = &modelResource});
                RUVIA_CHECK(temporary.has_value());
                if (temporary) {
                    RUVIA_CHECK(modelResource.liveAllocations() > retainedModelLive);
                    const auto output = ruvia::toJson(*temporary, {.resource = &outputResource});
                    RUVIA_CHECK(outputResource.liveAllocations() > retainedOutputLive);
                    RUVIA_CHECK_EQ(std::string_view(output), temporaryInput);
                }
            }
            RUVIA_CHECK_EQ(retained.get<"value">().view(), expectedToken);
            RUVIA_CHECK_EQ(std::string_view(retainedJson), saved);
            RUVIA_CHECK_EQ(modelResource.liveAllocations(), retainedModelLive);
            RUVIA_CHECK_EQ(outputResource.liveAllocations(), retainedOutputLive);
        }
        const std::string invalidInput = "{\"value\":\"" + std::string(160, 'i') + "\",\"object\":[]}";
        for (std::size_t i = 0; i < 32; ++i) {
            const auto allocated = modelResource.allocationCount();
            auto invalid = ruvia::fromJson<CodecDynamic>(invalidInput, {.resource = &modelResource});
            RUVIA_CHECK(!invalid.has_value());
            RUVIA_CHECK(modelResource.allocationCount() > allocated);
            RUVIA_CHECK_EQ(modelResource.liveAllocations(), retainedModelLive);
            RUVIA_CHECK_EQ(outputResource.liveAllocations(), retainedOutputLive);
        }
    }
    RUVIA_CHECK_EQ(modelResource.liveAllocations(), modelBaseline);
    RUVIA_CHECK_EQ(outputResource.liveAllocations(), outputBaseline);
    RUVIA_CHECK_EQ(modelResource.allocationCount(), modelResource.deallocationCount());
    RUVIA_CHECK_EQ(outputResource.allocationCount(), outputResource.deallocationCount());
}

RUVIA_TEST(model_json_codec_releases_allocations_when_parse_or_serialize_fails) {
    std::string input = R"({"single":{"wire\"id":9,"text":")";
    input.append(192, 'x');
    input += R"("},"array":[{"wire\"id":1}],"boxed":[{"wire\"id":2}],"tree":{"name":"root","children":[{"name":"leaf","children":[{"name":"end"}]}]}})";
    ruvia::test::CountingMemoryResource successfulResource;
    auto successful = ruvia::fromJson<CodecEnvelope>(input, {.resource = &successfulResource});
    RUVIA_CHECK(successful.has_value());
    if (!successful) {
        return;
    }
    const auto successfulAllocations = successfulResource.allocationCount();
    RUVIA_CHECK(successfulAllocations > 1);
    bool sawPartialConstructionFailure = false;
    for (std::size_t failure = 1; failure <= successfulAllocations; ++failure) {
        FailingResource resource(failure - 1);
        bool failed = false;
        try {
            (void)ruvia::fromJson<CodecEnvelope>(input, {.resource = &resource});
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.allocationCount(), failure - 1);
        sawPartialConstructionFailure = sawPartialConstructionFailure || resource.allocationCount() > 0;
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
    }
    RUVIA_CHECK(sawPartialConstructionFailure);

    auto model = ruvia::fromJson<CodecDynamic>(R"({"value":{"kept":true},"object":{}})");
    RUVIA_CHECK(model.has_value());
    if (!model) {
        return;
    }
    const auto before = std::string(ruvia::toJson(*model));
    FailingResource outputResource(0);
    bool serializeFailed = false;
    try {
        (void)ruvia::toJson(*model, {.resource = &outputResource});
    } catch (const std::bad_alloc&) {
        serializeFailed = true;
    }
    RUVIA_CHECK(serializeFailed);
    RUVIA_CHECK_EQ(outputResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(*model)), before);
}

RUVIA_TEST(model_json_codec_roundtrips_empty_schema_and_rejects_non_object_root) {
    auto parsed = ruvia::fromJson<CodecEmpty>(R"({})");
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    const auto output = ruvia::toJson(*parsed);
    RUVIA_CHECK_EQ(std::string_view(output), "{}");
    auto reparsed = ruvia::fromJson<CodecEmpty>(output);
    RUVIA_CHECK(reparsed.has_value());
    auto rejected = ruvia::fromJson<CodecEmpty>(R"([])");
    RUVIA_CHECK(!rejected.has_value());
}
