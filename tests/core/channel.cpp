#include "ruvia/core/channel.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"

#include "failing_memory_resource.h"
#include "worker_task_fixture.h"

namespace ruvia::test::channel_tests {

struct rejection_move_state final {
    std::function<void()> close_;
    std::binary_semaphore close_requested_{0};
    std::binary_semaphore close_completed_{0};
    bool observe_move_{true};
    bool close_completed_during_move_{false};
    bool same_thread_close_completed_{false};
    int destroyed_{0};

    void observe_move() {
        if (!std::exchange(observe_move_, false)) {
            return;
        }
        close_requested_.release();
        // If the return-value move holds the channel lock, let it finish after
        // this bounded wait so the closer can retire and the test can report.
        close_completed_during_move_ = close_completed_.try_acquire_for(std::chrono::milliseconds(300));
        if (close_completed_during_move_) {
            close_();
            same_thread_close_completed_ = true;
        }
    }
};

struct rejection_payload final {
    explicit rejection_payload(int value, rejection_move_state* state = nullptr)
        : value_(std::make_unique<int>(value)),
          state_(state) {}
    rejection_payload(const rejection_payload&) = delete;
    rejection_payload& operator=(const rejection_payload&) = delete;
    rejection_payload(rejection_payload&& other)
        : state_(other.state_) {
        if (state_ != nullptr) {
            state_->observe_move();
        }
        value_ = std::move(other.value_);
    }
    ~rejection_payload() {
        if (value_ && state_ != nullptr) {
            ++state_->destroyed_;
            state_->close_();
        }
    }

    std::unique_ptr<int> value_;
    rejection_move_state* state_;
};

bool test_rejected_payload_moves_can_close_the_channel() {
    bool success = true;
    for (const auto expected : {channel_send_status::closed, channel_send_status::full, channel_send_status::worker_stopping}) {
        failing_memory_resource resource;
        asio::io_context io_context;
        worker_runtime_context runtime(io_context, 8);
        {
            auto [sender, receiver] = make_channel<rejection_payload>(runtime.handle(), {.capacity_ = 1, .resource_ = &resource});
            if (expected == channel_send_status::closed) {
                sender.close();
            } else if (expected == channel_send_status::full) {
                success = sender.send(rejection_payload(11)).accepted() && success;
            } else {
                runtime.close();
            }
            rejection_move_state state;
            state.close_ = [&] { sender.close(); };
            std::thread closer([&] {
                state.close_requested_.acquire();
                sender.close();
                state.close_completed_.release();
            });
            {
                auto rejected = sender.send(rejection_payload(42, &state));
                closer.join();
                success = rejected == expected && expected == rejected &&
                          rejected != channel_send_status::sent && !rejected.accepted() &&
                          rejected.rejected() != nullptr && rejected.rejected()->value_ &&
                          *rejected.rejected()->value_ == 42 && state.close_completed_during_move_ &&
                          state.same_thread_close_completed_ && state.destroyed_ == 0 && success;
                auto taken = std::move(rejected).take_rejected();
                success = rejected.rejected() == nullptr && taken.value_ && *taken.value_ == 42 && success;
            }
            success = state.destroyed_ == 1 && success;
            receiver.close();
        }
        runtime.detach();
        success = resource.live_allocations() == 0 && success;
    }
    return success;
}

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

// Counts moves down to a failure, so a receive can fail after its first move
// has already emptied the queued value.
class late_throwing_move final {
public:
    explicit late_throwing_move(int value) noexcept
        : value_(value) {}

    late_throwing_move(const late_throwing_move&) = delete;
    late_throwing_move& operator=(const late_throwing_move&) = delete;
    // This fixture intentionally models a move that can throw.
    late_throwing_move(late_throwing_move&& other) noexcept(false) {
        if (moves_before_throw_ == 0) {
            moves_before_throw_ = -1;
            throw std::runtime_error("requested late move failure");
        }
        if (moves_before_throw_ > 0) {
            --moves_before_throw_;
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    // Negative disables failures; only the worker thread touches it.
    static inline int moves_before_throw_{-1};

private:
    int value_{0};
};

ruvia::task<void> receive_after_late_move_failure(ruvia::channel_sender<late_throwing_move> sender,
    ruvia::channel_receiver<late_throwing_move>& receiver, bool& success) {
    bool move_failed = false;
    late_throwing_move::moves_before_throw_ = 1;
    try {
        static_cast<void>(co_await receiver.receive());
    } catch (const std::runtime_error&) {
        move_failed = true;
    }
    late_throwing_move::moves_before_throw_ = -1;
    const auto next = co_await receiver.receive();
    sender.close();
    const auto closed = co_await receiver.receive();
    success = move_failed && next.has_value() && next.value().value() == 2 &&
              closed.status() == ruvia::worker_wait_status::closed;
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
    ruvia::worker_handle worker_value, bool timed, bool buffered = false) {
    auto [sender, receiver] = ruvia::make_channel<int>(std::move(worker_value), {.capacity_ = 1});
    if (buffered && !sender.send(42).accepted()) {
        throw std::logic_error("could not queue the cold receive input");
    }
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

ruvia::task<void> verify_channel_side_closure(ruvia::worker_handle worker_value, bool& success) {
    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    if (!sender.send(42).accepted()) {
        co_return;
    }
    auto cold_receive = receiver.receive();
    sender.close();
    const auto buffered = co_await std::move(cold_receive);
    const auto drained = co_await receiver.receive();
    if (!buffered.has_value() || buffered.value() != 42 ||
        drained.status() != ruvia::worker_wait_status::closed) {
        co_return;
    }

    auto [retired_sender, retired_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    if (!retired_sender.send(7).accepted()) {
        co_return;
    }
    auto receive = retired_receiver.receive();
    auto timed_receive = retired_receiver.receive_for(std::chrono::seconds(1));
    retired_sender.close();
    retired_receiver.close();
    retired_receiver.close();
    const auto closed = co_await std::move(receive);
    const auto timed_closed = co_await std::move(timed_receive);
    success = closed.status() == ruvia::worker_wait_status::closed &&
              timed_closed.status() == ruvia::worker_wait_status::closed &&
              retired_sender.send(8).status() == ruvia::channel_send_status::closed;
}

ruvia::task<void> verify_receiver_retirement_after_worker_stop(
    ruvia::worker_runtime_context& runtime, bool& success) {
    auto [sender, receiver] = ruvia::make_channel<int>(runtime.handle(), {.capacity_ = 1});
    if (!sender.send(9).accepted()) {
        co_return;
    }
    auto receive = receiver.receive();
    auto timed_receive = receiver.receive_for(std::chrono::seconds(1));
    receiver.close();
    runtime.close();
    const auto stopping = co_await std::move(receive);
    const auto timed_stopping = co_await std::move(timed_receive);
    success = stopping.status() == ruvia::worker_wait_status::worker_stopping &&
              timed_stopping.status() == ruvia::worker_wait_status::worker_stopping &&
              sender.send(10).status() == ruvia::channel_send_status::worker_stopping;
}

struct buffered_payload final {
    buffered_payload(std::pmr::memory_resource* resource, int& destructions)
        : bytes_(1024, 'x', resource),
          destructions_(&destructions) {}
    buffered_payload(const buffered_payload&) = delete;
    buffered_payload& operator=(const buffered_payload&) = delete;
    buffered_payload(buffered_payload&& other) noexcept
        : bytes_(std::move(other.bytes_)),
          destructions_(std::exchange(other.destructions_, nullptr)) {}
    ~buffered_payload() {
        if (destructions_ != nullptr) {
            ++*destructions_;
        }
    }

    std::pmr::string bytes_;
    int* destructions_;
};

ruvia::task<void> verify_retired_payload_receive(
    ruvia::task<ruvia::worker_wait_result<buffered_payload>> receive, bool& success) {
    const auto result_value = co_await std::move(receive);
    success = result_value.status() == ruvia::worker_wait_status::closed && !result_value.has_value();
}

bool test_retired_receiver_storage_is_released() {
    enum class cold_task_retirement { absent,
        discarded,
        completed };
    bool success = true;
    for (const auto retirement : {cold_task_retirement::absent, cold_task_retirement::discarded, cold_task_retirement::completed}) {
        failing_memory_resource resource;
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        int destructions = 0;
        std::optional<ruvia::task<ruvia::worker_wait_result<buffered_payload>>> cold_receive;
        {
            auto [sender, receiver] = ruvia::make_channel<buffered_payload>(
                runtime.handle(), {.capacity_ = 1, .resource_ = &resource});
            success = sender.send(buffered_payload(&resource, destructions)).accepted() && success;
            if (retirement != cold_task_retirement::absent) {
                cold_receive.emplace(receiver.receive());
            }
            receiver.close();
            // Retirement destroys the buffered value while the sender still
            // holds the shared state.
            success = destructions == 1 && success;
        }
        if (cold_receive) {
            success = resource.live_allocations() != 0 && success;
            if (retirement == cold_task_retirement::completed) {
                bool closed = false;
                ruvia::test::run_worker_tasks(runtime, verify_retired_payload_receive(std::move(*cold_receive), closed));
                success = closed && success;
            }
            cold_receive.reset();
        }
        success = destructions == 1 && resource.live_allocations() == 0 && success;
        runtime.detach();
    }
    return success;
}

struct teardown_payload;

// Endpoint owners a buffered payload can tear down from its destructor.
struct teardown_endpoints final {
    std::unique_ptr<channel_sender<teardown_payload>> sender_;
    std::unique_ptr<channel_receiver<teardown_payload>> receiver_;
    bool armed_{false};
    bool release_endpoints_{false};
    int armed_destructions_{0};
};

// The defaulted move keeps the endpoint reference in the moved-from source, so
// every payload object a drain retires runs the teardown, moved-from or not.
struct teardown_payload final {
    explicit teardown_payload(teardown_endpoints& endpoints) noexcept
        : endpoints_(&endpoints) {}
    teardown_payload(const teardown_payload&) = delete;
    teardown_payload& operator=(const teardown_payload&) = delete;
    teardown_payload(teardown_payload&&) noexcept = default;
    teardown_payload& operator=(teardown_payload&&) noexcept = default;
    ~teardown_payload() {
        if (!endpoints_->armed_) {
            return;
        }
        ++endpoints_->armed_destructions_;
        if (endpoints_->sender_) {
            endpoints_->sender_->close();
        }
        if (endpoints_->release_endpoints_) {
            endpoints_->receiver_.reset();
            endpoints_->sender_.reset();
        }
    }

    teardown_endpoints* endpoints_;
};

// Receiver retirement destroys buffered payloads whose destructors reenter the
// channel: closing the sender must not deadlock on the queue lock, and dropping
// every endpoint must not free the state while the drain is still running.
bool test_retired_payload_destructors_can_tear_down_the_channel() {
    bool success = true;
    for (const bool release_endpoints : {false, true}) {
        failing_memory_resource resource;
        asio::io_context io_context;
        worker_runtime_context runtime(io_context, 8);
        {
            teardown_endpoints endpoints;
            endpoints.release_endpoints_ = release_endpoints;
            auto [sender, receiver] =
                make_channel<teardown_payload>(runtime.handle(), {.capacity_ = 2, .resource_ = &resource});
            endpoints.sender_ = std::make_unique<channel_sender<teardown_payload>>(std::move(sender));
            endpoints.receiver_ = std::make_unique<channel_receiver<teardown_payload>>(std::move(receiver));
            success = endpoints.sender_->send(teardown_payload(endpoints)).accepted() &&
                      endpoints.sender_->send(teardown_payload(endpoints)).accepted() && success;
            endpoints.armed_ = true;
            endpoints.receiver_->close();
            success = endpoints.armed_destructions_ == 2 && success;
            endpoints.armed_ = false;
            if (release_endpoints) {
                success = !endpoints.sender_ && !endpoints.receiver_ && resource.live_allocations() == 0 && success;
            } else {
                success = endpoints.sender_->send(teardown_payload(endpoints)).status() == channel_send_status::closed &&
                          success;
            }
        }
        runtime.detach();
        success = resource.live_allocations() == 0 && success;
    }
    return success;
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

}  // namespace ruvia::test::channel_tests

int main() {
    using namespace ruvia::test::channel_tests;
    bool success = false;
    bool cold_receiver_tasks_safe = false;
    bool buffered_cold_receiver_tasks_safe = false;
    bool channel_side_closure = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        auto cold_receive = make_cold_receive_after_receiver_close(worker_value, false);
        auto timed_cold_receive = make_cold_receive_after_receiver_close(worker_value, true);
        auto buffered_cold_receive = make_cold_receive_after_receiver_close(worker_value, false, true);
        auto buffered_timed_cold_receive = make_cold_receive_after_receiver_close(worker_value, true, true);
        ruvia::test::run_worker_tasks(runtime, exercise(worker_value, success),
            verify_cold_receiver_tasks(std::move(cold_receive), std::move(timed_cold_receive),
                cold_receiver_tasks_safe),
            verify_cold_receiver_tasks(std::move(buffered_cold_receive), std::move(buffered_timed_cold_receive),
                buffered_cold_receiver_tasks_safe),
            verify_channel_side_closure(worker_value, channel_side_closure));
        runtime.detach();
    }

    bool retired_receiver_stopping = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        ruvia::test::run_worker_tasks(runtime, verify_receiver_retirement_after_worker_stop(runtime, retired_receiver_stopping));
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

    bool late_move_consumed_once = false;
    {
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        auto [sender, receiver] = ruvia::make_channel<late_throwing_move>(runtime.handle(), {.capacity_ = 2});
        if (!sender.send(late_throwing_move(1)).accepted() || !sender.send(late_throwing_move(2)).accepted()) {
            return 1;
        }
        ruvia::test::run_worker_tasks(
            runtime, receive_after_late_move_failure(std::move(sender), receiver, late_move_consumed_once));
        runtime.detach();
    }

    const bool all_passed = success && cold_receiver_tasks_safe && buffered_cold_receiver_tasks_safe &&
                            channel_side_closure && retired_receiver_stopping &&
                            worker_stopping && stopping_send &&
                            move_failed && recovered_value && recovered_send && late_move_consumed_once &&
                            test_rejected_payload_moves_can_close_the_channel() &&
                            test_retired_receiver_storage_is_released() &&
                            test_retired_payload_destructors_can_tear_down_the_channel();
    return all_passed ? 0 : 1;
}
