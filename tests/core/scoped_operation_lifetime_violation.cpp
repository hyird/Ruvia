#include <ruvia/core/Task.h>
#include <ruvia/web/ScopedOperation.h>

#include <coroutine>
#include <cstdlib>
#include <exception>
#include <utility>

namespace {

ruvia::Task<void> suspendedOperation() {
    co_await std::suspend_always{};
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
        void unhandled_exception() const noexcept { std::terminate(); }
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

    void start() const { handle_.resume(); }

private:
    std::coroutine_handle<promise_type> handle_;
};

ManualOwner startScopedOperation(ruvia::ScopedOperation<void>& operation) {
    co_await std::move(operation);
}

}  // namespace

int main() {
    std::set_terminate([] { std::_Exit(86); });
    ruvia::detail::ScopedOperationScope scope;
    auto operation = ruvia::detail::makeScopedOperation(
        scope, suspendedOperation());
    auto owner = startScopedOperation(operation);
    owner.start();
    scope.close();
    return 0;
}
