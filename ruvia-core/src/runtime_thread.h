#pragma once

#if defined(__linux__)
#include <pthread.h>

#include <cerrno>
#include <cstddef>
#include <exception>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>
#else
#include <thread>
#endif

namespace ruvia::detail {

#if defined(__linux__)
class runtime_thread final {
    using entry_type = void* (*)(void*) noexcept;

public:
    runtime_thread() noexcept = default;

    // A runtime remains alive until its thread is joined, so no launch object is needed.
    runtime_thread(entry_type entry, void* state) {
        start(entry, state);
    }

    // A detached pool thread must own its launch state independently of this thread object.
    template <typename function_type>
        requires std::is_nothrow_invocable_v<function_type&>
    explicit runtime_thread(function_type&& function) {
        using callable_type = std::decay_t<function_type>;
        auto* const callable = new callable_type(std::forward<function_type>(function));
        try {
            start([](void* state) noexcept -> void* {
                const std::unique_ptr<callable_type> running(static_cast<callable_type*>(state));
                (*running)();
                return nullptr;
            }, callable);
        } catch (...) {
            delete callable;
            throw;
        }
    }

    runtime_thread(const runtime_thread&) = delete;
    runtime_thread& operator=(const runtime_thread&) = delete;

    runtime_thread(runtime_thread&& other) noexcept :
        thread_(other.thread_),
        joinable_(std::exchange(other.joinable_, false)) {}

    runtime_thread& operator=(runtime_thread&& other) noexcept {
        if (joinable_) {
            std::terminate();
        }
        thread_ = other.thread_;
        joinable_ = std::exchange(other.joinable_, false);
        return *this;
    }

    ~runtime_thread() {
        if (joinable_) {
            std::terminate();
        }
    }

    [[nodiscard]] bool joinable() const noexcept {
        return joinable_;
    }

    void join() {
        check(joinable_ ? ::pthread_join(thread_, nullptr) : EINVAL, "pthread_join");
        joinable_ = false;
    }

    void detach() {
        check(joinable_ ? ::pthread_detach(thread_) : EINVAL, "pthread_detach");
        joinable_ = false;
    }

private:
    static void check(int error, const char* operation) {
        if (error != 0) {
            throw std::system_error(error, std::generic_category(), operation);
        }
    }

    void start(entry_type entry, void* state) {
        // Request the stack on this thread only, independent of libc or executable defaults.
        constexpr std::size_t stack_size = 8 * 1024 * 1024;
        pthread_attr_t attributes;
        check(::pthread_attr_init(&attributes), "pthread_attr_init");
        int error = ::pthread_attr_setstacksize(&attributes, stack_size);
        const char* operation = "pthread_attr_setstacksize";
        if (error == 0) {
            error = ::pthread_create(&thread_, &attributes, entry, state);
            operation = "pthread_create";
        }
        (void)::pthread_attr_destroy(&attributes);
        check(error, operation);
        joinable_ = true;
    }

    pthread_t thread_{};
    bool joinable_{false};
};
#else
using runtime_thread = std::thread;
#endif

}  // namespace ruvia::detail
