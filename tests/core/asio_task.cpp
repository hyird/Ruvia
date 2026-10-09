#include "ruvia/core/asio_task.h"

#include <concepts>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/event_loop_pool.h"

namespace {

struct throwing_move final {
    throwing_move() = default;
    throwing_move(const throwing_move&) = delete;
    throwing_move(throwing_move&&) noexcept(false) {}
};

ruvia::task<std::unique_ptr<int>> make_value(ruvia::worker_handle worker_value) {
    if (!worker_value.is_current()) {
        throw std::logic_error("task started outside its event loop");
    }
    co_return std::make_unique<int>(42);
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

}  // namespace

int main() {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.handle();
    bool void_completed = false;

    auto value = loop.start(make_value(worker_value));
    auto no_value = loop.start(complete_void(worker_value, void_completed));
    auto failure = loop.start(fail());
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
    try {
        const auto same_loop_guard_held = same_loop_get_result.get();
        auto result_value = value.get();
        no_value.get();
        bool failure_observed = false;
        try {
            failure.get();
        } catch (const std::runtime_error& error) {
            failure_observed = std::string_view(error.what()) == "root task failure";
        }
        valid = same_loop_guard_held && result_value != nullptr && *result_value == 42 && void_completed &&
                failure_observed;
    } catch (...) {
        valid = false;
    }
    loops.stop();
    loops.join();
    if (!valid) {
        return 1;
    }

    ruvia::event_loop_pool abandoned_pool({.loop_count_ = 1});
    {
        auto unobserved = abandoned_pool.loop(0).start(fail());
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
