#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/Model.h"
#include "ruvia/web/Validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_REQUEST_MODEL(ResourceChild,
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>));
RUVIA_REQUEST_MODEL(ResourceParent,
    RUVIA_REQUIRED_FIELD(title, ruvia::String),
    RUVIA_OPTIONAL_FIELD(child, ResourceChild),
    RUVIA_OPTIONAL_FIELD(children, ruvia::Array<ResourceChild>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::BoxedArray<ResourceChild>));
RUVIA_REQUEST_MODEL(ResourceNode,
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(children, ruvia::BoxedArray<ResourceNode>));
RUVIA_REQUEST_MODEL(ResourceDenseNode,
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(children, ruvia::Array<ResourceDenseNode>));
RUVIA_REQUEST_MODEL(ResourcePair,
    RUVIA_REQUIRED_FIELD(first, ruvia::String),
    RUVIA_REQUIRED_FIELD(second, ruvia::String));

class RejectAfterResource final : public std::pmr::memory_resource {
public:
    std::optional<std::size_t> remaining;
    ruvia::test::CountingMemoryResource allocations;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (remaining) {
            if (*remaining == 0) {
                throw std::bad_alloc();
            }
            --*remaining;
        }
        return allocations.allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        allocations.deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(model_resource_assignment_recursively_outlives_source_owner) {
    ruvia::test::CountingMemoryResource targetResource;
    const std::string text(160, 'a');
    {
        ResourceParent target({.resource = &targetResource});
        {
            std::pmr::monotonic_buffer_resource sourceResource;
            target.set<"title">(ruvia::String(text, {.resource = &sourceResource}));
            ResourceChild source({.resource = &sourceResource});
            source.set<"name">(text);
            source.ensure<"tags">().emplace_back(text);
            target.set<"child">(std::move(source));
            ResourceChild other({.resource = &sourceResource});
            other.set<"name">(text);
            other.ensure<"tags">().emplace_back(text);
            // Public insertion accepts a const model and owns it recursively.
            const auto& borrowed = other;
            target.ensure<"children">().emplace_back(borrowed);
            target.ensure<"boxed">().emplace(std::move(other));
        }
        RUVIA_CHECK_EQ(target.get<"title">().resource(), &targetResource);
        RUVIA_CHECK_EQ(target.get<"title">().view(), std::string_view(text));
        const auto verify = [&](const ResourceChild& child) {
            RUVIA_CHECK_EQ(child.resource(), &targetResource);
            RUVIA_CHECK_EQ(child.get<"name">().resource(), &targetResource);
            RUVIA_CHECK_EQ(child.get<"name">().view(), std::string_view(text));
            RUVIA_CHECK_EQ(child.get<"tags">()->resource(), &targetResource);
            RUVIA_CHECK_EQ(child.get<"tags">()->front().resource(), &targetResource);
            RUVIA_CHECK_EQ(child.get<"tags">()->front().view(), std::string_view(text));
        };
        verify(*target.get<"child">());
        verify(target.get<"children">()->front());
        verify(target.get<"boxed">()->front());
    }
    RUVIA_CHECK_EQ(targetResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(targetResource.allocationCount(), targetResource.deallocationCount());
}

RUVIA_TEST(model_resource_array_construction_and_mutable_assignment_keep_owner) {
    ruvia::test::CountingMemoryResource targetResource;
    const std::string text(160, 'b');
    {
        ruvia::Array<ruvia::String> strings({.resource = &targetResource});
        strings.emplace_back(text);
        strings.emplace_back().assignOwned(text);
        ruvia::Array<ResourceChild> children({.resource = &targetResource});
        children.emplace_back().set<"name">(text);
        ruvia::BoxedArray<ruvia::String> boxed({.resource = &targetResource});
        boxed.emplace(text);
        {
            std::pmr::monotonic_buffer_resource temporary;
            strings.front() = ruvia::String(text, {.resource = &temporary});
            ResourceChild replacement({.resource = &temporary});
            replacement.set<"name">(text);
            children.front() = std::move(replacement);
            boxed.front() = ruvia::String(text, {.resource = &temporary});
        }
        for (const auto& value : strings) {
            RUVIA_CHECK_EQ(value.resource(), &targetResource);
            RUVIA_CHECK_EQ(value.view(), std::string_view(text));
        }
        RUVIA_CHECK_EQ(children.front().resource(), &targetResource);
        RUVIA_CHECK_EQ(children.front().get<"name">().resource(), &targetResource);
        RUVIA_CHECK_EQ(children.front().get<"name">().view(), std::string_view(text));
        RUVIA_CHECK_EQ(boxed.front().resource(), &targetResource);
        RUVIA_CHECK_EQ(boxed.front().view(), std::string_view(text));
    }
    RUVIA_CHECK_EQ(targetResource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_public_insertion_owns_borrowed_parser_values) {
    ruvia::test::CountingMemoryResource resource;
    const std::string text(160, 'c');
    {
        std::string body = "{\"name\":\"" + text + "\",\"tags\":[\"" + text + "\"]}";
        std::string_view nameInput(body.data() + body.find(text) - 1, text.size() + 2);
        std::string_view tagsInput(body.data() + body.rfind('['), body.size() - body.rfind('[') - 1);
        auto name = ruvia::detail::parseJsonValue<ruvia::String>(nameInput, &resource);
        auto tags = ruvia::detail::parseJsonValue<ruvia::Array<ruvia::String>>(tagsInput, &resource);
        RUVIA_CHECK(name.has_value());
        RUVIA_CHECK(tags.has_value());
        RUVIA_CHECK_EQ(name->data(), body.data() + body.find(text));
        RUVIA_CHECK_EQ(tags->front().data(), body.data() + body.rfind(text));
        ResourceChild owned({.resource = &resource});
        owned.set<"name">(std::move(*name));
        owned.set<"tags">(std::move(*tags));
        body.assign(body.size(), 'x');
        RUVIA_CHECK_EQ(owned.get<"name">().view(), std::string_view(text));
        RUVIA_CHECK_EQ(owned.get<"tags">()->front().view(), std::string_view(text));
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_array_resize_accepts_its_own_fill_element) {
    ruvia::test::CountingMemoryResource resource;
    const std::string text(160, 'd');
    {
        ruvia::Array<ruvia::String> values({.resource = &resource});
        values.emplace_back(text);
        const auto count = values.capacity() + 8;
        values.resize(count, values.front());
        RUVIA_CHECK_EQ(values.size(), count);
        for (const auto& value : values) {
            RUVIA_CHECK_EQ(value.resource(), &resource);
            RUVIA_CHECK_EQ(value.view(), std::string_view(text));
        }
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_recursive_assignment_can_promote_an_owned_child) {
    ruvia::test::CountingMemoryResource resource;
    const std::string text(160, 'e');
    {
        ResourceNode root({.resource = &resource});
        root.set<"name">("parent");
        auto& child = root.ensure<"children">().emplace();
        child.set<"name">(text);
        child.ensure<"children">().emplace().set<"name">("grandchild");
        root = std::move(child);
        RUVIA_CHECK_EQ(root.get<"name">().view(), std::string_view(text));
        RUVIA_CHECK_EQ(root.get<"children">()->front().get<"name">().view(), std::string_view("grandchild"));
        RUVIA_CHECK_EQ(root.resource(), &resource);

        auto& children = root.ensure<"children">();
        children.front().ensure<"children">().emplace().set<"name">(text);
        children = std::move(children.front().ensure<"children">());
        RUVIA_CHECK_EQ(children.size(), std::size_t{1});
        RUVIA_CHECK_EQ(children.front().get<"name">().view(), std::string_view(text));

        ResourceDenseNode dense({.resource = &resource});
        auto& denseChildren = dense.ensure<"children">();
        denseChildren.emplace_back().ensure<"children">().emplace_back().set<"name">(text);
        denseChildren = std::move(denseChildren.front().ensure<"children">());
        RUVIA_CHECK_EQ(denseChildren.size(), std::size_t{1});
        RUVIA_CHECK_EQ(denseChildren.front().get<"name">().view(), std::string_view(text));
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_failed_rebind_preserves_destination_and_reclaims_temporaries) {
    RejectAfterResource targetResource;
    ruvia::test::CountingMemoryResource sourceResource;
    const std::string oldText(160, 'o');
    const std::string newText(160, 'n');
    {
        ResourcePair target({.resource = &targetResource});
        target.set<"first">(oldText);
        target.set<"second">(oldText);
        const auto retained = targetResource.allocations.liveAllocations();
        ResourcePair source({.resource = &sourceResource});
        source.set<"first">(newText);
        source.set<"second">(newText);
        targetResource.remaining = 1;
        bool failed = false;
        try {
            target = std::move(source);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        targetResource.remaining.reset();
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(target.get<"first">().view(), std::string_view(oldText));
        RUVIA_CHECK_EQ(target.get<"second">().view(), std::string_view(oldText));
        RUVIA_CHECK_EQ(targetResource.allocations.liveAllocations(), retained);
        for (int iteration = 0; iteration != 32; ++iteration) {
            source.set<"first">(newText);
            source.set<"second">(newText);
            target = std::move(source);
            RUVIA_CHECK_EQ(target.resource(), &targetResource);
            RUVIA_CHECK_EQ(targetResource.allocations.liveAllocations(), retained);
            RUVIA_CHECK_EQ(target.get<"first">().view(), std::string_view(newText));
        }
    }
    RUVIA_CHECK_EQ(targetResource.allocations.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(sourceResource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(model_resource_rebind_preserves_invalid_duplicate_and_missing_states) {
    ruvia::test::CountingMemoryResource resource;
    for (const auto json : {"{\"first\":42}", "{\"first\":\"a\",\"first\":\"b\"}", "{}"}) {
        ResourcePair target({.resource = &resource});
        {
            std::pmr::monotonic_buffer_resource sourceResource;
            auto source = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<ResourcePair>(json, &sourceResource);
            RUVIA_CHECK(source.has_value());
            if (!source) {
                continue;
            }
            target = std::move(*source);
        }
        ruvia::Validator validator;
        ruvia::detail::ModelValidationAccess::validateModel(target, validator);
        RUVIA_CHECK(!validator.ok());
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{2});
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}
