#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/detail/redis/RedisEntityCodec.h"
#include "ruvia/web/detail/redis/RedisEntityKey.h"
#include "ruvia/web/detail/redis/RedisRepositoryConfig.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_DB_ENTITY(RedisUser, "user",
    RUVIA_DB_COLUMN(id, ruvia::String, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(active, bool),
    RUVIA_DB_COLUMN(age, std::int32_t, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(score, ruvia::Double, ruvia::DbColumnOptions{.nullable = true}));

RUVIA_DB_ENTITY(IntegerRedisUser, "integer_user",
    RUVIA_DB_COLUMN(id, std::int64_t, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, ruvia::String));

template <typename Fn>
bool throwsRedisProtocolError(Fn&& function) {
    try {
        function();
    } catch (const ruvia::RedisError& error) {
        return error.code() == ruvia::RedisError::Code::kProtocolError;
    } catch (...) {
    }
    return false;
}

}  // namespace

RUVIA_TEST(redis_entity_tracks_unset_value_and_null_states) {
    ruvia::test::CountingMemoryResource resource;
    RedisUser user(&resource);

    RUVIA_CHECK_EQ(user.resource(), &resource);
    RUVIA_CHECK_EQ(RedisUser::tableName(), std::string_view("user"));
    RUVIA_CHECK(!user.isSet<"id">());
    RUVIA_CHECK(!user.isNull<"id">());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)user.get<"id">(); }));

    user.set<"id">("u-1");
    user.set<"name">("Alice");
    user.set<"active">(true);
    user.set<"age">(32);
    user.set<"score">(ruvia::Double{4.5});
    RUVIA_CHECK(user.isSet<"id">());
    RUVIA_CHECK_EQ(user.get<"id">().view(), std::string_view("u-1"));
    RUVIA_CHECK_EQ(std::string_view(user.get<"name">()), std::string_view("Alice"));
    RUVIA_CHECK(user.get<"active">());
    RUVIA_CHECK_EQ(user.get<"age">(), 32);
    RUVIA_CHECK_EQ(static_cast<double>(user.get<"score">()), 4.5);

    user.setNull<"age">();
    RUVIA_CHECK(user.isSet<"age">());
    RUVIA_CHECK(user.isNull<"age">());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)user.get<"age">(); }));

    user.reset<"age">();
    RUVIA_CHECK(!user.isSet<"age">());
    RUVIA_CHECK(!user.isNull<"age">());
}

RUVIA_TEST(redis_entity_move_preserves_owned_values_and_resource) {
    ruvia::test::CountingMemoryResource resource;
    {
        RedisUser source(&resource);
        source.set<"id">(std::string(120, 'i'));
        source.set<"name">(std::string(120, 'n'));
        source.set<"active">(true);

        RedisUser moved(std::move(source));
        RUVIA_CHECK_EQ(moved.resource(), &resource);
        RUVIA_CHECK_EQ(moved.get<"id">().size(), std::size_t{120});
        RUVIA_CHECK_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
        RUVIA_CHECK(moved.get<"active">());
        source.set<"id">("reused");
        source.set<"name">("new name");
        RUVIA_CHECK_EQ(moved.get<"id">().size(), std::size_t{120});
        RUVIA_CHECK_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
        RUVIA_CHECK(resource.liveAllocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(redis_entity_codec_round_trips_scalars_and_binary_text) {
    ruvia::test::CountingMemoryResource resource;

    const auto integer = ruvia::detail::encodeRedisScalar(std::int64_t{-9223372036854775807LL},
        &resource);
    RUVIA_CHECK_EQ(integer, std::string_view("-9223372036854775807"));
    RUVIA_CHECK_EQ(ruvia::detail::decodeRedisScalar<std::int64_t>(integer, &resource),
        std::int64_t{-9223372036854775807LL});

    const auto unsignedInteger =
        ruvia::detail::encodeRedisScalar(std::uint64_t{18446744073709551615ULL}, &resource);
    RUVIA_CHECK_EQ(ruvia::detail::decodeRedisScalar<std::uint64_t>(unsignedInteger, &resource),
        std::uint64_t{18446744073709551615ULL});

    const auto floating = ruvia::detail::encodeRedisScalar(ruvia::Double{1.0 / 3.0}, &resource);
    const auto decodedFloating =
        ruvia::detail::decodeRedisScalar<ruvia::Double>(floating, &resource);
    RUVIA_CHECK_EQ(static_cast<double>(decodedFloating), 1.0 / 3.0);

    const std::string binary{"a\0b", 3};
    ruvia::String text(ruvia::ModelOptions{.resource = &resource});
    text.assignOwned(std::string_view(binary.data(), binary.size()));
    const auto encodedText = ruvia::detail::encodeRedisScalar(text, &resource);
    RUVIA_CHECK_EQ(encodedText.size(), std::size_t{3});
    RUVIA_CHECK_EQ(std::string_view(encodedText.data(), encodedText.size()),
        std::string_view(binary.data(), binary.size()));
    const auto decodedText =
        ruvia::detail::decodeRedisScalar<ruvia::String>(encodedText, &resource);
    RUVIA_CHECK_EQ(decodedText.view(), std::string_view(binary.data(), binary.size()));

    const auto encodedBool = ruvia::detail::encodeRedisScalar(true, &resource);
    RUVIA_CHECK_EQ(encodedBool, std::string_view("1"));
    RUVIA_CHECK(ruvia::detail::decodeRedisScalar<bool>(encodedBool, &resource));

    const auto encodedModelBool = ruvia::detail::encodeRedisScalar(ruvia::Bool{true}, &resource);
    RUVIA_CHECK_EQ(encodedModelBool, std::string_view("1"));
    RUVIA_CHECK(static_cast<bool>(
        ruvia::detail::decodeRedisScalar<ruvia::Bool>(encodedModelBool, &resource)));
    const auto encodedModelInteger =
        ruvia::detail::encodeRedisScalar(ruvia::Int64{-123}, &resource);
    RUVIA_CHECK_EQ(static_cast<std::int64_t>(
                       ruvia::detail::decodeRedisScalar<ruvia::Int64>(encodedModelInteger, &resource)),
        std::int64_t{-123});
}

RUVIA_TEST(redis_entity_codec_rejects_malformed_and_non_finite_scalars) {
    ruvia::test::CountingMemoryResource resource;
    RUVIA_CHECK(throwsRedisProtocolError(
        [&] { (void)ruvia::detail::decodeRedisScalar<std::int32_t>("12x", &resource); }));
    RUVIA_CHECK(throwsRedisProtocolError(
        [&] { (void)ruvia::detail::decodeRedisScalar<std::int32_t>("", &resource); }));
    RUVIA_CHECK(throwsRedisProtocolError(
        [&] { (void)ruvia::detail::decodeRedisScalar<bool>("true", &resource); }));
    RUVIA_CHECK(throwsRedisProtocolError(
        [&] { (void)ruvia::detail::decodeRedisScalar<double>("nan", &resource); }));
    RUVIA_CHECK(throwsRedisProtocolError([&] {
        (void)ruvia::detail::encodeRedisScalar(std::numeric_limits<double>::infinity(), &resource);
    }));
}

RUVIA_TEST(redis_entity_codec_iterates_database_column_descriptors) {
    std::size_t count = 0;
    std::string_view first;
    ruvia::detail::forEachRedisField<RedisUser>([&]<typename Field>() {
        if (count == 0) {
            first = Field::name.view();
        }
        ++count;
    });
    RUVIA_CHECK_EQ(count, std::size_t{5});
    RUVIA_CHECK_EQ(first, std::string_view("id"));
}

RUVIA_TEST(redis_entity_storage_keys_encode_runtime_namespace) {
    ruvia::test::CountingMemoryResource resource;
    const auto prefix = ruvia::detail::redisEntityStoragePrefix("user", &resource);
    RUVIA_CHECK_EQ(prefix, std::string_view("ruvia:orm:75736572:"));

    const auto key = ruvia::detail::redisEntityKey<RedisUser>("u-1", &resource);
    RUVIA_CHECK_EQ(key, std::string_view("ruvia:orm:75736572:u-1"));
    const auto custom = ruvia::detail::redisEntityKey<RedisUser>("u-1", &resource, "tenant-a");
    RUVIA_CHECK_EQ(custom, std::string_view("ruvia:orm:74656e616e742d61:u-1"));
}

RUVIA_TEST(redis_entity_ids_encode_string_and_integer_primary_keys) {
    ruvia::test::CountingMemoryResource resource;
    RedisUser text(&resource);
    text.set<"id">("u-1");
    RUVIA_CHECK_EQ(ruvia::detail::redisEntityId(text, &resource), std::string_view("u-1"));

    IntegerRedisUser integer(&resource);
    integer.set<"id">(std::int64_t{-42});
    RUVIA_CHECK_EQ(ruvia::detail::redisEntityId(integer, &resource), std::string_view("-42"));
    integer.set<"id">(std::int64_t{0});
    RUVIA_CHECK_EQ(ruvia::detail::redisEntityId(integer, &resource), std::string_view("0"));
}

RUVIA_TEST(redis_entity_mapping_owns_config_and_reclaims_storage) {
    ruvia::test::CountingMemoryResource resource;
    {
        ruvia::RedisRepositoryConfig config{
            .prefix = std::string(128, 'p'),
            .indexes = {{.column = "name", .kind = ruvia::RedisIndexKind::kTag, .sortable = true},
                {.column = "age", .kind = ruvia::RedisIndexKind::kNumeric}}};
        auto mapping = ruvia::detail::normalizeRedisMapping<RedisUser>(config, &resource);
        config.prefix.clear();
        config.indexes.clear();
        RUVIA_CHECK_EQ(mapping.prefix.size(), std::size_t{128});
        RUVIA_CHECK_EQ(mapping.indexKind("name"), ruvia::RedisIndexKind::kTag);
        RUVIA_CHECK(mapping.sortable("name"));
        RUVIA_CHECK_EQ(mapping.indexKind("age"), ruvia::RedisIndexKind::kNumeric);
        RUVIA_CHECK_EQ(mapping.indexKind("active"), ruvia::RedisIndexKind::kNone);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(redis_entity_mapping_rejects_unknown_duplicate_and_mismatched_indexes) {
    auto rejected = [](ruvia::RedisRepositoryConfig config) {
        return ruvia::testing::throwsOn([&] {
            (void)ruvia::detail::normalizeRedisMapping<RedisUser>(config, std::pmr::get_default_resource());
        });
    };
    RUVIA_CHECK(rejected({.indexes = {{.column = "missing"}}}));
    RUVIA_CHECK(rejected({.indexes = {{.column = "name"}, {.column = "name"}}}));
    RUVIA_CHECK(rejected({.indexes = {{.column = "name", .kind = ruvia::RedisIndexKind::kNumeric}}}));
    RUVIA_CHECK(rejected({.indexes = {{.column = "age", .kind = ruvia::RedisIndexKind::kText}}}));
    RUVIA_CHECK(rejected({.indexes = {{.column = "age", .kind = ruvia::RedisIndexKind::kTag}}}));
    RUVIA_CHECK(rejected({.indexes = {{.column = "name", .kind = ruvia::RedisIndexKind::kNone}}}));
}
