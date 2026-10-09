#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/db/db_client.h"
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
            auto handle = client.with_options({});
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
