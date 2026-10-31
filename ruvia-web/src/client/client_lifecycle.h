#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_signal.h"

namespace ruvia::detail {

enum class client_phase : unsigned char { fresh,
    connecting,
    connected,
    closing,
    closed };

// Composed by an address-stable protocol owner, after its backend and memory.
// Owns the sole standalone-client startup/cancel/retirement chain. Backend
// connect/close/join and the not-ready error are explicit compile-time policies.
// No type erasure, extra sharing, or locking is added to operation admission.
template <typename owner_type>
class client_lifecycle final {
public:
    client_lifecycle(owner_type& owner_value, const event_loop& loop, const worker_handle& worker_value,
        client_phase initial = client_phase::fresh)
        : owner_(owner_value),
          loop_(loop),
          worker_(worker_value),
          close_signal_(worker_value),
          phase_(initial) {
        operations_.bind_worker(worker_value);
    }

    ~client_lifecycle() {
        if (phase_.load(std::memory_order_acquire) != client_phase::closed ||
            !close_complete_ || operations_.has_pending_operations()) {
            std::terminate();
        }
    }

    client_lifecycle(const client_lifecycle&) = delete;
    client_lifecycle& operator=(const client_lifecycle&) = delete;

    [[nodiscard]] static event_loop require_loop(event_loop loop) {
        if (!loop.valid()) {
            throw std::invalid_argument(message(" requires a valid event loop"));
        }
        return loop;
    }

    void bind_stop() {
        try {
            std::weak_ptr<owner_type> weak = owner_.shared_from_this();
            stop_registration_ = loop_.on_stop([weak = std::move(weak)]() -> task<void> {
                if (const auto state = weak.lock()) {
                    co_await shutdown_owned(state, shutdown_mode::retirement);
                }
            });
        } catch (...) {
            owner_.backend().close_now();
            phase_.store(client_phase::closed, std::memory_order_release);
            // No client or stop callback has been published yet.
            if (close_started_ || close_complete_) {
                std::terminate();
            }
            close_complete_ = true;
            throw;
        }
    }

    [[nodiscard]] task<void> connect() {
        return connect_owned(owner_.shared_from_this());
    }

    void require_ready() const {
        require_current(" must be used on its bound event loop");
        if (phase_.load(std::memory_order_acquire) != client_phase::connected) {
            owner_type::throw_not_ready();
        }
    }

    [[nodiscard]] ::ruvia::operation_scope& operation_scope() noexcept {
        return operations_;
    }

    [[nodiscard]] operation_options options(operation_options options) const {
        return merge_operation_options(
            operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_.token()}, std::move(options));
    }

    void request_close() noexcept {
        stop_.request_stop();
        if (!begin_close()) {
            return;
        }
        if (worker_.is_current()) {
            start_close();
            return;
        }
        try {
            if (!loop_.defer_cleanup([state = owner_.shared_from_this()] { state->lifecycle_.start_close(); }) &&
                phase_.load(std::memory_order_acquire) != client_phase::closed) {
                std::terminate();
            }
        } catch (...) {
            if (phase_.load(std::memory_order_acquire) != client_phase::closed) {
                std::terminate();
            }
        }
    }

    [[nodiscard]] task<void> shutdown() {
        return shutdown_owned(owner_.shared_from_this(), shutdown_mode::caller);
    }

private:
    enum class shutdown_mode : unsigned char { caller,
        retirement };

    [[nodiscard]] static std::string message(std::string_view suffix) {
        std::string result(owner_type::client_name);
        result.append(" client");
        result.append(suffix);
        return result;
    }

    void require_current(std::string_view suffix) const {
        if (!worker_.is_current()) {
            throw std::logic_error(message(suffix));
        }
    }

    [[nodiscard]] static task<void> connect_owned(std::shared_ptr<owner_type> state_value) {
        co_await state_value->lifecycle_.connect_on_worker();
    }

    task<void> connect_on_worker() {
        require_current(" must connect on its bound event loop");
        auto expected = client_phase::fresh;
        if (!phase_.compare_exchange_strong(expected, client_phase::connecting,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (expected == client_phase::closing || expected == client_phase::closed) {
                throw std::runtime_error(message(" closed before connecting"));
            }
            throw std::logic_error(message(" can only connect once"));
        }
        // Only the startup lease holder may tear down a failed connection.
        try {
            connecting_ = true;
            if (stop_.stop_requested() || !worker_.accepting()) {
                throw std::runtime_error(message(" closed before connecting"));
            }
            co_await owner_.backend().connect();
            expected = client_phase::connecting;
            if (!phase_.compare_exchange_strong(expected, client_phase::connected,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                throw std::runtime_error(message(" stopped while connecting"));
            }
            connecting_ = false;
            close_signal_.notify();
        } catch (...) {
            owner_.backend().close_now();
            stop_.request_stop();
            phase_.store(client_phase::closed, std::memory_order_release);
            connecting_ = false;
            if (close_started_) {
                close_signal_.notify();
            } else if (!close_complete_) {
                close_complete_ = true;
                close_signal_.notify();
            }
            throw;
        }
    }

    [[nodiscard]] bool begin_close() noexcept {
        auto phase = phase_.load(std::memory_order_acquire);
        while (phase != client_phase::closed && phase != client_phase::closing) {
            if (phase_.compare_exchange_weak(phase, client_phase::closing,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static task<void> shutdown_owned(std::shared_ptr<owner_type> state_value,
        shutdown_mode mode) {
        auto& lifecycle = state_value->lifecycle_;
        if (!lifecycle.loop_.is_current()) {
            throw std::logic_error("client shutdown must run on its bound event loop");
        }
        // The coroutine frame retains the client until its cleanup completes.
        lifecycle.start_close();
        while (!lifecycle.close_complete_) {
            co_await lifecycle.close_signal_.wait();
        }
        if (mode == shutdown_mode::caller && lifecycle.close_failure_) {
            std::rethrow_exception(lifecycle.close_failure_);
        }
    }

    void start_close() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        (void)begin_close();
        stop_.request_stop();
        owner_.backend().close_now();
        if (close_started_ || close_complete_) {
            return;
        }
        close_started_ = true;
        try {
            asio::co_spawn(loop_.executor(), ruvia::as_awaitable(close_on_worker()),
                [state = owner_.shared_from_this()](std::exception_ptr failure) {
                    state->lifecycle_.finish_close(std::move(failure));
                });
        } catch (...) {
            std::terminate();
        }
    }

    task<void> close_on_worker() {
        while (connecting_) {
            co_await close_signal_.wait();
        }
        std::exception_ptr failure;
        if constexpr (requires { owner_.backend().join(); }) {
            try {
                co_await owner_.backend().join();
            } catch (...) {
                failure = std::current_exception();
            }
        }
        try {
            co_await operations_.close_and_join();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

    void finish_close(std::exception_ptr failure) noexcept {
        if (!close_started_ || close_complete_ || connecting_ || operations_.has_pending_operations()) {
            std::terminate();
        }
        phase_.store(client_phase::closed, std::memory_order_release);
        close_failure_ = std::move(failure);
        close_complete_ = true;
        close_signal_.notify();
        if (close_failure_) {
            loop_.report_failure(close_failure_);
        }
    }

    owner_type& owner_;
    const event_loop& loop_;
    const worker_handle& worker_;
    stop_source stop_;
    event_loop_stop_registration stop_registration_;
    worker_signal close_signal_;
    bool close_started_{};
    bool close_complete_{};
    std::exception_ptr close_failure_;
    std::atomic<client_phase> phase_;
    bool connecting_{};
    // Last: invalidate cold operations before backend and allocator retirement.
    ::ruvia::operation_scope operations_;
};

}  // namespace ruvia::detail
