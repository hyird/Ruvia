#include <ruvia/core/Task.h>

#include <coroutine>
#include <cstdlib>
#include <exception>
#include <string_view>

namespace {

ruvia::Task<void> suspendedChild() {
    co_await std::suspend_always{};
}

// Task<T> is a distinct class template from the Task<void> specialisation and
// carries its own copy of the reset() ownership check, so it needs its own
// probe: a regression in one body leaves the other intact, and every guard over
// Task.h matches whole-file.
ruvia::Task<int> suspendedValueChild() {
    co_await std::suspend_always{};
    co_return 7;
}

class ManualOwner final {
public:
    struct promise_type {
        [[nodiscard]] ManualOwner get_return_object() noexcept {
            return ManualOwner(
                std::coroutine_handle<promise_type>::from_promise(*this));
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
            return {};
        }
        [[nodiscard]] std::suspend_always final_suspend() const noexcept {
            return {};
        }
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept {
            std::terminate();
        }
    };

    explicit ManualOwner(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    ~ManualOwner() {
        if (handle_ != nullptr) {
            handle_.destroy();
        }
    }

    ManualOwner(const ManualOwner&) = delete;
    ManualOwner& operator=(const ManualOwner&) = delete;

    void start() const {
        handle_.resume();
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

ManualOwner destroyParentWhileChildIsSuspended() {
    co_await suspendedChild();
}

ManualOwner destroyParentWhileValueChildIsSuspended() {
    (void)co_await suspendedValueChild();
}

}  // namespace

// Each probe terminates the process, so exactly one runs per invocation. An
// unrecognised selector runs the Task<void> probe rather than failing: under
// WILL_FAIL any non-zero exit reads as success, so a usage-error path would
// pass for the wrong reason.
int main(int argc, char** argv) {
    std::set_terminate([] { std::_Exit(86); });
    const std::string_view probe(argc > 1 ? argv[1] : "void");
    if (probe == "value") {
        auto owner = destroyParentWhileValueChildIsSuspended();
        owner.start();
    } else {
        auto owner = destroyParentWhileChildIsSuspended();
        owner.start();
    }
    return 0;
}
