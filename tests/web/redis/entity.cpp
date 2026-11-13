#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/redis/redis_repository.h"

#include "test_exceptions.h"

namespace {

RUVIA_REDIS_ENTITY(redis_user, "user",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, std::string),
    RUVIA_REDIS_COLUMN(active, bool),
    RUVIA_REDIS_COLUMN(age, std::int32_t, ruvia::redis_column_options{.nullable_ = true}),
    RUVIA_REDIS_COLUMN(score, double, ruvia::redis_column_options{.nullable_ = true}));

RUVIA_REDIS_ENTITY(nullable_redis_text, "text_value",
    RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(text, ruvia::string, ruvia::redis_column_options{.nullable_ = true}));

RUVIA_REDIS_ENTITY(numeric_redis_entity, "numeric_input",
    RUVIA_REDIS_COLUMN(id, std::int32_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(score, float, ruvia::redis_column_options{.nullable_ = true}));

}  // namespace

TEST(Ruvia, redis_entity_rejects_numeric_overflow_without_changing_identity_or_field_state) {
    numeric_redis_entity value;
    value.set<"id">(7);
    const auto above_signed = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()) + 1;
    bool id_rejected = false;
    try {
        value.set<"id">(above_signed);
    } catch (const std::out_of_range&) {
        id_rejected = true;
    }
    EXPECT_TRUE(id_rejected);
    EXPECT_EQ(value.get<"id">(), 7);
    // 绑定层接受的有限 double，仍可能无法表示为实体的 float 字段。
    const auto oversized = ruvia::from_json<ruvia::double_value>("1.7976931348623157e308");
    EXPECT_EQ(oversized.value_, (std::numeric_limits<double>::max)());
    for (int state = 0; state != 3; ++state) {
        if (state == 1) {
            value.set_null<"score">();
        } else if (state == 2) {
            value.set<"score">(1.5);
        }
        bool score_rejected = false;
        try {
            value.set<"score">(oversized);
        } catch (const std::out_of_range&) {
            score_rejected = true;
        }
        EXPECT_TRUE(score_rejected);
        EXPECT_EQ(value.is_set<"score">(), state != 0);
        EXPECT_EQ(value.is_null<"score">(), state == 1);
        if (state == 2) {
            EXPECT_EQ(value.get<"score">(), 1.5F);
        }
    }
    value.set<"id">(ruvia::int64{(std::numeric_limits<std::int32_t>::min)()});
    EXPECT_EQ(value.get<"id">(), (std::numeric_limits<std::int32_t>::min)());
}

TEST(Ruvia, redis_entity_tracks_unset_value_and_null_states) {
    redis_user user_value;

    EXPECT_EQ(redis_user::prefix(), std::string_view("user"));
    EXPECT_TRUE(!user_value.is_set<"id">());
    EXPECT_TRUE(!user_value.is_null<"id">());
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)user_value.get<"id">(); }));

    user_value.set<"id">("u-1");
    user_value.set<"name">("Alice");
    user_value.set<"active">(true);
    user_value.set<"age">(32);
    user_value.set<"score">(4.5);
    EXPECT_TRUE(user_value.is_set<"id">());
    EXPECT_EQ(user_value.get<"id">().view(), std::string_view("u-1"));
    EXPECT_EQ(std::string_view(user_value.get<"name">()), std::string_view("Alice"));
    EXPECT_TRUE(user_value.get<"active">());
    EXPECT_EQ(user_value.get<"age">(), 32);
    EXPECT_EQ(user_value.get<"score">(), 4.5);

    user_value.set_null<"age">();
    EXPECT_TRUE(user_value.is_set<"age">());
    EXPECT_TRUE(user_value.is_null<"age">());
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)user_value.get<"age">(); }));

    user_value.reset<"age">();
    EXPECT_TRUE(!user_value.is_set<"age">());
    EXPECT_TRUE(!user_value.is_null<"age">());
}

TEST(Ruvia, redis_entity_move_preserves_owned_values_and_resource) {
    {
        redis_user source_value;
        source_value.set<"id">(std::string(120, 'i'));
        source_value.set<"name">(std::string(120, 'n'));
        source_value.set<"active">(true);

        redis_user moved(std::move(source_value));
        EXPECT_EQ(moved.get<"id">().size(), std::size_t{120});
        EXPECT_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
        EXPECT_TRUE(moved.get<"active">());
        source_value.set<"id">("reused");
        source_value.set<"name">("new name");
        EXPECT_EQ(moved.get<"id">().size(), std::size_t{120});
        EXPECT_EQ(std::string_view(moved.get<"name">()).size(), std::size_t{120});
    }
}

RUVIA_REDIS_ENTITY(redis_byte_numbers, "byte_numbers",
    RUVIA_REDIS_COLUMN(id, std::int8_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(flags, std::uint8_t));

TEST(Ruvia, redis_entity_supports_8bit_integers) {
    redis_byte_numbers numbers;
    numbers.set<"id">(std::int8_t{-12});
    numbers.set<"flags">(std::uint8_t{250});
    EXPECT_EQ(numbers.get<"id">(), std::int8_t{-12});
    EXPECT_EQ(numbers.get<"flags">(), std::uint8_t{250});
}
