#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

RUVIA_TEST(db_transaction_options_support_cold_cancellation_and_reject_invalid_values) {
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
            auto handle = client.handle();
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
                        RUVIA_CHECK(false);
                    } catch (const db_error& error) {
                        observed_cancellation = error.code() == db_error::code_type::cancelled;
                    }
                    auto invalid = handle.begin_transaction(options);
                    options = {};
                    try {
                        auto transaction = co_await std::move(invalid);
                        RUVIA_CHECK(false);
                    } catch (const std::invalid_argument&) {
                        observed_invalid = true;
                    }
                    RUVIA_CHECK(observed_cancellation);
                    RUVIA_CHECK(observed_invalid);
                }
            };
            co_await exercise();
        });
    }
}

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace db_transaction_operation_test {

using text_entity = ruvia::db_entity<"records", ruvia::db_column<"v", std::pmr::string>>;

struct queued_transaction_operations final {
    explicit queued_transaction_operations(ruvia::db_transaction& transaction)
        : commit_(transaction.commit()),
          rollback_(transaction.rollback()),
          query_(transaction.query("SELECT v FROM records")),
          execute_(transaction.execute("UPDATE records SET v = 'raw'")),
          compiled_query_(transaction.get_repository<text_entity>().create_query_builder().get_raw_many()),
          compiled_execute_(transaction.get_repository<text_entity>()
                  .create_delete_builder()
                  .where(text_entity::column<"v">().is_not_null())
                  .execute()),
          mapped_(transaction.get_repository<text_entity>().find()),
          page_(transaction.get_repository<text_entity>().find_and_count()) {}

    ruvia::scoped_operation<void> commit_;
    ruvia::scoped_operation<void> rollback_;
    ruvia::scoped_operation<ruvia::db_rows> query_;
    ruvia::scoped_operation<ruvia::db_exec_result> execute_;
    ruvia::scoped_operation<ruvia::db_rows> compiled_query_;
    ruvia::scoped_operation<ruvia::db_exec_result> compiled_execute_;
    ruvia::scoped_operation<ruvia::entity_rows<text_entity>> mapped_;
    ruvia::scoped_operation<std::pair<ruvia::entity_rows<text_entity>, std::uint64_t>> page_;
};

struct queued_stream_operations final {
    explicit queued_stream_operations(ruvia::db_stream_result& stream)
        : close_(stream.close()),
          read_(stream.read()) {}

    ruvia::scoped_operation<void> close_;
    ruvia::scoped_operation<std::optional<ruvia::db_row>> read_;
};

RUVIA_TEST(db_transaction_can_prepare_cold_operations_while_a_statement_is_running) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
            {.rows_ = {"first"}, .status_ = 'T', .deferred_ = true},
            {.rows_ = {"raw"}, .status_ = 'T'},
            {.column_ = "", .command_ = "UPDATE 2", .status_ = 'T'},
            {.rows_ = {"compiled"}, .status_ = 'T'},
            {.column_ = "", .command_ = "DELETE 4", .status_ = 'T'},
            {.rows_ = {"mapped"}, .status_ = 'T'},
            {.rows_ = {"counted"}, .status_ = 'T'},
            {.column_ = "count", .rows_ = {"1"}, .status_ = 'T'},
            {.column_ = "", .command_ = "COMMIT"}});
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
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
        RUVIA_CHECK(first_completed);
        RUVIA_CHECK(transaction.active());
        RUVIA_CHECK(queued != nullptr);
        RUVIA_CHECK(concurrent_created && concurrent_rejected);
        if (!queued) {
            co_return;
        }
        const auto raw = co_await std::move(queued->query_);
        RUVIA_CHECK_EQ(raw.size(), std::size_t{1});
        if (!raw.empty()) {
            RUVIA_CHECK(raw.front()["v"].as<std::string_view>() == "raw");
        }
        const auto executed = co_await std::move(queued->execute_);
        RUVIA_CHECK_EQ(executed.affected_rows(), std::uint64_t{2});
        const auto compiled = co_await std::move(queued->compiled_query_);
        RUVIA_CHECK_EQ(compiled.size(), std::size_t{1});
        if (!compiled.empty()) {
            RUVIA_CHECK(compiled.front()["v"].as<std::string_view>() == "compiled");
        }
        const auto changed = co_await std::move(queued->compiled_execute_);
        RUVIA_CHECK_EQ(changed.affected_rows(), std::uint64_t{4});
        const auto mapped = co_await std::move(queued->mapped_);
        RUVIA_CHECK_EQ(mapped.size(), std::size_t{1});
        if (!mapped.empty()) {
            RUVIA_CHECK_EQ(mapped[0].get<"v">(), std::string_view("mapped"));
        }
        const auto page = co_await std::move(queued->page_);
        RUVIA_CHECK_EQ(page.first.size(), std::size_t{1});
        RUVIA_CHECK_EQ(page.second, std::uint64_t{1});
        if (!page.first.empty()) {
            RUVIA_CHECK_EQ(page.first[0].get<"v">(), std::string_view("counted"));
        }
        co_await std::move(queued->commit_);
        RUVIA_CHECK(!transaction.active());
    });
}

RUVIA_TEST(db_stream_cold_reads_and_close_wait_for_the_started_read_to_finish) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.rows_ = {"first", "second"}, .command_ = "SELECT 2", .deferred_ = true}});
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
        auto stream = co_await handle.query_stream("SELECT v FROM records");
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
        RUVIA_CHECK(rejected);
        RUVIA_CHECK(first_completed);
        const auto row = co_await std::move(queued->read_);
        RUVIA_CHECK(row.has_value());
        if (row) {
            RUVIA_CHECK((*row)["v"].as<std::string_view>() == "second");
        }
        RUVIA_CHECK(!(co_await stream.read()));
        RUVIA_CHECK(!stream.active());
    });
}

RUVIA_TEST(db_transaction_savepoint_commands_quote_identifiers_and_keep_transaction_active) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
            {.column_ = "", .command_ = "SAVEPOINT", .status_ = 'T'},
            {.column_ = "", .command_ = "ROLLBACK", .status_ = 'T'},
            {.column_ = "", .command_ = "RELEASE", .status_ = 'T'},
            {.column_ = "", .command_ = "COMMIT"}});
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto transaction = co_await connected.handle().begin_transaction();
        RUVIA_CHECK(testing::throws_on([&] { (void)transaction.savepoint(""); }));
        co_await transaction.savepoint("stage_1");
        RUVIA_CHECK(transaction.active());
        co_await transaction.rollback_to_savepoint("stage_1");
        RUVIA_CHECK(transaction.active());
        co_await transaction.release_savepoint("stage_1");
        RUVIA_CHECK(transaction.active());
        co_await transaction.commit();
        RUVIA_CHECK(!transaction.active());
    });
    const auto statements = peer.postgres_parses();
    RUVIA_CHECK_EQ(statements.size(), std::size_t{5});
    if (statements.size() == 5) {
        RUVIA_CHECK_EQ(statements[1].sql_, "SAVEPOINT \"stage_1\"");
        RUVIA_CHECK_EQ(statements[2].sql_, "ROLLBACK TO SAVEPOINT \"stage_1\"");
        RUVIA_CHECK_EQ(statements[3].sql_, "RELEASE SAVEPOINT \"stage_1\"");
    }
}

RUVIA_TEST(db_transaction_advisory_locks_notify_cursors_and_large_objects_execute_in_order) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
            {.rows_ = {""}, .status_ = 'T'},
            {.rows_ = {""}, .status_ = 'T'},
            {.rows_ = {""}, .status_ = 'T'},
            {.rows_ = {"t"}, .status_ = 'T'},
            {.rows_ = {""}, .status_ = 'T'},
            {.column_ = "", .command_ = "DECLARE CURSOR", .status_ = 'T'},
            {.column_ = "v", .rows_ = {"chunk-1", "chunk-2"}, .status_ = 'T'},
            {.column_ = "", .command_ = "CLOSE CURSOR", .status_ = 'T'},
            {.rows_ = {"12345"}, .status_ = 'T'},
            {.rows_ = {""}, .status_ = 'T'},
            {.rows_ = {"sealed-payload"}, .status_ = 'T'},
            {.rows_ = {"t"}, .status_ = 'T'},
            {.column_ = "", .command_ = "COMMIT"}});
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto transaction = co_await connected.handle().begin_transaction();
        co_await transaction.advisory_lock(7001);
        co_await transaction.advisory_lock(17, 1);
        co_await transaction.advisory_lock("resource-key", 99);
        RUVIA_CHECK(co_await transaction.try_advisory_lock(7001));
        co_await transaction.notify("iot_outbox_pending", "ready");

        co_await transaction.declare_cursor("chunk_cursor", "SELECT \"v\" FROM \"records\"");
        const auto fetched = co_await transaction.fetch_cursor("chunk_cursor", 8);
        RUVIA_CHECK_EQ(fetched.size(), std::size_t{2});
        co_await transaction.close_cursor("chunk_cursor");

        const auto oid = co_await transaction.lo_create();
        RUVIA_CHECK_EQ(oid, std::uint32_t{12345});
        co_await transaction.lo_put(oid, 0, "sealed-payload");
        const auto content = co_await transaction.lo_get_text(oid);
        RUVIA_CHECK_EQ(content, "sealed-payload");
        RUVIA_CHECK(co_await transaction.lo_unlink(oid));
        co_await transaction.commit();
    });
    const auto statements = peer.postgres_parses();
    RUVIA_CHECK_EQ(statements.size(), std::size_t{14});
    if (statements.size() == 14) {
        RUVIA_CHECK_EQ(statements[1].sql_, "SELECT \"pg_advisory_xact_lock\"($1::bigint)");
        RUVIA_CHECK_EQ(statements[2].sql_, "SELECT \"pg_advisory_xact_lock\"($1::integer, $2::integer)");
        RUVIA_CHECK_EQ(statements[3].sql_, "SELECT \"pg_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))");
        RUVIA_CHECK_EQ(statements[4].sql_, "SELECT \"pg_try_advisory_xact_lock\"($1::bigint)");
        RUVIA_CHECK_EQ(statements[5].sql_, "SELECT \"pg_notify\"($1::text, $2::text)");
        RUVIA_CHECK_EQ(statements[6].sql_, "DECLARE \"chunk_cursor\" NO SCROLL CURSOR FOR SELECT \"v\" FROM \"records\"");
        RUVIA_CHECK_EQ(statements[7].sql_, "FETCH FORWARD 8 FROM \"chunk_cursor\"");
        RUVIA_CHECK_EQ(statements[8].sql_, "CLOSE \"chunk_cursor\"");
        RUVIA_CHECK_EQ(statements[9].sql_, "SELECT \"lo_create\"($1::oid)::bigint");
        RUVIA_CHECK_EQ(statements[10].sql_, "SELECT \"lo_put\"($1::oid, $2::bigint, \"convert_to\"($3::text, 'UTF8'))");
        RUVIA_CHECK_EQ(statements[11].sql_, "SELECT \"convert_from\"(\"lo_get\"($1::oid), $2::text)");
        RUVIA_CHECK_EQ(statements[12].sql_, "SELECT (\"lo_unlink\"($1::oid) = 1)");
    }
}

RUVIA_TEST(db_transaction_scalar_decode_failure_releases_slot_for_next_transaction) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::reaccept,
        {{.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
            {.rows_ = {"invalid-bool"}, .status_ = 'T'},
            {.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
            {.rows_ = {"t"}, .status_ = 'T'},
            {.column_ = "", .command_ = "COMMIT"}});
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    auto config = peer.config();
    config.pool_size_per_worker_ = 1;
    db_client client(attachment.loop(), config);
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        {
            auto transaction = co_await connected.handle().begin_transaction();
            bool failed = false;
            try {
                (void)co_await transaction.try_advisory_lock(7001);
            } catch (const db_conversion_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK(!transaction.active());
        }
        auto next = co_await connected.handle().begin_transaction();
        RUVIA_CHECK(co_await next.try_advisory_lock(7001));
        co_await next.commit();
    });
}
}  // namespace db_transaction_operation_test
#endif
