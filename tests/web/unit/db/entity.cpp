#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/detail/db/db_entity_access.h"
#include "ruvia/web/detail/db/db_entity_codec.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_TEST(db_entity_array_codec_preserves_null_text_precision_and_boolean_values) {
    ruvia::test::counting_memory_resource resource;
    {
        std::pmr::vector<std::optional<std::pmr::string>> values(&resource);
        const auto field = ruvia::detail::db_result_access::owned_field("{\"NULL\",NULL,\"\",\"a,b\",\"a\\\"b\"}", &resource);
        ruvia::detail::decode_db_field(field, values, &resource);
        RUVIA_CHECK_EQ(values.size(), std::size_t{5});
        RUVIA_CHECK(values[0].has_value());
        RUVIA_CHECK_EQ(*values[0], std::string_view("NULL"));
        RUVIA_CHECK(!values[1].has_value());
        RUVIA_CHECK(values[2]->empty());
        RUVIA_CHECK_EQ(*values[3], std::string_view("a,b"));
        RUVIA_CHECK_EQ(*values[4], std::string_view("a\"b"));
        RUVIA_CHECK(values[0]->get_allocator().resource() == &resource);

        std::pmr::vector<double> input({1e-7, 1.23456789012345, -1e18}, &resource);
        auto encoded = ruvia::detail::entity_db_value(input, &resource);
        const auto numbers = ruvia::detail::db_result_access::borrowed_field(ruvia::detail::db_value_access::text(encoded), &resource);
        std::pmr::vector<double> decoded(&resource);
        ruvia::detail::decode_db_field(numbers, decoded, &resource);
        RUVIA_CHECK(input == decoded);

        std::pmr::vector<bool> booleans({true, false, true}, &resource);
        auto encoded_bools = ruvia::detail::entity_db_value(booleans, &resource);
        const auto bool_field = ruvia::detail::db_result_access::borrowed_field(ruvia::detail::db_value_access::text(encoded_bools), &resource);
        std::pmr::vector<bool> decoded_bools(&resource);
        ruvia::detail::decode_db_field(bool_field, decoded_bools, &resource);
        RUVIA_CHECK(booleans == decoded_bools);
        const auto malformed = ruvia::detail::db_result_access::owned_field("{\"unterminated}", &resource);
        RUVIA_CHECK(ruvia::testing::throws_on([&] { ruvia::detail::decode_db_field(malformed, values, &resource); }));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_DB_ENTITY(entity, "users",
    ruvia::db_column<ruvia::fixed_string{"id"}, int>,
    ruvia::db_column<ruvia::fixed_string{"name"}, std::pmr::string,
        ruvia::db_column_options{.nullable_ = true}>)
using computed_entity_type = ruvia::db_entity<"computed_users",
    ruvia::db_column<"first_name", std::pmr::string>,
    ruvia::db_column<"display_name", std::pmr::string,
        ruvia::db_column_options{.generated_type_ = ruvia::db_generated_type::stored}>>;
using array_entity_type = ruvia::db_entity<ruvia::fixed_string{"events"},
    ruvia::db_column<ruvia::fixed_string{"tags"}, std::pmr::vector<std::pmr::string>>,
    ruvia::db_column<ruvia::fixed_string{"scores"}, std::pmr::vector<std::optional<int>>>>;
using nullable_array_entity_type = ruvia::db_entity<"nullable_events",
    ruvia::db_column<"tags", std::pmr::vector<std::pmr::string>,
        ruvia::db_column_options{.nullable_ = true}>>;
using numeric_entity_type = ruvia::db_entity<"numeric_values", ruvia::db_column<"value", int>>;

class equivalent_memory_resource final : public std::pmr::memory_resource {
public:
    explicit equivalent_memory_resource(std::pmr::memory_resource* upstream)
        : upstream_(upstream) {}

    void reject_allocations(bool reject = true) noexcept {
        reject_allocations_ = reject;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_allocations_) {
            throw std::bad_alloc();
        }
        return upstream_->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        const auto* equivalent = dynamic_cast<const equivalent_memory_resource*>(&other);
        return equivalent != nullptr && upstream_ == equivalent->upstream_;
    }

    std::pmr::memory_resource* upstream_;
    bool reject_allocations_{false};
};

using relation_target_type = ruvia::db_entity<"relation_targets", ruvia::db_column<"id", std::int64_t>>;
using relation_owner_type = ruvia::db_entity<"relation_owners", ruvia::db_column<"id", std::int64_t>,
    ruvia::db_many_to_one<"parent", relation_target_type, ruvia::db_join_column<"id", "id">>,
    ruvia::db_one_to_one<"peer", relation_target_type, ruvia::db_join_column<"id", "id">>,
    ruvia::db_one_to_many<"children", relation_target_type, "parent">,
    ruvia::db_many_to_many<"tags", relation_target_type, ruvia::db_join_table<"owner_tags", ruvia::db_join_columns<ruvia::db_join_column<"owner_id", "id">>, ruvia::db_join_columns<ruvia::db_join_column<"tag_id", "id">>>>>;

struct self_node;
RUVIA_DB_ENTITY(self_node, "self_nodes", ruvia::db_column<"id", std::int64_t>,
    ruvia::db_many_to_one<"parent", self_node, ruvia::db_join_column<"parent_id", "id">>)

RUVIA_TEST(db_entity_tracks_unset_null_and_value_states) {
    entity entity;
    RUVIA_CHECK(!entity.is_set<"id">());
    entity.set<"id">(7);
    RUVIA_CHECK(entity.is_set<"id">());
    RUVIA_CHECK_EQ(entity.get<"id">(), 7);
    const auto& const_entity = entity;
    RUVIA_CHECK_EQ(const_entity.get<"id">(), 7);
    entity.set_null<"name">();
    RUVIA_CHECK(entity.is_null<"name">());
    entity.reset<"id">();
    RUVIA_CHECK(!entity.is_set<"id">());
    RUVIA_CHECK_EQ(entity::table_name(), std::string_view("users"));
    RUVIA_CHECK_EQ(entity::column_index<"name">(), std::size_t{1});
}

RUVIA_TEST(db_exec_result_exposes_affected_rows_and_optional_insert_id) {
    const auto inserted = ruvia::detail::db_result_access::make_exec_result(3, 91);
    RUVIA_CHECK_EQ(inserted.affected_rows(), std::uint64_t{3});
    RUVIA_CHECK(inserted.last_insert_id().has_value());
    RUVIA_CHECK_EQ(*inserted.last_insert_id(), std::uint64_t{91});

    const auto updated = ruvia::detail::db_result_access::make_exec_result(2);
    RUVIA_CHECK_EQ(updated.affected_rows(), std::uint64_t{2});
    RUVIA_CHECK(!updated.last_insert_id().has_value());
}

RUVIA_TEST(db_entity_relation_access_preserves_mixed_columns_and_relation_states) {
    ruvia::test::counting_memory_resource resource;
    relation_owner_type entity(&resource);
    entity.set<"id">(7);
    RUVIA_CHECK(entity.is_set<"id">());
    RUVIA_CHECK(!entity.is_set<"parent">());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)entity.get<"parent">(); }));

    auto& parent_value = ruvia::detail::db_entity_access<relation_owner_type>::emplace_relation<"parent">(entity);
    parent_value.set<"id">(11);
    RUVIA_CHECK(entity.is_set<"parent">());
    RUVIA_CHECK(!entity.is_null<"parent">());
    RUVIA_CHECK_EQ(entity.get<"parent">().get<"id">(), 11);

    ruvia::detail::db_entity_access<relation_owner_type>::set_relation_null<"peer">(entity);
    RUVIA_CHECK(entity.is_set<"peer">());
    RUVIA_CHECK(entity.is_null<"peer">());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)entity.get<"peer">(); }));
    entity.reset<"peer">();
    RUVIA_CHECK(!entity.is_set<"peer">());

    auto& children = ruvia::detail::db_entity_access<relation_owner_type>::ensure_relation_collection<"children">(entity);
    RUVIA_CHECK(entity.is_set<"children">());
    RUVIA_CHECK(children.empty());
    RUVIA_CHECK_EQ(entity.get<"id">(), 7);
    RUVIA_CHECK_EQ(entity.get<"parent">().get<"id">(), 11);
    entity.reset<"children">();
    RUVIA_CHECK(!entity.is_set<"children">());
}

RUVIA_TEST(db_entity_to_many_append_move_and_retained_result_use_entity_resource) {
    ruvia::test::counting_memory_resource resource;
    relation_owner_type entity(&resource);
    auto& children = ruvia::detail::db_entity_access<relation_owner_type>::ensure_relation_collection<"children">(entity);
    for (std::int64_t id = 1; id <= 4; ++id) {
        relation_target_type child_value(&resource);
        child_value.set<"id">(id);
        children.push_back(std::move(child_value));
    }
    RUVIA_CHECK_EQ(children.size(), std::size_t{4});
    RUVIA_CHECK_EQ(children[0].get<"id">(), 1);
    RUVIA_CHECK_EQ(children[3].get<"id">(), 4);
    auto moved = std::move(entity);
    RUVIA_CHECK_EQ(moved.get<"children">().size(), std::size_t{4});
    RUVIA_CHECK_EQ(moved.get<"children">()[2].get<"id">(), 3);
    moved.reset<"children">();
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(db_entity_relation_repeated_load_reset_reclaims_nested_storage) {
    ruvia::test::counting_memory_resource resource;
    relation_owner_type entity(&resource);
    for (std::int64_t round = 0; round < 32; ++round) {
        auto& parent_value = ruvia::detail::db_entity_access<relation_owner_type>::emplace_relation<"parent">(entity);
        parent_value.set<"id">(round);
        auto& children = ruvia::detail::db_entity_access<relation_owner_type>::ensure_relation_collection<"children">(entity);
        relation_target_type child_value(&resource);
        child_value.set<"id">(round + 1000);
        children.push_back(std::move(child_value));
        entity.reset<"parent">();
        entity.reset<"children">();
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(db_entity_self_referential_relation_can_build_a_finite_graph) {
    ruvia::test::counting_memory_resource resource;
    self_node root(&resource);
    root.set<"id">(1);
    auto& middle = ruvia::detail::db_entity_access<self_node>::emplace_relation<"parent">(root);
    middle.set<"id">(2);
    auto& leaf = ruvia::detail::db_entity_access<self_node>::emplace_relation<"parent">(middle);
    leaf.set<"id">(3);
    RUVIA_CHECK_EQ(root.get<"parent">().get<"parent">().get<"id">(), 3);
    root.reset<"parent">();
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(db_entity_relation_replacement_preserves_loaded_value_on_allocation_failure) {
    ruvia::test::counting_memory_resource upstream;
    equivalent_memory_resource target(&upstream);
    {
        self_node root(&target);
        auto& parent_value = ruvia::detail::db_entity_access<self_node>::emplace_relation<"parent">(root);
        parent_value.set<"id">(7);
        const auto allocations = upstream.live_allocations();
        target.reject_allocations();
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            (void)ruvia::detail::db_entity_access<self_node>::emplace_relation<"parent">(root);
        }));
        target.reject_allocations(false);
        RUVIA_CHECK(root.is_set<"parent">());
        RUVIA_CHECK(!root.is_null<"parent">());
        RUVIA_CHECK_EQ(root.get<"parent">().get<"id">(), 7);
        RUVIA_CHECK(root.get<"parent">().resource() == &target);
        RUVIA_CHECK_EQ(upstream.live_allocations(), allocations);
        auto moved = std::move(root);
        RUVIA_CHECK_EQ(moved.get<"parent">().get<"id">(), 7);
        moved.reset<"parent">();
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
}

RUVIA_TEST(db_entity_set_normalizes_nested_owned_values_to_entity_resource) {
    using owned_type = ruvia::db_entity<"owned", ruvia::db_column<"text", ruvia::string>,
        ruvia::db_column<"values", std::pmr::vector<std::optional<std::pmr::string>>>>;
    ruvia::test::counting_memory_resource source, target;
    {
        owned_type entity(&target);
        const auto baseline = target.live_allocations();
        {
            ruvia::string text(std::string(200, 'x'), {.resource_ = &source});
            entity.set<"text">(std::move(text));
            std::pmr::vector<std::optional<std::pmr::string>> values(&source);
            values.emplace_back(std::pmr::string(200, 'y', &source));
            values.emplace_back(std::nullopt);
            entity.set<"values">(std::move(values));
        }
        RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(std::string_view(entity.get<"text">()), std::string(200, 'x'));
        RUVIA_CHECK_EQ(std::string_view(*entity.get<"values">()[0]), std::string(200, 'y'));
        RUVIA_CHECK(entity.get<"values">()[0]->get_allocator().resource() == &target);
        entity.reset<"text">();
        entity.reset<"values">();
        RUVIA_CHECK_EQ(target.live_allocations(), baseline);
    }
    RUVIA_CHECK_EQ(target.allocation_count(), target.deallocation_count());
}

RUVIA_TEST(db_entity_string_vector_assignment_normalizes_equivalent_resource_allocators) {
    using string_array_entity = ruvia::db_entity<"string_arrays",
        ruvia::db_column<"values", std::pmr::vector<std::pmr::string>>>;
    ruvia::test::counting_memory_resource upstream;
    equivalent_memory_resource target(&upstream);
    {
        string_array_entity entity(&target);
        {
            equivalent_memory_resource source_value(&upstream);
            RUVIA_CHECK(source_value.is_equal(target));
            std::pmr::vector<std::pmr::string> values(&source_value);
            values.emplace_back(200, 'a');
            values.emplace_back(240, 'b');
            entity.set<"values">(values);
            RUVIA_CHECK_EQ(values.size(), std::size_t{2});
            RUVIA_CHECK_EQ(values[0], std::string_view(std::string(200, 'a')));
            RUVIA_CHECK_EQ(values[1], std::string_view(std::string(240, 'b')));
            RUVIA_CHECK(values.get_allocator().resource() == &source_value);
            RUVIA_CHECK(values[0].get_allocator().resource() == &source_value);
        }
        const auto& lvalue_stored = entity.get<"values">();
        RUVIA_CHECK(lvalue_stored.get_allocator().resource() == &target);
        RUVIA_CHECK(lvalue_stored[0].get_allocator().resource() == &target);
        RUVIA_CHECK(lvalue_stored[1].get_allocator().resource() == &target);
        RUVIA_CHECK_EQ(lvalue_stored[0], std::string_view(std::string(200, 'a')));
        RUVIA_CHECK_EQ(lvalue_stored[1], std::string_view(std::string(240, 'b')));

        std::uintptr_t transferred_address = 0;
        {
            equivalent_memory_resource source_value(&upstream);
            std::pmr::vector<std::pmr::string> values(&source_value);
            values.emplace_back(260, 'b');
            values.emplace_back(280, 'c');
            transferred_address = reinterpret_cast<std::uintptr_t>(values[0].data());
            entity.set<"values">(std::move(values));
        }
        const auto& rvalue_stored = entity.get<"values">();
        RUVIA_CHECK(rvalue_stored.get_allocator().resource() == &target);
        RUVIA_CHECK(rvalue_stored[0].get_allocator().resource() == &target);
        RUVIA_CHECK(rvalue_stored[1].get_allocator().resource() == &target);
        RUVIA_CHECK_EQ(reinterpret_cast<std::uintptr_t>(rvalue_stored[0].data()), transferred_address);
        RUVIA_CHECK_EQ(rvalue_stored[0], std::string_view(std::string(260, 'b')));
        RUVIA_CHECK_EQ(rvalue_stored[1], std::string_view(std::string(280, 'c')));

        target.reject_allocations();
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            equivalent_memory_resource source_value(&upstream);
            std::pmr::vector<std::pmr::string> values(&source_value);
            values.emplace_back(220, 'd');
            entity.set<"values">(std::move(values));
        }));
        target.reject_allocations(false);
        RUVIA_CHECK(entity.is_set<"values">());
        RUVIA_CHECK_EQ(entity.get<"values">().size(), std::size_t{2});
        RUVIA_CHECK_EQ(entity.get<"values">()[0], std::string_view(std::string(260, 'b')));
        RUVIA_CHECK(entity.get<"values">().get_allocator().resource() == &target);
        RUVIA_CHECK(entity.get<"values">()[0].get_allocator().resource() == &target);
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(db_entity_reset_owned_containers_preserves_value_on_allocation_failure) {
    using owned_container_entity = ruvia::db_entity<"owned_containers",
        ruvia::db_column<"text", std::pmr::string>,
        ruvia::db_column<"values", std::pmr::vector<std::pmr::string>>>;
    for (const bool text : {false, true}) {
        ruvia::test::counting_memory_resource upstream;
        equivalent_memory_resource target(&upstream);
        {
            owned_container_entity entity(&target);
            entity.set<"text">(std::string(200, 'a'));
            std::pmr::vector<std::pmr::string> values(&upstream);
            values.emplace_back(240, 'b');
            entity.set<"values">(values);
            bool failed = false;
            target.reject_allocations();
            try {
                if (text) {
                    entity.reset<"text">();
                } else {
                    entity.reset<"values">();
                }
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            target.reject_allocations(false);
            if (failed) {
                RUVIA_CHECK(entity.is_set<"text">());
                RUVIA_CHECK(entity.is_set<"values">());
                RUVIA_CHECK_EQ(entity.get<"text">(), std::string_view(std::string(200, 'a')));
                RUVIA_CHECK_EQ(entity.get<"values">().size(), std::size_t{1});
                RUVIA_CHECK_EQ(entity.get<"values">()[0], std::string_view(std::string(240, 'b')));
                RUVIA_CHECK(entity.get<"text">().get_allocator().resource() == &target);
                RUVIA_CHECK(entity.get<"values">().get_allocator().resource() == &target);
                RUVIA_CHECK(entity.get<"values">()[0].get_allocator().resource() == &target);
            } else {
                // Implementations without debug proxies can clear without allocating.
                RUVIA_CHECK(text ? !entity.is_set<"text">() : !entity.is_set<"values">());
            }
        }
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
    }
}

RUVIA_TEST(db_entity_rows_mapping_owns_field_storage) {
    auto rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back(std::pmr::string("id", std::pmr::get_default_resource()));
    names.emplace_back(std::pmr::string("name", std::pmr::get_default_resource()));
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("9", std::pmr::get_default_resource()));
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("alice", std::pmr::get_default_resource()));
    auto& result_rows = ruvia::detail::db_result_access::rows(rows);
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    auto entities = ruvia::detail::map_entity_rows<entity>(std::move(rows));
    RUVIA_CHECK_EQ(entities[0].get<"id">(), 9);
    RUVIA_CHECK_EQ(entities[0].get<"name">(), std::string_view("alice"));
    const auto& const_entities = entities;
    RUVIA_CHECK_EQ(const_entities[0].get<"id">(), 9);
    std::size_t visited = 0;
    for (const auto& entity : const_entities) {
        RUVIA_CHECK_EQ(entity.get<"id">(), 9);
        ++visited;
    }
    RUVIA_CHECK_EQ(visited, std::size_t{1});
}

RUVIA_TEST(db_entity_mapping_handles_nullable_null_and_rejects_required_null) {
    auto make_rows = [](bool null_id, bool null_name) {
        auto rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
        auto& names = ruvia::detail::db_result_access::column_names(rows);
        names.emplace_back(std::pmr::string("id", std::pmr::get_default_resource()));
        names.emplace_back(std::pmr::string("name", std::pmr::get_default_resource()));
        auto& fields_value = ruvia::detail::db_result_access::fields(rows);
        fields_value.push_back(null_id ? ruvia::detail::db_result_access::null_field(std::pmr::get_default_resource())
                                       : ruvia::detail::db_result_access::owned_field("7", std::pmr::get_default_resource()));
        fields_value.push_back(null_name ? ruvia::detail::db_result_access::null_field(std::pmr::get_default_resource())
                                         : ruvia::detail::db_result_access::owned_field("Ada", std::pmr::get_default_resource()));
        auto& result_rows = ruvia::detail::db_result_access::rows(rows);
        result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(),
            std::pmr::get_default_resource()));
        return rows;
    };

    auto required_null = make_rows(true, false);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::detail::map_entity_rows<entity>(std::move(required_null)); }));

    auto nullable_null = make_rows(false, true);
    auto mapped = ruvia::detail::map_entity_rows<entity>(std::move(nullable_null));
    RUVIA_CHECK_EQ(mapped.size(), std::size_t{1});
    RUVIA_CHECK(mapped[0].is_null<"name">());
    RUVIA_CHECK(mapped[0].is_set<"name">());
}

RUVIA_TEST(db_entity_mapping_reports_invalid_and_overflow_numeric_fields) {
    ruvia::test::counting_memory_resource resource;
    auto make_rows = [&](std::string_view text) {
        auto rows = ruvia::detail::db_result_access::make_result(&resource);
        auto& names = ruvia::detail::db_result_access::column_names(rows);
        names.emplace_back(std::pmr::string("value", &resource));
        auto& fields_value = ruvia::detail::db_result_access::fields(rows);
        fields_value.push_back(ruvia::detail::db_result_access::owned_field(text, &resource));
        auto& result_rows = ruvia::detail::db_result_access::rows(rows);
        result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), &resource));
        return rows;
    };

    {
        auto rows = make_rows("not-a-number");
        bool invalid = false;
        try {
            (void)ruvia::detail::map_entity_rows<numeric_entity_type>(std::move(rows), &resource);
        } catch (const ruvia::db_conversion_error& error) {
            invalid = error.code() == ruvia::db_conversion_error::code_type::invalid_format;
        }
        RUVIA_CHECK(invalid);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});

    {
        auto rows = make_rows("999999999999999999999999999999");
        bool out_of_range = false;
        try {
            (void)ruvia::detail::map_entity_rows<numeric_entity_type>(std::move(rows), &resource);
        } catch (const ruvia::db_conversion_error& error) {
            out_of_range = error.code() == ruvia::db_conversion_error::code_type::out_of_range;
        }
        RUVIA_CHECK(out_of_range);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(db_entity_mapping_rejects_sql_null_for_non_nullable_scalars) {
    auto rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back(std::pmr::string("value", std::pmr::get_default_resource()));
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::null_field(std::pmr::get_default_resource()));
    ruvia::detail::db_result_access::rows(rows).push_back(ruvia::detail::db_result_access::borrowed_row(
        fields_value.data(), fields_value.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::detail::map_entity_rows<numeric_entity_type>(std::move(rows));
    }));
}

RUVIA_TEST(db_entity_computed_column_is_mapped_as_a_read_value) {
    auto rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back(std::pmr::string("first_name", std::pmr::get_default_resource()));
    names.emplace_back(std::pmr::string("display_name", std::pmr::get_default_resource()));
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("Ada", std::pmr::get_default_resource()));
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("Ada Lovelace", std::pmr::get_default_resource()));
    auto& result_rows = ruvia::detail::db_result_access::rows(rows);
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    auto entities = ruvia::detail::map_entity_rows<computed_entity_type>(std::move(rows));
    RUVIA_CHECK_EQ(entities[0].get<"display_name">(), std::string_view("Ada Lovelace"));
    entities[0].set<"display_name">("ignored by repository writes");
    RUVIA_CHECK_EQ(entities[0].get<"display_name">(), std::string_view("ignored by repository writes"));
}

RUVIA_TEST(db_entity_postgresql_array_codec_handles_quotes_null_and_empty) {
    auto rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back(std::pmr::string("tags", std::pmr::get_default_resource()));
    names.emplace_back(std::pmr::string("scores", std::pmr::get_default_resource()));
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::owned_field(R"({"a,b","q\"x"})", std::pmr::get_default_resource()));
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("{1,NULL,3}", std::pmr::get_default_resource()));
    auto& result_rows = ruvia::detail::db_result_access::rows(rows);
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    auto entities = ruvia::detail::map_entity_rows<array_entity_type>(std::move(rows));
    RUVIA_CHECK_EQ(entities[0].get<"tags">().size(), std::size_t{2});
    RUVIA_CHECK_EQ(entities[0].get<"tags">()[1], std::string_view("q\"x"));
    RUVIA_CHECK_EQ(entities[0].get<"scores">().size(), std::size_t{3});
    RUVIA_CHECK(!entities[0].get<"scores">()[1].has_value());
}

RUVIA_TEST(db_entity_array_codec_handles_empty_and_rejects_null_non_nullable_elements) {
    ruvia::test::counting_memory_resource resource;
    std::pmr::vector<std::pmr::string> values(&resource);
    const auto empty = ruvia::detail::db_result_access::owned_field("{}", &resource);
    ruvia::detail::decode_db_field(empty, values, &resource);
    RUVIA_CHECK(values.empty());
    const auto null_element = ruvia::detail::db_result_access::owned_field("{NULL}", &resource);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { ruvia::detail::decode_db_field(null_element, values, &resource); }));
    const auto malformed_number = ruvia::detail::db_result_access::owned_field("{1,not-a-number}", &resource);
    std::pmr::vector<int> numbers(&resource);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { ruvia::detail::decode_db_field(malformed_number, numbers, &resource); }));
    RUVIA_CHECK_EQ(values.size(), std::size_t{0});
}

RUVIA_TEST(db_entity_array_encoding_rejects_embedded_nul) {
    ruvia::test::counting_memory_resource resource;
    {
        std::pmr::vector<std::pmr::string> values(&resource);
        values.emplace_back("a\0b", 3);
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            (void)ruvia::detail::entity_db_value(values, &resource);
        }));
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(db_entity_array_encoding_preserves_string_segments_and_escapes) {
    ruvia::test::counting_memory_resource resource;
    {
        std::pmr::vector<std::pmr::string> values(&resource);
        values.emplace_back(4096, 'x');
        values.emplace_back("\"\\");
        values.emplace_back(128, '\\');
        values.emplace_back("NULL");
        values.emplace_back("");
        values.emplace_back("a,b{}\n\t");
        const auto encoded = ruvia::detail::entity_db_value(values, &resource);
        const auto text = ruvia::detail::db_value_access::text(encoded);
        const std::string expected = "{\"" + std::string(4096, 'x') + "\",\"\\\"\\\\\",\"" +
                                     std::string(256, '\\') + "\",\"NULL\",\"\",\"a,b{}\n\t\"}";
        RUVIA_CHECK_EQ(text, std::string_view(expected));
        std::pmr::vector<std::pmr::string> decoded(&resource);
        ruvia::detail::decode_db_field(ruvia::detail::db_result_access::borrowed_field(text, &resource), decoded, &resource);
        RUVIA_CHECK_EQ(values, decoded);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(db_entity_nullable_array_mapping_distinguishes_empty_and_sql_null) {
    auto rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back(std::pmr::string("tags", std::pmr::get_default_resource()));
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("{}", std::pmr::get_default_resource()));
    fields_value.push_back(ruvia::detail::db_result_access::null_field(std::pmr::get_default_resource()));
    auto& result_rows = ruvia::detail::db_result_access::rows(rows);
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), 1, names.data(), names.size(),
        std::pmr::get_default_resource()));
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data() + 1, 1, names.data(), names.size(),
        std::pmr::get_default_resource()));

    auto mapped = ruvia::detail::map_entity_rows<nullable_array_entity_type>(std::move(rows));
    RUVIA_CHECK_EQ(mapped.size(), std::size_t{2});
    RUVIA_CHECK(mapped[0].is_set<"tags">());
    RUVIA_CHECK(!mapped[0].is_null<"tags">());
    RUVIA_CHECK(mapped[0].get<"tags">().empty());
    RUVIA_CHECK(mapped[1].is_set<"tags">());
    RUVIA_CHECK(mapped[1].is_null<"tags">());
}

RUVIA_TEST(db_entity_rows_reject_elements_from_a_different_memory_resource) {
    ruvia::test::counting_memory_resource result_resource;
    ruvia::test::counting_memory_resource entity_resource;
    {
        ruvia::entity_rows<entity> rows(&result_resource);
        entity entity(&entity_resource);
        entity.set<"id">(7);
        entity.set<"name">(std::string(200, 'x'));
        RUVIA_CHECK(ruvia::testing::throws_on([&] { rows.push_back(std::move(entity)); }));
        RUVIA_CHECK(rows.empty());
    }
    RUVIA_CHECK_EQ(result_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(entity_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(entity_resource.allocation_count(), entity_resource.deallocation_count());
}

RUVIA_TEST(db_entity_mapping_exception_and_result_retention_respect_resources) {
    ruvia::test::tracking_resource resource;
    auto make_rows = [&] {
        auto rows = ruvia::detail::db_result_access::make_result(&resource);
        auto& names = ruvia::detail::db_result_access::column_names(rows);
        names.emplace_back(std::pmr::string("id", &resource));
        auto& fields_value = ruvia::detail::db_result_access::fields(rows);
        fields_value.push_back(ruvia::detail::db_result_access::owned_field("11", &resource));
        auto& result_rows = ruvia::detail::db_result_access::rows(rows);
        result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), &resource));
        return rows;
    };
    auto first = ruvia::detail::map_entity_rows<ruvia::db_entity<ruvia::fixed_string{"one"}, ruvia::db_column<ruvia::fixed_string{"id"}, int>>>(make_rows(), &resource);
    auto second = ruvia::detail::map_entity_rows<ruvia::db_entity<ruvia::fixed_string{"two"}, ruvia::db_column<ruvia::fixed_string{"id"}, int>>>(make_rows(), &resource);
    RUVIA_CHECK_EQ(first[0].get<"id">(), 11);
    RUVIA_CHECK_EQ(second[0].get<"id">(), 11);
    RUVIA_CHECK(resource.allocation_count() > 0);
}

}  // namespace
