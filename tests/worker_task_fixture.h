#pragma once

#include <array>
#include <deque>
#include <exception>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>

#include "event_loop.h"
#include "task.h"
#include "task_scope.h"

namespace ruvia::test {

#ifndef RUVIA_TEST_THROWS_ON_DEFINED
#define RUVIA_TEST_THROWS_ON_DEFINED
template <typename fn_type>
[[nodiscard]] inline bool throws_on(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}
#endif

// Hands each socket-driven unit test a fresh asio::io_context that is deliberately
// never destroyed. This is a correctness requirement on Windows, not an optimization.
//
// asio's win_iocp backend spawns a dedicated timer thread the first time any
// steady_timer is bound to a context (e.g. via worker_dispatcher). Joining that thread
// while the context is destroyed can deadlock after a few rapid create/destroy cycles
// on the Windows IOCP backend -- see the long-standing Asio reports #424 and #520.
// Because every socket test builds and tears down its own io_context, that
// teardown is exactly the trigger: once enough of them have run, ~io_context hangs and
// the whole test binary times out. (A no-timer context never spawns the thread, so
// non-socket tests are unaffected -- but sharing one helper keeps the rule uniform.)
//
// Skipping io_context destruction entirely sidesteps the buggy join while keeping each
// test fully isolated with its own context. The contexts are owned by a leaked deque so
// they stay reachable (leak-sanitizer clean) and are reclaimed by the OS at process
// exit; deque never invalidates references to existing elements, so the returned
// reference stays valid for the test's lifetime.
inline asio::io_context& new_test_io_context() {
    static std::deque<asio::io_context>& registry = *new std::deque<asio::io_context>();
    return registry.emplace_back();
}

class scoped_temp_directory final {
public:
    explicit scoped_temp_directory(std::string_view prefix = "ruvia-test") {
        const std::filesystem::path root(RUVIA_TEST_TEMP_ROOT);
        std::filesystem::create_directories(root);
        std::random_device random;
        for (int attempt = 0; attempt != 100; ++attempt) {
            auto candidate = root / (std::string(prefix) + "-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create test directory");
    }

    scoped_temp_directory(const scoped_temp_directory&) = delete;
    scoped_temp_directory& operator=(const scoped_temp_directory&) = delete;

    ~scoped_temp_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// Start roots on their handle's owner and drain every root. The roots use
// their own stop sources when exercising cancellation.
template <std::size_t Count>
task<void> join_worker_tasks(const worker_ref& worker, std::array<task<void>, Count> tasks) {
    task_scope scope(worker);
    std::exception_ptr failure;
    try {
        for (auto& root : tasks) {
            scope.spawn(std::move(root));
        }
    } catch (...) {
        failure = std::current_exception();
    }
    co_await scope.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// The root task holds the loop's retirement open, so a test may stop the
// attachment while its roots still run; the loop retires once they joined.
template <std::size_t Count>
task<void> join_worker_tasks_then_stop(event_loop_attachment& attachment, const worker_ref& worker, std::array<task<void>, Count> tasks, bool& joined) {
    std::exception_ptr failure;
    try {
        co_await join_worker_tasks(worker, std::move(tasks));
    } catch (...) {
        failure = std::current_exception();
    }
    joined = true;
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Runs the roots on the attached loop, stops the attachment once they have
// joined, and drives the context until the loop retires.
template <typename... Tasks>
void run_worker_tasks(event_loop_attachment& attachment, Tasks&&... tasks) {
    bool joined = false;
    const auto loop = attachment.loop();
    auto root = loop.start(join_worker_tasks_then_stop(attachment, loop.worker(), std::array<task<void>, sizeof...(Tasks)>{std::forward<Tasks>(tasks)...}, joined));
    attachment.run();
    if (!joined) {
        throw std::logic_error("worker owner stopped before root tasks joined");
    }
    std::move(root).get();
}

}  // namespace ruvia::test