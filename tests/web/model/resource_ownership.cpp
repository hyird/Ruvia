#include <gtest/gtest.h>

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/model.h"

#include "memory_resource_fixture.h"

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
RUVIA_MODEL(resource_text_sequences,
    RUVIA_OPTIONAL_FIELD(dense, ruvia::array<ruvia::boxed_array<ruvia::string>>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<ruvia::string>));

class reject_after_resource final : public std::pmr::memory_resource {
public:
    std::optional<std::size_t> remaining_;
    ruvia::test::counting_memory_resource allocations_;

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (remaining_) {
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

TEST(Ruvia, model_resource_assignment_recursively_outlives_source_owner) {
    ruvia::test::counting_memory_resource target_resource;
    const std::pmr::string text(160, 'a', ruvia::process_resource());
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
            target.ensure<"boxed">().emplace_back(std::move(other));
        }
        EXPECT_EQ(target.get<"title">().view(), std::string_view(text));
        const auto verify = [&](const resource_child& child_value) {
            EXPECT_EQ(child_value.get<"name">().view(), std::string_view(text));
            EXPECT_EQ(child_value.get<"tags">()->front().view(), std::string_view(text));
        };
        verify(*target.get<"child">());
        verify(target.get<"children">()->front());
        verify(target.get<"boxed">()->front());
    }
    EXPECT_EQ(target_resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(target_resource.allocation_count(), target_resource.deallocation_count());
}

TEST(Ruvia, model_resource_array_construction_and_mutable_assignment_keep_owner) {
    ruvia::test::counting_memory_resource target_resource;
    const std::pmr::string text(160, 'b', ruvia::process_resource());
    {
        ruvia::array<ruvia::string> strings({.resource_ = &target_resource});
        strings.emplace_back(text);
        strings.emplace_back().assign_owned(text);
        ruvia::array<resource_child> children({.resource_ = &target_resource});
        children.emplace_back().set<"name">(text);
        ruvia::boxed_array<ruvia::string> boxed({.resource_ = &target_resource});
        boxed.emplace_back(text);
        {
            std::pmr::monotonic_buffer_resource temporary;
            strings.front() = ruvia::string(text, {.resource_ = &temporary});
            resource_child replacement({.resource_ = &temporary});
            replacement.set<"name">(text);
            children.front() = std::move(replacement);
            boxed.front() = ruvia::string(text, {.resource_ = &temporary});
        }
        for (const auto& value : strings) {
            EXPECT_EQ(value.view(), std::string_view(text));
        }
        EXPECT_EQ(children.front().get<"name">().view(), std::string_view(text));
        EXPECT_EQ(boxed.front().view(), std::string_view(text));
    }
    EXPECT_EQ(target_resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, model_resource_array_resize_accepts_its_own_fill_element) {
    ruvia::test::counting_memory_resource resource;
    const std::pmr::string text(160, 'd', ruvia::process_resource());
    {
        ruvia::array<ruvia::string> values({.resource_ = &resource});
        values.emplace_back(text);
        const auto count = values.capacity() + 8;
        values.resize(count, values.front());
        EXPECT_EQ(values.size(), count);
        for (const auto& value : values) {
            EXPECT_EQ(value.view(), std::string_view(text));
        }
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, model_resource_recursive_assignment_can_promote_an_owned_child) {
    ruvia::test::counting_memory_resource resource;
    const std::pmr::string text(160, 'e', ruvia::process_resource());
    {
        resource_node root({.resource_ = &resource});
        root.set<"name">("parent");
        auto& child_value = root.ensure<"children">().emplace_back();
        child_value.set<"name">(text);
        child_value.ensure<"children">().emplace_back().set<"name">("grandchild");
        root = std::move(child_value);
        EXPECT_EQ(root.get<"name">().view(), std::string_view(text));
        EXPECT_EQ(root.get<"children">()->front().get<"name">().view(), std::string_view("grandchild"));

        auto& children = root.ensure<"children">();
        children.front().ensure<"children">().emplace_back().set<"name">(text);
        children = std::move(children.front().ensure<"children">());
        EXPECT_EQ(children.size(), std::size_t{1});
        EXPECT_EQ(children.front().get<"name">().view(), std::string_view(text));

        resource_dense_node dense({.resource_ = &resource});
        auto& dense_children = dense.ensure<"children">();
        dense_children.emplace_back().ensure<"children">().emplace_back().set<"name">(text);
        dense_children = std::move(dense_children.front().ensure<"children">());
        EXPECT_EQ(dense_children.size(), std::size_t{1});
        EXPECT_EQ(dense_children.front().get<"name">().view(), std::string_view(text));
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, model_resource_failed_rebind_preserves_destination_and_reclaims_temporaries) {
    reject_after_resource target_resource;
    ruvia::test::counting_memory_resource source_resource;
    const std::pmr::string old_text(160, 'o', ruvia::process_resource());
    const std::pmr::string new_text(160, 'n', ruvia::process_resource());
    {
        resource_pair target({.resource_ = &target_resource});
        target.set<"first">(old_text);
        target.set<"second">(old_text);
        const auto retained = target_resource.allocations_.live_allocations();
        resource_pair source_value({.resource_ = &source_resource});
        source_value.set<"first">(new_text);
        source_value.set<"second">(new_text);
        if (ruvia::test::stl_allocation_failure_injection_enabled()) {
            target_resource.remaining_ = 1;
            bool failed = false;
            try {
                target = std::move(source_value);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            target_resource.remaining_.reset();
            EXPECT_TRUE(failed);
            EXPECT_EQ(target.get<"first">().view(), std::string_view(old_text));
            EXPECT_EQ(target.get<"second">().view(), std::string_view(old_text));
            EXPECT_EQ(source_value.get<"first">().view(), std::string_view(new_text));
            EXPECT_EQ(source_value.get<"second">().view(), std::string_view(new_text));
            EXPECT_EQ(target_resource.allocations_.live_allocations(), retained);
        }
        for (int iteration = 0; iteration != 32; ++iteration) {
            source_value.set<"first">(new_text);
            source_value.set<"second">(new_text);
            target = std::move(source_value);
            EXPECT_EQ(target_resource.allocations_.live_allocations(), retained);
            EXPECT_EQ(target.get<"first">().view(), std::string_view(new_text));
        }
    }
    EXPECT_EQ(target_resource.allocations_.live_allocations(), std::size_t{0});
    EXPECT_EQ(source_resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, model_dynamic_token_failed_move_keeps_source_destination_and_resources) {
    const std::pmr::string input("{\"payload\":\"" + std::pmr::string(160, 'x', ruvia::process_resource()) + "\"}", ruvia::process_resource());
    const bool inject = ruvia::test::stl_allocation_failure_injection_enabled();
    const auto check = [&]<typename token_type>(std::string_view empty) {
        reject_after_resource target_resource;
        ruvia::test::counting_memory_resource source_resource;
        {
            token_type source({.resource_ = &source_resource});
            source = token_type::parse(input);
            token_type target({.resource_ = &target_resource});
            if (inject) {
                target_resource.remaining_ = 0;
                bool failed = false;
                try {
                    target = std::move(source);
                } catch (const std::bad_alloc&) {
                    failed = true;
                }
                EXPECT_TRUE(failed);
                EXPECT_EQ(source.view(), std::string_view(input));
                EXPECT_EQ(target.view(), empty);
                target_resource.remaining_.reset();
            }
            target = std::move(source);
            EXPECT_EQ(source.view(), empty);
            EXPECT_EQ(target.view(), std::string_view(input));
        }
        EXPECT_EQ(source_resource.live_allocations(), std::size_t{0});
        EXPECT_EQ(target_resource.allocations_.live_allocations(), std::size_t{0});
    };
    check.template operator()<ruvia::json_value>("null");
    check.template operator()<ruvia::json_object>("{}");
}

TEST(Ruvia, model_resource_moves_reject_a_source_that_owns_the_destination) {
    ruvia::test::counting_memory_resource resource;
    const std::pmr::string text(160, 'f', ruvia::process_resource());
    const auto rejects = [](auto&& operation) {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    {
        resource_node root({.resource_ = &resource});
        root.set<"name">(text);
        auto& child_value = root.ensure<"children">().emplace_back();
        child_value.set<"name">("child");
        auto& grandchildren = child_value.ensure<"children">();
        grandchildren.emplace_back().set<"name">("grandchild");

        EXPECT_TRUE(rejects([&] { child_value = std::move(root); }));
        EXPECT_TRUE(rejects([&] { grandchildren = std::move(root.ensure<"children">()); }));
        EXPECT_TRUE(rejects([&] { grandchildren.push_back(std::move(root)); }));
        EXPECT_TRUE(rejects([&] { root.ensure<"children">().emplace_back(std::move(root)); }));
        EXPECT_TRUE(rejects([&] { child_value.set<"children">(std::move(root.ensure<"children">())); }));

        EXPECT_EQ(root.get<"name">().view(), std::string_view(text));
        EXPECT_EQ(root.get<"children">()->size(), std::size_t{1});
        EXPECT_TRUE(&root.get<"children">()->front() == &child_value);
        EXPECT_EQ(child_value.get<"name">().view(), std::string_view("child"));
        EXPECT_EQ(grandchildren.size(), std::size_t{1});
        EXPECT_EQ(grandchildren.front().get<"name">().view(), std::string_view("grandchild"));

        // A field reassigned from its own value does not own its destination.
        root.set<"children">(std::move(root.ensure<"children">()));
        EXPECT_EQ(root.get<"children">()->front().get<"name">().view(), std::string_view("child"));

        resource_dense_node dense({.resource_ = &resource});
        dense.set<"name">(text);
        auto& dense_children = dense.ensure<"children">();
        auto& dense_child = dense_children.emplace_back();
        dense_child.set<"name">("child");
        auto& dense_grandchildren = dense_child.ensure<"children">();

        EXPECT_TRUE(rejects([&] { dense_child = std::move(dense); }));
        EXPECT_TRUE(rejects([&] { dense_grandchildren = std::move(dense_children); }));
        EXPECT_TRUE(rejects([&] { dense_grandchildren.emplace_back(std::move(dense)); }));
        EXPECT_TRUE(rejects([&] { dense_children.emplace_back(std::move(dense)); }));

        EXPECT_EQ(dense.get<"name">().view(), std::string_view(text));
        EXPECT_EQ(dense_children.size(), std::size_t{1});
        EXPECT_EQ(dense_child.get<"name">().view(), std::string_view("child"));
        EXPECT_TRUE(dense_grandchildren.empty());
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, model_resource_self_field_rebind_failure_preserves_owned_elements) {
    if (!ruvia::test::stl_allocation_failure_injection_enabled()) {
        GTEST_SKIP() << "STL debug metadata allocation cannot be fault-injected safely";
    }
    const std::pmr::string first(160, 'a', ruvia::process_resource());
    const std::pmr::string second(160, 'b', ruvia::process_resource());
    const auto check = [&]<ruvia::fixed_string field>() {
        bool reached_success = false;
        bool failed_after_preparation = false;
        for (std::size_t allowance = 0; allowance != 16; ++allowance) {
            reject_after_resource resource;
            {
                resource_text_sequences value({.resource_ = &resource});
                auto& items = value.ensure<field>();
                const auto append = [&](std::string_view text) {
                    if constexpr (requires { items.front().view(); }) {
                        items.emplace_back(text);
                    } else {
                        items.emplace_back().emplace_back(text);
                    }
                };
                append(first);
                append(second);
                const auto retained = resource.allocations_.live_allocations();
                resource.remaining_ = allowance;
                bool failed = false;
                try {
                    value.set<field>(std::move(items));
                    reached_success = true;
                } catch (const std::bad_alloc&) {
                    failed = true;
                    failed_after_preparation |= allowance != 0;
                }
                resource.remaining_.reset();
                ASSERT_EQ(value.get<field>()->size(), std::size_t{2});
                const auto text_at = [&](std::size_t index) {
                    const auto& item = (*value.get<field>())[index];
                    if constexpr (requires { item.view(); }) {
                        return item.view();
                    } else {
                        return item.front().view();
                    }
                };
                EXPECT_EQ(text_at(0), std::string_view(first));
                EXPECT_EQ(text_at(1), std::string_view(second));
                if (failed) {
                    EXPECT_EQ(resource.allocations_.live_allocations(), retained);
                }
            }
            EXPECT_EQ(resource.allocations_.live_allocations(), std::size_t{0});
            if (reached_success) {
                break;
            }
        }
        EXPECT_TRUE(reached_success);
        EXPECT_TRUE(failed_after_preparation);
    };
    check.template operator()<"dense">();
    check.template operator()<"boxed">();
}

TEST(Ruvia, model_resource_insertion_failure_preserves_moved_strings_and_aliases) {
    if (!ruvia::test::stl_allocation_failure_injection_enabled()) {
        GTEST_SKIP() << "STL debug metadata allocation cannot be fault-injected safely";
    }
    const std::pmr::string text(160, 's', ruvia::process_resource());
    const auto check = [&]<typename sequence_type>(bool alias) {
        bool succeeded = false;
        bool rejected = false;
        for (std::size_t allowance = 0; allowance != 8 && !succeeded; ++allowance) {
            SCOPED_TRACE(allowance);
            reject_after_resource resource;
            {
                sequence_type values({.resource_ = &resource});
                ruvia::string external(text, {.resource_ = &resource});
                if (alias) {
                    values.emplace_back(text);
                    if constexpr (requires { values.capacity(); }) {
                        while (values.size() < values.capacity()) {
                            values.emplace_back(text);
                        }
                    }
                }
                const auto old_size = values.size();
                auto& source = alias ? values.front() : external;
                const char* const original_text = source.data();
                resource.remaining_ = allowance;
                try {
                    values.push_back(std::move(source));
                    succeeded = true;
                } catch (const std::bad_alloc&) {
                    rejected = true;
                }
                resource.remaining_.reset();
                if (succeeded) {
                    ASSERT_EQ(values.size(), old_size + 1);
                    EXPECT_EQ(values.back().view(), std::string_view(text));
                    EXPECT_EQ(values.back().data(), original_text);
                    EXPECT_TRUE((alias ? values.front() : external).empty());
                } else {
                    EXPECT_EQ(values.size(), old_size);
                    EXPECT_EQ((alias ? values.front() : external).view(), std::string_view(text));
                }
                for (std::size_t index = alias && succeeded ? 1 : 0; index < old_size; ++index) {
                    EXPECT_EQ(values[index].view(), std::string_view(text));
                }
            }
            EXPECT_EQ(resource.allocations_.live_allocations(), std::size_t{0});
        }
        EXPECT_TRUE(succeeded);
        EXPECT_TRUE(rejected);
    };
    for (bool alias : {false, true}) {
        SCOPED_TRACE(alias);
        check.template operator()<ruvia::array<ruvia::string>>(alias);
        check.template operator()<ruvia::boxed_array<ruvia::string>>(alias);
    }
}

TEST(Ruvia, model_resource_nested_insertion_failure_preserves_source_until_commit) {
    if (!ruvia::test::stl_allocation_failure_injection_enabled()) {
        GTEST_SKIP() << "STL debug metadata allocation cannot be fault-injected safely";
    }
    const std::pmr::string first(160, 'a', ruvia::process_resource());
    const std::pmr::string second(160, 'b', ruvia::process_resource());
    const auto check = [&]<typename sequence_type>() {
        bool succeeded = false;
        bool rejected_after_preparation = false;
        for (std::size_t allowance = 0; allowance != 16 && !succeeded; ++allowance) {
            SCOPED_TRACE(allowance);
            reject_after_resource resource;
            {
                sequence_type values({.resource_ = &resource});
                ruvia::array<ruvia::string> source({.resource_ = &resource});
                source.emplace_back(first);
                source.emplace_back(second);
                resource.remaining_ = allowance;
                try {
                    values.emplace_back(std::move(source));
                    succeeded = true;
                } catch (const std::bad_alloc&) {
                    rejected_after_preparation |= allowance != 0;
                }
                resource.remaining_.reset();
                if (succeeded) {
                    EXPECT_TRUE(source.empty());
                    ASSERT_EQ(values.size(), std::size_t{1});
                    ASSERT_EQ(values.front().size(), std::size_t{2});
                    EXPECT_EQ(values.front()[0].view(), std::string_view(first));
                    EXPECT_EQ(values.front()[1].view(), std::string_view(second));
                } else {
                    EXPECT_TRUE(values.empty());
                    ASSERT_EQ(source.size(), std::size_t{2});
                    EXPECT_EQ(source[0].view(), std::string_view(first));
                    EXPECT_EQ(source[1].view(), std::string_view(second));
                }
            }
            EXPECT_EQ(resource.allocations_.live_allocations(), std::size_t{0});
        }
        EXPECT_TRUE(succeeded);
        EXPECT_TRUE(rejected_after_preparation);
    };
    check.template operator()<ruvia::array<ruvia::array<ruvia::string>>>();
    check.template operator()<ruvia::boxed_array<ruvia::array<ruvia::string>>>();
}

TEST(Ruvia, model_resource_scalar_array_moves_transfer_same_resource_storage) {
    const bool inject = ruvia::test::stl_allocation_failure_injection_enabled();
    const auto check = [inject]<typename sequence_type>() {
        reject_after_resource resource;
        {
            sequence_type source({.resource_ = &resource});
            sequence_type target({.resource_ = &resource});
            for (int index = 0; index < 64; ++index) {
                source.push_back(ruvia::int32(index));
            }
            target.push_back(ruvia::int32(-1));
            if (inject) {
                resource.remaining_ = 0;
            }
            target = std::move(source);
            resource.remaining_.reset();
            EXPECT_TRUE(source.empty());
            EXPECT_EQ(target.size(), std::size_t{64});
            for (std::size_t index = 0; index < target.size(); ++index) {
                EXPECT_EQ(target[index].value_, static_cast<int>(index));
            }
        }
        EXPECT_EQ(resource.allocations_.live_allocations(), std::size_t{0});
    };
    check.template operator()<ruvia::array<ruvia::int32>>();
    check.template operator()<ruvia::boxed_array<ruvia::int32>>();
}

static_assert(!std::is_copy_constructible_v<resource_parent>);
static_assert(std::is_constructible_v<resource_parent, resource_child&>);
