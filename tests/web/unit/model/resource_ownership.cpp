#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_MODEL(resource_child,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));
RUVIA_MODEL(resource_parent,
    RUVIA_REQUIRED_FIELD(title, ruvia::string),
    RUVIA_OPTIONAL_FIELD(child, resource_child),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<resource_child>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<resource_child>));
RUVIA_MODEL(resource_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<resource_node>));
RUVIA_MODEL(resource_dense_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<resource_dense_node>));
RUVIA_MODEL(resource_pair,
    RUVIA_REQUIRED_FIELD(first, ruvia::string),
    RUVIA_REQUIRED_FIELD(second, ruvia::string));

class reject_after_resource final : public std::pmr::memory_resource {
public:
    std::optional<std::size_t> remaining_;
    ruvia::test::counting_memory_resource allocations_;

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // Skip MSVC debug iterator-proxy nodes. See binary_failing_resource.
        if (remaining_ && bytes_value >= 64) {
            if (*remaining_ == 0) {
                throw std::bad_alloc();
            }
            --*remaining_;
        }
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

RUVIA_TEST(model_resource_assignment_recursively_outlives_source_owner) {
    ruvia::test::counting_memory_resource target_resource;
    const std::string text(160, 'a');
    {
        resource_parent target({.resource_ = &target_resource});
        {
            std::pmr::monotonic_buffer_resource source_resource;
            target.set<"title">(ruvia::string(text, {.resource_ = &source_resource}));
            resource_child source_value({.resource_ = &source_resource});
            source_value.set<"name">(text);
            source_value.ensure<"tags">().emplace_back(text);
            target.set<"child">(std::move(source_value));
            resource_child other({.resource_ = &source_resource});
            other.set<"name">(text);
            other.ensure<"tags">().emplace_back(text);
            // Public insertion accepts a const model and owns it recursively.
            const auto& borrowed = other;
            target.ensure<"children">().emplace_back(borrowed);
            target.ensure<"boxed">().emplace(std::move(other));
        }
        RUVIA_CHECK_EQ(target.get<"title">().resource(), &target_resource);
        RUVIA_CHECK_EQ(target.get<"title">().view(), std::string_view(text));
        const auto verify = [&](const resource_child& child_value) {
            RUVIA_CHECK_EQ(child_value.resource(), &target_resource);
            RUVIA_CHECK_EQ(child_value.get<"name">().resource(), &target_resource);
            RUVIA_CHECK_EQ(child_value.get<"name">().view(), std::string_view(text));
            RUVIA_CHECK_EQ(child_value.get<"tags">()->resource(), &target_resource);
            RUVIA_CHECK_EQ(child_value.get<"tags">()->front().resource(), &target_resource);
            RUVIA_CHECK_EQ(child_value.get<"tags">()->front().view(), std::string_view(text));
        };
        verify(*target.get<"child">());
        verify(target.get<"children">()->front());
        verify(target.get<"boxed">()->front());
    }
    RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(target_resource.allocation_count(), target_resource.deallocation_count());
}

RUVIA_TEST(model_resource_array_construction_and_mutable_assignment_keep_owner) {
    ruvia::test::counting_memory_resource target_resource;
    const std::string text(160, 'b');
    {
        ruvia::array<ruvia::string> strings({.resource_ = &target_resource});
        strings.emplace_back(text);
        strings.emplace_back().assign_owned(text);
        ruvia::array<resource_child> children({.resource_ = &target_resource});
        children.emplace_back().set<"name">(text);
        ruvia::boxed_array<ruvia::string> boxed({.resource_ = &target_resource});
        boxed.emplace(text);
        {
            std::pmr::monotonic_buffer_resource temporary;
            strings.front() = ruvia::string(text, {.resource_ = &temporary});
            resource_child replacement({.resource_ = &temporary});
            replacement.set<"name">(text);
            children.front() = std::move(replacement);
            boxed.front() = ruvia::string(text, {.resource_ = &temporary});
        }
        for (const auto& value : strings) {
            RUVIA_CHECK_EQ(value.resource(), &target_resource);
            RUVIA_CHECK_EQ(value.view(), std::string_view(text));
        }
        RUVIA_CHECK_EQ(children.front().resource(), &target_resource);
        RUVIA_CHECK_EQ(children.front().get<"name">().resource(), &target_resource);
        RUVIA_CHECK_EQ(children.front().get<"name">().view(), std::string_view(text));
        RUVIA_CHECK_EQ(boxed.front().resource(), &target_resource);
        RUVIA_CHECK_EQ(boxed.front().view(), std::string_view(text));
    }
    RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_public_insertion_owns_borrowed_parser_values) {
    ruvia::test::counting_memory_resource resource;
    const std::string text(160, 'c');
    {
        std::string body = "{\"name\":\"" + text + "\",\"tags\":[\"" + text + "\"]}";
        std::string_view name_input(body.data() + body.find(text) - 1, text.size() + 2);
        std::string_view tags_input(body.data() + body.rfind('['), body.size() - body.rfind('[') - 1);
        auto name = ruvia::detail::parse_json_value<ruvia::string>(name_input, &resource);
        auto tags = ruvia::detail::parse_json_value<ruvia::array<ruvia::string>>(tags_input, &resource);
        RUVIA_CHECK(name.has_value());
        RUVIA_CHECK(tags.has_value());
        RUVIA_CHECK_EQ(name->data(), body.data() + body.find(text));
        RUVIA_CHECK_EQ(tags->front().data(), body.data() + body.rfind(text));
        resource_child owned({.resource_ = &resource});
        owned.set<"name">(std::move(*name));
        owned.set<"tags">(std::move(*tags));
        body.assign(body.size(), 'x');
        RUVIA_CHECK_EQ(owned.get<"name">().view(), std::string_view(text));
        RUVIA_CHECK_EQ(owned.get<"tags">()->front().view(), std::string_view(text));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_array_resize_accepts_its_own_fill_element) {
    ruvia::test::counting_memory_resource resource;
    const std::string text(160, 'd');
    {
        ruvia::array<ruvia::string> values({.resource_ = &resource});
        values.emplace_back(text);
        const auto count = values.capacity() + 8;
        values.resize(count, values.front());
        RUVIA_CHECK_EQ(values.size(), count);
        for (const auto& value : values) {
            RUVIA_CHECK_EQ(value.resource(), &resource);
            RUVIA_CHECK_EQ(value.view(), std::string_view(text));
        }
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_recursive_assignment_can_promote_an_owned_child) {
    ruvia::test::counting_memory_resource resource;
    const std::string text(160, 'e');
    {
        resource_node root({.resource_ = &resource});
        root.set<"name">("parent");
        auto& child_value = root.ensure<"children">().emplace();
        child_value.set<"name">(text);
        child_value.ensure<"children">().emplace().set<"name">("grandchild");
        root = std::move(child_value);
        RUVIA_CHECK_EQ(root.get<"name">().view(), std::string_view(text));
        RUVIA_CHECK_EQ(root.get<"children">()->front().get<"name">().view(), std::string_view("grandchild"));
        RUVIA_CHECK_EQ(root.resource(), &resource);

        auto& children = root.ensure<"children">();
        children.front().ensure<"children">().emplace().set<"name">(text);
        children = std::move(children.front().ensure<"children">());
        RUVIA_CHECK_EQ(children.size(), std::size_t{1});
        RUVIA_CHECK_EQ(children.front().get<"name">().view(), std::string_view(text));

        resource_dense_node dense({.resource_ = &resource});
        auto& dense_children = dense.ensure<"children">();
        dense_children.emplace_back().ensure<"children">().emplace_back().set<"name">(text);
        dense_children = std::move(dense_children.front().ensure<"children">());
        RUVIA_CHECK_EQ(dense_children.size(), std::size_t{1});
        RUVIA_CHECK_EQ(dense_children.front().get<"name">().view(), std::string_view(text));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_failed_rebind_preserves_destination_and_reclaims_temporaries) {
    reject_after_resource target_resource;
    ruvia::test::counting_memory_resource source_resource;
    const std::string old_text(160, 'o');
    const std::string new_text(160, 'n');
    {
        resource_pair target({.resource_ = &target_resource});
        target.set<"first">(old_text);
        target.set<"second">(old_text);
        const auto retained = target_resource.allocations_.live_allocations();
        resource_pair source_value({.resource_ = &source_resource});
        source_value.set<"first">(new_text);
        source_value.set<"second">(new_text);
        target_resource.remaining_ = 1;
        bool failed = false;
        try {
            target = std::move(source_value);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        target_resource.remaining_.reset();
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(target.get<"first">().view(), std::string_view(old_text));
        RUVIA_CHECK_EQ(target.get<"second">().view(), std::string_view(old_text));
        RUVIA_CHECK_EQ(source_value.get<"first">().view(), std::string_view(new_text));
        RUVIA_CHECK_EQ(source_value.get<"second">().view(), std::string_view(new_text));
        RUVIA_CHECK_EQ(source_value.resource(), &source_resource);
        RUVIA_CHECK_EQ(target_resource.allocations_.live_allocations(), retained);
        for (int iteration = 0; iteration != 32; ++iteration) {
            source_value.set<"first">(new_text);
            source_value.set<"second">(new_text);
            target = std::move(source_value);
            RUVIA_CHECK_EQ(target.resource(), &target_resource);
            RUVIA_CHECK_EQ(target_resource.allocations_.live_allocations(), retained);
            RUVIA_CHECK_EQ(target.get<"first">().view(), std::string_view(new_text));
        }
    }
    RUVIA_CHECK_EQ(target_resource.allocations_.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_rebind_preserves_invalid_duplicate_and_missing_states) {
    ruvia::test::counting_memory_resource resource;
    for (const auto json : {"{\"first\":42}", "{\"first\":\"a\",\"first\":\"b\"}", "{}"}) {
        resource_pair target({.resource_ = &resource});
        {
            std::pmr::monotonic_buffer_resource source_resource;
            auto source_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<resource_pair>(json, &source_resource);
            RUVIA_CHECK(source_value.has_value());
            if (!source_value) {
                continue;
            }
            target = std::move(*source_value);
        }
        ruvia::validator validator;
        ruvia::detail::model_validation_access::validate_model(target, validator);
        RUVIA_CHECK(!validator.ok());
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{2});
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
