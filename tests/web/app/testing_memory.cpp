#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
namespace {

class testing_memory_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/testing-memory")
    RUVIA_ROUTES_BEGIN(testing_memory_controller)
    RUVIA_GET("/owned", owned);
    RUVIA_GET("/throw", throwing);
    RUVIA_GET("/cancel", cancelled);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> owned(ruvia::context&) {
        ruvia::http_response response;
        std::string body(256 * 1024, 'm');
        response.owned_body(std::move(body));
        co_return response;
    }

    ruvia::task<ruvia::http_response> throwing(ruvia::context&) {
        ruvia::http_response response;
        std::string body(256 * 1024, 'e');
        response.owned_body(std::move(body));
        throw std::runtime_error("testing memory failure");
        co_return response;
    }

    ruvia::task<ruvia::http_response> cancelled(ruvia::context& c) {
        ruvia::http_response response;
        std::string body(256 * 1024, 'c');
        response.owned_body(std::move(body));
        // Exercise explicit downstream cancellation without a request-level timer.
        ruvia::stop_source stop;
        stop.request_stop();
        const auto result_value = co_await ruvia::sleep_for(
            c.worker(), std::chrono::hours(1), stop.token());
        if (result_value != ruvia::timer_sleep_result::stop_requested) {
            throw std::logic_error("explicit cancellation did not stop the operation");
        }
        co_return response;
    }
};

}  // namespace

TEST(Ruvia, testing_facade_retains_owned_response_memory_across_app_lifetime) {
    std::optional<ruvia::dispatch_response> retained;
    {
        ruvia::test_app app;

        retained.emplace(app.request(ruvia::test_request::get("/testing-memory/owned")));
        EXPECT_EQ(retained->body().size(), std::size_t(256 * 1024));
        EXPECT_TRUE(retained->body().front() == 'm');

        const auto cancelled = app.request(ruvia::test_request::get("/testing-memory/cancel"));
        EXPECT_EQ(cancelled.body(), std::string(256 * 1024, 'c'));

        const auto repeated = app.request(ruvia::test_request::get("/testing-memory/owned"));
        EXPECT_EQ(repeated.body().size(), std::size_t(256 * 1024));
        EXPECT_TRUE(retained->body().front() == 'm');

        const auto failed = app.request(ruvia::test_request::get("/testing-memory/throw"));
        EXPECT_TRUE(failed.status() == ruvia::http_status::internal_server_error);
    }
    EXPECT_EQ(retained->body().size(), std::size_t(256 * 1024));
    EXPECT_TRUE(retained->body().front() == 'm');
}
