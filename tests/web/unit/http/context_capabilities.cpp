#include <stdexcept>

#include "context_request_fixture.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif

RUVIA_TEST(context_rejects_unconfigured_worker_clients_consistently) {
    const auto response = context_request_test::with_context(
        ruvia::test_request::get("/capabilities"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            bool http_client_rejected = false;
            try {
                static_cast<void>(context_value.get_http_client());
            } catch (const ruvia::http_client_error& error) {
                http_client_rejected = error.code() == ruvia::http_client_error::code_type::not_configured;
            }
            RUVIA_CHECK(http_client_rejected);

#ifdef RUVIA_ENABLE_DATABASE
            bool database_rejected = false;
            try {
                static_cast<void>(context_value.db());
            } catch (const ruvia::db_error& error) {
                database_rejected = error.code() == ruvia::db_error::code_type::not_configured;
            }
            RUVIA_CHECK(database_rejected);
#endif

#ifdef RUVIA_ENABLE_REDIS
            bool redis_rejected = false;
            try {
                static_cast<void>(context_value.redis());
            } catch (const ruvia::redis_error& error) {
                redis_rejected = error.code() == ruvia::redis_error::code_type::not_configured;
            }
            RUVIA_CHECK(redis_rejected);
#endif
            co_return;
        });
    (void)response;
}

RUVIA_TEST(context_session_capability_requires_explicit_middleware_binding) {
    const auto response = context_request_test::with_context(
        ruvia::test_request::get("/capabilities"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            RUVIA_CHECK(!context_value.try_session().has_value());
            bool rejected = false;
            try {
                static_cast<void>(context_value.session());
            } catch (const std::logic_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);

            co_return;
        });
    (void)response;
}
