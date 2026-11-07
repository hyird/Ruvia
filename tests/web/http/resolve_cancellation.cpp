#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/http_client.h"

namespace resolve_cancellation_tests {

using namespace std::chrono_literals;

enum class stop_kind { timeout,
    cancellation,
    client_close,
    worker_stop };

struct held_queries final {
    std::binary_semaphore release_{0};
    std::atomic<unsigned> finished_{0};
    std::atomic<bool> released_{false};
    void release() {
        if (!released_.exchange(true)) {
            release_.release();
        }
    }
};

ruvia::task<void> hold_executor(ruvia::worker_handle worker, std::shared_ptr<held_queries> held) {
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
        auto response = co_await client.with_options({.stop_token_ = std::move(stop)}).get("/resolve").send();
        (void)co_await response.stream().read_all();
    } catch (const ruvia::http_client_error& error) {
        code = error.code();
    }
}

struct observation final {
    std::optional<ruvia::http_client_error::code_type> code_;
    bool retired_before_release_{false};
};

ruvia::task<void> exercise(ruvia::event_loop loop, const ruvia::blocking_pool& lookups, stop_kind kind,
    std::shared_ptr<held_queries> held, observation& observed, std::promise<void>& ready,
    std::promise<void>& retired) {
    auto worker = loop.handle();
    ruvia::task_scope occupied(worker);
    // 事件循环借用的单线程池被一个未释放的任务占住，名称查询确定性地排队等待，
    // 不依赖外部 DNS 服务器、网络超时或对系统解析器的进程级修改。
    occupied.spawn(hold_executor(worker, held));
    while (lookups.stats().running_ != 1) {
        (void)co_await ruvia::sleep_for(worker, 1ms);
    }
    ruvia::http_client client(loop, {.base_url_ = "http://localhost:1", .connect_timeout_ = kind == stop_kind::timeout ? 30ms : 3s, .request_timeout_ = 3s, .protocol_ = ruvia::http_client_protocol::http1_only});
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

[[nodiscard]] ruvia::http_client_config dns_http_config() {
    return {.base_url_ = "http://localhost:1"};
}

}  // namespace resolve_cancellation_tests

TEST(Ruvia, clients_need_a_loop_with_a_blocking_pool_to_look_up_a_dns_host) {
    using namespace resolve_cancellation_tests;
    bool http_rejected = false;
    {
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        try {
            ruvia::http_client client(loops.loop(0), dns_http_config());
        } catch (const std::invalid_argument&) {
            http_rejected = true;
        }
        loops.join();
    }
    EXPECT_TRUE(http_rejected);

    // The unified HTTP/WebSocket origin is accepted once the loop borrows a pool.
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &lookups});
    {
        ruvia::http_client http(loops.loop(0), dns_http_config());
    }
    loops.join();
    lookups.join();
}

TEST(Ruvia, host_lookup_timeout_cancel_and_shutdown_retire_before_system_work_finishes) {
    using namespace resolve_cancellation_tests;
    for (const auto kind : {stop_kind::timeout, stop_kind::cancellation, stop_kind::client_close, stop_kind::worker_stop}) {
        auto held = std::make_shared<held_queries>();
        observation observed;
        std::promise<void> ready;
        auto ready_result = ready.get_future();
        std::promise<void> retired;
        auto retired_result = retired.get_future();
        // 循环借用查询池，池须声明在循环之前并比它活得久。
        ruvia::blocking_pool lookups({.thread_count_ = 1});
        ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 64, .blocking_pool_ = &lookups});
        auto root = pool.loop(0).start(exercise(pool.loop(0), lookups, kind, held, observed, ready, retired));
        pool.start();
        if (kind == stop_kind::worker_stop) {
            const bool started = ready_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(started);
            pool.stop();
            const bool ended = retired_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(ended);
            if (!started || !ended) {
                held->release();
            }
            pool.join();
            held->release();
        } else {
            const bool ended = retired_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(ended);
            if (!ended) {
                held->release();
            }
            root.wait();
            pool.join();
        }
        lookups.join();
        root.get();
        EXPECT_TRUE(observed.retired_before_release_);
        const auto expected = kind == stop_kind::timeout        ? ruvia::http_client_error::code_type::timeout
                              : kind == stop_kind::cancellation ? ruvia::http_client_error::code_type::cancelled
                                                                : ruvia::http_client_error::code_type::closing;
        EXPECT_TRUE(observed.code_ == expected);
    }
}
