#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/redis/redis_query_compile.h"
#include "ruvia/web/redis/redis_entity.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_REDIS_ENTITY(redis_query_user, "query_users",
    RUVIA_REDIS_COLUMN(id, std::int64_t,
        ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(email, std::pmr::string),
    RUVIA_REDIS_COLUMN(title, std::pmr::string),
    RUVIA_REDIS_COLUMN(age, std::int32_t),
    RUVIA_REDIS_COLUMN(ratio, float),
    RUVIA_REDIS_COLUMN(active, bool,
        ruvia::redis_column_options{.nullable_ = true}));

RUVIA_REDIS_ENTITY(other_redis_query_user, "other_query_users",
    RUVIA_REDIS_COLUMN(id, std::int64_t,
        ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(email, std::pmr::string));

RUVIA_REDIS_ENTITY(redis_unsigned_key, "unsigned_users",
    RUVIA_REDIS_COLUMN(id, std::uint64_t, ruvia::redis_column_options{.primary_key_ = true}));

ruvia::detail::redis_mapping make_mapping(std::pmr::memory_resource* resource) {
    ruvia::detail::redis_mapping result_value{
        std::pmr::string("query-user", resource),
        std::pmr::vector<ruvia::detail::redis_index_definition>(resource)};
    result_value.indexes_.emplace_back(std::pmr::string("email", resource),
        ruvia::redis_index_kind::tag, false);
    result_value.indexes_.emplace_back(std::pmr::string("title", resource),
        ruvia::redis_index_kind::text, false);
    result_value.indexes_.emplace_back(std::pmr::string("age", resource),
        ruvia::redis_index_kind::numeric, true);
    result_value.indexes_.emplace_back(std::pmr::string("ratio", resource),
        ruvia::redis_index_kind::numeric, false);
    result_value.indexes_.emplace_back(std::pmr::string("active", resource),
        ruvia::redis_index_kind::tag, false);
    return result_value;
}

const std::pmr::string& arg(const std::pmr::vector<std::pmr::string>& args,
    std::size_t index) {
    return args[index];
}

}  // namespace

RUVIA_TEST(redis_query_compiles_binary_and_empty_tag_literals_safely) {
    auto* const resource = std::pmr::get_default_resource();
    const auto mapping = make_mapping(resource);
    const auto binary = std::string("a\0b", 3);
    ruvia::redis_find_options binary_options{
        .where_ = redis_query_user::field<"email">() ==
                  std::string_view(binary.data(), binary.size())};
    const auto binary_args = ruvia::detail::compile_redis_find<redis_query_user>(
        binary_options, mapping, resource);
    RUVIA_CHECK_EQ(arg(binary_args, 2),
        std::string_view("@email:{x610062}"));

    ruvia::redis_find_options empty_options{
        .where_ = redis_query_user::field<"email">() == std::string_view{}};
    const auto empty_args = ruvia::detail::compile_redis_find<redis_query_user>(
        empty_options, mapping, resource);
    RUVIA_CHECK_EQ(arg(empty_args, 2), std::string_view("@email:{x}"));
}

RUVIA_TEST(redis_query_compiles_parenthesized_logical_numeric_and_order) {
    const auto mapping = make_mapping(std::pmr::get_default_resource());
    auto predicate = (redis_query_user::field<"email">() == "alice@example.com") &&
                     (redis_query_user::field<"age">() >= 18);
    ruvia::redis_find_options options{
        .where_ = std::move(predicate),
        .order_ = {{.field_ = "age", .direction_ = ruvia::redis_order_direction::descending}},
        .skip_ = 5,
        .take_ = 10};
    const auto args = ruvia::detail::compile_redis_find<redis_query_user>(
        options, mapping, std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(arg(args, 2), std::string_view(
                                     "(@email:{x616c696365406578616d706c652e636f6d} "
                                     "@age:[18 +inf])"));
    RUVIA_CHECK_EQ(arg(args, 4), std::string_view("5"));
    RUVIA_CHECK_EQ(arg(args, 5), std::string_view("10"));
    RUVIA_CHECK_EQ(arg(args, 7), std::string_view("age"));
    RUVIA_CHECK_EQ(arg(args, 8), std::string_view("DESC"));
    RUVIA_CHECK_EQ(arg(args, 10), std::string_view("2"));
}

RUVIA_TEST(redis_query_compiles_null_between_and_in_with_presence) {
    auto* const resource = std::pmr::get_default_resource();
    const auto mapping = make_mapping(resource);
    ruvia::redis_find_options null_options{
        .where_ = redis_query_user::field<"active">().is_null()};
    const auto null_args = ruvia::detail::compile_redis_find<redis_query_user>(
        null_options, mapping, resource);
    RUVIA_CHECK_EQ(arg(null_args, 2),
        std::string_view("-@__ruvia_present_active:{1}"));

    ruvia::redis_find_options not_equal_options{
        .where_ = redis_query_user::field<"email">() != "disabled"};
    const auto not_equal_args = ruvia::detail::compile_redis_find<redis_query_user>(
        not_equal_options, mapping, resource);
    RUVIA_CHECK_EQ(arg(not_equal_args, 2), std::string_view(
                                               "(@__ruvia_present_email:{1} -@email:{x64697361626c6564})"));

    ruvia::redis_find_options numeric_not_equal_options{
        .where_ = redis_query_user::field<"age">() != 18};
    const auto numeric_not_equal_args = ruvia::detail::compile_redis_find<redis_query_user>(
        numeric_not_equal_options, mapping, resource);
    RUVIA_CHECK_EQ(arg(numeric_not_equal_args, 2), std::string_view(
                                                       "(@age:[-inf +inf] -@age:[18 18])"));

    ruvia::redis_find_options range_options{
        .where_ = redis_query_user::field<"age">().between(18, 65)};
    const auto range_args = ruvia::detail::compile_redis_find<redis_query_user>(
        range_options, mapping, resource);
    RUVIA_CHECK_EQ(arg(range_args, 2), std::string_view("@age:[18 65]"));

    ruvia::redis_find_options float_options{
        .where_ = redis_query_user::field<"ratio">() == 0.1F};
    const auto float_args = ruvia::detail::compile_redis_find<redis_query_user>(
        float_options, mapping, resource);
    const auto encoded_float = ruvia::detail::encode_redis_number(0.1F, resource);
    std::pmr::string expected_float(resource);
    expected_float = "@ratio:[";
    expected_float.append(encoded_float.data(), encoded_float.size());
    expected_float.push_back(' ');
    expected_float.append(encoded_float.data(), encoded_float.size());
    expected_float.push_back(']');
    RUVIA_CHECK_EQ(arg(float_args, 2), expected_float);

    const std::int32_t ages[] = {18, 21, 65};
    ruvia::redis_find_options in_options{
        .where_ = redis_query_user::field<"age">().in(std::span<const std::int32_t>(ages))};
    const auto in_args = ruvia::detail::compile_redis_find<redis_query_user>(
        in_options, mapping, resource);
    RUVIA_CHECK_EQ(arg(in_args, 2), std::string_view(
                                        "(@age:[18 18]|@age:[21 21]|@age:[65 65])"));
}

RUVIA_TEST(redis_query_rejects_foreign_text_and_invalid_sorting) {
    auto* const resource = std::pmr::get_default_resource();
    const auto mapping = make_mapping(resource);
    ruvia::redis_find_options foreign{
        .where_ = other_redis_query_user::field<"email">() == "alice@example.com"};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::detail::compile_redis_find<redis_query_user>(foreign, mapping, resource);
    }));

    ruvia::redis_find_options text{
        .where_ = redis_query_user::field<"title">() == "red*"};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::detail::compile_redis_find<redis_query_user>(text, mapping, resource);
    }));

    ruvia::redis_find_options two_orders{
        .order_ = {{.field_ = "age"}, {.field_ = "age"}}};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::detail::compile_redis_find<redis_query_user>(two_orders, mapping, resource);
    }));

    ruvia::redis_find_options not_sortable{
        .order_ = {{.field_ = "email"}}};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::detail::compile_redis_find<redis_query_user>(not_sortable, mapping, resource);
    }));

    ruvia::redis_find_options overflow{
        .where_ = redis_query_user::field<"age">() >
                  std::numeric_limits<std::int64_t>::max()};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::detail::compile_redis_find<redis_query_user>(overflow, mapping, resource);
    }));
}

RUVIA_TEST(redis_query_compiles_index_and_primary_key_fast_path) {
    auto* const resource = std::pmr::get_default_resource();
    const auto mapping = make_mapping(resource);
    const auto args = ruvia::detail::compile_redis_index<redis_query_user>(mapping, resource);
    RUVIA_CHECK_EQ(arg(args, 0), std::string_view("FT.CREATE"));
    RUVIA_CHECK_EQ(arg(args, 1), std::string_view("query-user:idx"));
    RUVIA_CHECK_EQ(arg(args, 4), std::string_view("PREFIX"));
    RUVIA_CHECK_EQ(arg(args, 7), std::string_view("SCHEMA"));
    RUVIA_CHECK_EQ(arg(args, 8), std::string_view("__ruvia_tag_email"));
    RUVIA_CHECK_EQ(arg(args, 9), std::string_view("AS"));
    RUVIA_CHECK_EQ(arg(args, 10), std::string_view("email"));
    RUVIA_CHECK_EQ(arg(args, 11), std::string_view("TAG"));
    RUVIA_CHECK_EQ(arg(args, 12), std::string_view("CASESENSITIVE"));
    RUVIA_CHECK_EQ(arg(args, 13), std::string_view("__ruvia_present_email"));
    RUVIA_CHECK_EQ(arg(args, 14), std::string_view("AS"));
    RUVIA_CHECK_EQ(arg(args, 15), std::string_view("__ruvia_present_email"));

    auto primary = redis_query_user::field<"id">() == 42;
    const auto key = ruvia::detail::redis_primary_key<redis_query_user>(primary, resource);
    RUVIA_CHECK(key.has_value());
    RUVIA_CHECK_EQ(*key, std::string_view("42"));

    auto large_primary = redis_query_user::field<"id">() ==
                         std::numeric_limits<std::int64_t>::max();
    const auto large_key = ruvia::detail::redis_primary_key<redis_query_user>(large_primary, resource);
    RUVIA_CHECK(large_key.has_value());
    RUVIA_CHECK_EQ(*large_key, std::string_view("9223372036854775807"));

    auto unsigned_primary = redis_unsigned_key::field<"id">() == std::numeric_limits<std::uint64_t>::max();
    const auto unsigned_key = ruvia::detail::redis_primary_key<redis_unsigned_key>(unsigned_primary, resource);
    RUVIA_CHECK(unsigned_key.has_value());
    RUVIA_CHECK_EQ(*unsigned_key, std::string_view("18446744073709551615"));
}

RUVIA_TEST(redis_query_compiles_open_and_closed_numeric_bounds) {
    auto* resource = std::pmr::get_default_resource();
    const auto mapping = make_mapping(resource);
    std::pair<ruvia::redis_predicate, std::string_view> cases[] = {
        {redis_query_user::field<"age">() > 5, "@age:[(5 +inf]"},
        {redis_query_user::field<"age">() >= 5, "@age:[5 +inf]"},
        {redis_query_user::field<"age">() < 5, "@age:[-inf (5]"},
        {redis_query_user::field<"age">() <= 5, "@age:[-inf 5]"},
    };
    for (auto& [predicate, expected] : cases) {
        ruvia::redis_find_options options{.where_ = std::move(predicate)};
        const auto args = ruvia::detail::compile_redis_find<redis_query_user>(options, mapping, resource);
        RUVIA_CHECK_EQ(arg(args, 2), expected);
    }
}

RUVIA_TEST(redis_predicate_owns_literals_and_reclaims_composed_storage) {
    ruvia::test::counting_memory_resource resource;
    struct default_resource_guard {
        std::pmr::memory_resource* previous_;
        ~default_resource_guard() {
            std::pmr::set_default_resource(previous_);
        }
    } guard_value{std::pmr::set_default_resource(&resource)};
    {
        auto mapping = make_mapping(&resource);
        std::string text(200, 'x');
        auto filter = redis_query_user::field<"email">() == text;
        text.assign(200, 'y');
        auto range = redis_query_user::field<"age">().between(18, 65);
        ruvia::redis_find_options options{.where_ = std::move(filter) && range};
        RUVIA_CHECK(filter.empty());
        const auto args = ruvia::detail::compile_redis_find<redis_query_user>(options, mapping, &resource);
        std::string expected{"(@email:{x"};
        for (int i = 0; i < 200; ++i) {
            expected += "78";
        }
        expected += "} @age:[18 65])";
        RUVIA_CHECK_EQ(std::string_view(arg(args, 2)), std::string_view(expected));
        RUVIA_CHECK(!range.empty());
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(redis_predicate_compiles_disjunction_not_in_and_model_scalars) {
    auto* resource = std::pmr::get_default_resource();
    auto mapping = make_mapping(resource);
    ruvia::redis_find_options options{
        .where_ = (redis_query_user::field<"age">() == ruvia::int32{18}) ||
                  (redis_query_user::field<"active">() == ruvia::bool_value{true})};
    auto args = ruvia::detail::compile_redis_find<redis_query_user>(options, mapping, resource);
    RUVIA_CHECK_EQ(arg(args, 2), std::string_view("(@age:[18 18]|@active:{x31})"));
    ruvia::redis_find_options excluded{.where_ = redis_query_user::field<"age">().not_in({18, 21})};
    auto excluded_args = ruvia::detail::compile_redis_find<redis_query_user>(excluded, mapping, resource);
    RUVIA_CHECK_EQ(arg(excluded_args, 2), std::string_view("(@__ruvia_present_age:{1} -(@age:[18 18]|@age:[21 21]))"));
}
