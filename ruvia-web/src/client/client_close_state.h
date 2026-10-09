#pragma once

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include <asio/co_spawn.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/worker_signal.h"

namespace ruvia::detail {

// Worker-affine completion state used by the composed client_lifecycle and
// protocol-specific retirement policies. Owns one-shot close completion and
// fatal cleanup failure reporting. Callers may observe the stored failure;
// runtime retirement only confirms completion after reporting it once.
class client_close_state final {
public:
    enum class observation_mode_type : unsigned char {
        caller,
        retirement,
    };

    // Both capabilities are borrowed from the client's address-stable owner.
    client_close_state(const event_loop& loop, const worker_handle& worker_value)
        : loop_(loop),
          signal_(worker_value) {}
    client_close_state(event_loop&&, const worker_handle&) = delete;
    client_close_state(const event_loop&&, const worker_handle&) = delete;
    client_close_state(const event_loop&, worker_handle&&) = delete;
    client_close_state(const event_loop&, const worker_handle&&) = delete;

    [[nodiscard]] bool task_started() const noexcept {
        return task_started_;
    }

    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }

    // One owner-preserving root cleanup bridge for every standalone client.
    // The factory is invoked only by the task that wins one-shot retirement.
    template <typename owner_type, typename factory_type, typename completion_type>
    void start_cleanup(std::shared_ptr<owner_type> owner_value, factory_type factory,
        completion_type completion) noexcept {
        if (!loop_.is_current()) {
            std::terminate();
        }
        if (task_started_ || complete_) {
            return;
        }
        task_started_ = true;
        try {
            asio::co_spawn(loop_.executor(), ruvia::as_awaitable(factory()),
                [owner_value = std::move(owner_value), completion = std::move(completion)](std::exception_ptr failure) mutable {
                    completion(std::move(failure));
                });
        } catch (...) {
            std::terminate();
        }
    }

    template <typename owner_type, typename start_type>
    [[nodiscard]] task<void> shutdown_owned(std::shared_ptr<owner_type> owner_value,
        start_type start, observation_mode_type mode) {
        if (!loop_.is_current()) {
            throw std::logic_error("client shutdown must run on its bound event loop");
        }
        (void)owner_value;  // Keep the address-stable client in the coroutine frame.
        start();
        while (!complete_) {
            co_await wait();
        }
        observe_failure(mode);
    }

    [[nodiscard]] auto wait() {
        return signal_.wait();
    }

    void notify_progress() noexcept {
        signal_.notify();
    }

    void complete_now() noexcept {
        complete_ = true;
        signal_.notify();
    }

    // Construction failed before a client or stop callback was published.
    // No worker can observe this state yet, so no signal notification is needed.
    // A published client must complete on its worker, even if never connected.
    void complete_before_publication() noexcept {
        if (task_started_ || complete_) {
            std::terminate();
        }
        complete_ = true;
    }

    void finish(std::exception_ptr failure) noexcept {
        if (!task_started_ || complete_) {
            std::terminate();
        }
        failure_ = std::move(failure);
        const bool report = failure_ != nullptr && !failure_reported_;
        failure_reported_ = failure_reported_ || report;
        complete_ = true;
        signal_.notify();
        if (report) {
            loop_.report_failure(failure_);
        }
    }

    void observe_failure(observation_mode_type mode) const {
        if (mode == observation_mode_type::caller && failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    const event_loop& loop_;
    worker_signal signal_;
    bool task_started_{false};
    bool complete_{false};
    bool failure_reported_{false};
    std::exception_ptr failure_;
};

}  // namespace ruvia::detail
