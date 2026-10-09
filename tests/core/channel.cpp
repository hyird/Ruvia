#include "ruvia/core/channel.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <memory_resource>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"

#include "worker_task_fixture.h"

namespace {

class throwing_move final {
public:
    explicit throwing_move(int value) noexcept
        : value_(value) {}

    throwing_move(const throwing_move&) = delete;
    throwing_move& operator=(const throwing_move&) = delete;
    // This fixture intentionally models a move that can throw.
    throwing_move(throwing_move&& other) noexcept(false) {
        if (throw_on_move_.load(std::memory_order_relaxed)) {
            throw std::runtime_error("requested move failure");
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    static inline std::atomic_bool throw_on_move_{false};

private:
    int value_{0};
};

ruvia::task<void> receive_last(ruvia::channel_receiver<int>& receiver, bool& success) {
    const auto value = co_await receiver.receive();
    const auto closed = co_await receiver.receive();
    success = value.has_value() && value.value() == 3 &&
              closed.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> receive_queued_then_stopping(ruvia::worker_runtime_context& runtime,
    ruvia::channel_receiver<int>& receiver, bool& success) {
    const auto value = co_await receiver.receive();
    asio::post(runtime.io_context(), [&runtime] { runtime.close(); });
    const auto stopping = co_await receiver.receive();
    success = value.has_value() && value.value() == 9 &&
              stopping.status() == ruvia::worker_wait_status::worker_stopping;
}

ruvia::task<void> receive_throwing_move(
    ruvia::channel_receiver<throwing_move>& receiver, bool& success) {
    const auto result_value = co_await receiver.receive_for(std::chrono::seconds(1));
    success = result_value.has_value() && result_value.value().value() == 6;
}

ruvia::task<void> receive_after_late_cancellation(
    ruvia::channel_receiver<int>& receiver, ruvia::stop_token stop_token_value,
    ruvia::stop_token next_stop_token, bool& success) {
    const auto first = co_await receiver.receive(std::move(stop_token_value));
    const auto second = co_await receiver.receive(std::move(next_stop_token));
    success = first.has_value() && first.value() == 1 && second.has_value() && second.value() == 2;
}

ruvia::task<void> receive_until_closed(ruvia::channel_receiver<int>& receiver, bool& success) {
    const auto result_value = co_await receiver.receive_for(std::chrono::seconds(1));
    success = result_value.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<ruvia::worker_wait_result<int>> make_cold_receive_after_receiver_close(
    ruvia::worker_handle worker_value, bool timed) {
    auto [sender, receiver] = ruvia::make_channel<int>(std::move(worker_value), {.capacity_ = 1});
    if (timed) {
        return receiver.receive_for(std::chrono::seconds(1));
    }
    return receiver.receive();
}

ruvia::task<void> verify_cold_receiver_tasks(ruvia::task<ruvia::worker_wait_result<int>> receive,
    ruvia::task<ruvia::worker_wait_result<int>> timed_receive, bool& success) {
    const auto cold_closed = co_await std::move(receive);
    const auto timed_cold_closed = co_await std::move(timed_receive);
    success = cold_closed.status() == ruvia::worker_wait_status::closed &&
              timed_cold_closed.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> exercise(ruvia::worker_handle worker_value, bool& success) {
    {
        ruvia::stop_source source;
        auto [cancel_sender, cancel_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        std::binary_semaphore receiver_scheduled{0};
        std::thread stopper([&] {
            receiver_scheduled.acquire();
            source.request_stop();
        });
        static_cast<void>(worker_value.post([&receiver_scheduled] { receiver_scheduled.release(); }));
        const auto cancelled =
            co_await cancel_receiver.receive_for(std::chrono::seconds(1), source.token());
        stopper.join();
        if (cancelled.status() != ruvia::worker_wait_status::cancelled ||
            !cancel_sender.send(5).accepted()) {
            co_return;
        }
        const auto recovered = co_await cancel_receiver.receive();
        if (!recovered.has_value() || recovered.value() != 5) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        source.request_stop();
        auto [cancel_sender, cancel_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        const auto cancelled = co_await cancel_receiver.receive(source.token());
        if (cancelled.status() != ruvia::worker_wait_status::cancelled) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        ruvia::stop_source next_source;
        auto [late_sender, late_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        bool generation_safe = false;
        ruvia::task_scope scope(worker_value);
        scope.spawn(receive_after_late_cancellation(
            late_receiver, source.token(), next_source.token(), generation_safe));
        if (!late_sender.send(1).accepted()) {
            late_receiver.close();
            scope.request_stop();
            co_await scope.join();
            co_return;
        }
        source.request_stop();
        static_cast<void>(worker_value.post([&late_sender] {
            if (!late_sender.send(2).accepted()) {
                std::terminate();
            }
        }));
        co_await scope.join();
        if (!generation_safe) {
            co_return;
        }
    }

    {
        auto [close_sender, close_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        bool pending_closed = false;
        ruvia::task_scope scope(worker_value);
        scope.spawn(receive_until_closed(close_receiver, pending_closed));
        bool duplicate_rejected = false;
        try {
            static_cast<void>(co_await close_receiver.receive());
        } catch (const std::logic_error&) {
            duplicate_rejected = true;
        }
        close_receiver.close();
        co_await scope.join();
        if (!pending_closed || !duplicate_rejected ||
            close_sender.send(7).status() != ruvia::channel_send_status::closed) {
            co_return;
        }
    }

    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 2});
    auto active_receiver = std::move(receiver);
    const auto send1 = sender.send(1);
    const auto send2 = sender.send(2);
    const auto send3 = sender.send(99);
    if (!send1.accepted() || !send2.accepted() ||
        send3.status() != ruvia::channel_send_status::full || send3.rejected() == nullptr ||
        *send3.rejected() != 99) {
        co_return;
    }

    const auto first = co_await active_receiver.receive();
    const auto second = co_await active_receiver.receive();
    if (!first.has_value() || first.value() != 1 || !second.has_value() || second.value() != 2) {
        co_return;
    }
    const auto timeout = co_await active_receiver.receive_for(std::chrono::milliseconds(1));
    if (timeout.status() != ruvia::worker_wait_status::timed_out) {
        co_return;
    }

    ruvia::task_scope scope(worker_value);
    scope.spawn(receive_last(active_receiver, success));
    const bool sent = sender.send(3).accepted();
    sender.close();
    scope.request_stop();
    co_await scope.join();
    success = success && sent;
    const auto closed_send = sender.send(4);
    if (closed_send.status() != ruvia::channel_send_status::closed ||
        closed_send.rejected() == nullptr || *closed_send.rejected() != 4) {
        success = false;
    }
}

}  // namespace

int main() {
    bool success = false;
    bool cold_receiver_tasks_safe = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        auto cold_receive = make_cold_receive_after_receiver_close(worker_value, false);
        auto timed_cold_receive = make_cold_receive_after_receiver_close(worker_value, true);
        ruvia::test::run_worker_tasks(runtime, exercise(worker_value, success),
            verify_cold_receiver_tasks(std::move(cold_receive), std::move(timed_cold_receive),
                cold_receiver_tasks_safe));
        runtime.detach();
    }

    bool worker_stopping = false;
    bool stopping_send = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        if (!sender.send(9).accepted()) {
            return 1;
        }
        ruvia::test::run_worker_tasks(runtime, receive_queued_then_stopping(runtime, receiver, worker_stopping));
        sender.close();
        const auto stopping_result = sender.send(10);
        stopping_send = stopping_result.status() == ruvia::channel_send_status::worker_stopping &&
                        stopping_result.rejected() != nullptr && *stopping_result.rejected() == 10;
        runtime.detach();
    }

    bool move_failed = false;
    bool recovered_value = false;
    bool recovered_send = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        auto [sender, receiver] = ruvia::make_channel<throwing_move>(worker_value, {.capacity_ = 1});
        std::binary_semaphore receiver_scheduled{0};
        asio::post(io_context, [&receiver_scheduled] { receiver_scheduled.release(); });
        std::thread sending_thread([&] {
            receiver_scheduled.acquire();
            throwing_move::throw_on_move_.store(true, std::memory_order_relaxed);
            try {
                static_cast<void>(sender.send(throwing_move(5)));
            } catch (const std::runtime_error&) {
                move_failed = true;
            }
            throwing_move::throw_on_move_.store(false, std::memory_order_relaxed);
            recovered_send = sender.send(throwing_move(6)).accepted();
        });
        ruvia::test::run_worker_tasks(runtime, receive_throwing_move(receiver, recovered_value));
        sending_thread.join();
        sender.close();
        runtime.detach();
    }

    const bool all_passed = success && cold_receiver_tasks_safe && worker_stopping && stopping_send &&
                            move_failed && recovered_value && recovered_send;
    return all_passed ? 0 : 1;
}
