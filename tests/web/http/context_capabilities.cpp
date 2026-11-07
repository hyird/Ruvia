#include <gtest/gtest.h>

#include <stdexcept>

#include "ruvia/web/session.h"

#include "context_request_fixture.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif

#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
TEST(Ruvia, context_rejects_unconfigured_worker_clients_consistently) {
    const auto response = context_request_test::with_context(
        ruvia::test_request::get("/capabilities"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
#ifdef RUVIA_ENABLE_DATABASE
            bool database_rejected = false;
            try {
                static_cast<void>(context_value.db());
            } catch (const ruvia::db_error& error) {
                database_rejected = error.code() == ruvia::db_error::code_type::not_configured;
            }
            EXPECT_TRUE(database_rejected);
#endif

#ifdef RUVIA_ENABLE_REDIS
            bool redis_rejected = false;
            try {
                static_cast<void>(context_value.redis());
            } catch (const ruvia::redis_error& error) {
                redis_rejected = error.code() == ruvia::redis_error::code_type::not_configured;
            }
            EXPECT_TRUE(redis_rejected);
#endif
            co_return;
        });
    (void)response;
}
#endif

TEST(Ruvia, context_session_capability_requires_explicit_middleware_binding) {
    const auto response = context_request_test::with_context(
        ruvia::test_request::get("/capabilities"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            bool rejected = false;
            try {
                static_cast<void>(context_value.session());
            } catch (const std::logic_error&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);

            co_return;
        });
    (void)response;
}
