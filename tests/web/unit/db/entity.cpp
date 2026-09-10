#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/detail/db/DbEntityAccess.h"
#include "ruvia/web/detail/db/DbEntityCodec.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_TEST(db_entity_array_codec_preserves_null_text_precision_and_boolean_values) {
    ruvia::test::CountingMemoryResource resource;
    {
        std::pmr::vector<std::optional<std::pmr::string>> values(&resource);
        const auto field = ruvia::detail::DbResultAccess::ownedField("{\"NULL\",NULL,\"\",\"a,b\",\"a\\\"b\"}", &resource);
        ruvia::detail::decodeDbField(field, values, &resource);
        RUVIA_CHECK_EQ(values.size(), std::size_t{5});
        RUVIA_CHECK(values[0].has_value());
        RUVIA_CHECK_EQ(*values[0], std::string_view("NULL"));
        RUVIA_CHECK(!values[1].has_value());
        RUVIA_CHECK(values[2]->empty());
        RUVIA_CHECK_EQ(*values[3], std::string_view("a,b"));
        RUVIA_CHECK_EQ(*values[4], std::string_view("a\"b"));
        RUVIA_CHECK(values[0]->get_allocator().resource() == &resource);

        std::pmr::vector<double> input({1e-7, 1.23456789012345, -1e18}, &resource);
        auto encoded = ruvia::detail::entityDbValue(input, &resource);
        const auto numbers = ruvia::detail::DbResultAccess::borrowedField(ruvia::detail::DbValueAccess::text(encoded), &resource);
        std::pmr::vector<double> decoded(&resource);
        ruvia::detail::decodeDbField(numbers, decoded, &resource);
        RUVIA_CHECK(input == decoded);

        std::pmr::vector<bool> booleans({true, false, true}, &resource);
        auto encodedBools = ruvia::detail::entityDbValue(booleans, &resource);
        const auto boolField = ruvia::detail::DbResultAccess::borrowedField(ruvia::detail::DbValueAccess::text(encodedBools), &resource);
        std::pmr::vector<bool> decodedBools(&resource);
        ruvia::detail::decodeDbField(boolField, decodedBools, &resource);
        RUVIA_CHECK(booleans == decodedBools);
        const auto malformed = ruvia::detail::DbResultAccess::ownedField("{\"unterminated}", &resource);
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { ruvia::detail::decodeDbField(malformed, values, &resource); }));
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

using Entity = ruvia::DbEntity<ruvia::FixedString{"users"},
    ruvia::DbColumn<ruvia::FixedString{"id"}, int>,
    ruvia::DbColumn<ruvia::FixedString{"name"}, std::pmr::string,
        ruvia::DbColumnOptions{.nullable = true}>>;
using ComputedEntity = ruvia::DbEntity<"computed_users",
    ruvia::DbColumn<"first_name", std::pmr::string>,
    ruvia::DbColumn<"display_name", std::pmr::string,
        ruvia::DbColumnOptions{.generatedType = ruvia::DbGeneratedType::kStored}>>;
using ArrayEntity = ruvia::DbEntity<ruvia::FixedString{"events"},
    ruvia::DbColumn<ruvia::FixedString{"tags"}, std::pmr::vector<std::pmr::string>>,
    ruvia::DbColumn<ruvia::FixedString{"scores"}, std::pmr::vector<std::optional<int>>>>;
using NullableArrayEntity = ruvia::DbEntity<"nullable_events",
    ruvia::DbColumn<"tags", std::pmr::vector<std::pmr::string>,
        ruvia::DbColumnOptions{.nullable = true}>>;
using NumericEntity = ruvia::DbEntity<"numeric_values", ruvia::DbColumn<"value", int>>;

using RelationTarget = ruvia::DbEntity<"relation_targets", ruvia::DbColumn<"id", std::int64_t>>;
using RelationOwner = ruvia::DbEntity<"relation_owners", ruvia::DbColumn<"id", std::int64_t>,
    ruvia::DbManyToOne<"parent", RelationTarget, ruvia::DbJoinColumn<"id", "id">>,
    ruvia::DbOneToOne<"peer", RelationTarget, ruvia::DbJoinColumn<"id", "id">>,
    ruvia::DbOneToMany<"children", RelationTarget, "parent">,
    ruvia::DbManyToMany<"tags", RelationTarget, ruvia::DbJoinTable<"owner_tags", ruvia::DbJoinColumns<ruvia::DbJoinColumn<"owner_id", "id">>, ruvia::DbJoinColumns<ruvia::DbJoinColumn<"tag_id", "id">>>>>;

struct SelfNode;
using SelfNodeBase = ruvia::DbEntity<"self_nodes", ruvia::DbColumn<"id", std::int64_t>,
    ruvia::DbManyToOne<"parent", SelfNode, ruvia::DbJoinColumn<"parent_id", "id">>>;
struct SelfNode final : SelfNodeBase {
    using SelfNodeBase::SelfNodeBase;
};

RUVIA_TEST(db_entity_tracks_unset_null_and_value_states) {
    Entity entity;
    RUVIA_CHECK(!entity.isSet<"id">());
    entity.set<"id">(7);
    RUVIA_CHECK(entity.isSet<"id">());
    RUVIA_CHECK_EQ(entity.get<"id">(), 7);
    const auto& constEntity = entity;
    RUVIA_CHECK_EQ(constEntity.get<"id">(), 7);
    entity.setNull<"name">();
    RUVIA_CHECK(entity.isNull<"name">());
    entity.reset<"id">();
    RUVIA_CHECK(!entity.isSet<"id">());
    RUVIA_CHECK_EQ(Entity::tableName(), std::string_view("users"));
    RUVIA_CHECK_EQ(Entity::columnIndex<"name">(), std::size_t{1});
}

RUVIA_TEST(db_exec_result_exposes_affected_rows_and_optional_insert_id) {
    const auto inserted = ruvia::detail::DbResultAccess::makeExecResult(3, 91);
    RUVIA_CHECK_EQ(inserted.affectedRows(), std::uint64_t{3});
    RUVIA_CHECK(inserted.lastInsertId().has_value());
    RUVIA_CHECK_EQ(*inserted.lastInsertId(), std::uint64_t{91});

    const auto updated = ruvia::detail::DbResultAccess::makeExecResult(2);
    RUVIA_CHECK_EQ(updated.affectedRows(), std::uint64_t{2});
    RUVIA_CHECK(!updated.lastInsertId().has_value());
}

RUVIA_TEST(db_entity_relation_access_preserves_mixed_columns_and_relation_states) {
    ruvia::test::CountingMemoryResource resource;
    RelationOwner entity(&resource);
    entity.set<"id">(7);
    RUVIA_CHECK(entity.isSet<"id">());
    RUVIA_CHECK(!entity.isSet<"parent">());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)entity.get<"parent">(); }));

    auto& parent = ruvia::detail::DbEntityAccess<RelationOwner>::emplaceRelation<"parent">(entity);
    parent.set<"id">(11);
    RUVIA_CHECK(entity.isSet<"parent">());
    RUVIA_CHECK(!entity.isNull<"parent">());
    RUVIA_CHECK_EQ(entity.get<"parent">().get<"id">(), 11);

    ruvia::detail::DbEntityAccess<RelationOwner>::setRelationNull<"peer">(entity);
    RUVIA_CHECK(entity.isSet<"peer">());
    RUVIA_CHECK(entity.isNull<"peer">());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)entity.get<"peer">(); }));
    entity.reset<"peer">();
    RUVIA_CHECK(!entity.isSet<"peer">());

    auto& children = ruvia::detail::DbEntityAccess<RelationOwner>::ensureRelationCollection<"children">(entity);
    RUVIA_CHECK(entity.isSet<"children">());
    RUVIA_CHECK(children.empty());
    RUVIA_CHECK_EQ(entity.get<"id">(), 7);
    RUVIA_CHECK_EQ(entity.get<"parent">().get<"id">(), 11);
    entity.reset<"children">();
    RUVIA_CHECK(!entity.isSet<"children">());
}

RUVIA_TEST(db_entity_to_many_append_move_and_retained_result_use_entity_resource) {
    ruvia::test::CountingMemoryResource resource;
    RelationOwner entity(&resource);
    auto& children = ruvia::detail::DbEntityAccess<RelationOwner>::ensureRelationCollection<"children">(entity);
    for (std::int64_t id = 1; id <= 4; ++id) {
        RelationTarget child(&resource);
        child.set<"id">(id);
        children.push_back(std::move(child));
    }
    RUVIA_CHECK_EQ(children.size(), std::size_t{4});
    RUVIA_CHECK_EQ(children[0].get<"id">(), 1);
    RUVIA_CHECK_EQ(children[3].get<"id">(), 4);
    auto moved = std::move(entity);
    RUVIA_CHECK_EQ(moved.get<"children">().size(), std::size_t{4});
    RUVIA_CHECK_EQ(moved.get<"children">()[2].get<"id">(), 3);
    moved.reset<"children">();
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(db_entity_relation_repeated_load_reset_reclaims_nested_storage) {
    ruvia::test::CountingMemoryResource resource;
    RelationOwner entity(&resource);
    for (std::int64_t round = 0; round < 32; ++round) {
        auto& parent = ruvia::detail::DbEntityAccess<RelationOwner>::emplaceRelation<"parent">(entity);
        parent.set<"id">(round);
        auto& children = ruvia::detail::DbEntityAccess<RelationOwner>::ensureRelationCollection<"children">(entity);
        RelationTarget child(&resource);
        child.set<"id">(round + 1000);
        children.push_back(std::move(child));
        entity.reset<"parent">();
        entity.reset<"children">();
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(db_entity_self_referential_relation_can_build_a_finite_graph) {
    ruvia::test::CountingMemoryResource resource;
    SelfNode root(&resource);
    root.set<"id">(1);
    auto& middle = ruvia::detail::DbEntityAccess<SelfNode>::emplaceRelation<"parent">(root);
    middle.set<"id">(2);
    auto& leaf = ruvia::detail::DbEntityAccess<SelfNode>::emplaceRelation<"parent">(middle);
    leaf.set<"id">(3);
    RUVIA_CHECK_EQ(root.get<"parent">().get<"parent">().get<"id">(), 3);
    root.reset<"parent">();
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(db_entity_set_normalizes_nested_owned_values_to_entity_resource) {
    using Owned = ruvia::DbEntity<"owned", ruvia::DbColumn<"text", ruvia::String>,
        ruvia::DbColumn<"values", std::pmr::vector<std::optional<std::pmr::string>>>>;
    ruvia::test::CountingMemoryResource source, target;
    {
        Owned entity(&target);
        const auto baseline = target.liveAllocations();
        {
            ruvia::String text(std::string(200, 'x'), {.resource = &source});
            entity.set<"text">(std::move(text));
            std::pmr::vector<std::optional<std::pmr::string>> values(&source);
            values.emplace_back(std::pmr::string(200, 'y', &source));
            values.emplace_back(std::nullopt);
            entity.set<"values">(std::move(values));
        }
        RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(std::string_view(entity.get<"text">()), std::string(200, 'x'));
        RUVIA_CHECK_EQ(std::string_view(*entity.get<"values">()[0]), std::string(200, 'y'));
        RUVIA_CHECK(entity.get<"values">()[0]->get_allocator().resource() == &target);
        entity.reset<"text">();
        entity.reset<"values">();
        RUVIA_CHECK_EQ(target.liveAllocations(), baseline);
    }
    RUVIA_CHECK_EQ(target.allocationCount(), target.deallocationCount());
}

RUVIA_TEST(db_entity_rows_mapping_owns_field_storage) {
    auto rows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back(std::pmr::string("id", std::pmr::get_default_resource()));
    names.emplace_back(std::pmr::string("name", std::pmr::get_default_resource()));
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("9", std::pmr::get_default_resource()));
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("alice", std::pmr::get_default_resource()));
    auto& resultRows = ruvia::detail::DbResultAccess::rows(rows);
    resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    auto entities = ruvia::detail::mapDbEntityRows<Entity>(std::move(rows));
    RUVIA_CHECK_EQ(entities[0].get<"id">(), 9);
    RUVIA_CHECK_EQ(entities[0].get<"name">(), std::string_view("alice"));
    const auto& constEntities = entities;
    RUVIA_CHECK_EQ(constEntities[0].get<"id">(), 9);
    std::size_t visited = 0;
    for (const auto& entity : constEntities) {
        RUVIA_CHECK_EQ(entity.get<"id">(), 9);
        ++visited;
    }
    RUVIA_CHECK_EQ(visited, std::size_t{1});
}

RUVIA_TEST(db_entity_mapping_handles_nullable_null_and_rejects_required_null) {
    auto makeRows = [](bool nullId, bool nullName) {
        auto rows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
        auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
        names.emplace_back(std::pmr::string("id", std::pmr::get_default_resource()));
        names.emplace_back(std::pmr::string("name", std::pmr::get_default_resource()));
        auto& fields = ruvia::detail::DbResultAccess::fields(rows);
        fields.push_back(nullId ? ruvia::detail::DbResultAccess::nullField(std::pmr::get_default_resource())
                                : ruvia::detail::DbResultAccess::ownedField("7", std::pmr::get_default_resource()));
        fields.push_back(nullName ? ruvia::detail::DbResultAccess::nullField(std::pmr::get_default_resource())
                                  : ruvia::detail::DbResultAccess::ownedField("Ada", std::pmr::get_default_resource()));
        auto& resultRows = ruvia::detail::DbResultAccess::rows(rows);
        resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(),
            std::pmr::get_default_resource()));
        return rows;
    };

    auto requiredNull = makeRows(true, false);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)ruvia::detail::mapDbEntityRows<Entity>(std::move(requiredNull)); }));

    auto nullableNull = makeRows(false, true);
    auto mapped = ruvia::detail::mapDbEntityRows<Entity>(std::move(nullableNull));
    RUVIA_CHECK_EQ(mapped.size(), std::size_t{1});
    RUVIA_CHECK(mapped[0].isNull<"name">());
    RUVIA_CHECK(mapped[0].isSet<"name">());
}

RUVIA_TEST(db_entity_mapping_reports_invalid_and_overflow_numeric_fields) {
    ruvia::test::CountingMemoryResource resource;
    auto makeRows = [&](std::string_view text) {
        auto rows = ruvia::detail::DbResultAccess::makeResult(&resource);
        auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
        names.emplace_back(std::pmr::string("value", &resource));
        auto& fields = ruvia::detail::DbResultAccess::fields(rows);
        fields.push_back(ruvia::detail::DbResultAccess::ownedField(text, &resource));
        auto& resultRows = ruvia::detail::DbResultAccess::rows(rows);
        resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(), &resource));
        return rows;
    };

    {
        auto rows = makeRows("not-a-number");
        bool invalid = false;
        try {
            (void)ruvia::detail::mapDbEntityRows<NumericEntity>(std::move(rows), &resource);
        } catch (const ruvia::DbConversionError& error) {
            invalid = error.code() == ruvia::DbConversionError::Code::kInvalidFormat;
        }
        RUVIA_CHECK(invalid);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});

    {
        auto rows = makeRows("999999999999999999999999999999");
        bool outOfRange = false;
        try {
            (void)ruvia::detail::mapDbEntityRows<NumericEntity>(std::move(rows), &resource);
        } catch (const ruvia::DbConversionError& error) {
            outOfRange = error.code() == ruvia::DbConversionError::Code::kOutOfRange;
        }
        RUVIA_CHECK(outOfRange);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(db_entity_mapping_rejects_sql_null_for_non_nullable_scalars) {
    auto rows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back(std::pmr::string("value", std::pmr::get_default_resource()));
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::nullField(std::pmr::get_default_resource()));
    ruvia::detail::DbResultAccess::rows(rows).push_back(ruvia::detail::DbResultAccess::borrowedRow(
        fields.data(), fields.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)ruvia::detail::mapDbEntityRows<NumericEntity>(std::move(rows));
    }));
}

RUVIA_TEST(db_entity_computed_column_is_mapped_as_a_read_value) {
    auto rows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back(std::pmr::string("first_name", std::pmr::get_default_resource()));
    names.emplace_back(std::pmr::string("display_name", std::pmr::get_default_resource()));
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("Ada", std::pmr::get_default_resource()));
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("Ada Lovelace", std::pmr::get_default_resource()));
    auto& resultRows = ruvia::detail::DbResultAccess::rows(rows);
    resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    auto entities = ruvia::detail::mapDbEntityRows<ComputedEntity>(std::move(rows));
    RUVIA_CHECK_EQ(entities[0].get<"display_name">(), std::string_view("Ada Lovelace"));
    entities[0].set<"display_name">("ignored by repository writes");
    RUVIA_CHECK_EQ(entities[0].get<"display_name">(), std::string_view("ignored by repository writes"));
}

RUVIA_TEST(db_entity_postgresql_array_codec_handles_quotes_null_and_empty) {
    auto rows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back(std::pmr::string("tags", std::pmr::get_default_resource()));
    names.emplace_back(std::pmr::string("scores", std::pmr::get_default_resource()));
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::ownedField(R"({"a,b","q\"x"})", std::pmr::get_default_resource()));
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("{1,NULL,3}", std::pmr::get_default_resource()));
    auto& resultRows = ruvia::detail::DbResultAccess::rows(rows);
    resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    auto entities = ruvia::detail::mapDbEntityRows<ArrayEntity>(std::move(rows));
    RUVIA_CHECK_EQ(entities[0].get<"tags">().size(), std::size_t{2});
    RUVIA_CHECK_EQ(entities[0].get<"tags">()[1], std::string_view("q\"x"));
    RUVIA_CHECK_EQ(entities[0].get<"scores">().size(), std::size_t{3});
    RUVIA_CHECK(!entities[0].get<"scores">()[1].has_value());
}

RUVIA_TEST(db_entity_array_codec_handles_empty_and_rejects_null_non_nullable_elements) {
    ruvia::test::CountingMemoryResource resource;
    std::pmr::vector<std::pmr::string> values(&resource);
    const auto empty = ruvia::detail::DbResultAccess::ownedField("{}", &resource);
    ruvia::detail::decodeDbField(empty, values, &resource);
    RUVIA_CHECK(values.empty());
    const auto nullElement = ruvia::detail::DbResultAccess::ownedField("{NULL}", &resource);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { ruvia::detail::decodeDbField(nullElement, values, &resource); }));
    const auto malformedNumber = ruvia::detail::DbResultAccess::ownedField("{1,not-a-number}", &resource);
    std::pmr::vector<int> numbers(&resource);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { ruvia::detail::decodeDbField(malformedNumber, numbers, &resource); }));
    RUVIA_CHECK_EQ(values.size(), std::size_t{0});
}

RUVIA_TEST(db_entity_array_encoding_rejects_embedded_nul) {
    ruvia::test::CountingMemoryResource resource;
    {
        std::pmr::vector<std::pmr::string> values(&resource);
        values.emplace_back("a\0b", 3);
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            (void)ruvia::detail::entityDbValue(values, &resource);
        }));
        RUVIA_CHECK(resource.liveAllocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(db_entity_nullable_array_mapping_distinguishes_empty_and_sql_null) {
    auto rows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back(std::pmr::string("tags", std::pmr::get_default_resource()));
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("{}", std::pmr::get_default_resource()));
    fields.push_back(ruvia::detail::DbResultAccess::nullField(std::pmr::get_default_resource()));
    auto& resultRows = ruvia::detail::DbResultAccess::rows(rows);
    resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), 1, names.data(), names.size(),
        std::pmr::get_default_resource()));
    resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data() + 1, 1, names.data(), names.size(),
        std::pmr::get_default_resource()));

    auto mapped = ruvia::detail::mapDbEntityRows<NullableArrayEntity>(std::move(rows));
    RUVIA_CHECK_EQ(mapped.size(), std::size_t{2});
    RUVIA_CHECK(mapped[0].isSet<"tags">());
    RUVIA_CHECK(!mapped[0].isNull<"tags">());
    RUVIA_CHECK(mapped[0].get<"tags">().empty());
    RUVIA_CHECK(mapped[1].isSet<"tags">());
    RUVIA_CHECK(mapped[1].isNull<"tags">());
}

RUVIA_TEST(db_entity_rows_reject_elements_from_a_different_memory_resource) {
    ruvia::test::CountingMemoryResource resultResource;
    ruvia::test::CountingMemoryResource entityResource;
    {
        ruvia::DbEntityRows<Entity> rows(&resultResource);
        Entity entity(&entityResource);
        entity.set<"id">(7);
        entity.set<"name">(std::string(200, 'x'));
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { rows.push_back(std::move(entity)); }));
        RUVIA_CHECK(rows.empty());
    }
    RUVIA_CHECK_EQ(resultResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(entityResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(entityResource.allocationCount(), entityResource.deallocationCount());
}

RUVIA_TEST(db_entity_mapping_exception_and_result_retention_respect_resources) {
    ruvia::test::TrackingResource resource;
    auto makeRows = [&] {
        auto rows = ruvia::detail::DbResultAccess::makeResult(&resource);
        auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
        names.emplace_back(std::pmr::string("id", &resource));
        auto& fields = ruvia::detail::DbResultAccess::fields(rows);
        fields.push_back(ruvia::detail::DbResultAccess::ownedField("11", &resource));
        auto& resultRows = ruvia::detail::DbResultAccess::rows(rows);
        resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(), &resource));
        return rows;
    };
    auto first = ruvia::detail::mapDbEntityRows<ruvia::DbEntity<ruvia::FixedString{"one"}, ruvia::DbColumn<ruvia::FixedString{"id"}, int>>>(makeRows(), &resource);
    auto second = ruvia::detail::mapDbEntityRows<ruvia::DbEntity<ruvia::FixedString{"two"}, ruvia::DbColumn<ruvia::FixedString{"id"}, int>>>(makeRows(), &resource);
    RUVIA_CHECK_EQ(first[0].get<"id">(), 11);
    RUVIA_CHECK_EQ(second[0].get<"id">(), 11);
    RUVIA_CHECK(resource.allocationCount() > 0);
}

}  // namespace
