#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/detail/redis/redis_entity_codec.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/detail/redis/redis_repository_config.h"
#include "ruvia/web/redis/redis_entity.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_REDIS_ENTITY(redis_user, "user",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, std::pmr::string),
    RUVIA_REDIS_COLUMN(active, bool),
    RUVIA_REDIS_COLUMN(age, std::int32_t, ruvia::redis_column_options{.nullable_ = true}),
    RUVIA_REDIS_COLUMN(score, ruvia::double_value, ruvia::redis_column_options{.nullable_ = true}));

RUVIA_REDIS_ENTITY(integer_redis_user, "integer_user",
    RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string));

RUVIA_REDIS_ENTITY(nullable_redis_text, "text_value",
    RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(text, ruvia::string, ruvia::redis_column_options{.nullable_ = true}));

template <typename fn_type>
bool throws_redis_protocol_error(fn_type&& function) {
    try {
        function();
    } catch (const ruvia::redis_error& error) {
        return error.code() == ruvia::redis_error::code_type::protocol_error;
    } catch (...) {
    }
    return false;
}

}  // namespace

RUVIA_TEST(redis_entity_tracks_unset_value_and_null_states) {
    ruvia::test::counting_memory_resource resource;
    redis_user user_value(&resource);

    RUVIA_CHECK_EQ(user_value.resource(), &resource);
    RUVIA_CHECK_EQ(redis_user::prefix(), std::string_view("user"));
    RUVIA_CHECK(!user_value.is_set<"id">());
    RUVIA_CHECK(!user_value.is_null<"id">());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)user_value.get<"id">(); }));

    user_value.set<"id">("u-1");
    user_value.set<"name">("Alice");
    user_value.set<"active">(true);
    user_value.set<"age">(32);
    user_value.set<"score">(ruvia::double_value{4.5});
    RUVIA_CHECK(user_value.is_set<"id">());
    RUVIA_CHECK_EQ(user_value.get<"id">().view(), std::string_view("u-1"));
    RUVIA_CHECK_EQ(std::string_view(user_value.get<"name">()), std::string_view("Alice"));
    RUVIA_CHECK(user_value.get<"active">());
    RUVIA_CHECK_EQ(user_value.get<"age">(), 32);
    RUVIA_CHECK_EQ(static_cast<double>(user_value.get<"score">()), 4.5);

    user_value.set_null<"age">();
    RUVIA_CHECK(user_value.is_set<"age">());
    RUVIA_CHECK(user_value.is_null<"age">());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)user_value.get<"age">(); }));

    user_value.reset<"age">();
    RUVIA_CHECK(!user_value.is_set<"age">());
    RUVIA_CHECK(!user_value.is_null<"age">());
}

RUVIA_TEST(redis_entity_move_preserves_owned_values_and_resource) {
    ruvia::test::counting_memory_resource resource;
    {
        redis_user source_value(&resource);
        source_value.set<"id">(std::string(120, 'i'));
        source_value.set<"name">(std::string(120, 'n'));
        source_value.set<"active">(true);

        redis_user moved(std::move(source_value));
        RUVIA_CHECK_EQ(moved.resource(), &resource);
        RUVIA_CHECK_EQ(moved.get<"id">().size(), std::size_t{120});
        RUVIA_CHECK_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
        RUVIA_CHECK(moved.get<"active">());
        source_value.set<"id">("reused");
        source_value.set<"name">("new name");
        RUVIA_CHECK_EQ(moved.get<"id">().size(), std::size_t{120});
        RUVIA_CHECK_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(redis_entity_failed_owning_assignment_preserves_each_column_state) {
    const std::string replacement(256, 'x');
    for (int state_value = 0; state_value < 3; ++state_value) {
        ruvia::test::rejecting_memory_resource resource;
        nullable_redis_text entity(&resource);
        entity.set<"id">(7);
        if (state_value == 1) {
            entity.set_null<"text">();
        } else if (state_value == 2) {
            entity.set<"text">("retained");
        }
        resource.reject_allocations();
        RUVIA_CHECK(ruvia::testing::throws_on([&] { entity.set<"text">(replacement); }));
        resource.reject_allocations(false);
        RUVIA_CHECK_EQ(entity.is_set<"text">(), state_value != 0);
        RUVIA_CHECK_EQ(entity.is_null<"text">(), state_value == 1);
        RUVIA_CHECK_EQ(entity.get<"id">(), 7);
        if (state_value == 2) {
            RUVIA_CHECK_EQ(entity.get<"text">().view(), std::string_view("retained"));
            RUVIA_CHECK(entity.get<"text">().resource() == &resource);
        } else {
            RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)entity.get<"text">(); }));
        }
    }
}

RUVIA_TEST(redis_entity_codec_round_trips_scalars_and_binary_text) {
    ruvia::test::counting_memory_resource resource;

    const auto integer = ruvia::detail::encode_redis_scalar(std::int64_t{-9223372036854775807LL},
        &resource);
    RUVIA_CHECK_EQ(integer, std::string_view("-9223372036854775807"));
    RUVIA_CHECK_EQ(ruvia::detail::decode_redis_scalar<std::int64_t>(integer, &resource),
        std::int64_t{-9223372036854775807LL});

    const auto unsigned_integer =
        ruvia::detail::encode_redis_scalar(std::uint64_t{18446744073709551615ULL}, &resource);
    RUVIA_CHECK_EQ(ruvia::detail::decode_redis_scalar<std::uint64_t>(unsigned_integer, &resource),
        std::uint64_t{18446744073709551615ULL});

    const auto floating = ruvia::detail::encode_redis_scalar(ruvia::double_value{1.0 / 3.0}, &resource);
    const auto decoded_floating =
        ruvia::detail::decode_redis_scalar<ruvia::double_value>(floating, &resource);
    RUVIA_CHECK_EQ(static_cast<double>(decoded_floating), 1.0 / 3.0);

    const std::string binary{"a\0b", 3};
    ruvia::string text(ruvia::model_options{.resource_ = &resource});
    text.assign_owned(std::string_view(binary.data(), binary.size()));
    const auto encoded_text = ruvia::detail::encode_redis_scalar(text, &resource);
    RUVIA_CHECK_EQ(encoded_text.size(), std::size_t{3});
    RUVIA_CHECK_EQ(std::string_view(encoded_text.data(), encoded_text.size()),
        std::string_view(binary.data(), binary.size()));
    const auto decoded_text =
        ruvia::detail::decode_redis_scalar<ruvia::string>(encoded_text, &resource);
    RUVIA_CHECK_EQ(decoded_text.view(), std::string_view(binary.data(), binary.size()));

    const auto encoded_bool = ruvia::detail::encode_redis_scalar(true, &resource);
    RUVIA_CHECK_EQ(encoded_bool, std::string_view("1"));
    RUVIA_CHECK(ruvia::detail::decode_redis_scalar<bool>(encoded_bool, &resource));

    const auto encoded_model_bool = ruvia::detail::encode_redis_scalar(ruvia::bool_value{true}, &resource);
    RUVIA_CHECK_EQ(encoded_model_bool, std::string_view("1"));
    RUVIA_CHECK(static_cast<bool>(
        ruvia::detail::decode_redis_scalar<ruvia::bool_value>(encoded_model_bool, &resource)));
    const auto encoded_model_integer =
        ruvia::detail::encode_redis_scalar(ruvia::int64{-123}, &resource);
    RUVIA_CHECK_EQ(static_cast<std::int64_t>(
                       ruvia::detail::decode_redis_scalar<ruvia::int64>(encoded_model_integer, &resource)),
        std::int64_t{-123});
}

RUVIA_TEST(redis_entity_codec_rejects_malformed_and_non_finite_scalars) {
    ruvia::test::counting_memory_resource resource;
    RUVIA_CHECK(throws_redis_protocol_error(
        [&] { (void)ruvia::detail::decode_redis_scalar<std::int32_t>("12x", &resource); }));
    RUVIA_CHECK(throws_redis_protocol_error(
        [&] { (void)ruvia::detail::decode_redis_scalar<std::int32_t>("", &resource); }));
    RUVIA_CHECK(throws_redis_protocol_error(
        [&] { (void)ruvia::detail::decode_redis_scalar<bool>("true", &resource); }));
    RUVIA_CHECK(throws_redis_protocol_error(
        [&] { (void)ruvia::detail::decode_redis_scalar<double>("nan", &resource); }));
    RUVIA_CHECK(throws_redis_protocol_error([&] {
        (void)ruvia::detail::encode_redis_scalar(std::numeric_limits<double>::infinity(), &resource);
    }));
}

RUVIA_TEST(redis_entity_codec_iterates_redis_column_descriptors) {
    std::size_t count = 0;
    std::string_view first;
    ruvia::detail::for_each_redis_field<redis_user>([&]<typename field>() {
        if (count == 0) {
            first = field::name.view();
        }
        ++count;
    });
    RUVIA_CHECK_EQ(count, std::size_t{5});
    RUVIA_CHECK_EQ(first, std::string_view("id"));
}

RUVIA_TEST(redis_entity_storage_keys_encode_runtime_namespace) {
    ruvia::test::counting_memory_resource resource;
    const auto prefix = ruvia::detail::redis_entity_storage_prefix("user", &resource);
    RUVIA_CHECK_EQ(prefix, std::string_view("ruvia:orm:75736572:"));

    const auto key = ruvia::detail::redis_entity_key<redis_user>("u-1", &resource);
    RUVIA_CHECK_EQ(key, std::string_view("ruvia:orm:75736572:u-1"));
    const auto custom_value = ruvia::detail::redis_entity_key<redis_user>("u-1", &resource, "tenant-a");
    RUVIA_CHECK_EQ(custom_value, std::string_view("ruvia:orm:74656e616e742d61:u-1"));
}

RUVIA_TEST(redis_entity_ids_encode_string_and_integer_primary_keys) {
    ruvia::test::counting_memory_resource resource;
    redis_user text(&resource);
    text.set<"id">("u-1");
    RUVIA_CHECK_EQ(ruvia::detail::redis_entity_id(text, &resource), std::string_view("u-1"));

    integer_redis_user integer(&resource);
    integer.set<"id">(std::int64_t{-42});
    RUVIA_CHECK_EQ(ruvia::detail::redis_entity_id(integer, &resource), std::string_view("-42"));
    integer.set<"id">(std::int64_t{0});
    RUVIA_CHECK_EQ(ruvia::detail::redis_entity_id(integer, &resource), std::string_view("0"));
}

RUVIA_TEST(redis_entity_mapping_owns_config_and_reclaims_storage) {
    ruvia::test::counting_memory_resource resource;
    {
        ruvia::redis_repository_config config{
            .prefix_ = std::string(128, 'p'),
            .indexes_ = {{.field_ = "name", .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true},
                {.field_ = "age", .kind_ = ruvia::redis_index_kind::numeric}}};
        auto mapping = ruvia::detail::normalize_redis_mapping<redis_user>(config, &resource);
        config.prefix_.clear();
        config.indexes_.clear();
        RUVIA_CHECK_EQ(mapping.prefix_.size(), std::size_t{128});
        RUVIA_CHECK_EQ(mapping.index_kind("name"), ruvia::redis_index_kind::tag);
        RUVIA_CHECK(mapping.sortable("name"));
        RUVIA_CHECK_EQ(mapping.index_kind("age"), ruvia::redis_index_kind::numeric);
        RUVIA_CHECK_EQ(mapping.index_kind("active"), ruvia::redis_index_kind::none);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(redis_entity_mapping_rejects_unknown_duplicate_and_mismatched_indexes) {
    auto rejected = [](ruvia::redis_repository_config config) {
        return ruvia::testing::throws_on([&] {
            (void)ruvia::detail::normalize_redis_mapping<redis_user>(config, std::pmr::get_default_resource());
        });
    };
    RUVIA_CHECK(rejected({.indexes_ = {{.field_ = "missing"}}}));
    RUVIA_CHECK(rejected({.indexes_ = {{.field_ = "name"}, {.field_ = "name"}}}));
    RUVIA_CHECK(rejected({.indexes_ = {{.field_ = "name", .kind_ = ruvia::redis_index_kind::numeric}}}));
    RUVIA_CHECK(rejected({.indexes_ = {{.field_ = "age", .kind_ = ruvia::redis_index_kind::text}}}));
    RUVIA_CHECK(rejected({.indexes_ = {{.field_ = "age", .kind_ = ruvia::redis_index_kind::tag}}}));
    RUVIA_CHECK(rejected({.indexes_ = {{.field_ = "name", .kind_ = ruvia::redis_index_kind::none}}}));
}
