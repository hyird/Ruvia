#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/model.h"

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
RUVIA_MODEL(resource_text_sequences,
    RUVIA_OPTIONAL_FIELD(dense, ruvia::array<ruvia::boxed_array<ruvia::string>>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<ruvia::string>));

}  // namespace

TEST(Ruvia, model_assignment_recursively_outlives_source_owner) {
    const std::string text(160, 'a');
    resource_parent target;
    {
        target.set<"title">(ruvia::string(text));
        resource_child source_value;
        source_value.set<"name">(text);
        source_value.ensure<"tags">().emplace_back(text);
        target.set<"child">(std::move(source_value));
        resource_child other;
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

TEST(Ruvia, model_array_construction_and_mutable_assignment_keep_values) {
    const std::string text(160, 'b');
    ruvia::array<ruvia::string> strings;
    strings.emplace_back(text);
    strings.emplace_back().assign_owned(text);
    ruvia::array<resource_child> children;
    children.emplace_back().set<"name">(text);
    ruvia::boxed_array<ruvia::string> boxed;
    boxed.emplace_back(text);
    {
        strings.front() = ruvia::string(text);
        resource_child replacement;
        replacement.set<"name">(text);
        children.front() = std::move(replacement);
        boxed.front() = ruvia::string(text);
    }
    for (const auto& value : strings) {
        EXPECT_EQ(value.view(), std::string_view(text));
    }
    EXPECT_EQ(children.front().get<"name">().view(), std::string_view(text));
    EXPECT_EQ(boxed.front().view(), std::string_view(text));
}

TEST(Ruvia, model_array_resize_accepts_its_own_fill_element) {
    const std::string text(160, 'd');
    ruvia::array<ruvia::string> values;
    values.emplace_back(text);
    const auto count = values.capacity() + 8;
    values.resize(count, values.front());
    EXPECT_EQ(values.size(), count);
    for (const auto& value : values) {
        EXPECT_EQ(value.view(), std::string_view(text));
    }
}

TEST(Ruvia, model_recursive_assignment_can_promote_an_owned_child) {
    const std::string text(160, 'e');
    resource_node root;
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

    resource_dense_node dense;
    auto& dense_children = dense.ensure<"children">();
    dense_children.emplace_back().ensure<"children">().emplace_back().set<"name">(text);
    dense_children = std::move(dense_children.front().ensure<"children">());
    EXPECT_EQ(dense_children.size(), std::size_t{1});
    EXPECT_EQ(dense_children.front().get<"name">().view(), std::string_view(text));
}

TEST(Ruvia, model_moves_reject_a_source_that_owns_the_destination) {
    const std::string text(160, 'f');
    const auto rejects = [](auto&& operation) {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    resource_node root;
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

    resource_dense_node dense;
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

TEST(Ruvia, model_self_field_and_aliased_element_moves_preserve_values) {
    const std::string first(160, 'a');
    const std::string second(160, 'b');
    resource_text_sequences value;
    value.ensure<"dense">().emplace_back().emplace_back(first);
    value.ensure<"dense">().emplace_back().emplace_back(second);
    value.set<"dense">(std::move(value.ensure<"dense">()));
    ASSERT_EQ(value.get<"dense">()->size(), std::size_t{2});
    EXPECT_EQ((*value.get<"dense">())[0].front().view(), std::string_view(first));
    EXPECT_EQ((*value.get<"dense">())[1].front().view(), std::string_view(second));

    value.ensure<"boxed">().emplace_back(first);
    value.ensure<"boxed">().emplace_back(second);
    value.set<"boxed">(std::move(value.ensure<"boxed">()));
    ASSERT_EQ(value.get<"boxed">()->size(), std::size_t{2});
    EXPECT_EQ((*value.get<"boxed">())[0].view(), std::string_view(first));
    EXPECT_EQ((*value.get<"boxed">())[1].view(), std::string_view(second));

    ruvia::array<ruvia::string> values;
    values.emplace_back(first);
    while (values.size() < values.capacity()) {
        values.emplace_back(second);
    }
    const char* const original_text = values.front().data();
    values.push_back(std::move(values.front()));
    EXPECT_EQ(values.back().view(), std::string_view(first));
    EXPECT_EQ(values.back().data(), original_text);
    EXPECT_TRUE(values.front().empty());
}

TEST(Ruvia, model_scalar_array_moves_transfer_storage) {
    const auto check = []<typename sequence_type>() {
        sequence_type source;
        sequence_type target;
        for (int index = 0; index < 64; ++index) {
            source.push_back(ruvia::int32(index));
        }
        target.push_back(ruvia::int32(-1));
        target = std::move(source);
        EXPECT_TRUE(source.empty());
        EXPECT_EQ(target.size(), std::size_t{64});
        for (std::size_t index = 0; index < target.size(); ++index) {
            EXPECT_EQ(target[index].value_, static_cast<int>(index));
        }
    };
    check.template operator()<ruvia::array<ruvia::int32>>();
    check.template operator()<ruvia::boxed_array<ruvia::int32>>();
}

static_assert(!std::is_copy_constructible_v<resource_parent>);
static_assert(!std::is_constructible_v<resource_parent, resource_child&>);
