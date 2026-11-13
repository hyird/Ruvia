#include "ruvia/core/task_scope.h"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <asio/io_context.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/timer.h"

#include "worker_task_fixture.h"

namespace ruvia::test::task_scope_tests {

ruvia::task<void> increment(ruvia::worker_ref worker_value, int& value) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    ++value;
    co_return;
}

ruvia::task<void> fail() {
    throw std::runtime_error("child failed");
    co_return;
}

ruvia::task<void> no_op() {
    co_return;
}

ruvia::task<void> observe_join(ruvia::task_scope::join_operation join, bool& completed) {
    co_await std::move(join);
    completed = true;
}

ruvia::task<void> join_and_destroy_on_failure(std::optional<ruvia::task_scope>& primary,
    bool& completed_after_node_release) {
    bool caught = false;
    try {
        co_await primary->join();
    } catch (const std::runtime_error& error) {
        caught = std::string_view(error.what()) == "child failed";
    }
    completed_after_node_release = caught && primary->size() == 0;
    primary.reset();
}

ruvia::task<void> verify_stop_callback_reentrant_join_waits_for_finish(
    ruvia::worker_ref worker_value, bool& success) {
    std::optional<ruvia::task_scope> primary(std::in_place, worker_value);
    ruvia::task_scope observer(worker_value);
    bool active_during_stop_callback = false;
    bool completed_after_node_release = false;
    ruvia::stop_registration registration;
    primary->token().register_callback(registration, [&] {
        active_during_stop_callback = primary.has_value() && primary->size() == 1;
        observer.spawn(join_and_destroy_on_failure(
            primary, completed_after_node_release));
    });
    primary->spawn(fail());
    while (primary.has_value()) {
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    }
    co_await observer.join();
    success = active_during_stop_callback && completed_after_node_release &&
              !primary.has_value();
}

ruvia::task<void> wait_for_stop(ruvia::worker_ref worker_value, ruvia::stop_token token, bool& stopped) {
    stopped = co_await ruvia::sleep_for(worker_value, std::chrono::hours(1), std::move(token)) == ruvia::timer_sleep_result::stop_requested;
}

ruvia::task<void> exercise(ruvia::worker_ref worker_value, bool& success) {
    {
        ruvia::task_scope empty_scope(worker_value);
        if (!empty_scope.empty()) {
            co_return;
        }
        {
            auto discarded_cold_join = empty_scope.join();
            static_cast<void>(discarded_cold_join);
        }
        co_await empty_scope.join();
        bool second_join_rejected = false;
        try {
            co_await empty_scope.join();
        } catch (const std::logic_error&) {
            second_join_rejected = true;
        }
        bool spawn_after_join_rejected = false;
        try {
            empty_scope.spawn(no_op());
        } catch (const std::logic_error&) {
            spawn_after_join_rejected = true;
        }
        if (!second_join_rejected || !spawn_after_join_rejected) {
            co_return;
        }
    }

    {
        ruvia::task_scope empty_task_scope(worker_value);
        auto moved_from = no_op();
        auto retained = std::move(moved_from);
        static_cast<void>(retained);
        bool empty_task_rejected = false;
        try {
            empty_task_scope.spawn(std::move(moved_from));
        } catch (const std::logic_error&) {
            empty_task_rejected = true;
        }
        co_await empty_task_scope.join();
        if (!empty_task_rejected || empty_task_scope.size() != 0) {
            co_return;
        }
    }

    {
        int completed = 0;
        ruvia::task_scope cold_join_scope(worker_value);
        cold_join_scope.spawn(increment(worker_value, completed));
        auto first_join = cold_join_scope.join();
        std::optional<ruvia::task_scope::join_operation> second_join;
        bool spawn_before_join_started = false;
        try {
            second_join.emplace(cold_join_scope.join());
        } catch (const std::logic_error&) {
        }
        try {
            cold_join_scope.spawn(increment(worker_value, completed));
            spawn_before_join_started = true;
        } catch (const std::logic_error&) {
        }
        co_await std::move(first_join);
        bool repeated_start_rejected = false;
        if (second_join) {
            try {
                co_await std::move(*second_join);
            } catch (const std::logic_error&) {
                repeated_start_rejected = true;
            }
        }
        if (!spawn_before_join_started || !repeated_start_rejected || completed != 2) {
            co_return;
        }
    }

    {
        int completed = 0;
        ruvia::task_scope running_scope(worker_value);
        auto first_join = running_scope.join();
        auto competing_join = running_scope.join();
        running_scope.spawn(increment(worker_value, completed));
        bool join_completed = false;
        ruvia::task_scope observer(worker_value);
        observer.spawn(observe_join(std::move(first_join), join_completed));
        bool competing_join_rejected = false;
        try {
            co_await std::move(competing_join);
        } catch (const std::logic_error&) {
            competing_join_rejected = true;
        }
        bool spawn_during_join_rejected = false;
        try {
            running_scope.spawn(no_op());
        } catch (const std::logic_error&) {
            spawn_during_join_rejected = true;
        }
        co_await observer.join();
        if (!join_completed || !competing_join_rejected || !spawn_during_join_rejected || completed != 1) {
            co_return;
        }
    }

    {
        ruvia::stop_token retained;
        int notifications = 0;
        {
            ruvia::task_scope scope(worker_value);
            const auto& observed = scope;
            retained = observed.token();
            ruvia::stop_registration registration;
            retained.register_callback(registration, [&] { ++notifications; });
            scope.request_stop();
            scope.request_stop();
            if (observed.token() != retained || !observed.stop_requested() ||
                !retained.stop_requested() || notifications != 1) {
                co_return;
            }
        }
        if (!retained.stop_requested() || notifications != 1) {
            co_return;
        }
    }

    {
        ruvia::task_scope cancelled_scope(worker_value);
        {
            auto discarded_join = cancelled_scope.join();
            static_cast<void>(discarded_join);
        }
        bool stopped = false;
        cancelled_scope.spawn(wait_for_stop(worker_value, cancelled_scope.token(), stopped));
        auto join = cancelled_scope.join();
        cancelled_scope.request_stop();
        co_await std::move(join);
        if (!stopped || !cancelled_scope.stop_requested() || !cancelled_scope.empty() || cancelled_scope.size() != 0) {
            co_return;
        }
    }

    {
        // A scope whose children all completed may be destroyed without a
        // join; an unobserved failure is reported instead of lost.
        int completed = 0;
        {
            ruvia::task_scope unjoined_scope(worker_value);
            unjoined_scope.spawn(increment(worker_value, completed));
            unjoined_scope.spawn(fail());
            // Completion is delivered after the timer pass that resumed the
            // child, so wait until every child has been retired.
            while (unjoined_scope.size() != 0) {
                static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
            }
            if (!unjoined_scope.stop_requested()) {
                co_return;
            }
        }
        if (completed != 1) {
            co_return;
        }
    }
    {
        bool scope_clean_on_retire = true;
        struct unjoined_scope_holder final {
            explicit unjoined_scope_holder(const ruvia::worker_ref& w, bool& clean)
                : clean_(clean),
                  scope_(w) {}
            ~unjoined_scope_holder() {
                if (!scope_.empty()) {
                    clean_ = false;
                }
            }
            bool& clean_;
            ruvia::task_scope scope_;
        };
        auto holder = std::make_shared<unjoined_scope_holder>(worker_value, scope_clean_on_retire);
        auto child_with_holder = [worker_value](std::shared_ptr<unjoined_scope_holder> owned, int delay_ms) -> ruvia::task<void> {
            if (delay_ms > 0) {
                static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(delay_ms)));
            }
            static_cast<void>(owned);
            co_return;
        };
        for (int index = 0; index < 16; ++index) {
            holder->scope_.spawn(child_with_holder(holder, index % 3));
        }
        std::weak_ptr<unjoined_scope_holder> weak_holder = holder;
        holder.reset();
        while (!weak_holder.expired()) {
            static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        }
        if (!scope_clean_on_retire) {
            co_return;
        }
    }

    {
        ruvia::task_scope completed_failure_scope(worker_value);
        completed_failure_scope.spawn(fail());
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        bool completed_failure_observed = false;
        try {
            co_await completed_failure_scope.join();
        } catch (const std::runtime_error& error) {
            completed_failure_observed = std::string_view(error.what()) == "child failed";
        }
        if (!completed_failure_observed) {
            co_return;
        }
    }

    int calls = 0;
    ruvia::task_scope scope(worker_value);
    scope.spawn(increment(worker_value, calls));
    scope.spawn(fail());
    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        success = calls == 1 && scope.size() == 0 && scope.stop_requested() &&
                  std::string_view(error.what()) == "child failed";
    }
}

#ifndef _WIN32
ruvia::task<void> fail_distinct() {
    throw std::logic_error("second child failed");
    co_return;
}

ruvia::task<void> join_distinct_failures(ruvia::worker_ref worker_value, bool& first_rethrown) {
    ruvia::task_scope scope(worker_value);
    scope.spawn(fail());
    scope.spawn(fail());
    scope.spawn(fail_distinct());
    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        first_rethrown = std::string_view(error.what()) == "child failed";
    }
}

// Redirects stderr, where the unhandled-failure diagnostic writes, into a
// temporary file for the guard's lifetime.
class stderr_capture final {
public:
    stderr_capture()
        : sink_(std::tmpfile()),
          saved_(::dup(STDERR_FILENO)) {
        std::fflush(stderr);
        if (sink_ != nullptr && saved_ >= 0) {
            redirected_ = ::dup2(::fileno(sink_), STDERR_FILENO) >= 0;
        }
    }
    stderr_capture(const stderr_capture&) = delete;
    stderr_capture& operator=(const stderr_capture&) = delete;
    ~stderr_capture() {
        restore();
        if (sink_ != nullptr) {
            std::fclose(sink_);
        }
    }

    // Restores stderr and returns everything written while redirected.
    [[nodiscard]] std::string finish() {
        restore();
        std::string text;
        if (sink_ == nullptr) {
            return text;
        }
        std::rewind(sink_);
        char chunk[256];
        std::size_t read_bytes = 0;
        while ((read_bytes = std::fread(chunk, 1, sizeof(chunk), sink_)) != 0) {
            text.append(chunk, read_bytes);
        }
        return text;
    }

private:
    void restore() noexcept {
        std::fflush(stderr);
        if (redirected_) {
            static_cast<void>(::dup2(saved_, STDERR_FILENO));
            redirected_ = false;
        }
        if (saved_ >= 0) {
            ::close(saved_);
            saved_ = -1;
        }
    }

    std::FILE* sink_;
    int saved_;
    bool redirected_{false};
};

// join rethrows the first child failure; a later, distinct one must still reach
// the unhandled-failure diagnostic, and one equivalent to the first must not
// be reported again.
bool verify_additional_child_failures_are_reported() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    bool first_rethrown = false;
    stderr_capture capture;
    ruvia::test::run_worker_tasks(attachment, join_distinct_failures(attachment.loop().worker(), first_rethrown));
    const auto text = capture.finish();
    return first_rethrown &&
           text.find("ruvia: additional task scope child failed: second child failed") != std::string_view::npos &&
           text.find("additional task scope child failed: child failed") == std::string_view::npos;
}
#endif

}  // namespace ruvia::test::task_scope_tests

TEST(Ruvia, TaskScopeLifecycle) {
    ruvia::stop_token retained_token;
    {
        ruvia::stop_source source;
        retained_token = source.token();
        source.request_stop();
    }
    EXPECT_TRUE(retained_token.stop_requested());

    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    bool success = false;
    bool reentrant_stop_join_safe = false;
    ruvia::test::run_worker_tasks(attachment,
        ruvia::test::task_scope_tests::exercise(worker_value, success),
        ruvia::test::task_scope_tests::verify_stop_callback_reentrant_join_waits_for_finish(
            worker_value, reentrant_stop_join_safe));
    bool additional_failures_reported = true;
#ifndef _WIN32
    additional_failures_reported = ruvia::test::task_scope_tests::verify_additional_child_failures_are_reported();
#endif
    EXPECT_TRUE(success);
    EXPECT_TRUE(reentrant_stop_join_safe);
    EXPECT_TRUE(additional_failures_reported);
}
