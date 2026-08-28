#include "test_harness.h"

#include <exception>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "ruvia/web/detail/server/HttpServerWorkerCompletion.h"

RUVIA_TEST(http_server_worker_completion_is_monotonic) {
    ruvia::detail::HttpServerWorkerCompletion completion;

    RUVIA_CHECK(completion.markStartupReady());
    RUVIA_CHECK(!completion.markStartupReady());
    RUVIA_CHECK(!completion.markStartupFailed(std::make_exception_ptr(std::runtime_error("late failure"))));
    completion.waitForStartup();
}

RUVIA_TEST(http_server_worker_completion_propagates_startup_failure) {
    ruvia::detail::HttpServerWorkerCompletion completion;
    std::thread worker([&completion] { (void)completion.markStartupFailed(std::make_exception_ptr(std::runtime_error("startup failed"))); });

    try {
        completion.waitForStartup();
        RUVIA_CHECK(false);
    } catch (const std::runtime_error& error) {
        RUVIA_CHECK(std::string_view(error.what()) == "startup failed");
    }
    worker.join();
}

RUVIA_TEST(http_server_worker_completion_keeps_first_terminal_failure) {
    ruvia::detail::HttpServerWorkerCompletion completion;
    const auto first = std::make_exception_ptr(std::runtime_error("first failure"));
    const auto second = std::make_exception_ptr(std::runtime_error("second failure"));

    RUVIA_CHECK(completion.recordWorkerFailure(first));
    RUVIA_CHECK(!completion.recordWorkerFailure(second));
    RUVIA_CHECK(completion.workerFailure() == first);
}
