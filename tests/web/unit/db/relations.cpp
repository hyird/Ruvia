#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbRelationQuery.h"
#include "ruvia/web/detail/db/DbResultAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;
using Id = DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>;

using Label = DbEntity<"labels", Id>;
using Address = DbEntity<"addresses", Id,
    DbColumn<"label_id", std::int64_t>,
    DbManyToOne<"label", Label, DbJoinColumn<"label_id", "id">>>;
using Account = DbEntity<"accounts", Id,
    DbColumn<"address_id", std::int64_t, DbColumnOptions{.nullable = true}>,
    DbManyToOne<"address", Address, DbJoinColumn<"address_id", "id">>,
    DbOneToOne<"label", Label, DbJoinColumn<"id", "id">>,
    DbManyToMany<"labels", Label, DbJoinTable<"account_labels", DbJoinColumns<DbJoinColumn<"account_id", "id">>, DbJoinColumns<DbJoinColumn<"label_id", "id">>>>>;

struct Child;
using ParentBase = DbEntity<"parents", Id,
    DbOneToMany<"children", Child, "parent">>;
using ChildBase = DbEntity<"children", Id,
    DbColumn<"parent_id", std::int64_t>,
    DbManyToOne<"parent", ParentBase, DbJoinColumn<"parent_id", "id">>>;
struct Child final : ChildBase {
    using ChildBase::ChildBase;
};
using Parent = ParentBase;

struct Node;
RUVIA_DB_ENTITY(Node, "nodes", Id, DbColumn<"parent_id", std::int64_t, DbColumnOptions{.nullable = true}>,
    RUVIA_DB_MANY_TO_ONE(parent, Node, RUVIA_DB_JOIN_COLUMN(parent_id, id)),
    RUVIA_DB_ONE_TO_MANY(children, Node, parent))

struct Student;
struct Course;
using Enrollment = RUVIA_DB_JOIN_TABLE("enrollments", RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(student_id, id)), RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(course_id, id)));
RUVIA_DB_ENTITY(Student, "students", Id, RUVIA_DB_MANY_TO_MANY(courses, Course, Enrollment))
RUVIA_DB_ENTITY(Course, "courses", Id, RUVIA_DB_MANY_TO_MANY(students, Student, RUVIA_DB_INVERSE(courses)))

struct User;
struct Profile;
RUVIA_DB_ENTITY(User, "users", Id, DbColumn<"profile_id", std::int64_t>,
    RUVIA_DB_ONE_TO_ONE(profile, Profile, RUVIA_DB_JOIN_COLUMN(profile_id, id)))
RUVIA_DB_ENTITY(Profile, "profiles", Id, RUVIA_DB_ONE_TO_ONE(user, User, RUVIA_DB_INVERSE(profile)))

using Pair = DbEntity<"pairs", DbColumn<"left_id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbColumn<"right_id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbManyToOne<"label", Label, DbJoinColumn<"left_id", "id">>>;
using NoPkTarget = DbEntity<"no_pk_targets", DbColumn<"id", std::int64_t>>;
using RootNoPkTarget = DbEntity<"root_no_pk_targets", DbColumn<"id", std::int64_t, DbColumnOptions{.primaryKey = true}>,
    DbManyToOne<"target", NoPkTarget, DbJoinColumn<"id", "id">>>;
using NoPkRoot = DbEntity<"no_pk_roots", DbColumn<"id", std::int64_t>,
    DbManyToOne<"label", Label, DbJoinColumn<"id", "id">>>;
void addRootSelect(DbQuery& query) {
    query.select({query.column("id", "a")});
}

RUVIA_TEST(db_relation_plan_builds_all_four_join_kinds_and_nested_paths) {
    DbQuery query;
    query.from("accounts", "a");
    addRootSelect(query);
    detail::DbRelationPlan plan(query.resource());
    plan.add<Account>(query, "a", "address", "addr", DbJoinType::kLeft);
    const auto addressStatement = query.compile(DbDriver::kPostgreSql, query.resource(), DbParameterMode::kLiteral);
    const auto addressSql = addressStatement.sql();
    RUVIA_CHECK(addressSql.find("LEFT JOIN \"addresses\" AS \"addr\"") != std::string_view::npos);
    RUVIA_CHECK(addressSql.find("\"a\".\"address_id\" = \"addr\".\"id\"") != std::string_view::npos);

    DbQuery one;
    one.from("accounts", "a");
    addRootSelect(one);
    detail::DbRelationPlan onePlan(one.resource());
    onePlan.add<Account>(one, "a", "label", "lbl", DbJoinType::kInner);
    const auto oneStatement = one.compile(DbDriver::kPostgreSql, one.resource(), DbParameterMode::kLiteral);
    const auto oneSql = oneStatement.sql();
    RUVIA_CHECK(oneSql.find("INNER JOIN \"labels\" AS \"lbl\"") != std::string_view::npos);

    DbQuery inverse;
    inverse.from("parents", "p");
    inverse.select({inverse.column("id", "p")});
    detail::DbRelationPlan inversePlan(inverse.resource());
    inversePlan.add<Parent>(inverse, "p", "children", "ch");
    const auto inverseStatement = inverse.compile(DbDriver::kPostgreSql, inverse.resource(), DbParameterMode::kLiteral);
    const auto inverseSql = inverseStatement.sql();
    RUVIA_CHECK(inverseSql.find("\"p\".\"id\" = \"ch\".\"parent_id\"") != std::string_view::npos);

    DbQuery many;
    many.from("accounts", "a");
    addRootSelect(many);
    detail::DbRelationPlan manyPlan(many.resource());
    manyPlan.add<Account>(many, "a", "labels", "lbl");
    const auto manyStatement = many.compile(DbDriver::kPostgreSql, many.resource(), DbParameterMode::kLiteral);
    const auto manySql = manyStatement.sql();
    RUVIA_CHECK(manySql.find("JOIN \"account_labels\"") != std::string_view::npos);
    RUVIA_CHECK(manySql.find("\"a\".\"id\" = \"__ruvia_relation_") != std::string_view::npos);

    DbQuery nested;
    nested.from("accounts", "a");
    addRootSelect(nested);
    detail::DbRelationPlan nestedPlan(nested.resource());
    nestedPlan.add<Account>(nested, "a", "address", "addr");
    nestedPlan.add<Account>(nested, "a", "address.label");
    RUVIA_CHECK_EQ(nestedPlan.nodes().size(), std::size_t{2});
    RUVIA_CHECK_EQ(nestedPlan.nodes()[1].path, std::string_view("address.label"));
    nestedPlan.add<Account>(nested, "a", "addr.label");
    RUVIA_CHECK_EQ(nestedPlan.nodes().size(), std::size_t{2});
    RUVIA_CHECK(testing::throwsOn([&] { nestedPlan.add<Account>(nested, "a", "missing"); }));
    RUVIA_CHECK(testing::throwsOn([&] { nestedPlan.add<Account>(nested, "a", "address."); }));
    RUVIA_CHECK(testing::throwsOn([&] { nestedPlan.add<Account>(nested, "a", "labels", "addr"); }));
}

RUVIA_TEST(db_relation_plan_resolves_inverse_one_to_one_and_many_to_many) {
    DbQuery query;
    query.select(query.column("id", "c")).from("courses", "c");
    detail::DbRelationPlan plan(query.resource());
    plan.add<Course>(query, "c", "students", "s");
    const auto statement = query.compile(DbDriver::kPostgreSql, query.resource());
    RUVIA_CHECK(statement.sql().find("\"c\".\"id\" = \"__ruvia_relation_0\".\"course_id\"") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("\"__ruvia_relation_0\".\"student_id\" = \"s\".\"id\"") != std::string_view::npos);
    const auto maria = query.compile(DbDriver::kMariaDb, query.resource());
    RUVIA_CHECK(maria.sql().find("`c`.`id` = `__ruvia_relation_0`.`course_id`") != std::string_view::npos);
    DbQuery one;
    one.select(one.column("id", "p")).from("profiles", "p");
    detail::DbRelationPlan onePlan(one.resource());
    onePlan.add<Profile>(one, "p", "user", "u");
    const auto oneStatement = one.compile(DbDriver::kPostgreSql, one.resource());
    RUVIA_CHECK(oneStatement.sql().find("\"p\".\"id\" = \"u\".\"profile_id\"") != std::string_view::npos);
}

RUVIA_TEST(db_relation_plan_rejects_malformed_paths_aliases_depth_and_missing_keys) {
    DbQuery query;
    query.select(query.column("id", "a")).from("accounts", "a");
    detail::DbRelationPlan plan(query.resource());
    RUVIA_CHECK(testing::throwsOn([&] { plan.add<Account>(query, "a", "missing"); }));
    RUVIA_CHECK(testing::throwsOn([&] { plan.add<Account>(query, "a", "address."); }));
    RUVIA_CHECK(testing::throwsOn([&] { plan.add<Account>(query, "a", "address", "a.bad"); }));
    const std::string nulAlias{"a\0bad", 5};
    RUVIA_CHECK(testing::throwsOn([&] { plan.add<Account>(query, "a", "address", nulAlias); }));

    DbQuery noTargetQuery;
    noTargetQuery.select(noTargetQuery.column("id", "r")).from("root_no_pk_targets", "r");
    detail::DbRelationPlan noTargetPlan(noTargetQuery.resource());
    RUVIA_CHECK(testing::throwsOn([&] { noTargetPlan.add<RootNoPkTarget>(noTargetQuery, "r", "target"); }));

    DbQuery noRootQuery;
    noRootQuery.select(noRootQuery.column("id", "r")).from("no_pk_roots", "r");
    detail::DbRelationPlan noRootPlan(noRootQuery.resource());
    RUVIA_CHECK(testing::throwsOn([&] { noRootPlan.add<NoPkRoot>(noRootQuery, "r", "label"); }));

    std::string deep;
    for (std::size_t i = 0; i < 257; ++i) {
        if (!deep.empty()) {
            deep.push_back('.');
        }
        deep.append("parent");
    }
    DbQuery deepQuery;
    deepQuery.select(deepQuery.column("id", "n")).from("nodes", "n");
    detail::DbRelationPlan deepPlan(deepQuery.resource());
    RUVIA_CHECK(testing::throwsOn([&] { deepPlan.add<Node>(deepQuery, "n", deep); }));
}

RUVIA_TEST(db_relation_plan_prepare_rejects_unsafe_paged_shapes_and_applies_scope_lock) {
    const auto makePlan = [](DbQuery& query) {
        detail::DbRelationPlan plan(query.resource());
        plan.add<Parent>(query, "p", "children");
        return plan;
    };

    DbQuery plain;
    plain.select(plain.column("id", "p")).from("parents", "p");
    auto plainPlan = makePlan(plain);
    RUVIA_CHECK(!plainPlan.prepare<Parent>(plain, "p", DbDriver::kPostgreSql).has_value());

    DbQuery locked;
    locked.select(locked.column("id", "p")).from("parents", "p").lock({.mode = DbRowLock::kUpdate});
    auto lockedPlan = makePlan(locked);
    auto lockedPrepared = lockedPlan.prepare<Parent>(locked, "p", DbDriver::kPostgreSql);
    RUVIA_CHECK(lockedPrepared.has_value());
    const auto lockedStatement = lockedPrepared->compile(DbDriver::kPostgreSql, nullptr);
    const auto lockedSql = lockedStatement.sql();
    RUVIA_CHECK(lockedSql.find("FOR UPDATE OF \"p\"") != std::string_view::npos);

    DbQuery grouped;
    grouped.select(grouped.column("id", "p")).from("parents", "p").groupBy({grouped.column("id", "p")}).limit(1);
    auto groupedPlan = makePlan(grouped);
    RUVIA_CHECK(testing::throwsOn([&] { (void)groupedPlan.prepare<Parent>(grouped, "p", DbDriver::kPostgreSql); }));

    DbQuery distinct;
    distinct.select(distinct.column("id", "p")).from("parents", "p").distinctOn({distinct.column("id", "p")}).limit(1);
    auto distinctPlan = makePlan(distinct);
    RUVIA_CHECK(testing::throwsOn([&] { (void)distinctPlan.prepare<Parent>(distinct, "p", DbDriver::kPostgreSql); }));

    DbQuery skipped;
    skipped.select(skipped.column("id", "p")).from("parents", "p").lock({.mode = DbRowLock::kUpdate, .skipLocked = true}).limit(1);
    auto skippedPlan = makePlan(skipped);
    RUVIA_CHECK(testing::throwsOn([&] { (void)skippedPlan.prepare<Parent>(skipped, "p", DbDriver::kPostgreSql); }));
}

void addRow(DbRows& result, const std::pmr::vector<std::pmr::string>& names,
    std::initializer_list<std::string_view> values, std::pmr::memory_resource* resource) {
    auto& fields = detail::DbResultAccess::fields(result);
    if (values.size() != names.size()) {
        throw std::logic_error("invalid relation fixture width");
    }
    for (const auto value : values) {
        fields.push_back(value == "<NULL>" ? detail::DbResultAccess::nullField(resource)
                                           : detail::DbResultAccess::ownedField(value, resource));
    }
    // Rebind after vector growth; borrowed rows must point at the final storage.
    auto& rows = detail::DbResultAccess::rows(result);
    rows.clear();
    for (std::size_t offset = 0; offset < fields.size(); offset += names.size()) {
        rows.push_back(detail::DbResultAccess::borrowedRow(fields.data() + offset, names.size(), names.data(), names.size(), resource));
    }
}

RUVIA_TEST(db_relation_decoder_scopes_recursive_children_to_each_parent) {
    DbQuery query;
    query.select({query.column("id", "n"), query.column("parent_id", "n")}).from("nodes", "n");
    detail::DbRelationPlan plan(query.resource());
    plan.add<Node>(query, "n", "children.children");
    plan.add<Node>(query, "n", "parent");
    std::pmr::vector<std::pmr::string> names(query.resource());
    names.emplace_back("id");
    names.emplace_back("parent_id");
    for (const auto& node : plan.nodes()) {
        for (const auto& name : node.columns) {
            names.emplace_back(name);
        }
    }
    auto rows = detail::DbResultAccess::makeResult(query.resource());
    addRow(rows, names, {"1", "9", "2", "1", "4", "2", "9", "<NULL>"}, query.resource());
    addRow(rows, names, {"1", "9", "2", "1", "5", "2", "9", "<NULL>"}, query.resource());
    addRow(rows, names, {"1", "9", "3", "1", "6", "3", "9", "<NULL>"}, query.resource());
    addRow(rows, names, {"1", "9", "2", "1", "4", "2", "9", "<NULL>"}, query.resource());
    auto result = detail::DbRelationDecoder(plan, query.resource()).decode<Node>(rows);
    RUVIA_CHECK_EQ(result.size(), std::size_t{1});
    const auto& children = result[0].get<"children">();
    RUVIA_CHECK_EQ(children.size(), std::size_t{2});
    RUVIA_CHECK_EQ(children[0].get<"children">().size(), std::size_t{2});
    RUVIA_CHECK_EQ(children[1].get<"children">()[0].get<"id">(), 6);
    RUVIA_CHECK_EQ(result[0].get<"parent">().get<"id">(), 9);
}

RUVIA_TEST(db_relation_decoder_hydrates_nested_collections_deduplicates_and_marks_empty) {
    test::CountingMemoryResource resource;
    DbQuery query(&resource);
    query.from("accounts", "a");
    addRootSelect(query);
    detail::DbRelationPlan plan(&resource);
    plan.add<Account>(query, "a", "address");
    plan.add<Account>(query, "a", "labels");
    plan.add<Account>(query, "a", "address.label");
    std::pmr::vector<std::pmr::string> names(&resource);
    names.emplace_back("id");
    names.emplace_back("address_id");
    for (const auto& node : plan.nodes()) {
        for (const auto& column : node.columns) {
            names.emplace_back(column);
        }
    }
    auto rows = detail::DbResultAccess::makeResult(&resource);
    addRow(rows, names, {"1", "10", "10", "100", "10", "100"}, &resource);
    addRow(rows, names, {"1", "10", "10", "100", "11", "100"}, &resource);
    addRow(rows, names, {"1", "10", "10", "100", "10", "100"}, &resource);
    addRow(rows, names, {"2", "<NULL>", "<NULL>", "<NULL>", "<NULL>", "<NULL>"}, &resource);
    auto result = detail::DbRelationDecoder(plan, &resource).decode<Account>(rows);
    RUVIA_CHECK_EQ(result.size(), std::size_t{2});
    RUVIA_CHECK_EQ(result[0].get<"labels">().size(), std::size_t{2});
    RUVIA_CHECK_EQ(result[0].get<"labels">()[1].get<"id">(), 11);
    RUVIA_CHECK(result[0].get<"address">().get<"id">() == 10);
    RUVIA_CHECK(result[0].get<"address">().get<"label">().get<"id">() == 100);
    RUVIA_CHECK(result[1].isNull<"address">());
    RUVIA_CHECK(result[1].isSet<"labels">());
    RUVIA_CHECK(result[1].get<"labels">().empty());
}

RUVIA_TEST(db_relation_decoder_rejects_conflicting_to_one_and_partial_composite_identity) {
    test::CountingMemoryResource resource;
    DbQuery query(&resource);
    query.from("accounts", "a");
    addRootSelect(query);
    detail::DbRelationPlan plan(&resource);
    plan.add<Account>(query, "a", "address");
    std::pmr::vector<std::pmr::string> names(&resource);
    names.emplace_back("id");
    names.emplace_back("address_id");
    for (const auto& node : plan.nodes()) {
        for (const auto& column : node.columns) {
            names.emplace_back(column);
        }
    }
    auto rows = detail::DbResultAccess::makeResult(&resource);
    addRow(rows, names, {"1", "10", "10", "100"}, &resource);
    addRow(rows, names, {"1", "10", "11", "100"}, &resource);
    RUVIA_CHECK(testing::throwsOn([&] { (void)detail::DbRelationDecoder(plan, &resource).decode<Account>(rows); }));

    DbQuery composite(&resource);
    composite.from("pairs", "p");
    composite.select({composite.column("left_id", "p"), composite.column("right_id", "p")});
    detail::DbRelationPlan emptyPlan(&resource);
    emptyPlan.add<Pair>(composite, "p", "label");
    std::pmr::vector<std::pmr::string> compositeNames(&resource);
    compositeNames.emplace_back("left_id");
    compositeNames.emplace_back("right_id");
    compositeNames.emplace_back(emptyPlan.nodes()[0].columns[0]);
    auto compositeRows = detail::DbResultAccess::makeResult(&resource);
    addRow(compositeRows, compositeNames, {"1", "23", "1"}, &resource);
    addRow(compositeRows, compositeNames, {"12", "3", "12"}, &resource);
    addRow(compositeRows, compositeNames, {"1", "23", "1"}, &resource);
    auto pairs = detail::DbRelationDecoder(emptyPlan, &resource).decode<Pair>(compositeRows);
    RUVIA_CHECK_EQ(pairs.size(), std::size_t{2});
    addRow(compositeRows, compositeNames, {"<NULL>", "3", "12"}, &resource);
    RUVIA_CHECK(testing::throwsOn([&] { (void)detail::DbRelationDecoder(emptyPlan, &resource).decode<Pair>(compositeRows); }));
}

RUVIA_TEST(db_relation_decoder_rejects_malformed_projection_and_root_identity) {
    DbQuery query;
    query.from("parents", "p");
    query.select(query.column("id", "p"));
    detail::DbRelationPlan plan(query.resource());
    plan.add<Parent>(query, "p", "children");
    const auto childId = plan.nodes()[0].columns[0];
    const auto childParent = plan.nodes()[0].columns[1];

    std::pmr::vector<std::pmr::string> names(query.resource());
    names.emplace_back("id");
    names.emplace_back(childId);
    names.emplace_back(childParent);
    auto rows = detail::DbResultAccess::makeResult(query.resource());
    addRow(rows, names, {"1", "10", "1"}, query.resource());
    auto missing = detail::DbResultAccess::makeResult(query.resource());
    auto& missingNames = detail::DbResultAccess::columnNames(missing);
    missingNames.emplace_back("id");
    auto& missingFields = detail::DbResultAccess::fields(missing);
    missingFields.push_back(detail::DbResultAccess::ownedField("1", query.resource()));
    detail::DbResultAccess::rows(missing).push_back(detail::DbResultAccess::borrowedRow(
        missingFields.data(), missingFields.size(), missingNames.data(), missingNames.size(), query.resource()));
    RUVIA_CHECK(testing::throwsOn([&] { (void)detail::DbRelationDecoder(plan, query.resource()).decode<Parent>(missing); }));

    std::pmr::vector<std::pmr::string> duplicateNames(query.resource());
    duplicateNames.emplace_back("id");
    duplicateNames.emplace_back(childId);
    duplicateNames.emplace_back(childId);
    duplicateNames.emplace_back(childParent);
    auto duplicate = detail::DbResultAccess::makeResult(query.resource());
    addRow(duplicate, duplicateNames, {"1", "10", "10", "1"}, query.resource());
    RUVIA_CHECK(testing::throwsOn([&] { (void)detail::DbRelationDecoder(plan, query.resource()).decode<Parent>(duplicate); }));

    auto nullRoot = detail::DbResultAccess::makeResult(query.resource());
    addRow(nullRoot, names, {"<NULL>", "10", "1"}, query.resource());
    RUVIA_CHECK(testing::throwsOn([&] { (void)detail::DbRelationDecoder(plan, query.resource()).decode<Parent>(nullRoot); }));

    auto twoRoots = detail::DbResultAccess::makeResult(query.resource());
    addRow(twoRoots, names, {"1", "10", "1"}, query.resource());
    addRow(twoRoots, names, {"2", "20", "2"}, query.resource());
    detail::DbMapOneRelatedEntity<Parent> one{plan.clone()};
    RUVIA_CHECK(testing::throwsOn([&] { (void)one(std::move(twoRoots), query.resource()); }));
}

RUVIA_TEST(db_relation_decoder_releases_operation_storage_and_retains_prior_result) {
    test::CountingMemoryResource resource;
    {
        DbQuery query(&resource);
        query.from("accounts", "a");
        addRootSelect(query);
        detail::DbRelationPlan plan(&resource);
        plan.add<Account>(query, "a", "address");
        std::pmr::vector<std::pmr::string> names(&resource);
        names.emplace_back("id");
        names.emplace_back("address_id");
        for (const auto& column : plan.nodes()[0].columns) {
            names.emplace_back(column);
        }
        auto rows = detail::DbResultAccess::makeResult(&resource);
        addRow(rows, names, {"1", "10", "10", "100"}, &resource);
        {
            auto retained = detail::DbRelationDecoder(plan, &resource).decode<Account>(rows);
            const auto baseline = resource.liveAllocations();
            for (int i = 0; i < 12; ++i) {
                {
                    auto again = detail::DbRelationDecoder(plan, &resource).decode<Account>(rows);
                    RUVIA_CHECK_EQ(again[0].get<"address">().get<"id">(), 10);
                }
                RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            }
            RUVIA_CHECK_EQ(retained[0].get<"address">().get<"id">(), 10);
        }
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}
}  // namespace
