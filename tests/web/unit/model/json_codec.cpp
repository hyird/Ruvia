#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

std::size_t default_evaluations = 0;
std::size_t predicate_evaluations = 0;
std::size_t nested_default_evaluations = 0;

std::string nested_default() {
    if (++nested_default_evaluations == 2) {
        throw std::runtime_error("array default failed");
    }
    return std::string(256, 'd');
}

int codec_default() {
    ++default_evaluations;
    return 7;
}

bool counted_predicate(const ruvia::string& value) {
    ++predicate_evaluations;
    return value.view() == "valid";
}

RUVIA_MODEL(codec_leaf,
    RUVIA_REQUIRED_FIELD_NAME("wire\"id", id, ruvia::uint64),
    RUVIA_OPTIONAL_FIELD(text, ruvia::string));
RUVIA_MODEL(codec_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<codec_node>));
RUVIA_MODEL(codec_envelope,
    RUVIA_REQUIRED_FIELD(single, codec_leaf),
    RUVIA_REQUIRED_FIELD(array, ruvia::array<codec_leaf>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<codec_leaf>),
    RUVIA_REQUIRED_FIELD(tree, codec_node));
RUVIA_MODEL(codec_presence,
    RUVIA_REQUIRED_FIELD(required, ruvia::string),
    RUVIA_OPTIONAL_FIELD(nullable, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(defaulted, ruvia::int32, RUVIA_DEFAULT(codec_default())),
    RUVIA_OPTIONAL_FIELD(emitted, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(codec_rules,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_CUSTOM("not valid", counted_predicate)));
RUVIA_MODEL(codec_dynamic,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value),
    RUVIA_REQUIRED_FIELD(object, ruvia::json_object));
RUVIA_MODEL(codec_empty);
RUVIA_MODEL(codec_root_nested,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(values, ruvia::array<ruvia::string>));
RUVIA_MODEL(codec_root_types,
    RUVIA_REQUIRED_FIELD(child, codec_root_nested),
    RUVIA_REQUIRED_FIELD(flag, ruvia::bool_value));
RUVIA_MODEL(codec_array_default,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT(nested_default())));

struct forward_branch;
RUVIA_MODEL(forward_root,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(branches, ruvia::array<forward_branch>));
RUVIA_MODEL(forward_branch,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(roots, ruvia::boxed_array<forward_root>));

class failing_resource final : public std::pmr::memory_resource {
public:
    explicit failing_resource(std::size_t remaining)
        : remaining_(remaining) {}

    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocations_.allocation_count();
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return allocations_.live_allocations();
    }

    [[nodiscard]] std::size_t deallocation_count() const noexcept {
        return allocations_.deallocation_count();
    }

    [[nodiscard]] std::size_t payload_allocations() const noexcept {
        return payload_allocations_;
    }

private:
    // See binary_failing_resource: MSVC debug proxy nodes must not be the
    // failure point. Payload buffers used below are larger than this cutoff.
    static constexpr std::size_t min_payload_bytes = 64;

    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value >= min_payload_bytes) {
            if (remaining_ == 0) {
                throw std::bad_alloc();
            }
            --remaining_;
            ++payload_allocations_;
        }
        return allocations_.allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        allocations_.deallocate(pointer, bytes_value, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t remaining_;
    std::size_t payload_allocations_{0};
    ruvia::test::counting_memory_resource allocations_;
};

}  // namespace

RUVIA_TEST(model_json_codec_supports_public_scalar_and_array_roots) {
    auto string_value = ruvia::from_json<ruvia::string>(R"("escaped\\\"text")");
    RUVIA_CHECK(string_value && string_value->view() == "escaped\\\"text");
    auto integer = ruvia::from_json<ruvia::int64>("-9223372036854775808");
    RUVIA_CHECK(integer && integer->value_ == INT64_MIN);
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*integer)), "-9223372036854775808");
    auto boolean = ruvia::from_json<ruvia::bool_value>("true");
    RUVIA_CHECK(boolean && boolean->value_);
    auto strings = ruvia::from_json<ruvia::array<ruvia::string>>(R"(["a","b"])");
    RUVIA_CHECK(strings && strings->size() == 2);
    auto boxed = ruvia::from_json<ruvia::boxed_array<ruvia::string>>(R"(["x",[1]])");
    RUVIA_CHECK(!boxed.has_value());
    RUVIA_CHECK(!ruvia::from_json<ruvia::int64>("1 trailing"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::string>("null"));
}

RUVIA_TEST(model_json_codec_rejects_nested_wrong_null_and_duplicate_fields) {
    RUVIA_CHECK(!ruvia::from_json<codec_root_types>(R"({"child":{"name":null,"values":[]},"flag":true})"));
    RUVIA_CHECK(!ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[1]},"flag":true})"));
    RUVIA_CHECK(!ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[]},"flag":true,"flag":false})"));
    RUVIA_CHECK(!ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[]},"flag":true}junk)"));
}

RUVIA_TEST(model_json_codec_root_array_failure_cleans_partial_elements) {
    const std::string input = "[\"" + std::string(80, 'a') + "\",\"" + std::string(80, 'b') +
                              "\",\"" + std::string(80, 'c') + "\"] ";
    failing_resource successful_resource((std::numeric_limits<std::size_t>::max)());
    auto successful = ruvia::from_json<ruvia::array<ruvia::string>>(input,
        {.resource_ = &successful_resource});
    RUVIA_CHECK(successful.has_value());
    if (!successful) {
        return;
    }
    const auto allocations = successful_resource.payload_allocations();
    RUVIA_CHECK(allocations > 1);
    for (std::size_t failure = 1; failure <= allocations; ++failure) {
        failing_resource resource(failure - 1);
        bool threw = false;
        try {
            (void)ruvia::from_json<ruvia::array<ruvia::string>>(input, {.resource_ = &resource});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(resource.payload_allocations(), failure - 1);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
    }
}

RUVIA_TEST(model_json_codec_root_model_arrays_validate_and_roundtrip_recursively) {
    constexpr auto input = R"([{"name":"root","children":[{"name":"child"}]}])";
    auto dense = ruvia::from_json<ruvia::array<codec_node>>(input);
    auto boxed = ruvia::from_json<ruvia::boxed_array<codec_node>>(input);
    RUVIA_CHECK(dense && boxed);
    if (dense && boxed) {
        RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*dense)), input);
        RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*boxed)), input);
    }
    for (const auto bad : {R"([{}])", R"([{"name":null}])",
             R"([{"name":"root","children":[{}]}])", R"([{"name":"a","name":"b"}])"}) {
        RUVIA_CHECK(!ruvia::from_json<ruvia::array<codec_node>>(bad));
        RUVIA_CHECK(!ruvia::from_json<ruvia::boxed_array<codec_node>>(bad));
    }
}

RUVIA_TEST(model_json_codec_forward_declared_graph_owns_nested_destination_values) {
    ruvia::test::counting_memory_resource destination;
    forward_root retained({.resource_ = &destination});
    constexpr std::string_view input =
        R"({"name":"root","branches":[{"name":"branch","roots":[{"name":"leaf"}]}]})";
    {
        std::pmr::monotonic_buffer_resource source;
        auto parsed_value = ruvia::from_json<forward_root>(input, {.resource_ = &source});
        RUVIA_CHECK(parsed_value.has_value());
        if (!parsed_value) {
            return;
        }
        retained = std::move(*parsed_value);
    }
    const auto& branches = *retained.get<"branches">();
    const auto& branch = branches.front();
    const auto& roots = *branch.get<"roots">();
    const auto& leaf = roots.front();
    RUVIA_CHECK_EQ(retained.resource(), &destination);
    RUVIA_CHECK_EQ(branches.resource(), &destination);
    RUVIA_CHECK_EQ(branch.resource(), &destination);
    RUVIA_CHECK_EQ(branch.get<"name">().resource(), &destination);
    RUVIA_CHECK_EQ(roots.resource(), &destination);
    RUVIA_CHECK_EQ(leaf.resource(), &destination);
    RUVIA_CHECK_EQ(leaf.get<"name">().resource(), &destination);
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(retained)), input);
}

RUVIA_TEST(model_json_codec_array_default_exception_releases_previous_elements) {
    const std::string input = "[{\"name\":\"" + std::string(256, 'a') +
                              "\"},{\"name\":\"" + std::string(256, 'b') + "\"}]";
    const auto check = [&]<typename array_t_type>() {
        nested_default_evaluations = 0;
        ruvia::test::counting_memory_resource memory;
        bool threw = false;
        try {
            (void)ruvia::from_json<array_t_type>(input, {.resource_ = &memory});
        } catch (const std::runtime_error& error) {
            threw = std::string_view(error.what()) == "array default failed";
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(nested_default_evaluations, std::size_t{2});
        RUVIA_CHECK(memory.allocation_count() >= 3);
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
    };
    check.template operator()<ruvia::array<codec_array_default>>();
    check.template operator()<ruvia::boxed_array<codec_array_default>>();
}

RUVIA_TEST(model_json_codec_owned_root_string_survives_input_destruction) {
    ruvia::test::counting_memory_resource resource;
    std::string input = R"("retained")";
    auto parsed_value = ruvia::from_json<ruvia::string>(input, {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    input.assign(input.size(), 'x');
    if (parsed_value) {
        RUVIA_CHECK_EQ(parsed_value->view(), "retained");
    }
}

RUVIA_TEST(model_json_codec_roundtrips_nested_and_recursive_models) {
    constexpr std::string_view input =
        R"({"single":{"wire\"id":18446744073709551615,"text":"a\\b\"c"},"array":[{"wire\"id":1}],"boxed":[{"wire\"id":2,"text":"two"}],"tree":{"name":"root","children":[{"name":"leaf","children":[{"name":"end"}]}]}})";
    auto parsed_value = ruvia::from_json<codec_envelope>(input);
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    const auto& single = parsed_value->get<"single">();
    const auto& tree = parsed_value->get<"tree">();
    RUVIA_CHECK_EQ(single.get<"id">().value_, std::uint64_t{18446744073709551615ULL});
    RUVIA_CHECK_EQ(single.get<"text">()->view(), "a\\b\"c");
    RUVIA_CHECK_EQ(parsed_value->get<"array">().size(), std::size_t{1});
    RUVIA_CHECK_EQ(parsed_value->get<"boxed">().size(), std::size_t{1});
    RUVIA_CHECK_EQ(tree.get<"children">()->front().get<"name">().view(), "leaf");

    const auto output = ruvia::to_json(*parsed_value);
    RUVIA_CHECK_EQ(std::string_view(output), input);
    auto reparsed = ruvia::from_json<codec_envelope>(output);
    RUVIA_CHECK(reparsed.has_value());
    if (reparsed) {
        RUVIA_CHECK_EQ(reparsed->get<"single">().get<"id">().value_,
            std::uint64_t{18446744073709551615ULL});
        RUVIA_CHECK_EQ(reparsed->get<"tree">().get<"children">()->front().get<"name">().view(),
            "leaf");
    }
}

RUVIA_TEST(model_json_codec_handles_presence_defaults_and_serialization_options) {
    default_evaluations = 0;
    auto missing = ruvia::from_json<codec_presence>(R"({"required":"x"})");
    RUVIA_CHECK(missing.has_value());
    if (!missing) {
        return;
    }
    RUVIA_CHECK_EQ(default_evaluations, std::size_t{1});
    RUVIA_CHECK(!missing->is_present<"defaulted">());
    RUVIA_CHECK(!missing->is_present<"nullable">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*missing)),
        std::string_view(R"({"required":"x","defaulted":7,"emitted":null})"));
    RUVIA_CHECK_EQ(default_evaluations, std::size_t{1});

    const auto serialized = ruvia::to_json(*missing);
    const std::string roundtrip_input(serialized);
    auto roundtrip = ruvia::from_json<codec_presence>(roundtrip_input);
    RUVIA_CHECK(roundtrip && roundtrip->is_present<"defaulted">());
    if (roundtrip) {
        RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*roundtrip)),
            std::string_view(R"({"required":"x","defaulted":7,"emitted":null})"));
    }

    auto values = ruvia::from_json<codec_presence>(
        R"({"required":"x","nullable":null,"empty":""})");
    RUVIA_CHECK(values.has_value());
    if (!values) {
        return;
    }
    RUVIA_CHECK(values->is_present<"nullable">() && values->is_null<"nullable">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*values)),
        std::string_view(R"({"required":"x","nullable":null,"defaulted":7,"emitted":null})"));
}

RUVIA_TEST(model_json_codec_does_not_validate_until_validation_access) {
    predicate_evaluations = 0;
    auto parsed_value = ruvia::from_json<codec_rules>(R"({"value":"invalid"})");
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK_EQ(predicate_evaluations, std::size_t{0});
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*parsed_value)), R"({"value":"invalid"})");
    RUVIA_CHECK_EQ(predicate_evaluations, std::size_t{0});

    ruvia::validator validator;
    ruvia::detail::model_validation_access::validate_model(*parsed_value, validator);
    RUVIA_CHECK_EQ(predicate_evaluations, std::size_t{1});
    RUVIA_CHECK(!validator.ok());
}

RUVIA_TEST(model_json_codec_owns_dynamic_tokens_and_preserves_retained_results) {
    ruvia::test::counting_memory_resource model_resource;
    ruvia::test::counting_memory_resource output_resource;
    const auto model_baseline = model_resource.live_allocations();
    const auto output_baseline = output_resource.live_allocations();
    {
        codec_dynamic retained({.resource_ = &model_resource});
        std::pmr::string retained_json(&output_resource);
        std::string input = R"({"value":{"keep":")";
        input.append(256, 'q');
        input += R"("},"object":{"n":1}})";
        auto parsed_value = ruvia::from_json<codec_dynamic>(input, {.resource_ = &model_resource});
        RUVIA_CHECK(parsed_value.has_value());
        if (!parsed_value) {
            return;
        }
        retained = std::move(*parsed_value);
        input.assign(input.size(), 'x');
        std::string expected_token = R"({"keep":")";
        expected_token.append(256, 'q');
        expected_token += R"("})";
        RUVIA_CHECK_EQ(retained.get<"value">().view(), expected_token);
        retained_json = ruvia::to_json(retained, {.resource_ = &output_resource});
        const auto saved = std::string(retained_json);
        const auto retained_model_live = model_resource.live_allocations();
        const auto retained_output_live = output_resource.live_allocations();
        const std::string temporary_input = "{\"value\":\"" + std::string(160, 't') +
                                            "\",\"object\":{\"later\":true}}";
        for (std::size_t i = 0; i < 32; ++i) {
            {
                auto temporary = ruvia::from_json<codec_dynamic>(temporary_input, {.resource_ = &model_resource});
                RUVIA_CHECK(temporary.has_value());
                if (temporary) {
                    RUVIA_CHECK(model_resource.live_allocations() > retained_model_live);
                    const auto output = ruvia::to_json(*temporary, {.resource_ = &output_resource});
                    RUVIA_CHECK(output_resource.live_allocations() > retained_output_live);
                    RUVIA_CHECK_EQ(std::string_view(output), temporary_input);
                }
            }
            RUVIA_CHECK_EQ(retained.get<"value">().view(), expected_token);
            RUVIA_CHECK_EQ(std::string_view(retained_json), saved);
            RUVIA_CHECK_EQ(model_resource.live_allocations(), retained_model_live);
            RUVIA_CHECK_EQ(output_resource.live_allocations(), retained_output_live);
        }
        const std::string invalid_input = "{\"value\":\"" + std::string(160, 'i') + "\",\"object\":[]}";
        for (std::size_t i = 0; i < 32; ++i) {
            const auto allocated = model_resource.allocation_count();
            auto invalid = ruvia::from_json<codec_dynamic>(invalid_input, {.resource_ = &model_resource});
            RUVIA_CHECK(!invalid.has_value());
            RUVIA_CHECK(model_resource.allocation_count() > allocated);
            RUVIA_CHECK_EQ(model_resource.live_allocations(), retained_model_live);
            RUVIA_CHECK_EQ(output_resource.live_allocations(), retained_output_live);
        }
    }
    RUVIA_CHECK_EQ(model_resource.live_allocations(), model_baseline);
    RUVIA_CHECK_EQ(output_resource.live_allocations(), output_baseline);
    RUVIA_CHECK_EQ(model_resource.allocation_count(), model_resource.deallocation_count());
    RUVIA_CHECK_EQ(output_resource.allocation_count(), output_resource.deallocation_count());
}

RUVIA_TEST(model_json_codec_releases_allocations_when_parse_or_serialize_fails) {
    std::string input = R"({"single":{"wire\"id":9,"text":")";
    input.append(192, 'x');
    input += R"("},"array":[{"wire\"id":1,"text":")";
    input.append(80, 'y');
    input += R"("}],"boxed":[{"wire\"id":2}],"tree":{"name":"root","children":[{"name":"leaf","children":[{"name":"end"}]}]}})";
    failing_resource successful_resource((std::numeric_limits<std::size_t>::max)());
    auto successful = ruvia::from_json<codec_envelope>(input, {.resource_ = &successful_resource});
    RUVIA_CHECK(successful.has_value());
    if (!successful) {
        return;
    }
    const auto successful_allocations = successful_resource.payload_allocations();
    RUVIA_CHECK(successful_allocations > 1);
    bool saw_partial_construction_failure = false;
    for (std::size_t failure = 1; failure <= successful_allocations; ++failure) {
        failing_resource resource(failure - 1);
        bool failed = false;
        try {
            (void)ruvia::from_json<codec_envelope>(input, {.resource_ = &resource});
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.payload_allocations(), failure - 1);
        saw_partial_construction_failure =
            saw_partial_construction_failure || resource.payload_allocations() > 0;
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
    }
    RUVIA_CHECK(saw_partial_construction_failure);

    const std::string kept(80, 'k');
    auto model = ruvia::from_json<codec_dynamic>(
        "{\"value\":{\"kept\":\"" + kept + "\"},\"object\":{}}");
    RUVIA_CHECK(model.has_value());
    if (!model) {
        return;
    }
    const auto before = std::string(ruvia::to_json(*model));
    failing_resource output_resource(0);
    bool serialize_failed = false;
    try {
        (void)ruvia::to_json(*model, {.resource_ = &output_resource});
    } catch (const std::bad_alloc&) {
        serialize_failed = true;
    }
    RUVIA_CHECK(serialize_failed);
    RUVIA_CHECK_EQ(output_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(*model)), before);
}

RUVIA_TEST(model_json_codec_roundtrips_empty_schema_and_rejects_non_object_root) {
    auto parsed_value = ruvia::from_json<codec_empty>(R"({})");
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    const auto output = ruvia::to_json(*parsed_value);
    RUVIA_CHECK_EQ(std::string_view(output), "{}");
    auto reparsed = ruvia::from_json<codec_empty>(output);
    RUVIA_CHECK(reparsed.has_value());
    auto rejected = ruvia::from_json<codec_empty>(R"([])");
    RUVIA_CHECK(!rejected.has_value());
}

RUVIA_TEST(json_array_limits_apply_across_nested_collections_and_model_fields) {
    using nested_array = ruvia::array<ruvia::array<ruvia::int32>>;
    RUVIA_CHECK(ruvia::from_json<nested_array>("[[1,2],[3]]", {.max_array_elements_ = 5}));
    RUVIA_CHECK(!ruvia::from_json<nested_array>("[[1,2],[3]]", {.max_array_elements_ = 4}));
    RUVIA_CHECK(ruvia::from_json<nested_array>("[]", {.max_array_elements_ = 0}));
    RUVIA_CHECK(!ruvia::from_json<nested_array>("[[]]", {.max_array_elements_ = 0}));

    constexpr auto tree = R"({"name":"a","children":[{"name":"b","children":[{"name":"c"}]},{"name":"d"}]})";
    RUVIA_CHECK(ruvia::from_json<codec_node>(tree, {.max_array_elements_ = 3}));
    RUVIA_CHECK(!ruvia::from_json<codec_node>(tree, {.max_array_elements_ = 2}));
}

RUVIA_TEST(json_representation_budget_bounds_owned_strings_and_array_storage) {
    using array = ruvia::array<ruvia::int32>;
    constexpr auto two_elements = sizeof(array) + 2 * 4 * (sizeof(ruvia::int32) + sizeof(ruvia::int32*));
    RUVIA_CHECK(ruvia::from_json<array>("[1,2]", {.max_representation_bytes_ = two_elements}));
    RUVIA_CHECK(!ruvia::from_json<array>("[1,2,3]", {.max_representation_bytes_ = two_elements}));
    RUVIA_CHECK(!ruvia::from_json<ruvia::int32>("1", {.max_representation_bytes_ = 0}));

    const std::string text = "\"" + std::string(4096, 'x') + "\"";
    RUVIA_CHECK(!ruvia::from_json<ruvia::string>(text, {.max_representation_bytes_ = 1024}));
    RUVIA_CHECK(ruvia::from_json<ruvia::string>(text, {.max_representation_bytes_ = 16384}));
}

RUVIA_TEST(json_budget_failure_releases_partial_results_and_preserves_retained_values) {
    ruvia::test::counting_memory_resource memory;
    {
        auto retained = ruvia::from_json<ruvia::array<ruvia::string>>(
            R"(["retained value with owned storage"])", {.resource_ = &memory});
        RUVIA_CHECK(retained.has_value());
        const auto retained_allocations = memory.live_allocations();
        for (std::size_t index = 0; index < 8; ++index) {
            auto failed = ruvia::from_json<ruvia::boxed_array<ruvia::string>>(
                R"(["first element with owned storage","second element with owned storage"])",
                {.resource_ = &memory, .max_array_elements_ = 1});
            RUVIA_CHECK(!failed);
            RUVIA_CHECK_EQ(memory.live_allocations(), retained_allocations);
            RUVIA_CHECK_EQ((*retained)[0].view(), std::string_view("retained value with owned storage"));
        }
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

RUVIA_TEST(request_json_binding_enforces_default_aggregate_array_limit) {
    std::string input = R"({"name":"node","values":[)";
    for (std::size_t index = 0; index < 64 * 1024 + 1; ++index) {
        if (index != 0) {
            input += ',';
        }
        input += R"("")";
    }
    input += "]}";
    ruvia::test::counting_memory_resource memory;
    RUVIA_CHECK(!ruvia::detail::model_parse_access::parse_json_borrowed_partial<codec_root_nested>(input, &memory));
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}
