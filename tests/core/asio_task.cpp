#include <concepts>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_pool.h"

namespace ruvia::test::asio_task_tests {

struct alignas(4096) aligned_result final {
    explicit aligned_result(int value)
        : value_(std::make_unique<int>(value)),
          addresses_aligned_(is_aligned(this)) {}
    aligned_result(aligned_result&& other) noexcept
        : value_(std::move(other.value_)),
          addresses_aligned_(other.addresses_aligned_ && is_aligned(&other) && is_aligned(this)) {}

    std::unique_ptr<int> value_;
    bool addresses_aligned_;

private:
    static bool is_aligned(const void* value) noexcept {
        return reinterpret_cast<std::uintptr_t>(value) % alignof(aligned_result) == 0;
    }
};

ruvia::task<aligned_result> make_value(ruvia::worker_handle worker_value,
    std::unique_ptr<aligned_result> result_value) {
    if (!worker_value.is_current()) {
        throw std::logic_error("task started outside its event loop");
    }
    // Keep the over-aligned object outside coroutine locals, whose alignment
    // is independently broken by GCC 13/14.
    co_return std::move(*result_value);
}

ruvia::task<void> complete_void(ruvia::worker_handle worker_value, bool& completed) {
    if (!worker_value.is_current()) {
        throw std::logic_error("task started outside its event loop");
    }
    completed = true;
    co_return;
}

ruvia::task<void> fail() {
    throw std::runtime_error("root task failure");
    co_return;
}

}  // namespace ruvia::test::asio_task_tests

int main() {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    bool void_completed = false;

    auto value = loop.start(ruvia::test::asio_task_tests::make_value(
        worker_value, std::make_unique<ruvia::test::asio_task_tests::aligned_result>(42)));
    auto no_value = loop.start(ruvia::test::asio_task_tests::complete_void(worker_value, void_completed));
    auto failure = loop.start(ruvia::test::asio_task_tests::fail());
    std::promise<bool> same_loop_get_rejected;
    auto same_loop_get_result = same_loop_get_rejected.get_future();
    const auto probe_posted = loop.post([&] {
        try {
            static_cast<void>(value.get());
            same_loop_get_rejected.set_value(false);
        } catch (const std::logic_error&) {
            same_loop_get_rejected.set_value(value.valid());
        }
    });
    if (!probe_posted.accepted()) {
        return 1;
    }

    loops.start();
    bool valid = false;
    std::optional<ruvia::test::asio_task_tests::aligned_result> result_value;
    try {
        const auto same_loop_guard_held = same_loop_get_result.get();
        result_value.emplace(value.get());
        no_value.get();
        bool failure_observed = false;
        try {
            failure.get();
        } catch (const std::runtime_error& error) {
            failure_observed = std::string_view(error.what()) == "root task failure";
        }
        valid = same_loop_guard_held && void_completed && failure_observed;
    } catch (...) {
        valid = false;
    }
    loops.stop();
    loops.join();
    valid = valid && result_value && result_value->addresses_aligned_ &&
            result_value->value_ != nullptr && *result_value->value_ == 42;
    if (!valid) {
        return 1;
    }

    ruvia::event_loop_pool abandoned_pool({.loop_count_ = 1});
    {
        auto unobserved = abandoned_pool.loop(0).start(ruvia::test::asio_task_tests::fail());
    }
    abandoned_pool.start();
    abandoned_pool.stop();
    try {
        abandoned_pool.join();
    } catch (const std::runtime_error& error) {
        return std::string_view(error.what()) == "root task failure" ? 0 : 2;
    }
    return 3;
}
