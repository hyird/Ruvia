#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <semaphore>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/http_client.h"

#include "test_harness.h"

namespace resolve_cancellation_tests {

using namespace std::chrono_literals;

enum class stop_kind { timeout,
    cancellation,
    client_close,
    worker_stop };

struct held_queries final {
    std::counting_semaphore<8> release_{0};
    std::atomic<unsigned> submitted_{0};
    std::atomic<unsigned> finished_{0};
    std::atomic<bool> released_{false};
    void release() {
        if (!released_.exchange(true)) {
            release_.release(8);
        }
    }
};

ruvia::task<void> hold_executor(ruvia::worker_handle worker, std::shared_ptr<held_queries> held) {
    ++held->submitted_;
    auto result = co_await ruvia::run_blocking(std::move(worker), {}, [held] {
        held->release_.acquire();
        ++held->finished_;
    });
    if (result.status() != ruvia::blocking_status::worker_stopping) {
        std::move(result).value();
    }
}

ruvia::task<void> send_query(ruvia::http_client& client, ruvia::stop_token stop,
    std::optional<ruvia::http_client_error::code_type>& code) {
    try {
        auto response = co_await client.with_options({.stop_token_ = std::move(stop)}).send({.target_ = "/resolve"});
        (void)co_await response.body().read_all();
    } catch (const ruvia::http_client_error& error) {
        code = error.code();
    }
}

struct observation final {
    std::optional<ruvia::http_client_error::code_type> code_;
    bool retired_before_release_{false};
};

ruvia::task<void> exercise(ruvia::event_loop loop, stop_kind kind, std::shared_ptr<held_queries> held,
    observation& observed, std::promise<void>& ready, std::promise<void>& retired) {
    auto worker = loop.handle();
    ruvia::task_scope occupied(worker);
    // 默认池至多 8 个线程。八个未释放的任务让名称查询确定性地等待，
    // 不依赖外部 DNS 服务器、网络超时或对系统解析器的进程级修改。
    for (unsigned index = 0; index != 8; ++index) {
        occupied.spawn(hold_executor(worker, held));
    }
    while (held->submitted_ != 8) {
        (void)co_await ruvia::sleep_for(worker, 1ms);
    }
    (void)co_await ruvia::sleep_for(worker, 1ms);
    ruvia::http_client client(loop, {.scheme_ = ruvia::http_scheme::http, .host_ = "localhost", .port_ = 1, .connect_timeout_ = kind == stop_kind::timeout ? 30ms : 3s, .request_timeout_ = 3s, .protocol_ = ruvia::http_client_protocol::http1_only});
    ruvia::stop_source stop;
    ruvia::task_scope operations(worker);
    std::exception_ptr failure;
    try {
        operations.spawn(send_query(client, stop.token(), observed.code_));
        if (kind != stop_kind::timeout) {
            (void)co_await ruvia::sleep_for(worker, 10ms);
            if (kind == stop_kind::cancellation) {
                stop.request_stop();
            } else if (kind == stop_kind::client_close) {
                client.close();
            } else {
                ready.set_value();
            }
        }
        co_await operations.join();
    } catch (...) {
        failure = std::current_exception();
    }
    co_await client.shutdown();
    observed.retired_before_release_ = held->finished_ == 0;
    retired.set_value();
    if (kind != stop_kind::worker_stop) {
        held->release();
    }
    co_await occupied.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace resolve_cancellation_tests

RUVIA_TEST(host_lookup_timeout_cancel_and_shutdown_retire_before_system_work_finishes) {
    using namespace resolve_cancellation_tests;
    for (const auto kind : {stop_kind::timeout, stop_kind::cancellation, stop_kind::client_close, stop_kind::worker_stop}) {
        auto held = std::make_shared<held_queries>();
        observation observed;
        std::promise<void> ready;
        auto ready_result = ready.get_future();
        std::promise<void> retired;
        auto retired_result = retired.get_future();
        ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 64});
        auto root = pool.loop(0).start(exercise(pool.loop(0), kind, held, observed, ready, retired));
        pool.start();
        if (kind == stop_kind::worker_stop) {
            const bool started = ready_result.wait_for(2s) == std::future_status::ready;
            RUVIA_CHECK(started);
            pool.stop();
            const bool ended = retired_result.wait_for(2s) == std::future_status::ready;
            RUVIA_CHECK(ended);
            if (!started || !ended) {
                held->release();
            }
            pool.join();
            held->release();
        } else {
            const bool ended = retired_result.wait_for(2s) == std::future_status::ready;
            RUVIA_CHECK(ended);
            if (!ended) {
                held->release();
            }
            root.wait();
            pool.join();
        }
        root.get();
        RUVIA_CHECK(observed.retired_before_release_);
        const auto expected = kind == stop_kind::timeout        ? ruvia::http_client_error::code_type::timeout
                              : kind == stop_kind::cancellation ? ruvia::http_client_error::code_type::cancelled
                                                                : ruvia::http_client_error::code_type::closing;
        RUVIA_CHECK(observed.code_ == expected);
    }
}
