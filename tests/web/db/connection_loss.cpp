#include <array>
#include <optional>
#include <string_view>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

enum class statement_kind { query,
    execute,
    stream };

ruvia::task<void> run_statement(ruvia::db_handle& handle, statement_kind kind) {
    switch (kind) {
        case statement_kind::query:
            (void)co_await handle.query("SELECT v");
            break;
        case statement_kind::execute:
            (void)co_await handle.execute("SELECT v");
            break;
        case statement_kind::stream: {
            auto stream = co_await handle.query_stream("SELECT v");
            while (co_await stream.read()) {
            }
            break;
        }
    }
}

}  // namespace

// The peer drops the connection after the first row of a row set. Every
// statement shape must report that as an I/O failure: neither as caller misuse
// ("requires row-producing SQL"), nor -- for execute() -- as success.
RUVIA_TEST(db_connection_lost_mid_result_is_an_io_error) {
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
        for (const auto kind : {statement_kind::query, statement_kind::execute, statement_kind::stream}) {
            auto& context_value = test::new_test_io_context();
            auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
            std::optional<db_error::code_type> code;
            test::with_connected_db_client(attachment, db_config{.driver_ = driver},
                test::backend_startup_peer::after_startup::drop_mid_result,
                [&](db_client& client) -> task<void> {
                    auto handle = client.handle();
                    try {
                        co_await run_statement(handle, kind);
                    } catch (const db_error& error) {
                        code = error.code();
                    }
                });
            RUVIA_CHECK(code == db_error::code_type::io_error);
        }
    }
}
