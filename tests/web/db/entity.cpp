#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/db/db_session.h"

#include "backend_client_fixture.h"
#include "test_exceptions.h"
#include "test_io_context.h"

namespace {

RUVIA_DB_ENTITY(entity, "users",
    ruvia::db_column<ruvia::fixed_string{"id"}, int>,
    ruvia::db_column<ruvia::fixed_string{"name"}, std::string,
        ruvia::db_column_options{.nullable_ = true}>)

RUVIA_DB_ENTITY(owned_type, "owned", ruvia::db_column<"text", ruvia::string>,
    ruvia::db_column<"values", std::vector<std::optional<std::string>>>)

RUVIA_DB_ENTITY(string_array_entity, "string_arrays",
    ruvia::db_column<"values", std::vector<std::string>>)

RUVIA_DB_ENTITY(owned_container_entity, "owned_containers",
    ruvia::db_column<"text", std::string>,
    ruvia::db_column<"values", std::vector<std::string>>)

RUVIA_DB_ENTITY(numeric_input_entity, "numeric_input",
    ruvia::db_column<"signed_value", std::int32_t, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"unsigned_value", std::uint32_t, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"real_value", float, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"values", std::vector<std::int32_t>>,
    ruvia::db_column<"optional_values", std::vector<std::optional<std::int32_t>>>,
    ruvia::db_column<"real_values", std::vector<float>>)

TEST(Ruvia, db_entity_tracks_unset_null_and_value_states) {
    entity entity;
    EXPECT_TRUE(!entity.is_set<"id">());
    entity.set<"id">(7);
    EXPECT_TRUE(entity.is_set<"id">());
    EXPECT_EQ(entity.get<"id">(), 7);
    const auto& const_entity = entity;
    EXPECT_EQ(const_entity.get<"id">(), 7);
    entity.set_null<"name">();
    EXPECT_TRUE(entity.is_null<"name">());
    entity.reset<"id">();
    EXPECT_TRUE(!entity.is_set<"id">());
    EXPECT_EQ(entity::table_name(), std::string_view("users"));
    EXPECT_EQ(entity::column_index<"name">(), std::size_t{1});
}

TEST(Ruvia, db_entity_numeric_overflow_preserves_value_null_and_unset) {
    const auto above_signed = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()) + 1;
    const auto below_signed = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)()) - 1;
    const auto above_unsigned = static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1;
    for (int state = 0; state != 3; ++state) {
        numeric_input_entity value;
        if (state == 1) {
            value.set_null<"signed_value">().set_null<"unsigned_value">().set_null<"real_value">();
        } else if (state == 2) {
            value.set<"signed_value">(7).set<"unsigned_value">(9).set<"real_value">(1.5);
        }
        const auto check_rejected = [&](auto assign) {
            bool rejected = false;
            try {
                assign();
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
        };
        check_rejected([&] { value.set<"signed_value">(above_signed); });
        check_rejected([&] { value.set<"signed_value">(below_signed); });
        check_rejected([&] { value.set<"signed_value">((std::numeric_limits<std::uint64_t>::max)()); });
        check_rejected([&] { value.set<"signed_value">(ruvia::int64{above_signed}); });
        check_rejected([&] { value.set<"unsigned_value">(-1); });
        check_rejected([&] { value.set<"unsigned_value">(above_unsigned); });
        check_rejected([&] { value.set<"real_value">((std::numeric_limits<double>::max)()); });
        check_rejected([&] { value.set<"real_value">(-(std::numeric_limits<double>::max)()); });
        check_rejected([&] { value.set<"real_value">(ruvia::double_value{(std::numeric_limits<double>::max)()}); });
        EXPECT_EQ(value.is_set<"signed_value">(), state != 0);
        EXPECT_EQ(value.is_set<"unsigned_value">(), state != 0);
        EXPECT_EQ(value.is_set<"real_value">(), state != 0);
        EXPECT_EQ(value.is_null<"signed_value">(), state == 1);
        EXPECT_EQ(value.is_null<"unsigned_value">(), state == 1);
        EXPECT_EQ(value.is_null<"real_value">(), state == 1);
        if (state == 2) {
            EXPECT_EQ(value.get<"signed_value">(), 7);
            EXPECT_EQ(value.get<"unsigned_value">(), 9U);
            EXPECT_EQ(value.get<"real_value">(), 1.5F);
        }
    }
}

TEST(Ruvia, db_entity_numeric_arrays_reject_late_overflow_without_partial_replacement) {
    numeric_input_entity value;
    value.set<"values">(std::vector<std::int32_t>({11, 12}));
    value.set<"optional_values">(std::vector<std::optional<std::int32_t>>({11, std::nullopt, 12}));
    value.set<"real_values">(std::vector<float>({1.5F, 2.5F}));
    const auto above_signed = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()) + 1;
    const auto check_rejected = [&](auto assign) {
        bool rejected = false;
        try {
            assign();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    };
    check_rejected([&] { value.set<"values">(std::vector<std::int64_t>({42, above_signed})); });
    check_rejected([&] {
        value.set<"optional_values">(std::vector<std::optional<std::int64_t>>({42, std::nullopt, above_signed}));
    });
    check_rejected([&] { value.set<"real_values">(std::vector<double>({0.5, (std::numeric_limits<double>::max)()})); });
    EXPECT_EQ(value.get<"values">().size(), std::size_t{2});
    EXPECT_EQ(value.get<"values">()[0], 11);
    EXPECT_EQ(value.get<"values">()[1], 12);
    EXPECT_EQ(value.get<"optional_values">().size(), std::size_t{3});
    EXPECT_EQ(*value.get<"optional_values">()[0], 11);
    EXPECT_TRUE(!value.get<"optional_values">()[1]);
    EXPECT_EQ(*value.get<"optional_values">()[2], 12);
    EXPECT_EQ(value.get<"real_values">().size(), std::size_t{2});
    EXPECT_EQ(value.get<"real_values">()[0], 1.5F);
    EXPECT_EQ(value.get<"real_values">()[1], 2.5F);
}

TEST(Ruvia, db_entity_numeric_set_accepts_representable_boundaries_and_scalar_wrappers) {
    numeric_input_entity value;
    const auto minimum = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)());
    const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
    value.set<"signed_value">(ruvia::int64{minimum});
    EXPECT_EQ(value.get<"signed_value">(), minimum);
    value.set<"signed_value">(maximum);
    EXPECT_EQ(value.get<"signed_value">(), (std::numeric_limits<std::int32_t>::max)());
    value.set<"unsigned_value">(static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()));
    EXPECT_EQ(value.get<"unsigned_value">(), (std::numeric_limits<std::uint32_t>::max)());
    value.set<"values">(std::vector<std::int64_t>({minimum, static_cast<std::int64_t>(maximum)}));
    EXPECT_EQ(value.get<"values">()[0], minimum);
    EXPECT_EQ(value.get<"values">()[1], (std::numeric_limits<std::int32_t>::max)());
    value.set<"real_value">(ruvia::double_value{static_cast<double>((std::numeric_limits<float>::max)())});
    EXPECT_EQ(value.get<"real_value">(), (std::numeric_limits<float>::max)());
    value.set<"real_value">(std::numeric_limits<double>::infinity());
    EXPECT_EQ(value.get<"real_value">(), std::numeric_limits<float>::infinity());
    value.set<"real_values">(std::vector<std::int64_t>({-7, 9}));
    EXPECT_EQ(value.get<"real_values">()[0], -7.0F);
    EXPECT_EQ(value.get<"real_values">()[1], 9.0F);
}

TEST(Ruvia, db_entity_set_owns_nested_values_and_supports_reset) {
    owned_type entity;
    {
        ruvia::string text(std::string(200, 'x'));
        entity.set<"text">(std::move(text));
        std::vector<std::optional<std::string>> values;
        values.emplace_back(std::string(200, 'y'));
        values.emplace_back(std::nullopt);
        entity.set<"values">(std::move(values));
    }
    EXPECT_EQ(std::string_view(entity.get<"text">()), std::string(200, 'x'));
    EXPECT_EQ(std::string_view(*entity.get<"values">()[0]), std::string(200, 'y'));
    entity.reset<"text">();
    entity.reset<"values">();
    EXPECT_FALSE(entity.is_set<"text">());
    EXPECT_FALSE(entity.is_set<"values">());
}

}  // namespace

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_DB_ENTITY(numeric_decode_entity, "numeric_decode",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"ratio", double>,
    ruvia::db_column<"ratios", std::vector<double>>,
    ruvia::db_column<"counts", std::vector<std::int64_t>>)

TEST(Ruvia, db_field_and_array_decoding_reject_nonfinite_floats_and_prioritize_format_errors) {
    using namespace ruvia;
    std::vector<test::backend_startup_peer::postgres_reply> replies({
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("nan")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("nan(123)")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("inf")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("-infinity")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("999999999999999999999abc")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("999999999999999999999")})},
        {.column_ = std::string("ratios"), .type_ = 1022, .rows_ = std::vector<std::string>({std::string("{nan}")})},
        {.column_ = std::string("ratios"), .type_ = 1022, .rows_ = std::vector<std::string>({std::string("{inf}")})},
        {.column_ = std::string("counts"), .type_ = 1016, .rows_ = std::vector<std::string>({std::string("{999999999999999999999abc}")})},
        {.column_ = std::string("counts"), .type_ = 1016, .rows_ = std::vector<std::string>({std::string("{999999999999999999999}")})},
    });
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        for (int i = 0; i < 4; ++i) {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            EXPECT_EQ(rows.size(), std::size_t{1});
            bool invalid_double = false;
            bool invalid_float = false;
            try {
                (void)rows.front()["val"].as<double>();
            } catch (const db_conversion_error& error) {
                invalid_double = error.code() == db_conversion_error::code_type::invalid_format;
            }
            try {
                (void)rows.front()["val"].as<float>();
            } catch (const db_conversion_error& error) {
                invalid_float = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(invalid_double);
            EXPECT_TRUE(invalid_float);
        }
        {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            bool invalid_format = false;
            try {
                (void)rows.front()["val"].as<std::int64_t>();
            } catch (const db_conversion_error& error) {
                invalid_format = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(invalid_format);
        }
        {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            bool out_of_range = false;
            try {
                (void)rows.front()["val"].as<std::int64_t>();
            } catch (const db_conversion_error& error) {
                out_of_range = error.code() == db_conversion_error::code_type::out_of_range;
            }
            EXPECT_TRUE(out_of_range);
        }

        auto repo = handle.repository<numeric_decode_entity>();
        for (int i = 0; i < 2; ++i) {
            bool invalid_array_float = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"ratios"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                invalid_array_float = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(invalid_array_float);
        }
        {
            bool invalid_array_int_format = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"counts"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                invalid_array_int_format = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(invalid_array_int_format);
        }
        {
            bool array_int_out_of_range = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"counts"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                array_int_out_of_range = error.code() == db_conversion_error::code_type::out_of_range;
            }
            EXPECT_TRUE(array_int_out_of_range);
        }
    });
}
#endif
