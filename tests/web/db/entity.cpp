#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_session.h"

#include "backend_client_fixture.h"
#include "test_exceptions.h"
#include "worker_task_fixture.h"
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

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace {
using namespace ruvia;
RUVIA_DB_ENTITY(mapped_user, "users", db_column<"id", int>, db_column<"name", std::string>)

TEST(Ruvia, db_public_mapping_owns_values_and_rejects_missing_or_ambiguous_columns) {
    using peer_type = test::backend_startup_peer;
    const std::string name(128, 'n');
    std::vector<peer_type::postgres_reply> replies;
    replies.push_back({.columns_ = {"name", "id", "ignored"}, .values_ = {{name, "7", "extra"}}});
    replies.push_back({.column_ = "values", .rows_ = {R"({"comma,quote\"slash\\",NULL,"NULL"})"}});
    replies.push_back({.rows_ = {""}, .null_rows_ = {true}});
    replies.push_back({.column_ = "id", .rows_ = {"8"}});
    replies.push_back({.columns_ = {"id", "id", "name"}, .values_ = {{"1", "2", "ambiguous"}}});
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        std::optional<mapped_user> owned;
        {
            const auto rows = co_await handle.query("SELECT name, id, ignored FROM users");
            owned.emplace(decode_db_row<mapped_user>(rows.front()));
            const auto batch = decode_db_rows<mapped_user>(rows);
            EXPECT_EQ(batch[0].get<"id">(), 7);
            EXPECT_EQ(batch[0].get<"name">(), name);
        }
        EXPECT_EQ(owned->get<"id">(), 7);
        EXPECT_EQ(owned->get<"name">(), name);
        using array_type = std::vector<std::optional<std::string>>;
        array_type values;
        {
            const auto rows = co_await handle.query("SELECT values FROM arrays");
            values = decode_db_field<array_type>(rows.front()["values"]);
        }
        EXPECT_EQ(values.size(), std::size_t{3});
        if (values.size() == 3) {
            EXPECT_EQ(values[0], "comma,quote\"slash\\");
            EXPECT_FALSE(values[1]);
            EXPECT_EQ(values[2], "NULL");
        }
        {
            const auto rows = co_await handle.query("SELECT NULL AS v");
            EXPECT_FALSE(decode_db_field<std::optional<std::string>>(rows.front()["v"]));
            bool rejected = false;
            try {
                (void)decode_db_field<int>(rows.front()["v"]);
            } catch (const db_conversion_error& error) {
                rejected = error.code() == db_conversion_error::code_type::unexpected_null;
            }
            EXPECT_TRUE(rejected);
        }
        for (const auto expected : {db_conversion_error::code_type::missing_column,
                 db_conversion_error::code_type::invalid_format}) {
            const auto rows = co_await handle.query("SELECT * FROM invalid_mapping");
            bool rejected = false;
            try {
                (void)decode_db_row<mapped_user>(rows.front());
            } catch (const db_conversion_error& error) {
                rejected = error.code() == expected;
            }
            EXPECT_TRUE(rejected);
        }
    });
}
}  // namespace
#endif

TEST(Ruvia, db_transaction_options_support_cold_cancellation_and_reject_invalid_values) {
    using namespace ruvia;
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        test::with_connected_db_client(attachment, db_config{.driver_ = driver}, [&](db_client& client) -> task<void> {
            auto handle = client.db();
            stop_source cancellation;
            cancellation.request_stop();
            auto cancelled = handle.with_options({.stop_token_ = cancellation.token()});
            auto exercise = [&]() -> task<void> {
                for (int iteration = 0; iteration < 8; ++iteration) {
                    {
                        const auto cold = handle.begin_transaction({.isolation_ = db_transaction_isolation::serializable,
                            .access_mode_ = db_transaction_access_mode::read_only});
                    }
                    bool observed_cancellation = false;
                    bool observed_invalid = false;
                    db_transaction_options options{.isolation_ = db_transaction_isolation::repeatable_read,
                        .access_mode_ = db_transaction_access_mode::read_only};
                    auto operation = cancelled.begin_transaction(options);
                    options.isolation_ = static_cast<db_transaction_isolation>(255);
                    try {
                        auto transaction = co_await std::move(operation);
                        EXPECT_TRUE(false);
                    } catch (const db_error& error) {
                        observed_cancellation = error.code() == db_error::code_type::cancelled;
                    }
                    auto invalid = handle.begin_transaction(options);
                    options = {};
                    try {
                        auto transaction = co_await std::move(invalid);
                        EXPECT_TRUE(false);
                    } catch (const std::invalid_argument&) {
                        observed_invalid = true;
                    }
                    EXPECT_TRUE(observed_cancellation);
                    EXPECT_TRUE(observed_invalid);
                }
            };
            co_await exercise();
        });
    }
}

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace db_transaction_operation_test {

RUVIA_DB_ENTITY(text_entity, "records", ruvia::db_column<"v", std::string>)

struct queued_transaction_operations final {
    explicit queued_transaction_operations(ruvia::db_transaction& transaction)
        : commit_(transaction.commit()),
          rollback_(transaction.rollback()),
          query_(transaction.query("SELECT v FROM records")),
          execute_(transaction.execute("UPDATE records SET v = 'raw'")),
          compiled_query_(transaction.repository<text_entity>().query_builder().get_raw_many()),
          compiled_execute_(transaction.repository<text_entity>()
                  .delete_builder()
                  .where(text_entity::column<"v">().is_not_null())
                  .execute()),
          mapped_(transaction.repository<text_entity>().find()),
          page_(transaction.repository<text_entity>().find_and_count()) {}

    ruvia::task<void> commit_;
    ruvia::task<void> rollback_;
    ruvia::task<ruvia::db_rows> query_;
    ruvia::task<ruvia::db_exec_result> execute_;
    ruvia::task<ruvia::db_rows> compiled_query_;
    ruvia::task<ruvia::db_exec_result> compiled_execute_;
    ruvia::task<ruvia::entity_rows<text_entity>> mapped_;
    ruvia::task<std::pair<ruvia::entity_rows<text_entity>, std::uint64_t>> page_;
};

struct queued_stream_operations final {
    explicit queued_stream_operations(ruvia::db_stream_result& stream)
        : close_(stream.close()),
          read_(stream.read()) {}

    ruvia::task<void> close_;
    ruvia::task<std::optional<ruvia::db_row>> read_;
};

TEST(Ruvia, db_transaction_can_prepare_cold_operations_while_a_statement_is_running) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("first")}), .status_ = 'T', .deferred_ = true},
            {.rows_ = std::vector<std::string>({std::string("raw")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("UPDATE 2"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("compiled")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("DELETE 4"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("mapped")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("counted")}), .status_ = 'T'},
            {.column_ = std::string("count"), .rows_ = std::vector<std::string>({std::string("1")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("COMMIT")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        auto transaction = co_await handle.begin_transaction();
        bool first_completed = false;
        task_scope running(connected.worker());
        const auto first = [&]() -> task<void> {
            const auto rows = co_await transaction.query("SELECT v FROM records");
            first_completed = rows.size() == 1 && rows.front()["v"].as<std::string_view>() == "first";
        };
        running.spawn(first());
        std::unique_ptr<queued_transaction_operations> queued;
        bool concurrent_created = false;
        bool concurrent_rejected = false;
        std::exception_ptr failure;
        try {
            queued = std::make_unique<queued_transaction_operations>(transaction);
        } catch (const std::logic_error&) {
            queued.reset();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            auto concurrent = transaction.query("SELECT v FROM records");
            concurrent_created = true;
            try {
                (void)co_await std::move(concurrent);
            } catch (const std::logic_error&) {
                concurrent_rejected = true;
            }
        } catch (const std::logic_error&) {
            concurrent_created = false;
        } catch (...) {
            failure = std::current_exception();
        }
        peer.release_replies();
        co_await running.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        EXPECT_TRUE(first_completed);
        EXPECT_TRUE(transaction.active());
        EXPECT_TRUE(queued != nullptr);
        EXPECT_TRUE(concurrent_created && concurrent_rejected);
        if (!queued) {
            co_return;
        }
        const auto raw = co_await std::move(queued->query_);
        EXPECT_EQ(raw.size(), std::size_t{1});
        if (!raw.empty()) {
            EXPECT_TRUE(raw.front()["v"].as<std::string_view>() == "raw");
        }
        const auto executed = co_await std::move(queued->execute_);
        EXPECT_EQ(executed.affected_rows(), std::uint64_t{2});
        const auto compiled = co_await std::move(queued->compiled_query_);
        EXPECT_EQ(compiled.size(), std::size_t{1});
        if (!compiled.empty()) {
            EXPECT_TRUE(compiled.front()["v"].as<std::string_view>() == "compiled");
        }
        const auto changed = co_await std::move(queued->compiled_execute_);
        EXPECT_EQ(changed.affected_rows(), std::uint64_t{4});
        const auto mapped = co_await std::move(queued->mapped_);
        EXPECT_EQ(mapped.size(), std::size_t{1});
        if (!mapped.empty()) {
            EXPECT_EQ(mapped[0].get<"v">(), std::string_view("mapped"));
        }
        const auto page = co_await std::move(queued->page_);
        EXPECT_EQ(page.first.size(), std::size_t{1});
        EXPECT_EQ(page.second, std::uint64_t{1});
        if (!page.first.empty()) {
            EXPECT_EQ(page.first[0].get<"v">(), std::string_view("counted"));
        }
        co_await std::move(queued->commit_);
        EXPECT_TRUE(!transaction.active());
    });
}

TEST(Ruvia, db_stream_cold_reads_and_close_wait_for_the_started_read_to_finish) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = {"prefetched", "first", "second"},
            .command_ = "SELECT 3",
            .deferred_ = true,
            .first_row_before_hold_ = true}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        auto stream = co_await handle.query_stream("SELECT v FROM records");
        const auto prefetched = co_await stream.read();
        EXPECT_TRUE(prefetched && (*prefetched)["v"].as<std::string_view>() == "prefetched");
        bool first_completed = false;
        task_scope reading(connected.worker());
        const auto first = [&]() -> task<void> {
            const auto row = co_await stream.read();
            first_completed = row && (*row)["v"].as<std::string_view>() == "first";
        };
        reading.spawn(first());
        std::exception_ptr failure;
        std::unique_ptr<queued_stream_operations> queued;
        bool rejected = false;
        try {
            queued = std::make_unique<queued_stream_operations>(stream);
            auto concurrent = stream.read();
            try {
                (void)co_await std::move(concurrent);
            } catch (const std::logic_error&) {
                rejected = true;
            }
        } catch (...) {
            failure = std::current_exception();
        }
        peer.release_replies();
        co_await reading.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        EXPECT_TRUE(rejected);
        EXPECT_TRUE(first_completed);
        const auto row = co_await std::move(queued->read_);
        EXPECT_TRUE(row.has_value());
        if (row) {
            EXPECT_TRUE((*row)["v"].as<std::string_view>() == "second");
        }
        EXPECT_TRUE(!(co_await stream.read()));
        EXPECT_TRUE(!stream.active());
    });
}

TEST(Ruvia, db_stream_close_after_exhaustion_is_idempotent) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("only")}), .command_ = std::string("SELECT 1")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto stream = co_await connected.db().query_stream("SELECT v FROM records");
        while (co_await stream.read()) {
        }
        EXPECT_TRUE(!stream.active());
        co_await stream.close();
        EXPECT_TRUE(!stream.active());
    });
}

// A read's own operation refines the stream's options. An already-stopped
// token is refused before the connection is touched, so the stream stays
// usable; once the read waits, its timeout or stop request closes the
// connection and ends the stream.
TEST(Ruvia, db_stream_read_takes_its_own_operation_options) {
    using namespace ruvia;
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel ? "cancelled" : "timed out");
        test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
            test::backend_startup_peer::after_startup::idle,
            std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = {"first", "withheld"},
                .command_ = "SELECT 2",
                .deferred_ = true,
                .first_row_before_hold_ = true}}));
        auto& context = test::new_test_io_context();
        auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), peer.config());
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            auto stream = co_await connected.db().query_stream("SELECT v FROM records");
            const auto failure_of = [](task<std::optional<db_row>> read) -> task<std::optional<db_error::code_type>> {
                try {
                    (void)co_await std::move(read);
                } catch (const db_error& error) {
                    co_return error.code();
                }
                co_return std::nullopt;
            };
            stop_source stopped;
            stopped.request_stop();
            EXPECT_TRUE(co_await failure_of(stream.read({.stop_token_ = stopped.token()})) == db_error::code_type::cancelled);
            EXPECT_TRUE(stream.active());
            EXPECT_THROW((void)stream.read({.timeout_ = std::chrono::milliseconds(0)}), std::invalid_argument);
            const auto first = co_await stream.read();
            EXPECT_TRUE(first && (*first)["v"].as<std::string_view>() == "first");
            stop_source stop;
            task_scope stopper(connected.worker());
            const auto stop_later = [&]() -> task<void> {
                (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(30));
                stop.request_stop();
            };
            operation_options operation{.timeout_ = std::chrono::milliseconds(30)};
            if (cancel) {
                operation = {.stop_token_ = stop.token()};
                stopper.spawn(stop_later());
            }
            const auto code = co_await failure_of(stream.read(std::move(operation)));
            co_await stopper.join();
            EXPECT_TRUE(code == (cancel ? db_error::code_type::cancelled : db_error::code_type::timeout));
            EXPECT_TRUE(!stream.active());
        });
    }
}

TEST(Ruvia, db_transaction_savepoint_commands_quote_identifiers_and_keep_transaction_active) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("SAVEPOINT"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("ROLLBACK"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("RELEASE"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("COMMIT")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto transaction = co_await connected.db().begin_transaction();
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)transaction.savepoint(""); }));
        co_await transaction.savepoint("stage_1");
        EXPECT_TRUE(transaction.active());
        co_await transaction.rollback_to_savepoint("stage_1");
        EXPECT_TRUE(transaction.active());
        co_await transaction.release_savepoint("stage_1");
        EXPECT_TRUE(transaction.active());
        co_await transaction.commit();
        EXPECT_TRUE(!transaction.active());
    });
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{5});
    if (statements.size() == 5) {
        EXPECT_EQ(statements[1].sql_, "SAVEPOINT \"stage_1\"");
        EXPECT_EQ(statements[2].sql_, "ROLLBACK TO SAVEPOINT \"stage_1\"");
        EXPECT_EQ(statements[3].sql_, "RELEASE SAVEPOINT \"stage_1\"");
    }
}

TEST(Ruvia, db_transaction_advisory_locks_notify_and_cursors_execute_in_order) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("t")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("DECLARE CURSOR"), .status_ = 'T'},
            {.column_ = std::string("v"), .rows_ = std::vector<std::string>({std::string("chunk-1"), std::string("chunk-2")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("CLOSE CURSOR"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("COMMIT")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto transaction = co_await connected.db().begin_transaction();
        co_await transaction.advisory_lock(7001);
        co_await transaction.advisory_lock(17, 1);
        co_await transaction.advisory_lock("resource-key", 99);
        EXPECT_TRUE(co_await transaction.try_advisory_lock(7001));
        co_await transaction.notify("iot_outbox_pending", "ready");

        co_await transaction.declare_cursor("chunk_cursor", "SELECT \"v\" FROM \"records\"");
        const auto fetched = co_await transaction.fetch_cursor("chunk_cursor", 8);
        EXPECT_EQ(fetched.size(), std::size_t{2});
        co_await transaction.close_cursor("chunk_cursor");
        co_await transaction.commit();
    });
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{10});
    if (statements.size() == 10) {
        EXPECT_EQ(statements[1].sql_, "SELECT \"pg_advisory_xact_lock\"($1::bigint)");
        EXPECT_EQ(statements[2].sql_, "SELECT \"pg_advisory_xact_lock\"($1::integer, $2::integer)");
        EXPECT_EQ(statements[3].sql_, "SELECT \"pg_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))");
        EXPECT_EQ(statements[4].sql_, "SELECT \"pg_try_advisory_xact_lock\"($1::bigint)");
        EXPECT_EQ(statements[5].sql_, "SELECT \"pg_notify\"($1::text, $2::text)");
        EXPECT_EQ(statements[6].sql_, "DECLARE \"chunk_cursor\" NO SCROLL CURSOR FOR SELECT \"v\" FROM \"records\"");
        EXPECT_EQ(statements[7].sql_, "FETCH FORWARD 8 FROM \"chunk_cursor\"");
        EXPECT_EQ(statements[8].sql_, "CLOSE \"chunk_cursor\"");
    }
}

TEST(Ruvia, db_transaction_scalar_decode_failure_releases_slot_for_next_transaction) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::reaccept,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("invalid-bool")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("t")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("COMMIT")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    auto config = peer.config();
    config.pool_size_per_worker_ = 1;
    db_client client(attachment.loop(), config);
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        {
            auto transaction = co_await connected.db().begin_transaction();
            bool failed = false;
            try {
                (void)co_await transaction.try_advisory_lock(7001);
            } catch (const db_conversion_error&) {
                failed = true;
            }
            EXPECT_TRUE(failed);
            EXPECT_TRUE(!transaction.active());
        }
        auto next = co_await connected.db().begin_transaction();
        EXPECT_TRUE(co_await next.try_advisory_lock(7001));
        co_await next.commit();
    });
}
}  // namespace db_transaction_operation_test
#endif

namespace db_transaction_timeout_test {

using peer_type = ruvia::test::backend_startup_peer;

constexpr std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
    ruvia::db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
    ruvia::db_driver::mariadb,
#endif
};

// What the peer answers, in order, on its one connection.
enum class step {
    begin,
    // One row inside the transaction.
    row,
    // One row inside the transaction, held until release_replies().
    held_row,
    commit,
    // Two rows outside a transaction, for a stream.
    stream_rows,
};

[[nodiscard]] std::unique_ptr<peer_type> make_peer(ruvia::db_driver driver, std::initializer_list<step> steps) {
    if (driver == ruvia::db_driver::postgresql) {
        std::vector<peer_type::postgres_reply> replies{};
        for (const auto value : steps) {
            switch (value) {
                case step::begin:
                    replies.push_back({.column_ = "", .command_ = "BEGIN", .status_ = 'T'});
                    break;
                case step::row:
                    replies.push_back({.rows_ = {"kept"}, .status_ = 'T'});
                    break;
                case step::held_row:
                    replies.push_back({.rows_ = {"late"}, .status_ = 'T', .deferred_ = true});
                    break;
                case step::commit:
                    replies.push_back({.column_ = "", .command_ = "COMMIT"});
                    break;
                case step::stream_rows:
                    replies.push_back({.rows_ = {"first", "second"}, .command_ = "SELECT 2"});
                    break;
            }
        }
        return std::make_unique<peer_type>(
            peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
    }
    // MariaDB status 3: autocommit with a transaction open.
    std::vector<peer_type::maria_reply> replies{};
    for (const auto value : steps) {
        switch (value) {
            case step::begin:
                replies.push_back({.column_ = "", .status_ = 3});
                break;
            case step::row:
                replies.push_back({.rows_ = {"kept"}, .status_ = 3});
                break;
            case step::held_row:
                replies.push_back({.rows_ = {"late"}, .status_ = 3, .deferred_ = true});
                break;
            case step::commit:
                replies.push_back({.column_ = "", .status_ = 2});
                break;
            case step::stream_rows:
                replies.push_back({.rows_ = {"first", "second"}});
                break;
        }
    }
    return std::make_unique<peer_type>(std::move(replies), peer_type::after_startup::idle);
}

[[nodiscard]] bool sent_statement(peer_type& peer, ruvia::db_driver driver, std::string_view sql) {
    if (driver == ruvia::db_driver::postgresql) {
        const auto parses = peer.postgres_parses();
        return std::ranges::any_of(parses, [&](const auto& parse) { return parse.sql_ == sql; });
    }
    const auto commands = peer.maria_commands();
    return std::ranges::any_of(
        commands, [&](const std::string& command) { return command.find(sql) != std::string::npos; });
}

// Waits for the peer's sessions to end on a blocking-pool thread, so the
// worker keeps running the timers and closes under test meanwhile. The peer
// outlives the wait: drive_client() joins the pool before returning.
ruvia::task<void> await_disconnects(const ruvia::worker_ref& worker, peer_type& peer) {
    auto waited = co_await ruvia::run_blocking(worker, [target = &peer] { target->wait_for_disconnects(); });
    waited.value();
}

template <typename callback_type>
void drive_client(peer_type& peer, ruvia::db_driver driver,
    std::optional<ruvia::timeout_duration> transaction_timeout, callback_type&& callback) {
    auto config = peer.config();
    config.driver_ = driver;
    config.transaction_timeout_ = transaction_timeout;
    ruvia::blocking_pool waits({.thread_count_ = 1});
    auto& context_value = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(context_value, {.queue_capacity_ = 8, .blocking_pool_ = &waits});
    ruvia::db_client client(attachment.loop(), config);
    try {
        ruvia::test::drive_backend_client(attachment, client, std::forward<callback_type>(callback));
    } catch (...) {
        waits.join();
        throw;
    }
    waits.join();
}

TEST(Ruvia, db_config_transaction_timeout_defaults_to_one_minute_and_rejects_non_positive_values) {
    using namespace ruvia;
    EXPECT_TRUE(db_config{}.transaction_timeout_->value() == std::chrono::milliseconds(60000));
    event_loop_pool pool({.loop_count_ = 1});
    for (const auto driver : drivers) {
        for (const auto value : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
            const db_config config{.driver_ = driver, .transaction_timeout_ = value};
            bool client_rejected = false;
            bool migrator_rejected = false;
            try {
                db_client client(pool.loop(0), config);
            } catch (const std::invalid_argument&) {
                client_rejected = true;
            }
            try {
                db_migrator migrator(pool.loop(0), config);
            } catch (const std::invalid_argument&) {
                migrator_rejected = true;
            }
            EXPECT_TRUE(client_rejected);
            EXPECT_TRUE(migrator_rejected);
        }
    }
    pool.join();
}

// The holder keeps the transaction while it awaits something else, such as a
// remote call. The lease timer closes the session at the deadline, so the
// server rolls back and releases the locks without waiting for the holder;
// commit then reports the timeout and sends nothing.
TEST(Ruvia, db_transaction_held_idle_past_its_deadline_closes_the_session_and_fails_commit) {
    using namespace ruvia;
    for (const auto driver : drivers) {
        auto peer = make_peer(driver, {step::begin});
        std::optional<db_error::code_type> code;
        bool ended = false;
        drive_client(*peer, driver, std::chrono::milliseconds(100), [&](db_client& connected) -> task<void> {
            auto transaction = co_await connected.db().begin_transaction();
            (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(300));
            // Throws unless the session already ended while the transaction sat idle.
            co_await await_disconnects(connected.worker(), *peer);
            try {
                co_await transaction.commit();
            } catch (const db_error& error) {
                code = error.code();
            }
            ended = !transaction.active();
        });
        EXPECT_TRUE(code == db_error::code_type::timeout);
        EXPECT_TRUE(ended);
        EXPECT_TRUE(!sent_statement(*peer, driver, "COMMIT"));
    }
}

// Half the transaction's time passes idle; the statement then waits on a reply
// that never comes. query_timeout_ (30 s) alone would let it wait on: it gets
// only what is left of the transaction, fails with timeout, ends the
// transaction and closes the connection instead of returning it to the pool.
TEST(Ruvia, db_transaction_statement_is_bounded_by_the_remaining_transaction_time) {
    using namespace ruvia;
    constexpr auto lease = std::chrono::milliseconds(600);
    for (const auto driver : drivers) {
        auto peer = make_peer(driver, {step::begin, step::held_row});
        std::optional<db_error::code_type> code;
        std::chrono::steady_clock::duration statement_time{};
        bool ended = false;
        drive_client(*peer, driver, lease, [&](db_client& connected) -> task<void> {
            auto transaction = co_await connected.db().begin_transaction();
            (void)co_await sleep_for(connected.worker(), lease / 2);
            const auto started = std::chrono::steady_clock::now();
            try {
                (void)co_await transaction.query("SELECT v FROM records");
            } catch (const db_error& error) {
                code = error.code();
            }
            statement_time = std::chrono::steady_clock::now() - started;
            ended = !transaction.active();
            peer->release_replies();
            // Throws unless the timed-out connection was closed.
            co_await await_disconnects(connected.worker(), *peer);
        });
        EXPECT_TRUE(code == db_error::code_type::timeout);
        EXPECT_TRUE(statement_time < lease);
        EXPECT_TRUE(ended);
    }
}

// Without a transaction deadline, idle time between operations ends nothing:
// the same idle period that expires a 100 ms transaction above leaves this one
// committing on its original session.
TEST(Ruvia, db_transaction_without_a_deadline_keeps_its_session_while_idle) {
    using namespace ruvia;
    for (const auto driver : drivers) {
        auto peer = make_peer(driver, {step::begin, step::row, step::commit});
        std::string value{};
        bool committed = false;
        drive_client(*peer, driver, std::nullopt, [&](db_client& connected) -> task<void> {
            auto transaction = co_await connected.db().begin_transaction();
            (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(300));
            const auto rows = co_await transaction.query("SELECT v FROM records");
            if (!rows.empty()) {
                value = rows.front()["v"].as<std::string_view>().value_or("");
            }
            co_await transaction.commit();
            committed = !transaction.active();
        });
        EXPECT_TRUE(value == "kept");
        EXPECT_TRUE(committed);
        EXPECT_TRUE(sent_statement(*peer, driver, "COMMIT"));
        EXPECT_EQ(peer->startups(), std::size_t{1});
    }
}

// A stream held between reads past its deadline loses its session the same
// way; either terminal operation reports timeout, even with unread rows buffered.
TEST(Ruvia, db_stream_held_idle_past_its_deadline_closes_the_session_and_fails_read_or_close) {
    using namespace ruvia;
    for (const auto driver : drivers) {
        for (const bool close : {false, true}) {
            auto peer = make_peer(driver, {step::stream_rows});
            std::string first{};
            std::optional<db_error::code_type> code;
            bool ended = false;
            drive_client(*peer, driver, std::chrono::milliseconds(100), [&](db_client& connected) -> task<void> {
                auto stream = co_await connected.db().query_stream("SELECT v FROM records");
                if (const auto row = co_await stream.read(); row) {
                    first = (*row)["v"].as<std::string_view>().value_or("");
                }
                (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(300));
                // Throws unless the session already ended while the stream sat idle.
                co_await await_disconnects(connected.worker(), *peer);
                try {
                    if (close) {
                        co_await stream.close();
                    } else {
                        (void)co_await stream.read();
                    }
                } catch (const db_error& error) {
                    code = error.code();
                }
                ended = !stream.active();
                co_await stream.close();
            });
            EXPECT_TRUE(first == "first");
            EXPECT_TRUE(code == db_error::code_type::timeout);
            EXPECT_TRUE(ended);
        }
    }
}

}  // namespace db_transaction_timeout_test
