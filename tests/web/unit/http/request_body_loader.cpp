#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"

#include "body/http_request_body_facade.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

struct suspended_loader final {
    explicit suspended_loader(const ruvia::worker_handle& worker_value)
        : signal_(worker_value) {}

    ruvia::task<std::string_view> read_all() {
        ++read_calls_;
        co_await signal_.wait();
        co_return "buffered";
    }

    ruvia::task<void> discard() {
        ++discard_calls_;
        co_await signal_.wait();
    }

    ruvia::worker_signal signal_;
    int read_calls_{0};
    int discard_calls_{0};
};

void run_bounded(ruvia::event_loop_attachment& attachment, asio::io_context& io) {
    asio::steady_timer timeout(io);
    auto timed_out = std::make_shared<bool>(false);
    timeout.expires_after(std::chrono::seconds(5));
    timeout.async_wait([timed_out, &io](const std::error_code& error) {
        if (!error) {
            *timed_out = true;
            io.stop();
        }
    });

    attachment.run();
    timeout.cancel();
    if (*timed_out) {
        throw std::runtime_error("request body loader test timed out");
    }
}

ruvia::task<void> complete_load(
    ruvia::detail::request_body_loader& loader, std::string_view& body) {
    body = co_await loader.read_all();
}

ruvia::task<void> reject_concurrent_discard(
    ruvia::detail::request_body_loader& loader, bool& rejected) {
    try {
        co_await loader.discard();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::task<void> discard_then_reject_load(
    ruvia::detail::request_body_loader& loader, bool& rejected) {
    co_await loader.discard();
    try {
        (void)co_await loader.read_all();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::task<void> load_twice(
    ruvia::detail::request_body_loader& loader, std::string_view& first, std::string_view& second) {
    first = co_await loader.read_all();
    second = co_await loader.read_all();
}

}  // namespace

RUVIA_TEST(request_body_loader_rejects_read_discard_overlap) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    ruvia::detail::request_body_loader_binding<suspended_loader> binding(worker_value);
    std::string_view body;
    bool rejected = false;
    std::exception_ptr first_failure;
    std::exception_ptr second_failure;
    int completed_operations = 0;
    const auto complete_value = [&] {
        if (++completed_operations == 2) {
            io.stop();
        }
    };

    asio::co_spawn(io, ruvia::as_awaitable(complete_load(binding.facade(), body)),
        [&](std::exception_ptr failure) {
            first_failure = failure;
            complete_value();
        });
    // Let the first read suspend before restarting the context for the overlap check.
    asio::post(io, [&io] { io.stop(); });
    run_bounded(attachment, io);
    RUVIA_CHECK(body.empty());
    RUVIA_CHECK_EQ(binding.loader().read_calls_, 1);

    io.restart();
    asio::co_spawn(io, ruvia::as_awaitable(reject_concurrent_discard(binding.facade(), rejected)),
        [&](std::exception_ptr failure) {
            second_failure = failure;
            complete_value();
        });
    asio::post(io, [&binding] { binding.loader().signal_.notify(); });
    run_bounded(attachment, io);
    if (first_failure) {
        std::rethrow_exception(first_failure);
    }
    if (second_failure) {
        std::rethrow_exception(second_failure);
    }

    RUVIA_CHECK_EQ(body, std::string_view("buffered"));
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(binding.loader().read_calls_, 1);
    RUVIA_CHECK_EQ(binding.loader().discard_calls_, 0);
}

RUVIA_TEST(request_body_loader_discard_is_terminal) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    ruvia::detail::request_body_loader_binding<suspended_loader> binding(worker_value);
    bool rejected = false;
    std::exception_ptr failure;

    asio::co_spawn(io,
        ruvia::as_awaitable(discard_then_reject_load(binding.facade(), rejected)),
        [&](std::exception_ptr error) {
            failure = error;
            io.stop();
        });
    asio::post(io, [&io] { io.stop(); });
    run_bounded(attachment, io);
    RUVIA_CHECK(!rejected);

    io.restart();
    asio::post(io, [&binding] { binding.loader().signal_.notify(); });
    run_bounded(attachment, io);
    if (failure) {
        std::rethrow_exception(failure);
    }

    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(binding.loader().discard_calls_, 1);
    RUVIA_CHECK_EQ(binding.loader().read_calls_, 0);
}

RUVIA_TEST(request_body_loader_reuses_one_buffered_result) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    ruvia::detail::request_body_loader_binding<suspended_loader> binding(worker_value);
    std::string_view first;
    std::string_view second;
    std::exception_ptr failure;

    asio::co_spawn(io, ruvia::as_awaitable(load_twice(binding.facade(), first, second)),
        [&](std::exception_ptr error) {
            failure = error;
            io.stop();
        });
    asio::post(io, [&io] { io.stop(); });
    run_bounded(attachment, io);
    RUVIA_CHECK(first.empty());

    io.restart();
    asio::post(io, [&binding] { binding.loader().signal_.notify(); });
    run_bounded(attachment, io);
    if (failure) {
        std::rethrow_exception(failure);
    }

    RUVIA_CHECK_EQ(first, std::string_view("buffered"));
    RUVIA_CHECK_EQ(second, first);
    RUVIA_CHECK_EQ(binding.loader().read_calls_, 1);
}
