#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/redis/RedisQueryCompile.h"
#include "ruvia/web/redis/RedisEntity.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_REDIS_ENTITY(RedisQueryUser, "query_users",
    RUVIA_REDIS_COLUMN(id, std::int64_t,
        ruvia::RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(email, std::pmr::string),
    RUVIA_REDIS_COLUMN(title, std::pmr::string),
    RUVIA_REDIS_COLUMN(age, std::int32_t),
    RUVIA_REDIS_COLUMN(ratio, float),
    RUVIA_REDIS_COLUMN(active, bool,
        ruvia::RedisColumnOptions{.nullable = true}));

RUVIA_REDIS_ENTITY(OtherRedisQueryUser, "other_query_users",
    RUVIA_REDIS_COLUMN(id, std::int64_t,
        ruvia::RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(email, std::pmr::string));

RUVIA_REDIS_ENTITY(RedisUnsignedKey, "unsigned_users",
    RUVIA_REDIS_COLUMN(id, std::uint64_t, ruvia::RedisColumnOptions{.primaryKey = true}));

ruvia::detail::RedisMapping makeMapping(std::pmr::memory_resource* resource) {
    ruvia::detail::RedisMapping result{
        std::pmr::string("query-user", resource),
        std::pmr::vector<ruvia::detail::RedisIndexDefinition>(resource)};
    result.indexes.emplace_back(std::pmr::string("email", resource),
        ruvia::RedisIndexKind::kTag, false);
    result.indexes.emplace_back(std::pmr::string("title", resource),
        ruvia::RedisIndexKind::kText, false);
    result.indexes.emplace_back(std::pmr::string("age", resource),
        ruvia::RedisIndexKind::kNumeric, true);
    result.indexes.emplace_back(std::pmr::string("ratio", resource),
        ruvia::RedisIndexKind::kNumeric, false);
    result.indexes.emplace_back(std::pmr::string("active", resource),
        ruvia::RedisIndexKind::kTag, false);
    return result;
}

const std::pmr::string& arg(const std::pmr::vector<std::pmr::string>& args,
    std::size_t index) {
    return args[index];
}

}  // namespace

RUVIA_TEST(redis_query_compiles_binary_and_empty_tag_literals_safely) {
    auto* const resource = std::pmr::get_default_resource();
    const auto mapping = makeMapping(resource);
    const auto binary = std::string("a\0b", 3);
    ruvia::redis_find_options binaryOptions{
        .where = RedisQueryUser::field<"email">() ==
                 std::string_view(binary.data(), binary.size())};
    const auto binaryArgs = ruvia::detail::compileRedisFind<RedisQueryUser>(
        binaryOptions, mapping, resource);
    RUVIA_CHECK_EQ(arg(binaryArgs, 2),
        std::string_view("@email:{x610062}"));

    ruvia::redis_find_options emptyOptions{
        .where = RedisQueryUser::field<"email">() == std::string_view{}};
    const auto emptyArgs = ruvia::detail::compileRedisFind<RedisQueryUser>(
        emptyOptions, mapping, resource);
    RUVIA_CHECK_EQ(arg(emptyArgs, 2), std::string_view("@email:{x}"));
}

RUVIA_TEST(redis_query_compiles_parenthesized_logical_numeric_and_order) {
    const auto mapping = makeMapping(std::pmr::get_default_resource());
    auto predicate = (RedisQueryUser::field<"email">() == "alice@example.com") &&
                     (RedisQueryUser::field<"age">() >= 18);
    ruvia::redis_find_options options{
        .where = std::move(predicate),
        .order = {{.field = "age", .direction = ruvia::redis_order_direction::descending}},
        .skip = 5,
        .take = 10};
    const auto args = ruvia::detail::compileRedisFind<RedisQueryUser>(
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
    const auto mapping = makeMapping(resource);
    ruvia::redis_find_options nullOptions{
        .where = RedisQueryUser::field<"active">().is_null()};
    const auto nullArgs = ruvia::detail::compileRedisFind<RedisQueryUser>(
        nullOptions, mapping, resource);
    RUVIA_CHECK_EQ(arg(nullArgs, 2),
        std::string_view("-@__ruvia_present_active:{1}"));

    ruvia::redis_find_options notEqualOptions{
        .where = RedisQueryUser::field<"email">() != "disabled"};
    const auto notEqualArgs = ruvia::detail::compileRedisFind<RedisQueryUser>(
        notEqualOptions, mapping, resource);
    RUVIA_CHECK_EQ(arg(notEqualArgs, 2), std::string_view(
                                             "(@__ruvia_present_email:{1} -@email:{x64697361626c6564})"));

    ruvia::redis_find_options numericNotEqualOptions{
        .where = RedisQueryUser::field<"age">() != 18};
    const auto numericNotEqualArgs = ruvia::detail::compileRedisFind<RedisQueryUser>(
        numericNotEqualOptions, mapping, resource);
    RUVIA_CHECK_EQ(arg(numericNotEqualArgs, 2), std::string_view(
                                                    "(@age:[-inf +inf] -@age:[18 18])"));

    ruvia::redis_find_options rangeOptions{
        .where = RedisQueryUser::field<"age">().between(18, 65)};
    const auto rangeArgs = ruvia::detail::compileRedisFind<RedisQueryUser>(
        rangeOptions, mapping, resource);
    RUVIA_CHECK_EQ(arg(rangeArgs, 2), std::string_view("@age:[18 65]"));

    ruvia::redis_find_options floatOptions{
        .where = RedisQueryUser::field<"ratio">() == 0.1F};
    const auto floatArgs = ruvia::detail::compileRedisFind<RedisQueryUser>(
        floatOptions, mapping, resource);
    const auto encodedFloat = ruvia::detail::encodeRedisNumber(0.1F, resource);
    std::pmr::string expectedFloat(resource);
    expectedFloat = "@ratio:[";
    expectedFloat.append(encodedFloat.data(), encodedFloat.size());
    expectedFloat.push_back(' ');
    expectedFloat.append(encodedFloat.data(), encodedFloat.size());
    expectedFloat.push_back(']');
    RUVIA_CHECK_EQ(arg(floatArgs, 2), expectedFloat);

    const std::int32_t ages[] = {18, 21, 65};
    ruvia::redis_find_options inOptions{
        .where = RedisQueryUser::field<"age">().in(std::span<const std::int32_t>(ages))};
    const auto inArgs = ruvia::detail::compileRedisFind<RedisQueryUser>(
        inOptions, mapping, resource);
    RUVIA_CHECK_EQ(arg(inArgs, 2), std::string_view(
                                       "(@age:[18 18]|@age:[21 21]|@age:[65 65])"));
}

RUVIA_TEST(redis_query_rejects_foreign_text_and_invalid_sorting) {
    auto* const resource = std::pmr::get_default_resource();
    const auto mapping = makeMapping(resource);
    ruvia::redis_find_options foreign{
        .where = OtherRedisQueryUser::field<"email">() == "alice@example.com"};
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)ruvia::detail::compileRedisFind<RedisQueryUser>(foreign, mapping, resource);
    }));

    ruvia::redis_find_options text{
        .where = RedisQueryUser::field<"title">() == "red*"};
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)ruvia::detail::compileRedisFind<RedisQueryUser>(text, mapping, resource);
    }));

    ruvia::redis_find_options twoOrders{
        .order = {{.field = "age"}, {.field = "age"}}};
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)ruvia::detail::compileRedisFind<RedisQueryUser>(twoOrders, mapping, resource);
    }));

    ruvia::redis_find_options notSortable{
        .order = {{.field = "email"}}};
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)ruvia::detail::compileRedisFind<RedisQueryUser>(notSortable, mapping, resource);
    }));

    ruvia::redis_find_options overflow{
        .where = RedisQueryUser::field<"age">() >
                 std::numeric_limits<std::int64_t>::max()};
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)ruvia::detail::compileRedisFind<RedisQueryUser>(overflow, mapping, resource);
    }));
}

RUVIA_TEST(redis_query_compiles_index_and_primary_key_fast_path) {
    auto* const resource = std::pmr::get_default_resource();
    const auto mapping = makeMapping(resource);
    const auto args = ruvia::detail::compileRedisIndex<RedisQueryUser>(mapping, resource);
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

    auto primary = RedisQueryUser::field<"id">() == 42;
    const auto key = ruvia::detail::redisPrimaryKey<RedisQueryUser>(primary, resource);
    RUVIA_CHECK(key.has_value());
    RUVIA_CHECK_EQ(*key, std::string_view("42"));

    auto largePrimary = RedisQueryUser::field<"id">() ==
                        std::numeric_limits<std::int64_t>::max();
    const auto largeKey = ruvia::detail::redisPrimaryKey<RedisQueryUser>(largePrimary, resource);
    RUVIA_CHECK(largeKey.has_value());
    RUVIA_CHECK_EQ(*largeKey, std::string_view("9223372036854775807"));

    auto unsignedPrimary = RedisUnsignedKey::field<"id">() == std::numeric_limits<std::uint64_t>::max();
    const auto unsignedKey = ruvia::detail::redisPrimaryKey<RedisUnsignedKey>(unsignedPrimary, resource);
    RUVIA_CHECK(unsignedKey.has_value());
    RUVIA_CHECK_EQ(*unsignedKey, std::string_view("18446744073709551615"));
}

RUVIA_TEST(redis_query_compiles_open_and_closed_numeric_bounds) {
    auto* resource = std::pmr::get_default_resource();
    const auto mapping = makeMapping(resource);
    std::pair<ruvia::redis_predicate, std::string_view> cases[] = {
        {RedisQueryUser::field<"age">() > 5, "@age:[(5 +inf]"},
        {RedisQueryUser::field<"age">() >= 5, "@age:[5 +inf]"},
        {RedisQueryUser::field<"age">() < 5, "@age:[-inf (5]"},
        {RedisQueryUser::field<"age">() <= 5, "@age:[-inf 5]"},
    };
    for (auto& [predicate, expected] : cases) {
        ruvia::redis_find_options options{.where = std::move(predicate)};
        const auto args = ruvia::detail::compileRedisFind<RedisQueryUser>(options, mapping, resource);
        RUVIA_CHECK_EQ(arg(args, 2), expected);
    }
}

RUVIA_TEST(redis_predicate_owns_literals_and_reclaims_composed_storage) {
    ruvia::test::CountingMemoryResource resource;
    struct default_resource_guard {
        std::pmr::memory_resource* previous;
        ~default_resource_guard() {
            std::pmr::set_default_resource(previous);
        }
    } guard{std::pmr::set_default_resource(&resource)};
    {
        auto mapping = makeMapping(&resource);
        std::string text(200, 'x');
        auto filter = RedisQueryUser::field<"email">() == text;
        text.assign(200, 'y');
        auto range = RedisQueryUser::field<"age">().between(18, 65);
        ruvia::redis_find_options options{.where = std::move(filter) && range};
        RUVIA_CHECK(filter.empty());
        const auto args = ruvia::detail::compileRedisFind<RedisQueryUser>(options, mapping, &resource);
        std::string expected{"(@email:{x"};
        for (int i = 0; i < 200; ++i) {
            expected += "78";
        }
        expected += "} @age:[18 65])";
        RUVIA_CHECK_EQ(std::string_view(arg(args, 2)), std::string_view(expected));
        RUVIA_CHECK(!range.empty());
        RUVIA_CHECK(resource.liveAllocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(redis_predicate_compiles_disjunction_not_in_and_model_scalars) {
    auto* resource = std::pmr::get_default_resource();
    auto mapping = makeMapping(resource);
    ruvia::redis_find_options options{
        .where = (RedisQueryUser::field<"age">() == ruvia::Int32{18}) ||
                 (RedisQueryUser::field<"active">() == ruvia::Bool{true})};
    auto args = ruvia::detail::compileRedisFind<RedisQueryUser>(options, mapping, resource);
    RUVIA_CHECK_EQ(arg(args, 2), std::string_view("(@age:[18 18]|@active:{x31})"));
    ruvia::redis_find_options excluded{.where = RedisQueryUser::field<"age">().not_in({18, 21})};
    auto excluded_args = ruvia::detail::compileRedisFind<RedisQueryUser>(excluded, mapping, resource);
    RUVIA_CHECK_EQ(arg(excluded_args, 2), std::string_view("(@__ruvia_present_age:{1} -(@age:[18 18]|@age:[21 21]))"));
}
