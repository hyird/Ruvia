#include "ruvia/core/task_scope.h"

#include <chrono>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/timer.h"

namespace {

ruvia::task<void> increment(ruvia::worker_handle worker_value, int& value) {
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

ruvia::task<void> exercise(ruvia::worker_handle worker_value, bool& success) {
    {
        ruvia::task_scope empty_scope(worker_value);
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
        if (!empty_task_rejected || empty_task_scope.size() != 0) {
            co_return;
        }
        co_await empty_task_scope.join();
    }

    {
        ruvia::task_scope reserved_join_scope(worker_value);
        reserved_join_scope.spawn(no_op());
        auto reserved_join = reserved_join_scope.join();
        bool spawn_after_reservation_rejected = false;
        try {
            reserved_join_scope.spawn(no_op());
        } catch (const std::logic_error&) {
            spawn_after_reservation_rejected = true;
        }
        if (!spawn_after_reservation_rejected) {
            co_return;
        }
        co_await std::move(reserved_join);
    }

    {
        ruvia::task_scope completed_failure_scope(worker_value);
        completed_failure_scope.spawn(fail());
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        if (completed_failure_scope.size() != 0) {
            co_return;
        }
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
    if (scope.size() != 2) {
        co_return;
    }

    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        success = calls == 1 && scope.size() == 0 && scope.stop_requested() &&
                  std::string_view(error.what()) == "child failed";
    }
}

}  // namespace

int main() {
    ruvia::stop_token retained_token;
    {
        ruvia::stop_source source;
        retained_token = source.token();
        source.request_stop();
    }
    if (!retained_token.stop_requested()) {
        return 1;
    }

    asio::io_context io_context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 8);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    bool success = false;

    asio::co_spawn(
        io_context, ruvia::detail::task_as_awaitable(exercise(worker_value, success)), asio::detached);
    io_context.run();
    dispatcher->close();
    return success ? 0 : 1;
}
