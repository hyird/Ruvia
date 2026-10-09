#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_relation_query.h"
#include "ruvia/web/detail/db/db_result_access.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;
using id_type = db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>;

using label_type = db_entity<"labels", id_type>;
using address_type = db_entity<"addresses", id_type,
    db_column<"label_id", std::int64_t>,
    db_many_to_one<"label", label_type, db_join_column<"label_id", "id">>>;
using account_type = db_entity<"accounts", id_type,
    db_column<"address_id", std::int64_t, db_column_options{.nullable_ = true}>,
    db_many_to_one<"address", address_type, db_join_column<"address_id", "id">>,
    db_one_to_one<"label", label_type, db_join_column<"id", "id">>,
    db_many_to_many<"labels", label_type, db_join_table<"account_labels", db_join_columns<db_join_column<"account_id", "id">>, db_join_columns<db_join_column<"label_id", "id">>>>>;

struct child;
struct parent;
RUVIA_DB_ENTITY(parent, "parents", id_type,
    db_one_to_many<"children", child, "parent">)
RUVIA_DB_ENTITY(child, "children", id_type,
    db_column<"parent_id", std::int64_t>,
    db_many_to_one<"parent", parent, db_join_column<"parent_id", "id">>)

struct node;
RUVIA_DB_ENTITY(node, "nodes", id_type, db_column<"parent_id", std::int64_t, db_column_options{.nullable_ = true}>,
    RUVIA_DB_MANY_TO_ONE(parent, node, RUVIA_DB_JOIN_COLUMN(parent_id, id)),
    RUVIA_DB_ONE_TO_MANY(children, node, parent))

struct student;
struct course;
using enrollment_type = RUVIA_DB_JOIN_TABLE("enrollments", RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(student_id, id)), RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(course_id, id)));
RUVIA_DB_ENTITY(student, "students", id_type, RUVIA_DB_MANY_TO_MANY(courses, course, enrollment_type))
RUVIA_DB_ENTITY(course, "courses", id_type, RUVIA_DB_MANY_TO_MANY(students, student, RUVIA_DB_INVERSE(courses)))

struct user;
struct profile;
RUVIA_DB_ENTITY(user, "users", id_type, db_column<"profile_id", std::int64_t>,
    RUVIA_DB_ONE_TO_ONE(profile, profile, RUVIA_DB_JOIN_COLUMN(profile_id, id)))
RUVIA_DB_ENTITY(profile, "profiles", id_type, RUVIA_DB_ONE_TO_ONE(user, user, RUVIA_DB_INVERSE(profile)))

using pair_type = db_entity<"pairs", db_column<"left_id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"right_id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_many_to_one<"label", label_type, db_join_column<"left_id", "id">>>;
using no_pk_target_type = db_entity<"no_pk_targets", db_column<"id", std::int64_t>>;
using root_no_pk_target_type = db_entity<"root_no_pk_targets", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_many_to_one<"target", no_pk_target_type, db_join_column<"id", "id">>>;
using no_pk_root_type = db_entity<"no_pk_roots", db_column<"id", std::int64_t>,
    db_many_to_one<"label", label_type, db_join_column<"id", "id">>>;
void add_root_select(db_query& query) {
    query.select({query.column("id", "a")});
}

RUVIA_TEST(db_relation_plan_builds_all_four_join_kinds_and_nested_paths) {
    db_query query;
    query.from("accounts", "a");
    add_root_select(query);
    detail::db_relation_plan plan(query.resource());
    plan.add<account_type>(query, "a", "address", "addr", db_join_type::left);
    const auto address_statement = query.compile(db_driver::postgresql, query.resource(), db_parameter_mode::literal);
    const auto address_sql = address_statement.sql();
    RUVIA_CHECK(address_sql.find("LEFT JOIN \"addresses\" AS \"addr\"") != std::string_view::npos);
    RUVIA_CHECK(address_sql.find("\"a\".\"address_id\" = \"addr\".\"id\"") != std::string_view::npos);

    db_query one;
    one.from("accounts", "a");
    add_root_select(one);
    detail::db_relation_plan one_plan(one.resource());
    one_plan.add<account_type>(one, "a", "label", "lbl", db_join_type::inner);
    const auto one_statement = one.compile(db_driver::postgresql, one.resource(), db_parameter_mode::literal);
    const auto one_sql = one_statement.sql();
    RUVIA_CHECK(one_sql.find("INNER JOIN \"labels\" AS \"lbl\"") != std::string_view::npos);

    db_query inverse;
    inverse.from("parents", "p");
    inverse.select({inverse.column("id", "p")});
    detail::db_relation_plan inverse_plan(inverse.resource());
    inverse_plan.add<parent>(inverse, "p", "children", "ch");
    const auto inverse_statement = inverse.compile(db_driver::postgresql, inverse.resource(), db_parameter_mode::literal);
    const auto inverse_sql = inverse_statement.sql();
    RUVIA_CHECK(inverse_sql.find("\"p\".\"id\" = \"ch\".\"parent_id\"") != std::string_view::npos);

    db_query many;
    many.from("accounts", "a");
    add_root_select(many);
    detail::db_relation_plan many_plan(many.resource());
    many_plan.add<account_type>(many, "a", "labels", "lbl");
    const auto many_statement = many.compile(db_driver::postgresql, many.resource(), db_parameter_mode::literal);
    const auto many_sql = many_statement.sql();
    RUVIA_CHECK(many_sql.find("JOIN \"account_labels\"") != std::string_view::npos);
    RUVIA_CHECK(many_sql.find("\"a\".\"id\" = \"__ruvia_relation_") != std::string_view::npos);

    db_query nested;
    nested.from("accounts", "a");
    add_root_select(nested);
    detail::db_relation_plan nested_plan(nested.resource());
    nested_plan.add<account_type>(nested, "a", "address", "addr");
    nested_plan.add<account_type>(nested, "a", "address.label");
    RUVIA_CHECK_EQ(nested_plan.nodes().size(), std::size_t{2});
    RUVIA_CHECK_EQ(nested_plan.nodes()[1].path_, std::string_view("address.label"));
    nested_plan.add<account_type>(nested, "a", "addr.label");
    RUVIA_CHECK_EQ(nested_plan.nodes().size(), std::size_t{2});
    RUVIA_CHECK(testing::throws_on([&] { nested_plan.add<account_type>(nested, "a", "missing"); }));
    RUVIA_CHECK(testing::throws_on([&] { nested_plan.add<account_type>(nested, "a", "address."); }));
    RUVIA_CHECK(testing::throws_on([&] { nested_plan.add<account_type>(nested, "a", "labels", "addr"); }));
}

RUVIA_TEST(db_relation_plan_resolves_inverse_one_to_one_and_many_to_many) {
    db_query query;
    query.select(query.column("id", "c")).from("courses", "c");
    detail::db_relation_plan plan(query.resource());
    plan.add<course>(query, "c", "students", "s");
    const auto statement = query.compile(db_driver::postgresql, query.resource());
    RUVIA_CHECK(statement.sql().find("\"c\".\"id\" = \"__ruvia_relation_0\".\"course_id\"") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("\"__ruvia_relation_0\".\"student_id\" = \"s\".\"id\"") != std::string_view::npos);
    const auto maria = query.compile(db_driver::mariadb, query.resource());
    RUVIA_CHECK(maria.sql().find("`c`.`id` = `__ruvia_relation_0`.`course_id`") != std::string_view::npos);
    db_query one;
    one.select(one.column("id", "p")).from("profiles", "p");
    detail::db_relation_plan one_plan(one.resource());
    one_plan.add<profile>(one, "p", "user", "u");
    const auto one_statement = one.compile(db_driver::postgresql, one.resource());
    RUVIA_CHECK(one_statement.sql().find("\"p\".\"id\" = \"u\".\"profile_id\"") != std::string_view::npos);
}

RUVIA_TEST(db_relation_plan_rejects_malformed_paths_aliases_depth_and_missing_keys) {
    db_query query;
    query.select(query.column("id", "a")).from("accounts", "a");
    detail::db_relation_plan plan(query.resource());
    RUVIA_CHECK(testing::throws_on([&] { plan.add<account_type>(query, "a", "missing"); }));
    RUVIA_CHECK(testing::throws_on([&] { plan.add<account_type>(query, "a", "address."); }));
    RUVIA_CHECK(testing::throws_on([&] { plan.add<account_type>(query, "a", "address", "a.bad"); }));
    const std::string nul_alias{"a\0bad", 5};
    RUVIA_CHECK(testing::throws_on([&] { plan.add<account_type>(query, "a", "address", nul_alias); }));

    db_query no_target_query;
    no_target_query.select(no_target_query.column("id", "r")).from("root_no_pk_targets", "r");
    detail::db_relation_plan no_target_plan(no_target_query.resource());
    RUVIA_CHECK(testing::throws_on([&] { no_target_plan.add<root_no_pk_target_type>(no_target_query, "r", "target"); }));

    db_query no_root_query;
    no_root_query.select(no_root_query.column("id", "r")).from("no_pk_roots", "r");
    detail::db_relation_plan no_root_plan(no_root_query.resource());
    RUVIA_CHECK(testing::throws_on([&] { no_root_plan.add<no_pk_root_type>(no_root_query, "r", "label"); }));

    std::string deep;
    for (std::size_t i = 0; i < 257; ++i) {
        if (!deep.empty()) {
            deep.push_back('.');
        }
        deep.append("parent");
    }
    db_query deep_query;
    deep_query.select(deep_query.column("id", "n")).from("nodes", "n");
    detail::db_relation_plan deep_plan(deep_query.resource());
    RUVIA_CHECK(testing::throws_on([&] { deep_plan.add<node>(deep_query, "n", deep); }));
}

RUVIA_TEST(db_relation_plan_prepare_rejects_unsafe_paged_shapes_and_applies_scope_lock) {
    const auto make_plan = [](db_query& query) {
        detail::db_relation_plan plan(query.resource());
        plan.add<parent>(query, "p", "children");
        return plan;
    };

    db_query plain;
    plain.select(plain.column("id", "p")).from("parents", "p");
    auto plain_plan = make_plan(plain);
    RUVIA_CHECK(!plain_plan.prepare<parent>(plain, "p", db_driver::postgresql).has_value());

    db_query locked;
    locked.select(locked.column("id", "p")).from("parents", "p").lock({.mode_ = db_row_lock::update});
    auto locked_plan = make_plan(locked);
    auto locked_prepared = locked_plan.prepare<parent>(locked, "p", db_driver::postgresql);
    RUVIA_CHECK(locked_prepared.has_value());
    const auto locked_statement = locked_prepared->compile(db_driver::postgresql, nullptr);
    const auto locked_sql = locked_statement.sql();
    RUVIA_CHECK(locked_sql.find("FOR UPDATE OF \"p\"") != std::string_view::npos);

    db_query grouped;
    grouped.select(grouped.column("id", "p")).from("parents", "p").group_by({grouped.column("id", "p")}).limit(1);
    auto grouped_plan = make_plan(grouped);
    RUVIA_CHECK(testing::throws_on([&] { (void)grouped_plan.prepare<parent>(grouped, "p", db_driver::postgresql); }));

    db_query distinct;
    distinct.select(distinct.column("id", "p")).from("parents", "p").distinct_on({distinct.column("id", "p")}).limit(1);
    auto distinct_plan = make_plan(distinct);
    RUVIA_CHECK(testing::throws_on([&] { (void)distinct_plan.prepare<parent>(distinct, "p", db_driver::postgresql); }));

    db_query skipped;
    skipped.select(skipped.column("id", "p")).from("parents", "p").lock({.mode_ = db_row_lock::update, .skip_locked_ = true}).limit(1);
    auto skipped_plan = make_plan(skipped);
    RUVIA_CHECK(testing::throws_on([&] { (void)skipped_plan.prepare<parent>(skipped, "p", db_driver::postgresql); }));
}

void add_row(db_rows& result_value, const std::pmr::vector<std::pmr::string>& names,
    std::initializer_list<std::string_view> values, std::pmr::memory_resource* resource) {
    auto& fields_value = detail::db_result_access::fields(result_value);
    if (values.size() != names.size()) {
        throw std::logic_error("invalid relation fixture width");
    }
    for (const auto value : values) {
        fields_value.push_back(value == "<NULL>" ? detail::db_result_access::null_field(resource)
                                                 : detail::db_result_access::owned_field(value, resource));
    }
    // Rebind after vector growth; borrowed rows must point at the final storage.
    auto& rows = detail::db_result_access::rows(result_value);
    rows.clear();
    for (std::size_t offset = 0; offset < fields_value.size(); offset += names.size()) {
        rows.push_back(detail::db_result_access::borrowed_row(fields_value.data() + offset, names.size(), names.data(), names.size(), resource));
    }
}

RUVIA_TEST(db_relation_decoder_scopes_recursive_children_to_each_parent) {
    db_query query;
    query.select({query.column("id", "n"), query.column("parent_id", "n")}).from("nodes", "n");
    detail::db_relation_plan plan(query.resource());
    plan.add<node>(query, "n", "children.children");
    plan.add<node>(query, "n", "parent");
    std::pmr::vector<std::pmr::string> names(query.resource());
    names.emplace_back("id");
    names.emplace_back("parent_id");
    for (const auto& node : plan.nodes()) {
        for (const auto& name : node.columns_) {
            names.emplace_back(name);
        }
    }
    auto rows = detail::db_result_access::make_result(query.resource());
    add_row(rows, names, {"1", "9", "2", "1", "4", "2", "9", "<NULL>"}, query.resource());
    add_row(rows, names, {"1", "9", "2", "1", "5", "2", "9", "<NULL>"}, query.resource());
    add_row(rows, names, {"1", "9", "3", "1", "6", "3", "9", "<NULL>"}, query.resource());
    add_row(rows, names, {"1", "9", "2", "1", "4", "2", "9", "<NULL>"}, query.resource());
    auto result_value = detail::db_relation_decoder(plan, query.resource()).decode<node>(rows);
    RUVIA_CHECK_EQ(result_value.size(), std::size_t{1});
    const auto& children = result_value[0].get<"children">();
    RUVIA_CHECK_EQ(children.size(), std::size_t{2});
    RUVIA_CHECK_EQ(children[0].get<"children">().size(), std::size_t{2});
    RUVIA_CHECK_EQ(children[1].get<"children">()[0].get<"id">(), 6);
    RUVIA_CHECK_EQ(result_value[0].get<"parent">().get<"id">(), 9);
}

RUVIA_TEST(db_relation_decoder_hydrates_nested_collections_deduplicates_and_marks_empty) {
    test::counting_memory_resource resource;
    db_query query(&resource);
    query.from("accounts", "a");
    add_root_select(query);
    detail::db_relation_plan plan(&resource);
    plan.add<account_type>(query, "a", "address");
    plan.add<account_type>(query, "a", "labels");
    plan.add<account_type>(query, "a", "address.label");
    std::pmr::vector<std::pmr::string> names(&resource);
    names.emplace_back("id");
    names.emplace_back("address_id");
    for (const auto& node : plan.nodes()) {
        for (const auto& column : node.columns_) {
            names.emplace_back(column);
        }
    }
    auto rows = detail::db_result_access::make_result(&resource);
    add_row(rows, names, {"1", "10", "10", "100", "10", "100"}, &resource);
    add_row(rows, names, {"1", "10", "10", "100", "11", "100"}, &resource);
    add_row(rows, names, {"1", "10", "10", "100", "10", "100"}, &resource);
    add_row(rows, names, {"2", "<NULL>", "<NULL>", "<NULL>", "<NULL>", "<NULL>"}, &resource);
    auto result_value = detail::db_relation_decoder(plan, &resource).decode<account_type>(rows);
    RUVIA_CHECK_EQ(result_value.size(), std::size_t{2});
    RUVIA_CHECK_EQ(result_value[0].get<"labels">().size(), std::size_t{2});
    RUVIA_CHECK_EQ(result_value[0].get<"labels">()[1].get<"id">(), 11);
    RUVIA_CHECK(result_value[0].get<"address">().get<"id">() == 10);
    RUVIA_CHECK(result_value[0].get<"address">().get<"label">().get<"id">() == 100);
    RUVIA_CHECK(result_value[1].is_null<"address">());
    RUVIA_CHECK(result_value[1].is_set<"labels">());
    RUVIA_CHECK(result_value[1].get<"labels">().empty());
}

RUVIA_TEST(db_relation_decoder_rejects_conflicting_to_one_and_partial_composite_identity) {
    test::counting_memory_resource resource;
    db_query query(&resource);
    query.from("accounts", "a");
    add_root_select(query);
    detail::db_relation_plan plan(&resource);
    plan.add<account_type>(query, "a", "address");
    std::pmr::vector<std::pmr::string> names(&resource);
    names.emplace_back("id");
    names.emplace_back("address_id");
    for (const auto& node : plan.nodes()) {
        for (const auto& column : node.columns_) {
            names.emplace_back(column);
        }
    }
    auto rows = detail::db_result_access::make_result(&resource);
    add_row(rows, names, {"1", "10", "10", "100"}, &resource);
    add_row(rows, names, {"1", "10", "11", "100"}, &resource);
    RUVIA_CHECK(testing::throws_on([&] { (void)detail::db_relation_decoder(plan, &resource).decode<account_type>(rows); }));

    db_query composite(&resource);
    composite.from("pairs", "p");
    composite.select({composite.column("left_id", "p"), composite.column("right_id", "p")});
    detail::db_relation_plan empty_plan(&resource);
    empty_plan.add<pair_type>(composite, "p", "label");
    std::pmr::vector<std::pmr::string> composite_names(&resource);
    composite_names.emplace_back("left_id");
    composite_names.emplace_back("right_id");
    composite_names.emplace_back(empty_plan.nodes()[0].columns_[0]);
    auto composite_rows = detail::db_result_access::make_result(&resource);
    add_row(composite_rows, composite_names, {"1", "23", "1"}, &resource);
    add_row(composite_rows, composite_names, {"12", "3", "12"}, &resource);
    add_row(composite_rows, composite_names, {"1", "23", "1"}, &resource);
    auto pairs = detail::db_relation_decoder(empty_plan, &resource).decode<pair_type>(composite_rows);
    RUVIA_CHECK_EQ(pairs.size(), std::size_t{2});
    add_row(composite_rows, composite_names, {"<NULL>", "3", "12"}, &resource);
    RUVIA_CHECK(testing::throws_on([&] { (void)detail::db_relation_decoder(empty_plan, &resource).decode<pair_type>(composite_rows); }));
}

RUVIA_TEST(db_relation_decoder_rejects_malformed_projection_and_root_identity) {
    db_query query;
    query.from("parents", "p");
    query.select(query.column("id", "p"));
    detail::db_relation_plan plan(query.resource());
    plan.add<parent>(query, "p", "children");
    const auto child_id = plan.nodes()[0].columns_[0];
    const auto child_parent = plan.nodes()[0].columns_[1];

    std::pmr::vector<std::pmr::string> names(query.resource());
    names.emplace_back("id");
    names.emplace_back(child_id);
    names.emplace_back(child_parent);
    auto rows = detail::db_result_access::make_result(query.resource());
    add_row(rows, names, {"1", "10", "1"}, query.resource());
    auto missing = detail::db_result_access::make_result(query.resource());
    auto& missing_names = detail::db_result_access::column_names(missing);
    missing_names.emplace_back("id");
    auto& missing_fields = detail::db_result_access::fields(missing);
    missing_fields.push_back(detail::db_result_access::owned_field("1", query.resource()));
    detail::db_result_access::rows(missing).push_back(detail::db_result_access::borrowed_row(
        missing_fields.data(), missing_fields.size(), missing_names.data(), missing_names.size(), query.resource()));
    RUVIA_CHECK(testing::throws_on([&] { (void)detail::db_relation_decoder(plan, query.resource()).decode<parent>(missing); }));

    std::pmr::vector<std::pmr::string> duplicate_names(query.resource());
    duplicate_names.emplace_back("id");
    duplicate_names.emplace_back(child_id);
    duplicate_names.emplace_back(child_id);
    duplicate_names.emplace_back(child_parent);
    auto duplicate = detail::db_result_access::make_result(query.resource());
    add_row(duplicate, duplicate_names, {"1", "10", "10", "1"}, query.resource());
    RUVIA_CHECK(testing::throws_on([&] { (void)detail::db_relation_decoder(plan, query.resource()).decode<parent>(duplicate); }));

    auto null_root = detail::db_result_access::make_result(query.resource());
    add_row(null_root, names, {"<NULL>", "10", "1"}, query.resource());
    RUVIA_CHECK(testing::throws_on([&] { (void)detail::db_relation_decoder(plan, query.resource()).decode<parent>(null_root); }));

    auto two_roots = detail::db_result_access::make_result(query.resource());
    add_row(two_roots, names, {"1", "10", "1"}, query.resource());
    add_row(two_roots, names, {"2", "20", "2"}, query.resource());
    detail::db_map_one_related_entity<parent> one{plan.clone()};
    RUVIA_CHECK(testing::throws_on([&] { (void)one(std::move(two_roots), query.resource()); }));
}

RUVIA_TEST(db_relation_decoder_releases_operation_storage_and_retains_prior_result) {
    test::counting_memory_resource resource;
    {
        db_query query(&resource);
        query.from("accounts", "a");
        add_root_select(query);
        detail::db_relation_plan plan(&resource);
        plan.add<account_type>(query, "a", "address");
        std::pmr::vector<std::pmr::string> names(&resource);
        names.emplace_back("id");
        names.emplace_back("address_id");
        for (const auto& column : plan.nodes()[0].columns_) {
            names.emplace_back(column);
        }
        auto rows = detail::db_result_access::make_result(&resource);
        add_row(rows, names, {"1", "10", "10", "100"}, &resource);
        {
            auto retained = detail::db_relation_decoder(plan, &resource).decode<account_type>(rows);
            const auto baseline = resource.live_allocations();
            for (int i = 0; i < 12; ++i) {
                {
                    auto again = detail::db_relation_decoder(plan, &resource).decode<account_type>(rows);
                    RUVIA_CHECK_EQ(again[0].get<"address">().get<"id">(), 10);
                }
                RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            }
            RUVIA_CHECK_EQ(retained[0].get<"address">().get<"id">(), 10);
        }
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}
}  // namespace
